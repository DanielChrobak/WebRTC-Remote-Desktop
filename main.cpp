/**
 * @file main.cpp
 * @brief SlipStream Server - Main entry point
 * @copyright 2025-2026 Daniel Chrobak
 */

#include "common.hpp"
#include "capture.hpp"
#include "encoder.hpp"
#include "webrtc.hpp"
#include "audio.hpp"
#include "input.hpp"

std::vector<MonitorInfo> g_monitors;
std::mutex g_monitorsMutex;

void RefreshMonitorList() {
    std::lock_guard<std::mutex> lock(g_monitorsMutex);
    g_monitors.clear();

    EnumDisplayMonitors(nullptr, nullptr, [](HMONITOR hMon, HDC, LPRECT, LPARAM lp) -> BOOL {
        auto* mons = reinterpret_cast<std::vector<MonitorInfo>*>(lp);
        MONITORINFOEXW mi{sizeof(mi)};
        DEVMODEW dm{.dmSize = sizeof(dm)};
        char name[64];

        if (!GetMonitorInfoW(hMon, &mi)) return TRUE;
        EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm);
        WideCharToMultiByte(CP_UTF8, 0, mi.szDevice, -1, name, sizeof(name), nullptr, nullptr);

        mons->push_back({hMon, static_cast<int>(mons->size()),
            mi.rcMonitor.right - mi.rcMonitor.left, mi.rcMonitor.bottom - mi.rcMonitor.top,
            dm.dmDisplayFrequency ? static_cast<int>(dm.dmDisplayFrequency) : 60,
            (mi.dwFlags & MONITORINFOF_PRIMARY) != 0, name});
        return TRUE;
    }, reinterpret_cast<LPARAM>(&g_monitors));

    std::sort(g_monitors.begin(), g_monitors.end(), [](const auto& a, const auto& b) {
        return a.isPrimary != b.isPrimary ? a.isPrimary : a.index < b.index;
    });
    for (size_t i = 0; i < g_monitors.size(); i++) g_monitors[i].index = static_cast<int>(i);
    LOG("Found %zu monitor(s)", g_monitors.size());
}

std::string LoadFile(const char* path) {
    std::ifstream f(path);
    return f.is_open() ? std::string(std::istreambuf_iterator<char>(f), {}) : "";
}

struct Config { std::string username, pin; } g_config;

bool LoadConfig() {
    try {
        std::ifstream f("auth.json");
        if (!f.is_open()) return false;
        json c = json::parse(f);
        if (c.contains("username") && c.contains("pin")) {
            g_config.username = c["username"]; g_config.pin = c["pin"];
            return g_config.username.size() >= 3 && g_config.pin.size() == 6;
        }
    } catch (...) {}
    return false;
}

bool SaveConfig() {
    try { std::ofstream("auth.json") << json{{"username", g_config.username}, {"pin", g_config.pin}}.dump(2); return true; }
    catch (...) { return false; }
}

bool ValidateUsername(const std::string& u) {
    if (u.length() < 3 || u.length() > 32) return false;
    for (char c : u) if (!isalnum(c) && c != '_' && c != '-') return false;
    return true;
}

bool ValidatePin(const std::string& p) {
    if (p.length() != 6) return false;
    for (char c : p) if (c < '0' || c > '9') return false;
    return true;
}

void SetupConfig() {
    if (LoadConfig()) { printf("\033[32mLoaded config (user: %s)\033[0m\n\n", g_config.username.c_str()); return; }

    printf("\n\033[1;36m=== First Time Setup ===\033[0m\n\n\033[1mAuthentication\033[0m\n");

    while (true) {
        printf("  Username (3-32 chars): ");
        std::getline(std::cin, g_config.username);
        if (ValidateUsername(g_config.username)) break;
        printf("  \033[31mInvalid username\033[0m\n");
    }

    std::string confirm;
    while (true) {
        printf("  PIN (6 digits): ");
        std::getline(std::cin, g_config.pin);
        if (!ValidatePin(g_config.pin)) { printf("  \033[31mInvalid PIN\033[0m\n"); continue; }
        printf("  Confirm PIN: ");
        std::getline(std::cin, confirm);
        if (g_config.pin == confirm) break;
        printf("  \033[31mPINs don't match\033[0m\n");
    }

    printf(SaveConfig() ? "\n\033[32mConfiguration saved to auth.json\033[0m\n\n" : "\n\033[31mFailed to save configuration\033[0m\n");
    if (!SaveConfig()) SetupConfig();
}

int main() {
    try {
        SetConsoleOutputCP(CP_UTF8); SetConsoleCP(CP_UTF8);
        HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
        if (h != INVALID_HANDLE_VALUE) { DWORD m = 0; if (GetConsoleMode(h, &m)) SetConsoleMode(h, m | ENABLE_VIRTUAL_TERMINAL_PROCESSING); }

        puts("\n\033[1;36m=== SlipStream Server ===\033[0m\n");
        SetupConfig();

        constexpr int PORT = 6060;
        SetPriorityClass(GetCurrentProcess(), ABOVE_NORMAL_PRIORITY_CLASS);

        FrameSlot frameSlot;
        auto rtcServer = std::make_shared<WebRTCServer>();
        rtcServer->SetAuthCredentials(g_config.username, g_config.pin);

        ScreenCapture capture(&frameSlot);
        std::unique_ptr<AV1Encoder> encoder;
        std::mutex encoderMutex;
        std::atomic<bool> encoderReady{false}, running{true};

        InputHandler inputHandler;
        inputHandler.Enable();

        auto updateInputBounds = [&](int idx) {
            std::lock_guard<std::mutex> lock(g_monitorsMutex);
            if (idx >= 0 && idx < static_cast<int>(g_monitors.size()))
                inputHandler.UpdateFromMonitorInfo(g_monitors[idx]);
        };

        updateInputBounds(capture.GetCurrentMonitorIndex());
        rtcServer->SetInputHandler(&inputHandler);

        std::unique_ptr<AudioCapture> audioCapture;
        try { audioCapture = std::make_unique<AudioCapture>(); } catch (...) {}

        auto createEncoder = [&](int w, int h, int fps) {
            std::lock_guard<std::mutex> lock(encoderMutex);
            encoderReady = false; encoder.reset();
            try { encoder = std::make_unique<AV1Encoder>(w, h, fps, capture.GetDev(), capture.GetCtx(), capture.GetMT()); encoderReady = true; LOG("Encoder: %dx%d @ %d FPS", w, h, fps); }
            catch (const std::exception& e) { ERR("Encoder: %s", e.what()); }
        };

        createEncoder(capture.GetW(), capture.GetH(), capture.GetHostFPS());
        capture.SetResolutionChangeCallback([&](int w, int h, int fps) { createEncoder(w, h, fps); });
        rtcServer->SetGetHostFpsCallback([&] { return capture.RefreshHostFPS(); });
        rtcServer->SetAuthenticatedCallback([&] { std::thread([&] { std::this_thread::sleep_for(100ms); inputHandler.WiggleCenter(); }).detach(); });
        rtcServer->SetFpsChangeCallback([&](int fps, uint8_t) { capture.SetFPS(fps); if (!capture.IsCapturing()) capture.StartCapture(); });
        rtcServer->SetGetCurrentMonitorCallback([&] { return capture.GetCurrentMonitorIndex(); });
        rtcServer->SetMonitorChangeCallback([&](int idx) {
            bool ok = capture.SwitchMonitor(idx);
            if (ok) { updateInputBounds(idx); std::thread([&] { std::this_thread::sleep_for(100ms); inputHandler.WiggleCenter(); }).detach(); }
            return ok;
        });
        rtcServer->SetDisconnectCallback([&] { capture.PauseCapture(); });

        httplib::Server httpServer;
        httpServer.set_post_routing_handler([](auto&, auto& r) {
            r.set_header("Access-Control-Allow-Origin", "*");
            r.set_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
            r.set_header("Access-Control-Allow-Headers", "Content-Type");
            r.set_header("Cache-Control", "no-cache");
        });

        httpServer.Options(".*", [](auto&, auto& r) { r.status = 204; });
        httpServer.Get("/", [](auto&, auto& r) { auto c = LoadFile("index.html"); r.set_content(c.empty() ? "<h1>index.html not found</h1>" : c, "text/html"); });
        httpServer.Get("/styles.css", [](auto&, auto& r) { r.set_content(LoadFile("styles.css"), "text/css"); });

        for (auto js : {"input", "media", "network", "renderer", "state", "ui"})
            httpServer.Get(std::string("/js/") + js + ".js", [js](auto&, auto& r) { r.set_content(LoadFile((std::string("js/") + js + ".js").c_str()), "application/javascript"); });

        httpServer.Post("/api/offer", [&](const httplib::Request& req, httplib::Response& res) {
            try {
                auto body = json::parse(req.body);
                std::string offer = body["sdp"].get<std::string>();
                LOG("Received offer from client");
                rtcServer->SetRemote(offer, "offer");
                std::string answer = rtcServer->GetLocal();
                if (answer.empty()) { res.status = 500; res.set_content(R"({"error":"Failed to generate answer"})", "application/json"); return; }
                if (size_t p = answer.find("a=setup:actpass"); p != std::string::npos) answer.replace(p, 15, "a=setup:active");
                res.set_content(json{{"sdp", answer}, {"type", "answer"}}.dump(), "application/json");
                LOG("Sent answer to client");
            } catch (const std::exception& e) { ERR("Offer error: %s", e.what()); res.status = 400; res.set_content(R"({"error":"Invalid offer"})", "application/json"); }
        });

        std::thread serverThread([&] { httpServer.listen("0.0.0.0", PORT); });
        std::this_thread::sleep_for(100ms);

        printf("\n\033[1;36m==========================================\033[0m\n");
        printf("\033[1;36m            SLIPSTREAM SERVER             \033[0m\n");
        printf("\033[1;36m==========================================\033[0m\n\n");
        printf("  \033[1mLocal:\033[0m  http://localhost:%d\n\n  User: %s | Display: %dHz\n", PORT, g_config.username.c_str(), capture.GetHostFPS());
        printf("\033[1;36m==========================================\033[0m\n\n");

        if (audioCapture) audioCapture->Start();

        std::thread audioThread([&] {
            if (!audioCapture) return;
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
            AudioPacket pkt;
            while (running) {
                if (!rtcServer->IsConnected() || !rtcServer->IsAuthenticated() || !rtcServer->IsFpsReceived()) { std::this_thread::sleep_for(10ms); continue; }
                if (audioCapture->PopPacket(pkt, 5)) rtcServer->SendAudio(pkt.data, pkt.ts, pkt.samples);
            }
        });

        std::thread statsThread([&] {
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
            uint64_t hist[10] = {}; int idx = 0;
            while (running) {
                std::this_thread::sleep_for(1s);
                auto stats = rtcServer->GetStats();
                uint64_t enc = 0;
                { std::lock_guard<std::mutex> lock(encoderMutex); if (encoder) enc = encoder->GetEncoded(); }
                hist[idx++ % 10] = enc;
                int cnt = std::min(idx, 10);
                uint64_t sum = 0; for (int i = 0; i < cnt; i++) sum += hist[i];
                const char* st = stats.connected ? (rtcServer->IsAuthenticated() ? (rtcServer->IsFpsReceived() ? "\033[32m[LIVE]\033[0m" : "\033[33m[WAIT]\033[0m") : "\033[33m[AUTH]\033[0m") : "\033[33m[WAIT]\033[0m";
                printf("%s FPS: %3llu @ %d | %5.2f Mbps | V:%4llu A:%3llu | Avg: %.1f\n", st, enc, capture.GetCurrentFPS(), stats.bytes * 8.0 / 1048576.0, stats.sent, rtcServer->GetAudioSent(), cnt > 0 ? static_cast<double>(sum) / cnt : 0.0);
            }
        });

        std::thread encodeThread([&] {
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
            FrameData fd; bool was = false;
            while (running) {
                if (!rtcServer->IsConnected() || !rtcServer->IsAuthenticated() || !rtcServer->IsFpsReceived() || !encoderReady) { std::this_thread::sleep_for(10ms); was = false; continue; }
                if (!frameSlot.Pop(fd)) continue;

                bool streaming = rtcServer->IsConnected() && rtcServer->IsAuthenticated() && rtcServer->IsFpsReceived() && encoderReady;
                if (streaming && !was) { LOG("Streaming at %d FPS", rtcServer->GetCurrentFps()); std::lock_guard<std::mutex> lock(encoderMutex); if (encoder) encoder->Flush(); }
                was = streaming;

                if (!streaming || !fd.tex) { frameSlot.MarkReleased(fd.poolIdx); fd.Release(); continue; }
                if (fd.fence > 0 && !capture.IsReady(fd.fence) && !capture.WaitReady(fd.fence)) { frameSlot.MarkReleased(fd.poolIdx); fd.Release(); continue; }

                { std::lock_guard<std::mutex> lock(encoderMutex); if (encoder) if (auto* out = encoder->Encode(fd.tex, fd.ts, rtcServer->NeedsKey())) rtcServer->Send(*out); }
                frameSlot.MarkReleased(fd.poolIdx); fd.Release();
            }
        });

        serverThread.join();
        running = false;
        SetEvent(frameSlot.GetEvent());
        encodeThread.join(); audioThread.join(); statsThread.join();
        if (audioCapture) audioCapture->Stop();
        LOG("Shutdown complete");
    } catch (const std::exception& e) { ERR("Fatal: %s", e.what()); getchar(); return 1; }
    return 0;
}
