#pragma once
#include "common.hpp"
#include "encoder.hpp"
#include "input.hpp"

extern std::vector<struct MonitorInfo> g_monitors;
extern std::mutex g_monitorsMutex;

#pragma pack(push, 1)
struct PktHdr { int64_t ts; uint32_t encUs, fid; uint16_t idx, total; uint8_t type; };
struct AudioPktHdr { uint32_t magic; int64_t ts; uint16_t samples, dataLen; };
struct AuthRequestMsg { uint32_t magic; uint8_t usernameLen; uint8_t pinLen; /* followed by username and pin bytes */ };
struct AuthResponseMsg { uint32_t magic; uint8_t success; uint8_t errorLen; /* followed by error message if failed */ };
#pragma pack(pop)

struct TurnConfig {
    struct Server { std::string urls, username, credential; };
    std::vector<Server> servers;
    std::string fetchUrl;
    bool meteredEnabled = false, manualEnabled = false;

    static TurnConfig Load(const std::string& path = "turn_config.json") {
        TurnConfig cfg;
        try {
            std::ifstream f(path);
            if (!f.is_open()) {
                WARN("turn_config.json not found, using default STUN servers");
                cfg.servers = {{"stun:stun.l.google.com:19302", "", ""}, {"stun:stun1.l.google.com:19302", "", ""}};
                return cfg;
            }
            json j = json::parse(f);
            if (j.contains("fallback") && j["fallback"]["enabled"].get<bool>())
                for (const auto& s : j["fallback"]["servers"])
                    cfg.servers.push_back({s["urls"].get<std::string>(), "", ""});
            if (j.contains("metered") && j["metered"]["enabled"].get<bool>()) {
                cfg.meteredEnabled = true;
                cfg.fetchUrl = j["metered"]["fetchUrl"].get<std::string>();
                LOG("Metered TURN enabled");
            }
            if (j.contains("manual") && j["manual"]["enabled"].get<bool>()) {
                cfg.manualEnabled = true;
                std::string defUser = j["manual"].contains("credentials") ? j["manual"]["credentials"].value("username", "") : "";
                std::string defPass = j["manual"].contains("credentials") ? j["manual"]["credentials"].value("password", "") : "";
                for (const auto& s : j["manual"]["servers"]) {
                    std::string user = s.value("username", ""), cred = s.value("credential", "");
                    cfg.servers.push_back({s["urls"].get<std::string>(), user.empty() ? defUser : user, cred.empty() ? defPass : cred});
                }
                LOG("Loaded %zu manual TURN servers", j["manual"]["servers"].size());
            }
            if (cfg.meteredEnabled && !cfg.manualEnabled)
                cfg.servers.push_back({"stun:stun.relay.metered.ca:80", "", ""});
            LOG("TURN config loaded: %zu total servers", cfg.servers.size());
        } catch (const std::exception& e) {
            ERR("Failed to load turn_config.json: %s", e.what());
            cfg.servers = {{"stun:stun.l.google.com:19302", "", ""}, {"stun:stun1.l.google.com:19302", "", ""}};
        }
        return cfg;
    }

    json ToClientJson() const {
        json result = {{"fetchUrl", fetchUrl}, {"meteredEnabled", meteredEnabled}, {"servers", json::array()}};
        for (const auto& s : servers) {
            json sj = {{"urls", s.urls}};
            if (!s.username.empty()) sj["username"] = s.username;
            if (!s.credential.empty()) sj["credential"] = s.credential;
            result["servers"].push_back(sj);
        }
        return result;
    }
};

class WebRTCServer {
    std::shared_ptr<rtc::PeerConnection> pc;
    std::shared_ptr<rtc::DataChannel> dc;
    std::atomic<bool> conn{false}, nkey{true}, fpsr{false}, gc{false};
    std::atomic<bool> authenticated{false};  // Authentication state
    std::string ldesc;
    std::mutex dmx;
    std::condition_variable dcv;
    rtc::Configuration cfg;
    static constexpr size_t BT = 32768, BTH = BT * 2, CHK = 1400, HDR = sizeof(PktHdr), DCK = CHK - HDR;
    std::vector<uint8_t> pb, ab;
    std::atomic<uint64_t> sc{0}, bc{0}, dpc{0}, asc{0};
    std::atomic<uint32_t> fid{0};
    std::atomic<int> cfps{60};
    std::atomic<uint8_t> cfm{0};
    std::function<void(int, uint8_t)> onFps;
    std::function<int()> getHfps, getCmon;
    std::function<bool(int)> onMon;
    std::function<void()> onDisconnect;
    std::function<void()> onAuthenticated;  // Callback when auth succeeds
    InputHandler* inputHandler{nullptr};
    std::function<bool(const uint8_t*, size_t)> clipboardHandler;
    TurnConfig turnConfig;
    static constexpr int MAX_OVERFLOWS = 10;
    static constexpr int64_t PING_TIMEOUT_MS = 3000;
    std::atomic<int> consecutiveOverflows{0};
    std::atomic<int64_t> lastPingResponse{0};
    std::atomic<bool> pingTimeoutTriggered{false};

    // Auth credentials (set by main)
    std::string authUsername;
    std::string authPin;
    std::mutex authMutex;

    bool SafeSend(const void* d, size_t l) {
        auto ch = dc;
        if (!ch || !ch->isOpen()) return false;
        try { ch->send((const std::byte*)d, l); return true; } catch (...) { return false; }
    }

    void SendAuthResponse(bool success, const std::string& error = "") {
        auto ch = dc;
        if (!ch || !ch->isOpen()) return;

        size_t msgLen = sizeof(AuthResponseMsg) + (success ? 0 : error.size());
        std::vector<uint8_t> buf(msgLen);
        auto* msg = (AuthResponseMsg*)buf.data();
        msg->magic = MSG_AUTH_RESPONSE;
        msg->success = success ? 1 : 0;
        msg->errorLen = success ? 0 : (uint8_t)std::min(error.size(), (size_t)255);
        if (!success && !error.empty()) {
            memcpy(buf.data() + sizeof(AuthResponseMsg), error.c_str(), msg->errorLen);
        }
        SafeSend(buf.data(), buf.size());

        if (success) {
            LOG("Client authenticated successfully");
        } else {
            WARN("Client authentication failed: %s", error.c_str());
        }
    }

    bool ValidateAuth(const std::string& username, const std::string& pin) {
        std::lock_guard<std::mutex> lk(authMutex);
        return username == authUsername && pin == authPin;
    }

    bool HandleAuthRequest(const uint8_t* data, size_t len) {
        if (len < sizeof(AuthRequestMsg)) {
            SendAuthResponse(false, "Invalid auth message");
            return false;
        }

        auto* msg = (AuthRequestMsg*)data;
        if (msg->magic != MSG_AUTH_REQUEST) return false;

        size_t expectedLen = sizeof(AuthRequestMsg) + msg->usernameLen + msg->pinLen;
        if (len < expectedLen) {
            SendAuthResponse(false, "Invalid auth message length");
            return false;
        }

        std::string username((char*)data + sizeof(AuthRequestMsg), msg->usernameLen);
        std::string pin((char*)data + sizeof(AuthRequestMsg) + msg->usernameLen, msg->pinLen);

        if (ValidateAuth(username, pin)) {
            authenticated.store(true, std::memory_order_release);
            SendAuthResponse(true);

            // Send host info and monitor list after successful auth
            SendHostInfo();
            SendMonitorList();

            if (onAuthenticated) onAuthenticated();
            return true;
        } else {
            SendAuthResponse(false, "Invalid credentials");
            // Close connection after failed auth
            std::thread([this]() {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                ForceDisconnect("Authentication failed");
            }).detach();
            return false;
        }
    }

    void SendHostInfo() {
        auto ch = dc;
        if (!ch || !ch->isOpen()) return;
        int fps = getHfps ? getHfps() : 60;
        uint8_t b[6]; *(uint32_t*)b = MSG_HOST_INFO; *(uint16_t*)(b + 4) = (uint16_t)fps;
        SafeSend(b, 6); LOG("Host info: %dHz", fps);
    }

    void SendMonitorList() {
        auto ch = dc;
        if (!ch || !ch->isOpen()) return;
        std::lock_guard<std::mutex> lk(g_monitorsMutex);
        std::vector<uint8_t> b(6 + g_monitors.size() * 74);
        size_t o = 0;
        *(uint32_t*)&b[o] = MSG_MONITOR_LIST; o += 4;
        b[o++] = (uint8_t)g_monitors.size();
        b[o++] = (uint8_t)(getCmon ? getCmon() : 0);
        for (const auto& m : g_monitors) {
            b[o++] = (uint8_t)m.index;
            *(uint16_t*)&b[o] = (uint16_t)m.width; *(uint16_t*)&b[o+2] = (uint16_t)m.height;
            *(uint16_t*)&b[o+4] = (uint16_t)m.refreshRate; o += 6;
            b[o++] = m.isPrimary ? 1 : 0;
            size_t nl = std::min(m.name.size(), (size_t)63);
            b[o++] = (uint8_t)nl; memcpy(&b[o], m.name.c_str(), nl); o += nl;
        }
        SafeSend(b.data(), o); LOG("Monitor list: %zu monitors", g_monitors.size());
    }

    void ForceDisconnect(const char* reason) {
        if (!conn.load(std::memory_order_relaxed)) return;
        WARN("Force disconnect: %s", reason);
        conn.store(false, std::memory_order_release);
        fpsr.store(false, std::memory_order_release);
        authenticated.store(false, std::memory_order_release);
        consecutiveOverflows.store(0, std::memory_order_relaxed);
        pingTimeoutTriggered.store(false, std::memory_order_relaxed);
        try { if (dc) dc->close(); } catch (...) {}
        try { if (pc) pc->close(); } catch (...) {}
        if (onDisconnect) onDisconnect();
    }

    void HandleMsg(const rtc::binary& b) {
        if (b.size() < 4) return;
        uint32_t mg = *(uint32_t*)b.data();

        // Handle auth request first (before checking authenticated state)
        if (mg == MSG_AUTH_REQUEST) {
            HandleAuthRequest((const uint8_t*)b.data(), b.size());
            return;
        }

        // Require authentication for all other messages
        if (!authenticated.load(std::memory_order_acquire)) {
            WARN("Received message before authentication, ignoring");
            return;
        }

        if (inputHandler && (mg == MSG_MOUSE_MOVE || mg == MSG_MOUSE_BTN || mg == MSG_MOUSE_WHEEL || mg == MSG_KEY)) {
            inputHandler->HandleMessage((const uint8_t*)b.data(), b.size()); return;
        }
        // Handle clipboard messages
        if ((mg == MSG_CLIPBOARD_TEXT || mg == MSG_CLIPBOARD_IMAGE || mg == MSG_CLIPBOARD_REQUEST) && clipboardHandler) {
            clipboardHandler((const uint8_t*)b.data(), b.size()); return;
        }
        if (mg == MSG_PING && b.size() == 16) {
            lastPingResponse.store(GetTimestamp() / 1000, std::memory_order_relaxed);
            consecutiveOverflows.store(0, std::memory_order_relaxed);
            pingTimeoutTriggered.store(false, std::memory_order_relaxed);
            uint8_t p[24]; memcpy(p, b.data(), 16); *(uint64_t*)(p + 16) = GetTimestamp(); SafeSend(p, 24);
        } else if (mg == MSG_FPS_SET && b.size() == 7) {
            uint16_t fps = *(uint16_t*)(b.data() + 4); uint8_t md = (uint8_t)b[6];
            if (fps >= 1 && fps <= 240 && md <= 2) {
                int ac = (md == 1 && getHfps) ? getHfps() : fps;
                cfps = ac; cfm = md; fpsr = true;
                LOG("FPS request: %d (mode=%d, actual=%d)", fps, md, ac);
                if (onFps) onFps(ac, md);
                uint8_t a[7]; *(uint32_t*)a = MSG_FPS_ACK; *(uint16_t*)(a + 4) = (uint16_t)ac; a[6] = md; SafeSend(a, 7);
            }
        } else if (mg == MSG_REQUEST_KEY && b.size() == 4) { nkey = true; LOG("Keyframe requested"); }
        else if (mg == MSG_MONITOR_SET && b.size() == 5) {
            uint8_t i = (uint8_t)b[4]; LOG("Monitor request: %d", i);
            if (onMon && onMon(i)) { nkey = true; SendMonitorList(); SendHostInfo(); }
        }
    }

    void Setup() {
        if (pc) { if (dc && dc->isOpen()) dc->close(); dc.reset(); pc->close(); }
        conn = false; nkey = true; fpsr = gc = false;
        authenticated.store(false, std::memory_order_release);
        consecutiveOverflows.store(0, std::memory_order_relaxed);
        lastPingResponse.store(0, std::memory_order_relaxed);
        pingTimeoutTriggered.store(false, std::memory_order_relaxed);
        { std::lock_guard<std::mutex> lk(dmx); ldesc.clear(); }
        pc = std::make_shared<rtc::PeerConnection>(cfg);

        pc->onLocalDescription([this](rtc::Description d) {
            std::lock_guard<std::mutex> lk(dmx); ldesc = std::string(d);
            LOG("Local description ready (%zu bytes)", ldesc.size()); dcv.notify_all();
        });
        pc->onLocalCandidate([](rtc::Candidate c) {
            std::string cand = std::string(c);
            if (cand.find("relay") != std::string::npos) {
                LOG("\033[32mTURN relay candidate: %s\033[0m", cand.c_str());
            } else {
                LOG("ICE candidate: %s", cand.c_str());
            }
        });
        pc->onStateChange([this](auto s) {
            LOG("WebRTC state: %d", (int)s);
            bool was = conn.load();
            conn = (s == rtc::PeerConnection::State::Connected);
            if (conn && !was) { nkey = true; lastPingResponse.store(GetTimestamp() / 1000, std::memory_order_relaxed); LOG("Peer connected - awaiting authentication"); }
            if (!conn && was) { fpsr = false; authenticated.store(false, std::memory_order_release); consecutiveOverflows.store(0, std::memory_order_relaxed); WARN("Peer disconnected"); if (onDisconnect) onDisconnect(); }
            if (s == rtc::PeerConnection::State::Failed) ERR("Connection failed");
        });
        pc->onIceStateChange([](rtc::PeerConnection::IceState s) {
            LOG("ICE state: %d", (int)s);
            if (s == rtc::PeerConnection::IceState::Failed) ERR("ICE failed - check firewall settings or TURN config");
        });
        pc->onGatheringStateChange([this](rtc::PeerConnection::GatheringState s) {
            LOG("ICE gathering: %d", (int)s);
            if (s == rtc::PeerConnection::GatheringState::Complete) { gc = true; dcv.notify_all(); }
        });
        pc->onDataChannel([this](auto ch) {
            if (ch->label() != "screen") return;
            dc = ch;
            dc->onOpen([this] {
                conn = nkey = true;
                authenticated.store(false, std::memory_order_release);  // Start unauthenticated
                lastPingResponse.store(GetTimestamp() / 1000, std::memory_order_relaxed);
                consecutiveOverflows.store(0, std::memory_order_relaxed);
                LOG("Data channel opened - awaiting authentication");
                // Don't send host info/monitor list until authenticated
            });
            dc->onClosed([this] { conn = fpsr = false; authenticated.store(false, std::memory_order_release); consecutiveOverflows.store(0, std::memory_order_relaxed); WARN("Data channel closed"); });
            dc->onError([](std::string e) { ERR("Channel error: %s", e.c_str()); });
            dc->onMessage([this](auto d) { if (auto* b = std::get_if<rtc::binary>(&d)) HandleMsg(*b); });
        });
    }

    static rtc::IceServer ParseIceServer(const TurnConfig::Server& s) {
        if (s.username.empty() && s.credential.empty()) return rtc::IceServer(s.urls);
        std::string url = s.urls;
        uint16_t port = 3478;
        rtc::IceServer::RelayType relayType = rtc::IceServer::RelayType::TurnUdp;
        if (url.find("turns:") == 0) { relayType = rtc::IceServer::RelayType::TurnTls; url = url.substr(6); port = 443; }
        else if (url.find("turn:") == 0) { url = url.substr(5); if (url.find("transport=tcp") != std::string::npos) relayType = rtc::IceServer::RelayType::TurnTcp; }
        else return rtc::IceServer(s.urls);
        size_t qp = url.find('?'); if (qp != std::string::npos) url = url.substr(0, qp);
        size_t cp = url.rfind(':');
        std::string host = (cp != std::string::npos && cp > 0) ? url.substr(0, cp) : url;
        if (cp != std::string::npos) try { port = static_cast<uint16_t>(std::stoi(url.substr(cp + 1))); } catch (...) {}
        return rtc::IceServer(host, port, s.username, s.credential, relayType);
    }

    bool IsConnectionStale() {
        if (!conn.load(std::memory_order_relaxed)) return false;
        int64_t lastPing = lastPingResponse.load(std::memory_order_relaxed);
        int64_t now = GetTimestamp() / 1000;
        if (lastPing > 0 && (now - lastPing) > PING_TIMEOUT_MS) {
            if (!pingTimeoutTriggered.exchange(true, std::memory_order_relaxed))
                WARN("Ping timeout detected (last response: %lld ms ago)", now - lastPing);
            return true;
        }
        return consecutiveOverflows.load(std::memory_order_relaxed) >= MAX_OVERFLOWS;
    }

public:
    WebRTCServer() {
        turnConfig = TurnConfig::Load();
        for (const auto& s : turnConfig.servers) {
            try {
                cfg.iceServers.push_back(ParseIceServer(s));
                if (s.username.empty()) {
                    LOG("Added STUN: %s", s.urls.c_str());
                } else {
                    LOG("Added TURN: %s (with credentials)", s.urls.c_str());
                }
            } catch (const std::exception& e) { WARN("Failed to add ICE server %s: %s", s.urls.c_str(), e.what()); }
        }
        cfg.portRangeBegin = 50000; cfg.portRangeEnd = 50100; cfg.enableIceTcp = true;
        pb.resize(CHK); ab.resize(4096);
        Setup();
        LOG("WebRTC initialized with %zu ICE servers", cfg.iceServers.size());
    }

    void SetAuthCredentials(const std::string& username, const std::string& pin) {
        std::lock_guard<std::mutex> lk(authMutex);
        authUsername = username;
        authPin = pin;
        LOG("Auth credentials configured for user: %s", username.c_str());
    }

    json GetTurnConfigJson() const { return turnConfig.ToClientJson(); }
    void SetInputHandler(InputHandler* h) { inputHandler = h; }
    void SetFpsChangeCallback(std::function<void(int, uint8_t)> cb) { onFps = cb; }
    void SetGetHostFpsCallback(std::function<int()> cb) { getHfps = cb; }
    void SetMonitorChangeCallback(std::function<bool(int)> cb) { onMon = cb; }
    void SetGetCurrentMonitorCallback(std::function<int()> cb) { getCmon = cb; }
    void SetDisconnectCallback(std::function<void()> cb) { onDisconnect = cb; }
    void SetClipboardHandler(std::function<bool(const uint8_t*, size_t)> cb) { clipboardHandler = cb; }
    void SetAuthenticatedCallback(std::function<void()> cb) { onAuthenticated = cb; }

    std::string GetLocal() {
        std::unique_lock<std::mutex> lk(dmx);
        dcv.wait_for(lk, 5s, [this] { return !ldesc.empty() && gc.load(); }); return ldesc;
    }

    void SetRemote(const std::string& sdp, const std::string& type) {
        if (type == "offer") Setup();
        pc->setRemoteDescription(rtc::Description(sdp, type));
        if (type == "offer") pc->setLocalDescription();
    }

    bool IsConnected() const { return conn.load(); }
    bool IsAuthenticated() const { return authenticated.load(); }
    bool IsFpsReceived() const { return fpsr.load(); }
    int GetCurrentFps() const { return cfps.load(); }
    bool NeedsKey() { return nkey.exchange(false); }

    void Send(const EncodedFrame& f) {
        if (!conn.load(std::memory_order_relaxed) || !authenticated.load(std::memory_order_relaxed)) return;
        auto ch = dc;
        if (!ch || !ch->isOpen()) { if (conn.load(std::memory_order_relaxed)) ForceDisconnect("Data channel closed unexpectedly"); return; }
        if (IsConnectionStale()) { ForceDisconnect("Connection stale (ping timeout or repeated buffer overflows)"); return; }
        try {
            size_t bufAmt = ch->bufferedAmount();
            if (bufAmt > BT) {
                int overflows = consecutiveOverflows.fetch_add(1, std::memory_order_relaxed) + 1;
                dpc++; nkey.store(true, std::memory_order_relaxed);
                if (overflows <= 3 || overflows % 10 == 0) WARN("Buffer overflow: %zu bytes (count: %d)", bufAmt, overflows);
                if (overflows >= MAX_OVERFLOWS) ForceDisconnect("Too many consecutive buffer overflows");
                return;
            }
            consecutiveOverflows.store(0, std::memory_order_relaxed);
            size_t sz = f.data.size(), n = (sz + DCK - 1) / DCK;
            if (n > 65535 || sz == 0) return;
            uint32_t currentFid = fid.fetch_add(1, std::memory_order_relaxed);
            PktHdr hd = {f.ts, (uint32_t)f.encUs, currentFid, 0, (uint16_t)n, f.isKey ? (uint8_t)1 : (uint8_t)0};
            size_t bytesSent = 0;
            for (size_t i = 0; i < n; i++) {
                if (i > 0 && (i % 16) == 0 && ch->bufferedAmount() > BTH) {
                    int overflows = consecutiveOverflows.fetch_add(1, std::memory_order_relaxed) + 1;
                    if (overflows <= 3) WARN("Buffer overflow mid-send: %zu bytes at chunk %zu/%zu", ch->bufferedAmount(), i, n);
                    dpc++; nkey.store(true, std::memory_order_relaxed); break;
                }
                hd.idx = (uint16_t)i;
                memcpy(pb.data(), &hd, HDR);
                size_t o = i * DCK, l = std::min(DCK, sz - o);
                memcpy(pb.data() + HDR, f.data.data() + o, l);
                if (!SafeSend(pb.data(), HDR + l)) {
                    int overflows = consecutiveOverflows.fetch_add(1, std::memory_order_relaxed) + 1;
                    if (overflows <= 3) WARN("Send failed at chunk %zu/%zu", i, n);
                    dpc++; nkey.store(true, std::memory_order_relaxed); break;
                }
                bytesSent += HDR + l;
            }
            if (bytesSent > 0) { bc.fetch_add(bytesSent, std::memory_order_relaxed); sc.fetch_add(1, std::memory_order_relaxed); }
        } catch (const std::exception& e) {
            ERR("Send exception: %s", e.what());
            dpc++; nkey.store(true, std::memory_order_relaxed);
            consecutiveOverflows.fetch_add(1, std::memory_order_relaxed);
        }
    }

    void SendAudio(const std::vector<uint8_t>& d, int64_t ts, int sp) {
        if (!conn.load(std::memory_order_relaxed) || !authenticated.load(std::memory_order_relaxed)) return;
        auto ch = dc;
        if (!ch || !ch->isOpen() || d.empty() || d.size() > 4000) return;
        if (consecutiveOverflows.load(std::memory_order_relaxed) >= MAX_OVERFLOWS / 2) return;
        try {
            if (ch->bufferedAmount() > BT / 2) return;
            size_t tl = sizeof(AudioPktHdr) + d.size();
            if (ab.size() < tl) ab.resize(tl);
            auto* h = (AudioPktHdr*)ab.data();
            h->magic = MSG_AUDIO_DATA; h->ts = ts; h->samples = (uint16_t)sp; h->dataLen = (uint16_t)d.size();
            memcpy(ab.data() + sizeof(AudioPktHdr), d.data(), d.size());
            if (SafeSend(ab.data(), tl)) { bc.fetch_add(tl, std::memory_order_relaxed); asc.fetch_add(1, std::memory_order_relaxed); }
        } catch (const std::exception& e) { ERR("SendAudio: %s", e.what()); }
    }

    void SendClipboard(const std::vector<uint8_t>& data) {
        if (!conn.load(std::memory_order_relaxed) || !authenticated.load(std::memory_order_relaxed)) return;
        auto ch = dc;
        if (!ch || !ch->isOpen() || data.empty()) return;

        // Don't send if buffer is getting full
        if (ch->bufferedAmount() > BT / 2) {
            WARN("Clipboard send skipped - buffer full");
            return;
        }

        try {
            // For large clipboard data, send in chunks
            if (data.size() <= CHK) {
                SafeSend(data.data(), data.size());
            } else {
                // Send as single message for clipboard (reliable delivery)
                SafeSend(data.data(), data.size());
            }
            LOG("Clipboard: sent %zu bytes", data.size());
        } catch (const std::exception& e) {
            ERR("SendClipboard: %s", e.what());
        }
    }

    struct Stats { uint64_t sent, bytes, dropped; bool conn; };
    Stats GetStats() { return {sc.exchange(0), bc.exchange(0), dpc.exchange(0), conn.load()}; }
    uint64_t GetAudioSent() { return asc.exchange(0); }
};
