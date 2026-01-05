#include "common.hpp"
#include "capture.hpp"
#include "encoder.hpp"
#include "webrtc.hpp"
#include "audio.hpp"
#include "input.hpp"
#include "clipboard.hpp"

std::vector<MonitorInfo> g_monitors;
std::mutex g_monitorsMutex;

void RefreshMonitorList() {
    std::lock_guard<std::mutex> lk(g_monitorsMutex);
    g_monitors.clear();
    EnumDisplayMonitors(0, 0, [](HMONITOR hm, HDC, LPRECT, LPARAM lp) -> BOOL {
        auto* v = (std::vector<MonitorInfo>*)lp;
        MONITORINFOEXW mi{sizeof(mi)}; DEVMODEW dm{.dmSize = sizeof(dm)}; char nm[64];
        if (!GetMonitorInfoW(hm, &mi)) return TRUE;
        EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm);
        WideCharToMultiByte(CP_UTF8, 0, mi.szDevice, -1, nm, sizeof(nm), 0, 0);
        v->push_back({hm, (int)v->size(), mi.rcMonitor.right - mi.rcMonitor.left,
            mi.rcMonitor.bottom - mi.rcMonitor.top, dm.dmDisplayFrequency ? (int)dm.dmDisplayFrequency : 60,
            (mi.dwFlags & MONITORINFOF_PRIMARY) != 0, nm});
        return TRUE;
    }, (LPARAM)&g_monitors);
    std::sort(g_monitors.begin(), g_monitors.end(), [](auto& a, auto& b) {
        return a.isPrimary != b.isPrimary ? a.isPrimary : a.index < b.index;
    });
    for (size_t i = 0; i < g_monitors.size(); i++) g_monitors[i].index = (int)i;
    LOG("Found %zu monitor(s)", g_monitors.size());
    for (const auto& m : g_monitors)
        LOG("  [%d] %s: %dx%d @ %dHz%s", m.index, m.name.c_str(), m.width, m.height, m.refreshRate, m.isPrimary ? " (Primary)" : "");
}

void UpdateInputBounds(InputHandler& input, int idx) {
    std::lock_guard<std::mutex> lk(g_monitorsMutex);
    if (idx >= 0 && idx < (int)g_monitors.size()) input.UpdateFromMonitorInfo(g_monitors[idx]);
}

std::string LoadFile(const char* name) {
    std::ifstream f(name);
    return f.is_open() ? std::string(std::istreambuf_iterator<char>(f), {}) : "";
}

std::string g_authUsername, g_authPin;

bool LoadAuthCredentials() {
    try {
        std::ifstream f("auth.json");
        if (!f.is_open()) return false;
        json j = json::parse(f);
        if (j.contains("username") && j.contains("pin")) {
            g_authUsername = j["username"].get<std::string>();
            g_authPin = j["pin"].get<std::string>();
            if (!g_authUsername.empty() && g_authPin.length() == 6) {
                LOG("Loaded existing credentials from auth.json");
                return true;
            }
        }
    } catch (...) {}
    return false;
}

bool SaveAuthCredentials(const std::string& u, const std::string& p) {
    try {
        std::ofstream f("auth.json");
        if (!f.is_open()) return false;
        f << json{{"username", u}, {"pin", p}}.dump(2);
        LOG("Credentials saved to auth.json");
        return true;
    } catch (...) { ERR("Failed to save credentials"); return false; }
}

bool IsValidUsername(const std::string& u) {
    if (u.length() < 3 || u.length() > 32) return false;
    for (char c : u) if (!isalnum(c) && c != '_' && c != '-') return false;
    return true;
}

bool IsValidPin(const std::string& p) {
    if (p.length() != 6) return false;
    for (char c : p) if (c < '0' || c > '9') return false;
    return true;
}

void SetupAuthCredentials() {
    if (LoadAuthCredentials()) {
        printf("\033[32mUsing existing credentials (username: %s).\033[0m\n\n", g_authUsername.c_str());
        return;
    }
    printf("\n\033[1;36m=== Authentication Setup ===\033[0m\n");
    printf("Set up credentials for client authentication.\n\n");
    std::string username, pin1, pin2;

    while (true) {
        printf("\033[1mEnter username (3-32 chars, alphanumeric/_/-): \033[0m");
        std::getline(std::cin, username);
        if (IsValidUsername(username)) break;
        printf("\033[31mInvalid username. Must be 3-32 characters, alphanumeric with _ or - allowed.\033[0m\n\n");
    }

    while (true) {
        printf("\033[1mEnter PIN (6 digits): \033[0m");
        std::getline(std::cin, pin1);
        if (!IsValidPin(pin1)) { printf("\033[31mInvalid PIN. Must be exactly 6 numeric digits.\033[0m\n\n"); continue; }
        printf("\033[1mConfirm PIN (6 digits): \033[0m");
        std::getline(std::cin, pin2);
        if (pin1 == pin2) break;
        printf("\033[31mPINs do not match. Please try again.\033[0m\n\n");
    }

    g_authUsername = username; g_authPin = pin1;
    if (SaveAuthCredentials(g_authUsername, g_authPin)) {
        printf("\033[32mCredentials set successfully!\033[0m\n");
        printf("\033[1;36mUsername: %s\033[0m\n\n", g_authUsername.c_str());
    } else { printf("\033[31mFailed to save credentials. Please try again.\033[0m\n\n"); SetupAuthCredentials(); }
}

int main() {
    try {
        HANDLE ho = GetStdHandle(STD_OUTPUT_HANDLE);
        if (ho != INVALID_HANDLE_VALUE) { DWORD md = 0; if (GetConsoleMode(ho, &md)) SetConsoleMode(ho, md | ENABLE_VIRTUAL_TERMINAL_PROCESSING); }
        puts("\n\033[1;36m=== Remote Desktop Server ===\033[0m\n");
        SetupAuthCredentials();
        SetPriorityClass(GetCurrentProcess(), ABOVE_NORMAL_PRIORITY_CLASS);

        FrameSlot slot;
        auto rtc = std::make_shared<WebRTCServer>();

        // Set auth credentials on WebRTC server (auth now happens over data channel)
        rtc->SetAuthCredentials(g_authUsername, g_authPin);

        ScreenCapture cap(&slot);
        std::unique_ptr<AV1Encoder> enc;
        std::mutex em;
        std::atomic<bool> er{false}, run{true};

        InputHandler input; input.Enable();
        UpdateInputBounds(input, cap.GetCurrentMonitorIndex());
        rtc->SetInputHandler(&input);

        // Initialize clipboard sync
        std::unique_ptr<ClipboardSync> clipboard;
        try {
            clipboard = std::make_unique<ClipboardSync>();
            clipboard->SetOnChange([rtc](const std::vector<uint8_t>& data) {
                if (rtc->IsConnected() && rtc->IsAuthenticated()) {
                    rtc->SendClipboard(data);
                }
            });
            rtc->SetClipboardHandler([&clipboard](const uint8_t* data, size_t len) -> bool {
                if (clipboard) {
                    return clipboard->HandleMessage(data, len);
                }
                return false;
            });
            LOG("Clipboard sync: enabled");
        } catch (const std::exception& e) {
            WARN("Clipboard sync unavailable: %s", e.what());
        }

        std::unique_ptr<AudioCapture> aud;
        try { aud = std::make_unique<AudioCapture>(); }
        catch (const std::exception& e) { WARN("Audio unavailable: %s", e.what()); }

        auto mkEnc = [&](int w, int h, int fps) {
            std::lock_guard<std::mutex> lk(em); er = false; enc.reset();
            try { enc = std::make_unique<AV1Encoder>(w, h, fps, cap.GetDev(), cap.GetCtx(), cap.GetMT()); er = true;
                  LOG("Encoder: %dx%d @ %d FPS", w, h, fps); }
            catch (const std::exception& e) { ERR("Encoder failed: %s", e.what()); }
        };
        mkEnc(cap.GetW(), cap.GetH(), cap.GetHostFPS());

        cap.SetResolutionChangeCallback([&](int w, int h, int fps) { LOG("Resolution changed"); mkEnc(w, h, fps); });
        rtc->SetGetHostFpsCallback([&cap] { return cap.RefreshHostFPS(); });

        // Send current clipboard when client authenticates
        rtc->SetAuthenticatedCallback([&clipboard] {
            if (clipboard) {
                clipboard->SendCurrentClipboard();
            }
        });

        rtc->SetFpsChangeCallback([&cap](int fps, uint8_t mode) {
            cap.SetFPS(fps);
            if (!cap.IsCapturing()) cap.StartCapture();
        });
        rtc->SetGetCurrentMonitorCallback([&cap] { return cap.GetCurrentMonitorIndex(); });
        rtc->SetMonitorChangeCallback([&cap, &input](int i) { bool ok = cap.SwitchMonitor(i); if (ok) UpdateInputBounds(input, i); return ok; });
        rtc->SetDisconnectCallback([&cap] { cap.PauseCapture(); });

        httplib::Server srv;
        srv.set_post_routing_handler([](auto&, auto& r) {
            r.set_header("Access-Control-Allow-Origin", "*");
            r.set_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
            r.set_header("Access-Control-Allow-Headers", "Content-Type");
            r.set_header("Cache-Control", "no-cache");
        });
        srv.Options(".*", [](auto&, auto& r) { r.status = 204; });
        srv.Get("/", [](auto&, auto& r) { auto c = LoadFile("index.html"); r.set_content(c.empty() ? "<h1>index.html not found</h1>" : c, "text/html"); });
        srv.Get("/styles.css", [](auto&, auto& r) { r.set_content(LoadFile("styles.css"), "text/css"); });
        for (const char* js : {"clipboard", "input", "media", "network", "renderer", "state", "ui"})
            srv.Get(std::string("/js/") + js + ".js", [js](auto&, auto& r) { r.set_content(LoadFile((std::string("js/") + js + ".js").c_str()), "application/javascript"); });

        srv.Get("/api/turn", [rtc](auto&, auto& r) {
            try { r.set_content(rtc->GetTurnConfigJson().dump(), "application/json"); }
            catch (const std::exception& e) { ERR("TURN config error: %s", e.what()); r.status = 500; r.set_content(R"({"error":"Failed to get TURN config"})", "application/json"); }
        });

        srv.Post("/offer", [rtc](auto& q, auto& r) {
            try {
                auto j = json::parse(q.body);
                rtc->SetRemote(j["sdp"], j["type"]);
                std::string d = rtc->GetLocal();
                if (d.empty()) { r.status = 500; return; }
                size_t p = d.find("a=setup:actpass");
                if (p != std::string::npos) d.replace(p, 15, "a=setup:active");
                r.set_content(json{{"sdp", d}, {"type", "answer"}}.dump(), "application/json");
            } catch (const std::exception& e) { ERR("Offer: %s", e.what()); r.status = 500; }
        });

        // Note: /auth endpoint removed - authentication now happens over WebRTC data channel

        std::thread st([&] { srv.listen("0.0.0.0", 6060); });
        std::this_thread::sleep_for(100ms);
        puts("\033[1;32mServer: http://localhost:6060\033[0m\n");
        LOG("Display: %dHz - waiting for client...", cap.GetHostFPS());
        LOG("Input: enabled");
        LOG("Authentication: WebRTC-based (credentials sent over encrypted data channel)");

        if (aud) aud->Start();

        std::thread at([&] {
            if (!aud) return;
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
            AudioPacket pk;
            while (run) {
                if (!rtc->IsConnected() || !rtc->IsAuthenticated() || !rtc->IsFpsReceived()) { std::this_thread::sleep_for(10ms); continue; }
                if (aud->PopPacket(pk, 5)) rtc->SendAudio(pk.data, pk.ts, pk.samples);
            }
        });

        std::thread tt([&] {
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
            uint64_t fp[10] = {}; int i = 0;
            while (run) {
                std::this_thread::sleep_for(1s);
                auto s = rtc->GetStats();
                uint64_t ef = 0, ef2 = 0;
                { std::lock_guard<std::mutex> lk(em); if (enc) { ef = enc->GetEncoded(); ef2 = enc->GetFailed(); } }
                uint64_t sd = slot.GetDropped();
                uint64_t tc = cap.GetTexConflicts();
                uint64_t as = rtc->GetAudioSent();
                auto is = input.GetStats();
                fp[i++ % 10] = ef;
                int n = std::min(i, 10);
                uint64_t sm = 0; for (int j = 0; j < n; j++) sm += fp[j];
                printf("%s FPS: %3llu @ %d | %5.2f Mbps | V:%4llu A:%3llu",
                    s.conn ? (rtc->IsAuthenticated() ? (rtc->IsFpsReceived() ? "\033[32m[LIVE]\033[0m" : "\033[33m[WAIT FPS]\033[0m") : "\033[33m[AUTH]\033[0m") : "\033[33m[WAIT]\033[0m",
                    ef, cap.GetCurrentFPS(), s.bytes * 8.0 / 1048576.0, s.sent, as);
                if (is.moves + is.clicks + is.keys) printf(" | \033[36mInput: m=%llu c=%llu k=%llu\033[0m", is.moves, is.clicks, is.keys);
                if (sd + s.dropped + ef2 + tc) printf(" | \033[31mDrop: c=%llu n=%llu e=%llu t=%llu\033[0m", sd, s.dropped, ef2, tc);
                printf(" | Avg: %.1f\n", n > 0 ? (double)sm / n : 0);
            }
        });

        std::thread et([&] {
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
            FrameData fd; bool was = false;
            while (run) {
                if (!rtc->IsConnected() || !rtc->IsAuthenticated() || !rtc->IsFpsReceived() || !er) { std::this_thread::sleep_for(10ms); was = false; continue; }
                if (!slot.Pop(fd)) continue;
                bool c = rtc->IsConnected() && rtc->IsAuthenticated() && rtc->IsFpsReceived() && er;
                if (c && !was) { LOG("Client authenticated and streaming at %d FPS", rtc->GetCurrentFps()); std::lock_guard<std::mutex> lk(em); if (enc) enc->Flush(); }
                was = c;
                if (!c || !fd.tex) {
                    slot.MarkReleased(fd.poolIdx);  // Mark pool texture as available
                    fd.Release();
                    continue;
                }
                // Always wait for GPU to complete the copy for this frame's texture
                if (fd.fence > 0 && !cap.IsReady(fd.fence)) {
                    if (!cap.WaitReady(fd.fence)) {
                        WARN("GPU fence wait timeout, skipping frame");
                        slot.MarkReleased(fd.poolIdx);  // Mark pool texture as available
                        fd.Release();
                        continue;
                    }
                }
                {
                    std::lock_guard<std::mutex> lk(em);
                    if (enc) {
                        if (auto* o = enc->Encode(fd.tex, fd.ts, rtc->NeedsKey())) {
                            rtc->Send(*o);
                        }
                    }
                }
                slot.MarkReleased(fd.poolIdx);  // Mark pool texture as available after encoding
                fd.Release();
            }
        });

        st.join(); run = false;
        SetEvent(slot.GetEvent());
        et.join(); at.join(); tt.join();
        if (aud) aud->Stop();
        LOG("Shutdown complete");
    } catch (const std::exception& e) { ERR("Fatal: %s", e.what()); getchar(); return 1; }
    return 0;
}
