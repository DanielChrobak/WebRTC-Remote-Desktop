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
        v->push_back({hm, (int)v->size(), mi.rcMonitor.right - mi.rcMonitor.left, mi.rcMonitor.bottom - mi.rcMonitor.top,
            dm.dmDisplayFrequency ? (int)dm.dmDisplayFrequency : 60, (mi.dwFlags & MONITORINFOF_PRIMARY) != 0, nm});
        return TRUE;
    }, (LPARAM)&g_monitors);
    std::sort(g_monitors.begin(), g_monitors.end(), [](auto& a, auto& b) { return a.isPrimary != b.isPrimary ? a.isPrimary : a.index < b.index; });
    for (size_t i = 0; i < g_monitors.size(); i++) g_monitors[i].index = (int)i;
    LOG("Found %zu monitor(s)", g_monitors.size());
}

std::string LoadFile(const char* name) { std::ifstream f(name); return f.is_open() ? std::string(std::istreambuf_iterator<char>(f), {}) : ""; }

struct Auth { std::string user, pin; } g_auth;

bool LoadAuth() {
    try { std::ifstream f("auth.json"); if (!f.is_open()) return false; json j = json::parse(f);
        if (j.contains("username") && j.contains("pin")) { g_auth = {j["username"], j["pin"]}; return g_auth.user.size() >= 3 && g_auth.pin.size() == 6; }
    } catch (...) {} return false;
}

bool SaveAuth() { try { std::ofstream f("auth.json"); f << json{{"username", g_auth.user}, {"pin", g_auth.pin}}.dump(2); return true; } catch (...) { return false; } }

bool ValidUser(const std::string& u) { if (u.length() < 3 || u.length() > 32) return false; for (char c : u) if (!isalnum(c) && c != '_' && c != '-') return false; return true; }
bool ValidPin(const std::string& p) { if (p.length() != 6) return false; for (char c : p) if (c < '0' || c > '9') return false; return true; }

void SetupAuth() {
    if (LoadAuth()) { printf("\033[32mUsing existing credentials (user: %s)\033[0m\n\n", g_auth.user.c_str()); return; }
    printf("\n\033[1;36m=== Auth Setup ===\033[0m\n");
    while (true) { printf("Username (3-32 chars): "); std::getline(std::cin, g_auth.user); if (ValidUser(g_auth.user)) break; printf("\033[31mInvalid\033[0m\n"); }
    std::string p2;
    while (true) { printf("PIN (6 digits): "); std::getline(std::cin, g_auth.pin); if (!ValidPin(g_auth.pin)) { printf("\033[31mInvalid\033[0m\n"); continue; }
        printf("Confirm PIN: "); std::getline(std::cin, p2); if (g_auth.pin == p2) break; printf("\033[31mMismatch\033[0m\n"); }
    if (SaveAuth()) printf("\033[32mCredentials saved\033[0m\n\n"); else { printf("\033[31mSave failed\033[0m\n"); SetupAuth(); }
}

int main() {
    try {
        HANDLE ho = GetStdHandle(STD_OUTPUT_HANDLE);
        if (ho != INVALID_HANDLE_VALUE) { DWORD md = 0; if (GetConsoleMode(ho, &md)) SetConsoleMode(ho, md | ENABLE_VIRTUAL_TERMINAL_PROCESSING); }
        puts("\n\033[1;36m=== Remote Desktop Server ===\033[0m\n");
        SetupAuth();
        SetPriorityClass(GetCurrentProcess(), ABOVE_NORMAL_PRIORITY_CLASS);

        FrameSlot slot;
        auto rtc = std::make_shared<WebRTCServer>();
        rtc->SetAuthCredentials(g_auth.user, g_auth.pin);

        ScreenCapture cap(&slot);
        std::unique_ptr<AV1Encoder> enc; std::mutex em; std::atomic<bool> er{false}, run{true};

        InputHandler input; input.Enable();
        auto updateBounds = [&](int i) { std::lock_guard<std::mutex> lk(g_monitorsMutex); if (i >= 0 && i < (int)g_monitors.size()) input.UpdateFromMonitorInfo(g_monitors[i]); };
        updateBounds(cap.GetCurrentMonitorIndex());
        rtc->SetInputHandler(&input);

        std::unique_ptr<ClipboardSync> clip;
        try {
            clip = std::make_unique<ClipboardSync>();
            clip->SetOnChange([rtc](const std::vector<uint8_t>& d) { if (rtc->IsConnected() && rtc->IsAuthenticated()) rtc->SendClipboard(d); });
            rtc->SetClipboardHandler([&clip](const uint8_t* d, size_t l) { return clip ? clip->HandleMessage(d, l) : false; });
        } catch (...) {}

        std::unique_ptr<AudioCapture> aud;
        try { aud = std::make_unique<AudioCapture>(); } catch (...) {}

        auto mkEnc = [&](int w, int h, int fps) {
            std::lock_guard<std::mutex> lk(em); er = false; enc.reset();
            try { enc = std::make_unique<AV1Encoder>(w, h, fps, cap.GetDev(), cap.GetCtx(), cap.GetMT()); er = true; LOG("Encoder: %dx%d @ %d", w, h, fps); }
            catch (const std::exception& e) { ERR("Encoder: %s", e.what()); }
        };
        mkEnc(cap.GetW(), cap.GetH(), cap.GetHostFPS());

        cap.SetResolutionChangeCallback([&](int w, int h, int fps) { mkEnc(w, h, fps); });
        rtc->SetGetHostFpsCallback([&cap] { return cap.RefreshHostFPS(); });
        rtc->SetAuthenticatedCallback([&clip, &input] {
            if (clip) clip->SendCurrentClipboard();
            // Wiggle cursor at center to help trigger keyframe on connection
            std::thread([&input] {
                std::this_thread::sleep_for(100ms);
                input.WiggleCenter();
            }).detach();
        });
        rtc->SetFpsChangeCallback([&cap](int fps, uint8_t) { cap.SetFPS(fps); if (!cap.IsCapturing()) cap.StartCapture(); });
        rtc->SetGetCurrentMonitorCallback([&cap] { return cap.GetCurrentMonitorIndex(); });
        rtc->SetMonitorChangeCallback([&cap, &updateBounds, &input](int i) {
            bool ok = cap.SwitchMonitor(i);
            if (ok) {
                updateBounds(i);
                // Wiggle cursor at center to help trigger keyframe on monitor switch
                std::thread([&input] {
                    std::this_thread::sleep_for(100ms);
                    input.WiggleCenter();
                }).detach();
            }
            return ok;
        });
        rtc->SetDisconnectCallback([&cap] { cap.PauseCapture(); });

        httplib::Server srv;
        srv.set_post_routing_handler([](auto&, auto& r) {
            r.set_header("Access-Control-Allow-Origin", "*"); r.set_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
            r.set_header("Access-Control-Allow-Headers", "Content-Type"); r.set_header("Cache-Control", "no-cache");
        });
        srv.Options(".*", [](auto&, auto& r) { r.status = 204; });
        srv.Get("/", [](auto&, auto& r) { auto c = LoadFile("index.html"); r.set_content(c.empty() ? "<h1>index.html not found</h1>" : c, "text/html"); });
        srv.Get("/styles.css", [](auto&, auto& r) { r.set_content(LoadFile("styles.css"), "text/css"); });
        for (const char* js : {"clipboard", "input", "media", "network", "renderer", "state", "ui"})
            srv.Get(std::string("/js/") + js + ".js", [js](auto&, auto& r) { r.set_content(LoadFile((std::string("js/") + js + ".js").c_str()), "application/javascript"); });
        srv.Get("/api/turn", [rtc](auto&, auto& r) { try { r.set_content(rtc->GetTurnConfigJson().dump(), "application/json"); } catch (...) { r.status = 500; } });
        srv.Post("/offer", [rtc](auto& q, auto& r) {
            try { auto j = json::parse(q.body); rtc->SetRemote(j["sdp"], j["type"]);
                std::string d = rtc->GetLocal(); if (d.empty()) { r.status = 500; return; }
                size_t p = d.find("a=setup:actpass"); if (p != std::string::npos) d.replace(p, 15, "a=setup:active");
                r.set_content(json{{"sdp", d}, {"type", "answer"}}.dump(), "application/json");
            } catch (...) { r.status = 500; }
        });

        std::thread st([&] { srv.listen("0.0.0.0", 6060); });
        std::this_thread::sleep_for(100ms);
        puts("\033[1;32mServer: http://localhost:6060\033[0m\n");
        LOG("Display: %dHz", cap.GetHostFPS());

        if (aud) aud->Start();

        std::thread at([&] {
            if (!aud) return; SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
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
                std::this_thread::sleep_for(1s); auto s = rtc->GetStats();
                uint64_t ef = 0; { std::lock_guard<std::mutex> lk(em); if (enc) ef = enc->GetEncoded(); }
                fp[i++ % 10] = ef; int n = std::min(i, 10); uint64_t sm = 0; for (int j = 0; j < n; j++) sm += fp[j];
                printf("%s FPS: %3llu @ %d | %5.2f Mbps | V:%4llu A:%3llu | Avg: %.1f\n",
                    s.conn ? (rtc->IsAuthenticated() ? (rtc->IsFpsReceived() ? "\033[32m[LIVE]\033[0m" : "\033[33m[WAIT]\033[0m") : "\033[33m[AUTH]\033[0m") : "\033[33m[WAIT]\033[0m",
                    ef, cap.GetCurrentFPS(), s.bytes * 8.0 / 1048576.0, s.sent, rtc->GetAudioSent(), n > 0 ? (double)sm / n : 0);
            }
        });

        std::thread et([&] {
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
            FrameData fd; bool was = false;
            while (run) {
                if (!rtc->IsConnected() || !rtc->IsAuthenticated() || !rtc->IsFpsReceived() || !er) { std::this_thread::sleep_for(10ms); was = false; continue; }
                if (!slot.Pop(fd)) continue;
                bool c = rtc->IsConnected() && rtc->IsAuthenticated() && rtc->IsFpsReceived() && er;
                if (c && !was) { LOG("Streaming at %d FPS", rtc->GetCurrentFps()); std::lock_guard<std::mutex> lk(em); if (enc) enc->Flush(); }
                was = c;
                if (!c || !fd.tex) { slot.MarkReleased(fd.poolIdx); fd.Release(); continue; }
                if (fd.fence > 0 && !cap.IsReady(fd.fence) && !cap.WaitReady(fd.fence)) { slot.MarkReleased(fd.poolIdx); fd.Release(); continue; }
                { std::lock_guard<std::mutex> lk(em); if (enc) if (auto* o = enc->Encode(fd.tex, fd.ts, rtc->NeedsKey())) rtc->Send(*o); }
                slot.MarkReleased(fd.poolIdx); fd.Release();
            }
        });

        st.join(); run = false; SetEvent(slot.GetEvent());
        et.join(); at.join(); tt.join();
        if (aud) aud->Stop();
        LOG("Shutdown complete");
    } catch (const std::exception& e) { ERR("Fatal: %s", e.what()); getchar(); return 1; }
    return 0;
}
