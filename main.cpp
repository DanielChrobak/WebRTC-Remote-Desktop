/**
 * @file main.cpp
 * @brief WebRTC Remote Desktop Server - Main entry point
 * @copyright 2025-2026 Daniel Chrobak
 */

#include "common.hpp"
#include "capture.hpp"
#include "encoder.hpp"
#include "webrtc.hpp"
#include "audio.hpp"
#include "input.hpp"

#include <io.h>
#include <fcntl.h>

std::vector<MonitorInfo> g_monitors;
std::mutex g_monitorsMutex;

/**
 * @brief Refreshes the global monitor list with current display information
 */
void RefreshMonitorList() {
    std::lock_guard<std::mutex> lock(g_monitorsMutex);
    g_monitors.clear();

    EnumDisplayMonitors(nullptr, nullptr, [](HMONITOR hMonitor, HDC, LPRECT, LPARAM lParam) -> BOOL {
        auto* monitors = reinterpret_cast<std::vector<MonitorInfo>*>(lParam);

        MONITORINFOEXW monitorInfo{sizeof(monitorInfo)};
        DEVMODEW displayMode{.dmSize = sizeof(displayMode)};
        char deviceName[64];

        if (!GetMonitorInfoW(hMonitor, &monitorInfo)) {
            return TRUE;
        }

        EnumDisplaySettingsW(monitorInfo.szDevice, ENUM_CURRENT_SETTINGS, &displayMode);
        WideCharToMultiByte(CP_UTF8, 0, monitorInfo.szDevice, -1, deviceName, sizeof(deviceName), nullptr, nullptr);

        monitors->push_back({
            hMonitor,
            static_cast<int>(monitors->size()),
            monitorInfo.rcMonitor.right - monitorInfo.rcMonitor.left,
            monitorInfo.rcMonitor.bottom - monitorInfo.rcMonitor.top,
            displayMode.dmDisplayFrequency ? static_cast<int>(displayMode.dmDisplayFrequency) : 60,
            (monitorInfo.dwFlags & MONITORINFOF_PRIMARY) != 0,
            deviceName
        });

        return TRUE;
    }, reinterpret_cast<LPARAM>(&g_monitors));

    std::sort(g_monitors.begin(), g_monitors.end(), [](const auto& a, const auto& b) {
        return a.isPrimary != b.isPrimary ? a.isPrimary : a.index < b.index;
    });

    for (size_t i = 0; i < g_monitors.size(); i++) {
        g_monitors[i].index = static_cast<int>(i);
    }

    LOG("Found %zu monitor(s)", g_monitors.size());
}

/**
 * @brief Loads file contents as a string
 * @param filename Path to file
 * @return File contents or empty string on failure
 */
std::string LoadFile(const char* filename) {
    std::ifstream file(filename);
    return file.is_open() ? std::string(std::istreambuf_iterator<char>(file), {}) : "";
}

struct Config {
    std::string username;
    std::string pin;
};

Config g_config;

bool LoadConfig() {
    try {
        std::ifstream file("auth.json");
        if (!file.is_open()) {
            return false;
        }

        json config = json::parse(file);

        if (config.contains("username") && config.contains("pin")) {
            g_config.username = config["username"];
            g_config.pin = config["pin"];
            return g_config.username.size() >= 3 && g_config.pin.size() == 6;
        }
    } catch (...) {}

    return false;
}

bool SaveConfig() {
    try {
        std::ofstream file("auth.json");
        file << json{
            {"username", g_config.username},
            {"pin", g_config.pin}
        }.dump(2);
        return true;
    } catch (...) {
        return false;
    }
}

bool ValidateUsername(const std::string& username) {
    if (username.length() < 3 || username.length() > 32) {
        return false;
    }

    for (char c : username) {
        if (!isalnum(c) && c != '_' && c != '-') {
            return false;
        }
    }

    return true;
}

bool ValidatePin(const std::string& pin) {
    if (pin.length() != 6) {
        return false;
    }

    for (char c : pin) {
        if (c < '0' || c > '9') {
            return false;
        }
    }

    return true;
}

void SetupConfig() {
    if (LoadConfig()) {
        printf("\033[32mLoaded config (user: %s)\033[0m\n\n", g_config.username.c_str());
        return;
    }

    printf("\n\033[1;36m=== First Time Setup ===\033[0m\n\n\033[1mAuthentication\033[0m\n");

    while (true) {
        printf("  Username (3-32 chars): ");
        std::getline(std::cin, g_config.username);

        if (ValidateUsername(g_config.username)) {
            break;
        }
        printf("  \033[31mInvalid username\033[0m\n");
    }

    std::string confirmPin;

    while (true) {
        printf("  PIN (6 digits): ");
        std::getline(std::cin, g_config.pin);

        if (!ValidatePin(g_config.pin)) {
            printf("  \033[31mInvalid PIN\033[0m\n");
            continue;
        }

        printf("  Confirm PIN: ");
        std::getline(std::cin, confirmPin);

        if (g_config.pin == confirmPin) {
            break;
        }
        printf("  \033[31mPINs don't match\033[0m\n");
    }

    if (SaveConfig()) {
        printf("\n\033[32mConfiguration saved to auth.json\033[0m\n\n");
    } else {
        printf("\n\033[31mFailed to save configuration\033[0m\n");
        SetupConfig();
    }
}

int main() {
    try {
        SetConsoleOutputCP(CP_UTF8);
        SetConsoleCP(CP_UTF8);

        HANDLE consoleHandle = GetStdHandle(STD_OUTPUT_HANDLE);
        if (consoleHandle != INVALID_HANDLE_VALUE) {
            DWORD consoleMode = 0;
            if (GetConsoleMode(consoleHandle, &consoleMode)) {
                SetConsoleMode(consoleHandle, consoleMode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
            }
        }

        puts("\n\033[1;36m=== WebRTC Remote Desktop Server ===\033[0m\n");
        SetupConfig();

        constexpr int SERVER_PORT = 6060;
        SetPriorityClass(GetCurrentProcess(), ABOVE_NORMAL_PRIORITY_CLASS);

        FrameSlot frameSlot;
        auto rtcServer = std::make_shared<WebRTCServer>();
        rtcServer->SetAuthCredentials(g_config.username, g_config.pin);

        ScreenCapture capture(&frameSlot);
        std::unique_ptr<AV1Encoder> encoder;
        std::mutex encoderMutex;
        std::atomic<bool> encoderReady{false};
        std::atomic<bool> running{true};

        InputHandler inputHandler;
        inputHandler.Enable();

        auto updateInputBounds = [&](int monitorIndex) {
            std::lock_guard<std::mutex> lock(g_monitorsMutex);
            if (monitorIndex >= 0 && monitorIndex < static_cast<int>(g_monitors.size())) {
                inputHandler.UpdateFromMonitorInfo(g_monitors[monitorIndex]);
            }
        };

        updateInputBounds(capture.GetCurrentMonitorIndex());
        rtcServer->SetInputHandler(&inputHandler);

        std::unique_ptr<AudioCapture> audioCapture;
        try {
            audioCapture = std::make_unique<AudioCapture>();
        } catch (...) {}

        auto createEncoder = [&](int width, int height, int fps) {
            std::lock_guard<std::mutex> lock(encoderMutex);
            encoderReady = false;
            encoder.reset();

            try {
                encoder = std::make_unique<AV1Encoder>(
                    width, height, fps,
                    capture.GetDev(),
                    capture.GetCtx(),
                    capture.GetMT()
                );
                encoderReady = true;
                LOG("Encoder: %dx%d @ %d FPS", width, height, fps);
            } catch (const std::exception& e) {
                ERR("Encoder: %s", e.what());
            }
        };

        createEncoder(capture.GetW(), capture.GetH(), capture.GetHostFPS());

        capture.SetResolutionChangeCallback([&](int width, int height, int fps) {
            createEncoder(width, height, fps);
        });

        rtcServer->SetGetHostFpsCallback([&capture] {
            return capture.RefreshHostFPS();
        });

        rtcServer->SetAuthenticatedCallback([&inputHandler] {
            std::thread([&inputHandler] {
                std::this_thread::sleep_for(100ms);
                inputHandler.WiggleCenter();
            }).detach();
        });

        rtcServer->SetFpsChangeCallback([&capture](int fps, uint8_t mode) {
            capture.SetFPS(fps);
            if (!capture.IsCapturing()) {
                capture.StartCapture();
            }
        });

        rtcServer->SetGetCurrentMonitorCallback([&capture] {
            return capture.GetCurrentMonitorIndex();
        });

        rtcServer->SetMonitorChangeCallback([&capture, &updateInputBounds, &inputHandler](int index) {
            bool success = capture.SwitchMonitor(index);
            if (success) {
                updateInputBounds(index);
                std::thread([&inputHandler] {
                    std::this_thread::sleep_for(100ms);
                    inputHandler.WiggleCenter();
                }).detach();
            }
            return success;
        });

        rtcServer->SetDisconnectCallback([&capture] {
            capture.PauseCapture();
        });

        httplib::Server httpServer;

        httpServer.set_post_routing_handler([](auto&, auto& response) {
            response.set_header("Access-Control-Allow-Origin", "*");
            response.set_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
            response.set_header("Access-Control-Allow-Headers", "Content-Type");
            response.set_header("Cache-Control", "no-cache");
        });

        httpServer.Options(".*", [](auto&, auto& response) {
            response.status = 204;
        });

        httpServer.Get("/", [](auto&, auto& response) {
            auto content = LoadFile("index.html");
            response.set_content(
                content.empty() ? "<h1>index.html not found</h1>" : content,
                "text/html"
            );
        });

        httpServer.Get("/styles.css", [](auto&, auto& response) {
            response.set_content(LoadFile("styles.css"), "text/css");
        });

        const char* jsFiles[] = {"input", "media", "network", "renderer", "state", "ui"};
        for (const auto* jsFile : jsFiles) {
            httpServer.Get(
                std::string("/js/") + jsFile + ".js",
                [jsFile](auto&, auto& response) {
                    response.set_content(
                        LoadFile((std::string("js/") + jsFile + ".js").c_str()),
                        "application/javascript"
                    );
                }
            );
        }

        httpServer.Post("/api/offer", [&rtcServer](const httplib::Request& request, httplib::Response& response) {
            try {
                auto body = json::parse(request.body);
                std::string offerSdp = body["sdp"].get<std::string>();

                LOG("Received offer from client");

                rtcServer->SetRemote(offerSdp, "offer");
                std::string answer = rtcServer->GetLocal();

                if (answer.empty()) {
                    response.status = 500;
                    response.set_content(R"({"error":"Failed to generate answer"})", "application/json");
                    return;
                }

                if (size_t pos = answer.find("a=setup:actpass"); pos != std::string::npos) {
                    answer.replace(pos, 15, "a=setup:active");
                }

                response.set_content(
                    json{{"sdp", answer}, {"type", "answer"}}.dump(),
                    "application/json"
                );

                LOG("Sent answer to client");
            } catch (const std::exception& e) {
                ERR("Offer error: %s", e.what());
                response.status = 400;
                response.set_content(R"({"error":"Invalid offer"})", "application/json");
            }
        });

        std::thread serverThread([&] {
            httpServer.listen("0.0.0.0", SERVER_PORT);
        });

        std::this_thread::sleep_for(100ms);

        printf("\n\033[1;36m==========================================\033[0m\n");
        printf("\033[1;36m       WEBRTC REMOTE DESKTOP SERVER       \033[0m\n");
        printf("\033[1;36m==========================================\033[0m\n\n");
        printf("  \033[1mLocal:\033[0m  http://localhost:%d\n\n", SERVER_PORT);
        printf("  User: %s | Display: %dHz\n", g_config.username.c_str(), capture.GetHostFPS());
        printf("\033[1;36m==========================================\033[0m\n\n");

        if (audioCapture) {
            audioCapture->Start();
        }

        std::thread audioThread([&] {
            if (!audioCapture) return;

            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
            AudioPacket packet;

            while (running) {
                if (!rtcServer->IsConnected() || !rtcServer->IsAuthenticated() || !rtcServer->IsFpsReceived()) {
                    std::this_thread::sleep_for(10ms);
                    continue;
                }

                if (audioCapture->PopPacket(packet, 5)) {
                    rtcServer->SendAudio(packet.data, packet.ts, packet.samples);
                }
            }
        });

        std::thread statsThread([&] {
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);

            uint64_t fpsHistory[10] = {};
            int historyIndex = 0;

            while (running) {
                std::this_thread::sleep_for(1s);

                auto stats = rtcServer->GetStats();
                uint64_t encodedFrames = 0;

                {
                    std::lock_guard<std::mutex> lock(encoderMutex);
                    if (encoder) {
                        encodedFrames = encoder->GetEncoded();
                    }
                }

                fpsHistory[historyIndex++ % 10] = encodedFrames;
                int count = std::min(historyIndex, 10);
                uint64_t sum = 0;

                for (int i = 0; i < count; i++) {
                    sum += fpsHistory[i];
                }

                const char* status = stats.connected
                    ? (rtcServer->IsAuthenticated()
                        ? (rtcServer->IsFpsReceived() ? "\033[32m[LIVE]\033[0m" : "\033[33m[WAIT]\033[0m")
                        : "\033[33m[AUTH]\033[0m")
                    : "\033[33m[WAIT]\033[0m";

                printf("%s FPS: %3llu @ %d | %5.2f Mbps | V:%4llu A:%3llu | Avg: %.1f\n",
                       status,
                       encodedFrames,
                       capture.GetCurrentFPS(),
                       stats.bytes * 8.0 / 1048576.0,
                       stats.sent,
                       rtcServer->GetAudioSent(),
                       count > 0 ? static_cast<double>(sum) / count : 0.0);
            }
        });

        std::thread encodeThread([&] {
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);

            FrameData frameData;
            bool wasStreaming = false;

            while (running) {
                if (!rtcServer->IsConnected() || !rtcServer->IsAuthenticated() ||
                    !rtcServer->IsFpsReceived() || !encoderReady) {
                    std::this_thread::sleep_for(10ms);
                    wasStreaming = false;
                    continue;
                }

                if (!frameSlot.Pop(frameData)) {
                    continue;
                }

                bool isStreaming = rtcServer->IsConnected() && rtcServer->IsAuthenticated() &&
                                   rtcServer->IsFpsReceived() && encoderReady;

                if (isStreaming && !wasStreaming) {
                    LOG("Streaming at %d FPS", rtcServer->GetCurrentFps());
                    std::lock_guard<std::mutex> lock(encoderMutex);
                    if (encoder) {
                        encoder->Flush();
                    }
                }

                wasStreaming = isStreaming;

                if (!isStreaming || !frameData.tex) {
                    frameSlot.MarkReleased(frameData.poolIdx);
                    frameData.Release();
                    continue;
                }

                if (frameData.fence > 0 && !capture.IsReady(frameData.fence) &&
                    !capture.WaitReady(frameData.fence)) {
                    frameSlot.MarkReleased(frameData.poolIdx);
                    frameData.Release();
                    continue;
                }

                {
                    std::lock_guard<std::mutex> lock(encoderMutex);
                    if (encoder) {
                        if (auto* output = encoder->Encode(frameData.tex, frameData.ts, rtcServer->NeedsKey())) {
                            rtcServer->Send(*output);
                        }
                    }
                }

                frameSlot.MarkReleased(frameData.poolIdx);
                frameData.Release();
            }
        });

        serverThread.join();
        running = false;
        SetEvent(frameSlot.GetEvent());

        encodeThread.join();
        audioThread.join();
        statsThread.join();

        if (audioCapture) {
            audioCapture->Stop();
        }

        LOG("Shutdown complete");
    } catch (const std::exception& e) {
        ERR("Fatal: %s", e.what());
        getchar();
        return 1;
    }

    return 0;
}
