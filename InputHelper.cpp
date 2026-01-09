// InputHelper.cpp - Windows Service for UAC-aware input injection
// Runs as SYSTEM to enable input on Secure Desktop (UAC, lock screen)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <string>
#include <cstdint>
#include <cstdio>

// Service configuration
#define SERVICE_NAME L"ScreenShareInput"
#define PIPE_NAME L"\\\\.\\pipe\\ScreenShareInput"

// Message types (must match common.hpp)
constexpr uint32_t MSG_MOUSE_MOVE  = 0x4D4F5645;
constexpr uint32_t MSG_MOUSE_BTN   = 0x4D42544E;
constexpr uint32_t MSG_MOUSE_WHEEL = 0x4D57484C;
constexpr uint32_t MSG_KEY         = 0x4B455920;
constexpr uint32_t MSG_MONITOR_BOUNDS = 0x4D4F4E42;

// Input message structure
#pragma pack(push, 1)
struct InputMsg {
    uint32_t type;
    union {
        struct { float x, y; } move;
        struct { uint8_t button, action; } btn;
        struct { int16_t deltaX, deltaY; } wheel;
        struct { uint16_t keyCode, scanCode; uint8_t action, modifiers; } key;
        struct { int32_t x, y, w, h; } bounds;
    };
};
#pragma pack(pop)

// Service globals
SERVICE_STATUS_HANDLE g_StatusHandle = NULL;
SERVICE_STATUS g_ServiceStatus = {};
HANDLE g_StopEvent = NULL;
volatile bool g_Running = true;

// Monitor bounds
int g_MonitorX = 0, g_MonitorY = 0, g_MonitorW = 1920, g_MonitorH = 1080;
int g_VirtScreenX = 0, g_VirtScreenY = 0, g_VirtScreenW = 1920, g_VirtScreenH = 1080;

// Forward declarations
void WINAPI ServiceMain(DWORD argc, LPWSTR* argv);
void WINAPI ServiceCtrlHandler(DWORD ctrl);
void RunInputServer();

// Logging
void Log(const char* fmt, ...) {
    char buf[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    OutputDebugStringA(buf);
    OutputDebugStringA("\n");
}

// Install service
bool InstallService() {
    wchar_t path[MAX_PATH];
    if (!GetModuleFileNameW(NULL, path, MAX_PATH)) {
        printf("Failed to get module path\n");
        return false;
    }

    SC_HANDLE scm = OpenSCManager(NULL, NULL, SC_MANAGER_CREATE_SERVICE);
    if (!scm) {
        printf("Failed to open SCM (run as admin)\n");
        return false;
    }

    // Check if service already exists
    SC_HANDLE existing = OpenServiceW(scm, SERVICE_NAME, SERVICE_QUERY_STATUS);
    if (existing) {
        printf("Service already installed\n");
        CloseServiceHandle(existing);
        CloseServiceHandle(scm);
        return true;
    }

    SC_HANDLE svc = CreateServiceW(
        scm,
        SERVICE_NAME,
        L"ScreenShare Input Helper",
        SERVICE_ALL_ACCESS,
        SERVICE_WIN32_OWN_PROCESS,
        SERVICE_AUTO_START,
        SERVICE_ERROR_NORMAL,
        path,
        NULL, NULL, NULL,
        NULL,  // LocalSystem account
        NULL
    );

    if (!svc) {
        DWORD err = GetLastError();
        printf("Failed to create service: %lu\n", err);
        CloseServiceHandle(scm);
        return false;
    }

    // Configure service to restart on failure
    SERVICE_FAILURE_ACTIONSW fa = {};
    SC_ACTION actions[3] = {
        { SC_ACTION_RESTART, 1000 },  // Restart after 1 second
        { SC_ACTION_RESTART, 5000 },  // Restart after 5 seconds
        { SC_ACTION_RESTART, 10000 }  // Restart after 10 seconds
    };
    fa.dwResetPeriod = 86400;  // Reset failure count after 1 day
    fa.cActions = 3;
    fa.lpsaActions = actions;
    ChangeServiceConfig2W(svc, SERVICE_CONFIG_FAILURE_ACTIONS, &fa);

    // Start the service
    if (StartServiceW(svc, 0, NULL)) {
        printf("Service installed and started\n");
    } else {
        DWORD err = GetLastError();
        if (err == ERROR_SERVICE_ALREADY_RUNNING) {
            printf("Service installed (already running)\n");
        } else {
            printf("Service installed but failed to start: %lu\n", err);
        }
    }

    CloseServiceHandle(svc);
    CloseServiceHandle(scm);
    return true;
}

// Uninstall service
bool UninstallService() {
    SC_HANDLE scm = OpenSCManager(NULL, NULL, SC_MANAGER_ALL_ACCESS);
    if (!scm) {
        printf("Failed to open SCM (run as admin)\n");
        return false;
    }

    SC_HANDLE svc = OpenServiceW(scm, SERVICE_NAME, SERVICE_ALL_ACCESS);
    if (!svc) {
        DWORD err = GetLastError();
        if (err == ERROR_SERVICE_DOES_NOT_EXIST) {
            printf("Service not installed\n");
            CloseServiceHandle(scm);
            return true;
        }
        printf("Failed to open service: %lu\n", err);
        CloseServiceHandle(scm);
        return false;
    }

    // Stop service if running
    SERVICE_STATUS status;
    if (ControlService(svc, SERVICE_CONTROL_STOP, &status)) {
        printf("Stopping service...\n");
        Sleep(1000);
    }

    // Delete service
    if (DeleteService(svc)) {
        printf("Service uninstalled\n");
    } else {
        printf("Failed to delete service: %lu\n", GetLastError());
    }

    CloseServiceHandle(svc);
    CloseServiceHandle(scm);
    return true;
}

// Desktop-aware input injection
class DesktopInputInjector {
    HDESK currentDesk = NULL;

public:
    DesktopInputInjector() {
        UpdateVirtualScreen();
    }

    ~DesktopInputInjector() {
        if (currentDesk) CloseDesktop(currentDesk);
    }

    void UpdateVirtualScreen() {
        g_VirtScreenX = GetSystemMetrics(SM_XVIRTUALSCREEN);
        g_VirtScreenY = GetSystemMetrics(SM_YVIRTUALSCREEN);
        g_VirtScreenW = GetSystemMetrics(SM_CXVIRTUALSCREEN);
        g_VirtScreenH = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    }

    // CRITICAL: Switch to the currently active input desktop before SendInput
    bool SwitchToInputDesktop() {
        HDESK desk = OpenInputDesktop(0, FALSE, GENERIC_ALL);
        if (!desk) {
            Log("OpenInputDesktop failed: %lu", GetLastError());
            return false;
        }

        if (currentDesk != desk) {
            if (!SetThreadDesktop(desk)) {
                Log("SetThreadDesktop failed: %lu", GetLastError());
                CloseDesktop(desk);
                return false;
            }
            if (currentDesk) CloseDesktop(currentDesk);
            currentDesk = desk;
        } else {
            CloseDesktop(desk);
        }
        return true;
    }

    void MouseMove(float nx, float ny) {
        if (!SwitchToInputDesktop()) return;

        // Clamp normalized coordinates
        nx = nx < 0.f ? 0.f : (nx > 1.f ? 1.f : nx);
        ny = ny < 0.f ? 0.f : (ny > 1.f ? 1.f : ny);

        int px = g_MonitorX + (int)(nx * g_MonitorW);
        int py = g_MonitorY + (int)(ny * g_MonitorH);

        LONG ax = (LONG)((px - g_VirtScreenX) * 65535 / g_VirtScreenW);
        LONG ay = (LONG)((py - g_VirtScreenY) * 65535 / g_VirtScreenH);

        INPUT inp = {};
        inp.type = INPUT_MOUSE;
        inp.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
        inp.mi.dx = ax;
        inp.mi.dy = ay;
        SendInput(1, &inp, sizeof(INPUT));
    }

    void MouseButton(uint8_t btn, bool down) {
        if (!SwitchToInputDesktop()) return;

        static const DWORD flags[5][2] = {
            {MOUSEEVENTF_LEFTUP, MOUSEEVENTF_LEFTDOWN},
            {MOUSEEVENTF_RIGHTUP, MOUSEEVENTF_RIGHTDOWN},
            {MOUSEEVENTF_MIDDLEUP, MOUSEEVENTF_MIDDLEDOWN},
            {MOUSEEVENTF_XUP, MOUSEEVENTF_XDOWN},
            {MOUSEEVENTF_XUP, MOUSEEVENTF_XDOWN}
        };

        if (btn > 4) return;

        INPUT inp = {};
        inp.type = INPUT_MOUSE;
        inp.mi.dwFlags = flags[btn][down ? 1 : 0];
        if (btn >= 3) inp.mi.mouseData = (btn == 3) ? XBUTTON1 : XBUTTON2;
        SendInput(1, &inp, sizeof(INPUT));
    }

    void MouseWheel(int16_t dx, int16_t dy) {
        if (!SwitchToInputDesktop()) return;

        if (dy != 0) {
            INPUT inp = {};
            inp.type = INPUT_MOUSE;
            inp.mi.dwFlags = MOUSEEVENTF_WHEEL;
            inp.mi.mouseData = (DWORD)(-dy * WHEEL_DELTA / 100);
            SendInput(1, &inp, sizeof(INPUT));
        }
        if (dx != 0) {
            INPUT inp = {};
            inp.type = INPUT_MOUSE;
            inp.mi.dwFlags = MOUSEEVENTF_HWHEEL;
            inp.mi.mouseData = (DWORD)(dx * WHEEL_DELTA / 100);
            SendInput(1, &inp, sizeof(INPUT));
        }
    }

    void Key(uint16_t vk, uint16_t scan, bool down) {
        if (!SwitchToInputDesktop()) return;

        INPUT inp = {};
        inp.type = INPUT_KEYBOARD;
        inp.ki.wVk = vk;
        inp.ki.wScan = scan ? scan : (WORD)MapVirtualKey(vk, MAPVK_VK_TO_VSC);
        inp.ki.dwFlags = down ? 0 : KEYEVENTF_KEYUP;

        // Extended key detection
        if (vk == VK_INSERT || vk == VK_DELETE || vk == VK_HOME || vk == VK_END ||
            vk == VK_PRIOR || vk == VK_NEXT || vk == VK_LEFT || vk == VK_RIGHT ||
            vk == VK_UP || vk == VK_DOWN || vk == VK_LWIN || vk == VK_RWIN ||
            vk == VK_APPS || vk == VK_DIVIDE || vk == VK_NUMLOCK ||
            vk == VK_RCONTROL || vk == VK_RMENU) {
            inp.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
        }

        SendInput(1, &inp, sizeof(INPUT));
    }
};

DesktopInputInjector* g_Injector = nullptr;

void HandleMessage(const InputMsg& msg) {
    if (!g_Injector) return;

    switch (msg.type) {
        case MSG_MOUSE_MOVE:
            g_Injector->MouseMove(msg.move.x, msg.move.y);
            break;
        case MSG_MOUSE_BTN:
            g_Injector->MouseButton(msg.btn.button, msg.btn.action != 0);
            break;
        case MSG_MOUSE_WHEEL:
            g_Injector->MouseWheel(msg.wheel.deltaX, msg.wheel.deltaY);
            break;
        case MSG_KEY:
            g_Injector->Key(msg.key.keyCode, msg.key.scanCode, msg.key.action != 0);
            break;
        case MSG_MONITOR_BOUNDS:
            g_MonitorX = msg.bounds.x;
            g_MonitorY = msg.bounds.y;
            g_MonitorW = msg.bounds.w;
            g_MonitorH = msg.bounds.h;
            g_Injector->UpdateVirtualScreen();
            Log("Monitor bounds updated: %d,%d %dx%d", g_MonitorX, g_MonitorY, g_MonitorW, g_MonitorH);
            break;
    }
}

void RunInputServer() {
    Log("Input server starting...");

    g_Injector = new DesktopInputInjector();

    while (g_Running && WaitForSingleObject(g_StopEvent, 0) != WAIT_OBJECT_0) {
        // Create named pipe with security that allows admin processes to connect
        SECURITY_DESCRIPTOR sd;
        InitializeSecurityDescriptor(&sd, SECURITY_DESCRIPTOR_REVISION);
        SetSecurityDescriptorDacl(&sd, TRUE, NULL, FALSE);  // Allow all access

        SECURITY_ATTRIBUTES sa = { sizeof(sa), &sd, FALSE };

        HANDLE pipe = CreateNamedPipeW(
            PIPE_NAME,
            PIPE_ACCESS_DUPLEX,
            PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
            PIPE_UNLIMITED_INSTANCES,
            sizeof(InputMsg) * 16,
            sizeof(InputMsg) * 16,
            0,
            &sa
        );

        if (pipe == INVALID_HANDLE_VALUE) {
            Log("CreateNamedPipe failed: %lu", GetLastError());
            Sleep(1000);
            continue;
        }

        Log("Waiting for client connection...");

        // Wait for client connection (with timeout so we can check stop event)
        OVERLAPPED ov = {};
        ov.hEvent = CreateEvent(NULL, TRUE, FALSE, NULL);

        BOOL connected = ConnectNamedPipe(pipe, &ov);
        DWORD err = GetLastError();

        if (!connected && err == ERROR_IO_PENDING) {
            // Wait for connection or stop event
            HANDLE events[2] = { ov.hEvent, g_StopEvent };
            DWORD wait = WaitForMultipleObjects(2, events, FALSE, INFINITE);

            if (wait == WAIT_OBJECT_0 + 1) {
                // Stop event signaled
                CancelIo(pipe);
                CloseHandle(ov.hEvent);
                CloseHandle(pipe);
                break;
            }

            DWORD transferred;
            connected = GetOverlappedResult(pipe, &ov, &transferred, FALSE);
        } else if (err == ERROR_PIPE_CONNECTED) {
            connected = TRUE;
        }

        CloseHandle(ov.hEvent);

        if (connected) {
            Log("Client connected");
            InputMsg msg;
            DWORD bytesRead;

            while (g_Running && WaitForSingleObject(g_StopEvent, 0) != WAIT_OBJECT_0) {
                if (!ReadFile(pipe, &msg, sizeof(msg), &bytesRead, NULL)) {
                    DWORD readErr = GetLastError();
                    if (readErr == ERROR_BROKEN_PIPE || readErr == ERROR_PIPE_NOT_CONNECTED) {
                        Log("Client disconnected");
                    } else {
                        Log("ReadFile failed: %lu", readErr);
                    }
                    break;
                }

                if (bytesRead >= sizeof(uint32_t)) {
                    HandleMessage(msg);
                }
            }
        }

        DisconnectNamedPipe(pipe);
        CloseHandle(pipe);
    }

    delete g_Injector;
    g_Injector = nullptr;

    Log("Input server stopped");
}

void WINAPI ServiceMain(DWORD argc, LPWSTR* argv) {
    (void)argc; (void)argv;

    g_StatusHandle = RegisterServiceCtrlHandlerW(SERVICE_NAME, ServiceCtrlHandler);
    if (!g_StatusHandle) {
        Log("RegisterServiceCtrlHandler failed: %lu", GetLastError());
        return;
    }

    g_ServiceStatus.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    g_ServiceStatus.dwCurrentState = SERVICE_START_PENDING;
    g_ServiceStatus.dwControlsAccepted = 0;
    g_ServiceStatus.dwWin32ExitCode = 0;
    g_ServiceStatus.dwServiceSpecificExitCode = 0;
    g_ServiceStatus.dwCheckPoint = 0;
    g_ServiceStatus.dwWaitHint = 3000;
    SetServiceStatus(g_StatusHandle, &g_ServiceStatus);

    g_StopEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
    if (!g_StopEvent) {
        g_ServiceStatus.dwCurrentState = SERVICE_STOPPED;
        g_ServiceStatus.dwWin32ExitCode = GetLastError();
        SetServiceStatus(g_StatusHandle, &g_ServiceStatus);
        return;
    }

    g_ServiceStatus.dwCurrentState = SERVICE_RUNNING;
    g_ServiceStatus.dwControlsAccepted = SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN;
    g_ServiceStatus.dwCheckPoint = 0;
    g_ServiceStatus.dwWaitHint = 0;
    SetServiceStatus(g_StatusHandle, &g_ServiceStatus);

    Log("Service started");

    // Run the input server
    RunInputServer();

    CloseHandle(g_StopEvent);
    g_StopEvent = NULL;

    g_ServiceStatus.dwCurrentState = SERVICE_STOPPED;
    g_ServiceStatus.dwControlsAccepted = 0;
    g_ServiceStatus.dwCheckPoint = 0;
    g_ServiceStatus.dwWaitHint = 0;
    SetServiceStatus(g_StatusHandle, &g_ServiceStatus);

    Log("Service stopped");
}

void WINAPI ServiceCtrlHandler(DWORD ctrl) {
    switch (ctrl) {
        case SERVICE_CONTROL_STOP:
        case SERVICE_CONTROL_SHUTDOWN:
            g_ServiceStatus.dwCurrentState = SERVICE_STOP_PENDING;
            g_ServiceStatus.dwControlsAccepted = 0;
            g_ServiceStatus.dwCheckPoint = 1;
            g_ServiceStatus.dwWaitHint = 5000;
            SetServiceStatus(g_StatusHandle, &g_ServiceStatus);

            g_Running = false;
            if (g_StopEvent) SetEvent(g_StopEvent);
            break;

        case SERVICE_CONTROL_INTERROGATE:
            SetServiceStatus(g_StatusHandle, &g_ServiceStatus);
            break;
    }
}

int wmain(int argc, wchar_t* argv[]) {
    // Handle command line arguments
    if (argc > 1) {
        if (wcscmp(argv[1], L"--install") == 0 || wcscmp(argv[1], L"-i") == 0) {
            return InstallService() ? 0 : 1;
        }
        if (wcscmp(argv[1], L"--uninstall") == 0 || wcscmp(argv[1], L"-u") == 0) {
            return UninstallService() ? 0 : 1;
        }
        if (wcscmp(argv[1], L"--console") == 0 || wcscmp(argv[1], L"-c") == 0) {
            // Run in console mode for debugging
            printf("Running in console mode (Ctrl+C to stop)...\n");
            g_StopEvent = CreateEvent(NULL, TRUE, FALSE, NULL);

            SetConsoleCtrlHandler([](DWORD type) -> BOOL {
                if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT) {
                    g_Running = false;
                    SetEvent(g_StopEvent);
                    return TRUE;
                }
                return FALSE;
            }, TRUE);

            RunInputServer();
            CloseHandle(g_StopEvent);
            return 0;
        }
        if (wcscmp(argv[1], L"--help") == 0 || wcscmp(argv[1], L"-h") == 0) {
            printf("InputHelper - UAC-aware input injection service\n\n");
            printf("Usage:\n");
            printf("  InputHelper --install    Install and start the service\n");
            printf("  InputHelper --uninstall  Stop and remove the service\n");
            printf("  InputHelper --console    Run in console mode (for debugging)\n");
            printf("  InputHelper --help       Show this help\n");
            return 0;
        }
    }

    // Normal service startup
    SERVICE_TABLE_ENTRYW serviceTable[] = {
        { (LPWSTR)SERVICE_NAME, ServiceMain },
        { NULL, NULL }
    };

    if (!StartServiceCtrlDispatcherW(serviceTable)) {
        DWORD err = GetLastError();
        if (err == ERROR_FAILED_SERVICE_CONTROLLER_CONNECT) {
            printf("This program is a Windows Service.\n");
            printf("Use --install to install it, or --console to run in debug mode.\n");
            return 1;
        }
        printf("StartServiceCtrlDispatcher failed: %lu\n", err);
        return 1;
    }

    return 0;
}
