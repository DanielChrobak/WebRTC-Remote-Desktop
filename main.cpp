#include "common.hpp"
#include "capture.hpp"
#include "encoder.hpp"
#include "webrtc.hpp"
#include "audio.hpp"
#include "input.hpp"
#include "clipboard.hpp"
#include "signaling.hpp"
#include <io.h>
#include <fcntl.h>
#include <random>

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

struct Config {
    std::string user, pin;
    std::string signalingUrl;
    std::string hostId;
} g_config;

std::string GenerateHostId() {
    static const char* letters = "ABCDEFGHJKLMNPQRSTUVWXYZ"; // No I, O (avoid confusion)
    static const char* digits = "0123456789";
    std::random_device rd;
    std::mt19937 gen(rd());
    std::string id;
    for (int i = 0; i < 3; i++) id += letters[gen() % 24];
    for (int i = 0; i < 3; i++) id += digits[gen() % 10];
    return id;
}

bool LoadConfig() {
    try {
        std::ifstream f("auth.json");
        if (!f.is_open()) return false;
        json j = json::parse(f);
        if (j.contains("username") && j.contains("pin")) {
            g_config.user = j["username"];
            g_config.pin = j["pin"];
            if (j.contains("signalingUrl")) g_config.signalingUrl = j["signalingUrl"];
            if (j.contains("hostId")) g_config.hostId = j["hostId"];
            return g_config.user.size() >= 3 && g_config.pin.size() == 6;
        }
    } catch (...) {}
    return false;
}

bool SaveConfig() {
    try {
        json j = {
            {"username", g_config.user},
            {"pin", g_config.pin}
        };
        if (!g_config.signalingUrl.empty()) j["signalingUrl"] = g_config.signalingUrl;
        if (!g_config.hostId.empty()) j["hostId"] = g_config.hostId;
        std::ofstream f("auth.json");
        f << j.dump(2);
        return true;
    } catch (...) {
        return false;
    }
}

bool ValidUser(const std::string& u) { if (u.length() < 3 || u.length() > 32) return false; for (char c : u) if (!isalnum(c) && c != '_' && c != '-') return false; return true; }
bool ValidPin(const std::string& p) { if (p.length() != 6) return false; for (char c : p) if (c < '0' || c > '9') return false; return true; }
bool ValidHostId(const std::string& id) {
    if (id.length() != 6) return false;
    for (int i = 0; i < 3; i++) if (!isalpha(id[i])) return false;
    for (int i = 3; i < 6; i++) if (!isdigit(id[i])) return false;
    return true;
}

void SetupConfig() {
    if (LoadConfig()) {
        printf("\033[32mLoaded config (user: %s", g_config.user.c_str());
        if (!g_config.signalingUrl.empty()) printf(", remote: %s", g_config.hostId.c_str());
        printf(")\033[0m\n\n");
        return;
    }

    printf("\n\033[1;36m=== First Time Setup ===\033[0m\n\n");

    // Username
    printf("\033[1mAuthentication\033[0m\n");
    while (true) {
        printf("  Username (3-32 chars): ");
        std::getline(std::cin, g_config.user);
        if (ValidUser(g_config.user)) break;
        printf("  \033[31mInvalid username\033[0m\n");
    }

    // PIN
    std::string p2;
    while (true) {
        printf("  PIN (6 digits): ");
        std::getline(std::cin, g_config.pin);
        if (!ValidPin(g_config.pin)) { printf("  \033[31mInvalid PIN\033[0m\n"); continue; }
        printf("  Confirm PIN: ");
        std::getline(std::cin, p2);
        if (g_config.pin == p2) break;
        printf("  \033[31mPINs don't match\033[0m\n");
    }

    // Signaling Server (optional)
    printf("\n\033[1mRemote Access (Optional)\033[0m\n");
    printf("  To enable remote access without port forwarding, enter a signaling server URL.\n");
    printf("  Example: https://your-signaling-server.workers.dev\n");
    printf("  Leave blank to use local mode only.\n\n");
    printf("  Signaling Server URL: ");
    std::getline(std::cin, g_config.signalingUrl);

    // Trim whitespace
    while (!g_config.signalingUrl.empty() && (g_config.signalingUrl.front() == ' ' || g_config.signalingUrl.front() == '\t'))
        g_config.signalingUrl.erase(0, 1);
    while (!g_config.signalingUrl.empty() && (g_config.signalingUrl.back() == ' ' || g_config.signalingUrl.back() == '\t' || g_config.signalingUrl.back() == '/'))
        g_config.signalingUrl.pop_back();

    // Add https:// if missing
    if (!g_config.signalingUrl.empty() &&
        g_config.signalingUrl.find("https://") != 0 &&
        g_config.signalingUrl.find("http://") != 0) {
        g_config.signalingUrl = "https://" + g_config.signalingUrl;
    }

    // Host ID (only if signaling server is set)
    if (!g_config.signalingUrl.empty()) {
        printf("\n  Host ID (3 letters + 3 numbers, e.g., ABC123)\n");
        printf("  Leave blank to auto-generate: ");
        std::string hostIdInput;
        std::getline(std::cin, hostIdInput);

        // Convert to uppercase
        for (char& c : hostIdInput) c = toupper(c);

        if (hostIdInput.empty()) {
            g_config.hostId = GenerateHostId();
            printf("  \033[32mGenerated Host ID: %s\033[0m\n", g_config.hostId.c_str());
        } else if (ValidHostId(hostIdInput)) {
            g_config.hostId = hostIdInput;
        } else {
            printf("  \033[33mInvalid format, generating one...\033[0m\n");
            g_config.hostId = GenerateHostId();
            printf("  \033[32mGenerated Host ID: %s\033[0m\n", g_config.hostId.c_str());
        }
    }

    if (SaveConfig())
        printf("\n\033[32mConfiguration saved to auth.json\033[0m\n\n");
    else {
        printf("\n\033[31mFailed to save configuration\033[0m\n");
        SetupConfig();
    }
}

int main() {
    try {
        // Set console to UTF-8 mode for proper character display
        SetConsoleOutputCP(CP_UTF8);
        SetConsoleCP(CP_UTF8);

        HANDLE ho = GetStdHandle(STD_OUTPUT_HANDLE);
        if (ho != INVALID_HANDLE_VALUE) { DWORD md = 0; if (GetConsoleMode(ho, &md)) SetConsoleMode(ho, md | ENABLE_VIRTUAL_TERMINAL_PROCESSING); }
        puts("\n\033[1;36m=== Remote Desktop Server ===\033[0m\n");
        SetupConfig();

        const int PORT = 6060;
        bool remoteEnabled = !g_config.signalingUrl.empty();

        SetPriorityClass(GetCurrentProcess(), ABOVE_NORMAL_PRIORITY_CLASS);

        FrameSlot slot;
        auto rtc = std::make_shared<WebRTCServer>();
        rtc->SetAuthCredentials(g_config.user, g_config.pin);

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
                std::thread([&input] {
                    std::this_thread::sleep_for(100ms);
                    input.WiggleCenter();
                }).detach();
            }
            return ok;
        });
        rtc->SetDisconnectCallback([&cap] { cap.PauseCapture(); });

        // Signaling client for remote mode (if enabled)
        std::unique_ptr<SignalingClient> signaling;

        if (remoteEnabled) {
            signaling = std::make_unique<SignalingClient>(g_config.signalingUrl, g_config.hostId);

            signaling->SetOnOffer([&rtc, &signaling](const json& offer, const std::vector<json>& clientIce, const std::string& sessionId) {
                LOG("Processing offer (session: %s)", sessionId.c_str());
                rtc->SetRemote(offer["sdp"].get<std::string>(), "offer");
                std::string answer = rtc->GetLocal();
                if (answer.empty()) {
                    ERR("Failed to generate answer");
                    return;
                }
                size_t p = answer.find("a=setup:actpass");
                if (p != std::string::npos) answer.replace(p, 15, "a=setup:active");
                signaling->SendAnswer(answer);
            });

            signaling->SetOnClientIce([&rtc](const std::vector<json>& ice) {
                LOG("Received %zu trickled ICE candidates", ice.size());
            });

            signaling->Start();
        }

        // HTTP server for serving static files and local WebRTC signaling
        httplib::Server srv;
        srv.set_post_routing_handler([](auto&, auto& r) {
            r.set_header("Access-Control-Allow-Origin", "*");
            r.set_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
            r.set_header("Access-Control-Allow-Headers", "Content-Type");
            r.set_header("Cache-Control", "no-cache");
        });
        srv.Options(".*", [](auto&, auto& r) { r.status = 204; });

        // Static file routes
        srv.Get("/", [](auto&, auto& r) { auto c = LoadFile("index.html"); r.set_content(c.empty() ? "<h1>index.html not found</h1>" : c, "text/html"); });
        srv.Get("/styles.css", [](auto&, auto& r) { r.set_content(LoadFile("styles.css"), "text/css"); });
        for (const char* js : {"clipboard", "input", "media", "network", "renderer", "state", "ui"})
            srv.Get(std::string("/js/") + js + ".js", [js](auto&, auto& r) { r.set_content(LoadFile((std::string("js/") + js + ".js").c_str()), "application/javascript"); });
        srv.Get("/api/turn", [rtc](auto&, auto& r) { try { r.set_content(rtc->GetTurnConfigJson().dump(), "application/json"); } catch (...) { r.status = 500; } });

        // Local WebRTC signaling via HTTP
        srv.Post("/api/offer", [&rtc](const httplib::Request& req, httplib::Response& res) {
            try {
                auto body = json::parse(req.body);
                std::string offerSdp = body["sdp"].get<std::string>();

                LOG("Received offer from client (local)");
                rtc->SetRemote(offerSdp, "offer");

                std::string answer = rtc->GetLocal();
                if (answer.empty()) {
                    res.status = 500;
                    res.set_content(R"({"error":"Failed to generate answer"})", "application/json");
                    return;
                }

                // Fix SDP for proper answering
                size_t p = answer.find("a=setup:actpass");
                if (p != std::string::npos) answer.replace(p, 15, "a=setup:active");

                json response = {{"sdp", answer}, {"type", "answer"}};
                res.set_content(response.dump(), "application/json");
                LOG("Sent answer to client (local)");
            } catch (const std::exception& e) {
                ERR("Offer error: %s", e.what());
                res.status = 400;
                res.set_content(R"({"error":"Invalid offer"})", "application/json");
            }
        });

        // Connection mode info endpoint
        srv.Get("/api/mode", [&signaling, remoteEnabled](auto&, auto& r) {
            json response = {{"mode", "local"}};
            if (remoteEnabled && signaling) {
                response["remoteEnabled"] = true;
                response["hostId"] = signaling->GetHostId();
                response["signalingUrl"] = signaling->GetWorkerUrl();
            }
            r.set_content(response.dump(), "application/json");
        });

        std::thread st([&] { srv.listen("0.0.0.0", PORT); });
        std::this_thread::sleep_for(100ms);

        // Print connection info
        printf("\n");
        printf("\033[1;36m==========================================\033[0m\n");
        printf("\033[1;36m         REMOTE DESKTOP SERVER            \033[0m\n");
        printf("\033[1;36m==========================================\033[0m\n\n");
        printf("  \033[1mLocal:\033[0m  http://localhost:%d\n", PORT);
        if (remoteEnabled) {
            printf("  \033[1mRemote:\033[0m Host ID: \033[32m%s\033[0m\n", signaling->GetHostId().c_str());
            printf("          Server:  %s\n", g_config.signalingUrl.c_str());
        } else {
            printf("  \033[33mRemote access disabled (no signaling server configured)\033[0m\n");
        }
        printf("\n  User: %s | Display: %dHz\n", g_config.user.c_str(), cap.GetHostFPS());
        printf("\033[1;36m==========================================\033[0m\n\n");

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
