#pragma once
#include "common.hpp"
#include "encoder.hpp"
#include "input.hpp"

#pragma pack(push, 1)
struct PktHdr { int64_t ts; uint32_t encUs, fid; uint16_t idx, total; uint8_t type; };
struct AudioPktHdr { uint32_t magic; int64_t ts; uint16_t samples, dataLen; };
struct AuthReqMsg { uint32_t magic; uint8_t usernameLen, pinLen; };
struct AuthRespMsg { uint32_t magic; uint8_t success, errorLen; };
#pragma pack(pop)

class WebRTCServer {
    std::shared_ptr<rtc::PeerConnection> pc; std::shared_ptr<rtc::DataChannel> dc;
    std::atomic<bool> conn{false}, nkey{true}, fpsr{false}, gc{false}, authenticated{false};
    std::string ldesc, authUser, authPin; std::mutex dmx, authMtx; std::condition_variable dcv;
    rtc::Configuration cfg;

    // Many internet paths have MTU < 1500 due to tunneling (VPNs, PPPoE, etc.)
    static constexpr size_t BT = 32768, CHK = 1200, HDR = sizeof(PktHdr), DCK = CHK - HDR;
    std::vector<uint8_t> pb, ab;
    std::atomic<uint64_t> sc{0}, bc{0}, dpc{0}, asc{0}; std::atomic<uint32_t> fid{0};
    std::atomic<int> cfps{60}; std::atomic<uint8_t> cfm{0};
    std::atomic<int> overflows{0}; std::atomic<int64_t> lastPing{0}; std::atomic<bool> pingTimeout{false};
    std::function<void(int, uint8_t)> onFps; std::function<int()> getHfps, getCmon;
    std::function<bool(int)> onMon; std::function<void()> onDisconnect, onAuth;
    InputHandler* input = nullptr; std::function<bool(const uint8_t*, size_t)> clipHandler;

    // ICE candidate counters for logging
    std::atomic<int> hostCandidates{0}, srflxCandidates{0}, relayCandidates{0};

    // Debug counters
    std::atomic<uint64_t> bufferOverflowCount{0}, sendFailCount{0}, midFrameDropCount{0};
    std::atomic<size_t> maxBufferedAmount{0};

    bool SafeSend(const void* d, size_t l) {
        auto ch = dc;
        if (!ch || !ch->isOpen()) return false;
        try {
            ch->send((const std::byte*)d, l);
            return true;
        } catch (const std::exception&) {
            sendFailCount++;
            return false;
        }
    }

    void SendAuthResp(bool ok, const std::string& err = "") {
        auto ch = dc; if (!ch || !ch->isOpen()) return;
        std::vector<uint8_t> buf(sizeof(AuthRespMsg) + (ok ? 0 : err.size()));
        auto* msg = (AuthRespMsg*)buf.data(); msg->magic = MSG_AUTH_RESPONSE; msg->success = ok; msg->errorLen = ok ? 0 : (uint8_t)std::min(err.size(), (size_t)255);
        if (!ok) memcpy(buf.data() + sizeof(AuthRespMsg), err.c_str(), msg->errorLen);
        SafeSend(buf.data(), buf.size());
        if (ok) { LOG("Client authenticated"); } else { WARN("Auth failed: %s", err.c_str()); std::thread([this] { std::this_thread::sleep_for(100ms); ForceDisconnect("Auth failed"); }).detach(); }
    }

    void SendHostInfo() { if (auto ch = dc; ch && ch->isOpen()) { int fps = getHfps ? getHfps() : 60; uint8_t b[6]; *(uint32_t*)b = MSG_HOST_INFO; *(uint16_t*)(b+4) = (uint16_t)fps; SafeSend(b, 6); } }

    void SendMonitorList() {
        auto ch = dc; if (!ch || !ch->isOpen()) return;
        std::lock_guard<std::mutex> lk(g_monitorsMutex);
        std::vector<uint8_t> b(6 + g_monitors.size() * 74); size_t o = 0;
        *(uint32_t*)&b[o] = MSG_MONITOR_LIST; o += 4; b[o++] = (uint8_t)g_monitors.size(); b[o++] = (uint8_t)(getCmon ? getCmon() : 0);
        for (const auto& m : g_monitors) {
            b[o++] = (uint8_t)m.index; *(uint16_t*)&b[o] = (uint16_t)m.width; *(uint16_t*)&b[o+2] = (uint16_t)m.height;
            *(uint16_t*)&b[o+4] = (uint16_t)m.refreshRate; o += 6; b[o++] = m.isPrimary;
            size_t nl = std::min(m.name.size(), (size_t)63); b[o++] = (uint8_t)nl; memcpy(&b[o], m.name.c_str(), nl); o += nl;
        }
        SafeSend(b.data(), o);
    }

    void ForceDisconnect(const char* r) {
        if (!conn) return; WARN("Disconnect: %s", r);
        conn = fpsr = authenticated = false; overflows = 0; pingTimeout = false;
        try { if (dc) dc->close(); } catch (...) {} try { if (pc) pc->close(); } catch (...) {}
        if (onDisconnect) onDisconnect();
    }

    void HandleMsg(const rtc::binary& b) {
        if (b.size() < 4) return;
        uint32_t mg = *(uint32_t*)reinterpret_cast<const uint8_t*>(b.data());
        if (mg == MSG_AUTH_REQUEST && b.size() >= sizeof(AuthReqMsg)) {
            auto* m = (AuthReqMsg*)reinterpret_cast<const uint8_t*>(b.data());
            if (b.size() >= sizeof(AuthReqMsg) + m->usernameLen + m->pinLen) {
                std::string u((char*)reinterpret_cast<const uint8_t*>(b.data()) + sizeof(AuthReqMsg), m->usernameLen);
                std::string p((char*)reinterpret_cast<const uint8_t*>(b.data()) + sizeof(AuthReqMsg) + m->usernameLen, m->pinLen);
                std::lock_guard<std::mutex> lk(authMtx);
                if (u == authUser && p == authPin) { authenticated = true; SendAuthResp(true); SendHostInfo(); SendMonitorList(); if (onAuth) onAuth(); }
                else SendAuthResp(false, "Invalid credentials");
            }
            return;
        }
        if (!authenticated) return;
        if (input && (mg == MSG_MOUSE_MOVE || mg == MSG_MOUSE_BTN || mg == MSG_MOUSE_WHEEL || mg == MSG_KEY)) { input->HandleMessage(reinterpret_cast<const uint8_t*>(b.data()), b.size()); return; }
        if ((mg == MSG_CLIPBOARD_TEXT || mg == MSG_CLIPBOARD_IMAGE || mg == MSG_CLIPBOARD_REQUEST) && clipHandler) { clipHandler(reinterpret_cast<const uint8_t*>(b.data()), b.size()); return; }
        if (mg == MSG_PING && b.size() == 16) { lastPing = GetTimestamp() / 1000; overflows = 0; pingTimeout = false; uint8_t p[24]; memcpy(p, b.data(), 16); *(uint64_t*)(p+16) = GetTimestamp(); SafeSend(p, 24); }
        else if (mg == MSG_FPS_SET && b.size() == 7) {
            uint16_t fps = *(uint16_t*)(reinterpret_cast<const uint8_t*>(b.data())+4); uint8_t md = static_cast<uint8_t>(b[6]);
            if (fps >= 1 && fps <= 240 && md <= 2) { int ac = (md == 1 && getHfps) ? getHfps() : fps; cfps = ac; cfm = md; fpsr = true; if (onFps) onFps(ac, md); uint8_t a[7]; *(uint32_t*)a = MSG_FPS_ACK; *(uint16_t*)(a+4) = (uint16_t)ac; a[6] = md; SafeSend(a, 7); }
        }
        else if (mg == MSG_REQUEST_KEY) { nkey = true; }
        else if (mg == MSG_MONITOR_SET && b.size() == 5) { if (onMon && onMon(static_cast<int>(static_cast<uint8_t>(b[4])))) { nkey = true; SendMonitorList(); SendHostInfo(); } }
    }

    const char* StateToString(rtc::PeerConnection::State s) {
        switch (s) {
            case rtc::PeerConnection::State::New: return "New";
            case rtc::PeerConnection::State::Connecting: return "Connecting";
            case rtc::PeerConnection::State::Connected: return "Connected";
            case rtc::PeerConnection::State::Disconnected: return "Disconnected";
            case rtc::PeerConnection::State::Failed: return "Failed";
            case rtc::PeerConnection::State::Closed: return "Closed";
            default: return "Unknown";
        }
    }

    const char* IceStateToString(rtc::PeerConnection::IceState s) {
        switch (s) {
            case rtc::PeerConnection::IceState::New: return "New";
            case rtc::PeerConnection::IceState::Checking: return "Checking";
            case rtc::PeerConnection::IceState::Connected: return "Connected";
            case rtc::PeerConnection::IceState::Completed: return "Completed";
            case rtc::PeerConnection::IceState::Failed: return "Failed";
            case rtc::PeerConnection::IceState::Disconnected: return "Disconnected";
            case rtc::PeerConnection::IceState::Closed: return "Closed";
            default: return "Unknown";
        }
    }

    const char* GatheringStateToString(rtc::PeerConnection::GatheringState s) {
        switch (s) {
            case rtc::PeerConnection::GatheringState::New: return "New";
            case rtc::PeerConnection::GatheringState::InProgress: return "InProgress";
            case rtc::PeerConnection::GatheringState::Complete: return "Complete";
            default: return "Unknown";
        }
    }

    void Setup() {
        if (pc) { if (dc && dc->isOpen()) dc->close(); dc.reset(); pc->close(); }
        conn = nkey = true; fpsr = gc = authenticated = false; overflows = 0; lastPing = 0; pingTimeout = false;
        hostCandidates = srflxCandidates = relayCandidates = 0;
        bufferOverflowCount = sendFailCount = midFrameDropCount = 0;
        maxBufferedAmount = 0;
        { std::lock_guard<std::mutex> lk(dmx); ldesc.clear(); }
        pc = std::make_shared<rtc::PeerConnection>(cfg);

        pc->onLocalDescription([this](rtc::Description d) {
            std::lock_guard<std::mutex> lk(dmx);
            ldesc = std::string(d);
            LOG("Local description ready (type: %s)", d.typeString().c_str());
            dcv.notify_all();
        });

        pc->onLocalCandidate([this](rtc::Candidate c) {
            std::string cand = std::string(c);
            // Parse candidate type from SDP
            std::string type = "unknown";
            if (cand.find("typ host") != std::string::npos) { type = "host"; hostCandidates++; }
            else if (cand.find("typ srflx") != std::string::npos) { type = "srflx"; srflxCandidates++; }
            else if (cand.find("typ relay") != std::string::npos) { type = "relay"; relayCandidates++; }
            else if (cand.find("typ prflx") != std::string::npos) { type = "prflx"; }

            // Extract address for logging
            size_t addrStart = cand.find("candidate:");
            std::string addrInfo = "";
            if (addrStart != std::string::npos) {
                // Format: candidate:foundation component protocol priority address port typ type
                std::istringstream iss(cand.substr(addrStart + 10));
                std::string foundation, component, protocol, priority, address, port;
                iss >> foundation >> component >> protocol >> priority >> address >> port;
                addrInfo = address + ":" + port + " (" + protocol + ")";
            }

            LOG("ICE candidate: %s - %s", type.c_str(), addrInfo.c_str());
        });

        pc->onStateChange([this](auto s) {
            LOG("Connection state: %s", StateToString(s));
            bool was = conn.load();
            conn = (s == rtc::PeerConnection::State::Connected);
            if (conn && !was) {
                nkey = true;
                lastPing = GetTimestamp() / 1000;
                LOG("WebRTC connected!");
            }
            if (!conn && was) {
                fpsr = authenticated = false;
                overflows = 0;
                if (onDisconnect) onDisconnect();
            }
        });

        pc->onIceStateChange([this](auto s) {
            LOG("ICE state: %s", IceStateToString(s));
            if (s == rtc::PeerConnection::IceState::Failed) {
                WARN("ICE negotiation failed - check firewall/NAT settings");
            }
        });

        pc->onGatheringStateChange([this](auto s) {
            LOG("ICE gathering: %s", GatheringStateToString(s));
            if (s == rtc::PeerConnection::GatheringState::Complete) {
                LOG("ICE gathering complete: host=%d, srflx=%d, relay=%d",
                    hostCandidates.load(), srflxCandidates.load(), relayCandidates.load());
                if (srflxCandidates.load() == 0) {
                    WARN("No server-reflexive candidates! STUN may be blocked by firewall.");
                    WARN("Internet connections will likely fail. Check UDP outbound to ports 19302/3478.");
                }
                gc = true;
                dcv.notify_all();
            }
        });

        pc->onDataChannel([this](auto ch) {
            LOG("Data channel received: %s", ch->label().c_str());
            if (ch->label() != "screen") return;
            dc = ch;
            dc->onOpen([this] {
                LOG("Data channel opened");
                conn = nkey = true;
                authenticated = false;
                lastPing = GetTimestamp() / 1000;
                overflows = 0;
            });
            dc->onClosed([this] {
                LOG("Data channel closed");
                conn = fpsr = authenticated = false;
                overflows = 0;
            });
            dc->onMessage([this](auto d) { if (auto* b = std::get_if<rtc::binary>(&d)) HandleMsg(*b); });
        });
    }

    bool IsStale() { if (!conn) return false; int64_t lp = lastPing.load(), now = GetTimestamp() / 1000; if (lp > 0 && (now - lp) > 3000) { if (!pingTimeout.exchange(true)) WARN("Ping timeout"); return true; } return overflows >= 10; }

public:
    WebRTCServer() {
        cfg.iceServers.push_back(rtc::IceServer("stun:stun.l.google.com:19302"));
        cfg.iceServers.push_back(rtc::IceServer("stun:stun1.l.google.com:19302"));
        cfg.portRangeBegin = 50000; cfg.portRangeEnd = 50100; cfg.enableIceTcp = true;
        pb.resize(CHK); ab.resize(4096); Setup();
        LOG("WebRTC initialized with STUN servers");
        LOG("Chunk size: %zu bytes (MTU-safe for internet)", CHK);
    }

    void SetAuthCredentials(const std::string& u, const std::string& p) { std::lock_guard<std::mutex> lk(authMtx); authUser = u; authPin = p; }
    void SetInputHandler(InputHandler* h) { input = h; }
    void SetFpsChangeCallback(std::function<void(int, uint8_t)> cb) { onFps = cb; }
    void SetGetHostFpsCallback(std::function<int()> cb) { getHfps = cb; }
    void SetMonitorChangeCallback(std::function<bool(int)> cb) { onMon = cb; }
    void SetGetCurrentMonitorCallback(std::function<int()> cb) { getCmon = cb; }
    void SetDisconnectCallback(std::function<void()> cb) { onDisconnect = cb; }
    void SetClipboardHandler(std::function<bool(const uint8_t*, size_t)> cb) { clipHandler = cb; }
    void SetAuthenticatedCallback(std::function<void()> cb) { onAuth = cb; }

    std::string GetLocal() {
        std::unique_lock<std::mutex> lk(dmx);
        // Wait up to 10 seconds for ICE gathering (STUN can take a while)
        bool ready = dcv.wait_for(lk, 10s, [this] { return !ldesc.empty() && gc.load(); });
        if (!ready) {
            WARN("ICE gathering timeout after 10s");
        }
        // Get the CURRENT local description which includes all gathered candidates
        // The cached ldesc from onLocalDescription may not have srflx candidates yet
        if (pc) {
            auto desc = pc->localDescription();
            if (desc.has_value()) {
                std::string currentSdp = std::string(desc.value());
                // Count candidates in SDP for logging
                int candCount = 0;
                size_t pos = 0;
                while ((pos = currentSdp.find("a=candidate:", pos)) != std::string::npos) { candCount++; pos++; }
                int srflxCount = 0;
                pos = 0;
                while ((pos = currentSdp.find("typ srflx", pos)) != std::string::npos) { srflxCount++; pos++; }
                LOG("Final SDP: %d candidates (%d srflx)", candCount, srflxCount);
                return currentSdp;
            }
        }
        return ldesc;
    }

    void SetRemote(const std::string& sdp, const std::string& type) {
        LOG("Setting remote description (type: %s)", type.c_str());
        if (type == "offer") Setup();
        pc->setRemoteDescription(rtc::Description(sdp, type));
        if (type == "offer") {
            LOG("Creating local description...");
            pc->setLocalDescription();
        }
    }

    bool IsConnected() const { return conn; }
    bool IsAuthenticated() const { return authenticated; }
    bool IsFpsReceived() const { return fpsr; }
    int GetCurrentFps() const { return cfps; }
    bool NeedsKey() { return nkey.exchange(false); }

    void Send(const EncodedFrame& f) {
        if (!conn || !authenticated) return;
        auto ch = dc; if (!ch || !ch->isOpen()) { if (conn) ForceDisconnect("Channel closed"); return; }
        if (IsStale()) { ForceDisconnect("Stale connection"); return; }
        try {
            size_t buffered = ch->bufferedAmount();
            maxBufferedAmount = std::max(maxBufferedAmount.load(), buffered);

            if (buffered > BT) {
                overflows++;
                dpc++;
                nkey = true;
                bufferOverflowCount++;

                // Log buffer overflow with details
                if (bufferOverflowCount % 10 == 1) {
                    WARN("Buffer overflow: %zu bytes buffered (threshold: %zu), overflow #%llu",
                         buffered, BT, bufferOverflowCount.load());
                }

                if (overflows >= 10) ForceDisconnect("Buffer overflow");
                return;
            }
            overflows = 0;
            size_t sz = f.data.size(), n = (sz + DCK - 1) / DCK; if (n > 65535 || !sz) return;
            PktHdr hd = {f.ts, (uint32_t)f.encUs, fid++, 0, (uint16_t)n, f.isKey ? (uint8_t)1 : (uint8_t)0};
            size_t sent = 0;
            bool frameFailed = false;

            for (size_t i = 0; i < n; i++) {
                // Check buffer pressure mid-frame
                if (i > 0 && (i % 16) == 0) {
                    size_t midBuffered = ch->bufferedAmount();
                    if (midBuffered > BT * 2) {
                        overflows++;
                        dpc++;
                        nkey = true;
                        midFrameDropCount++;
                        frameFailed = true;

                        // Log mid-frame drop
                        if (midFrameDropCount % 10 == 1) {
                            WARN("Mid-frame buffer overflow at chunk %zu/%zu: %zu bytes buffered",
                                 i, n, midBuffered);
                        }
                        break;
                    }
                }

                hd.idx = (uint16_t)i;
                memcpy(pb.data(), &hd, HDR);
                size_t o = i * DCK, l = std::min(DCK, sz - o);
                memcpy(pb.data() + HDR, f.data.data() + o, l);

                if (!SafeSend(pb.data(), HDR + l)) {
                    overflows++;
                    dpc++;
                    nkey = true;
                    frameFailed = true;
                    break;
                }
                sent += HDR + l;
            }

            if (sent && !frameFailed) { bc += sent; sc++; }
        } catch (const std::exception& e) {
            dpc++;
            nkey = true;
            overflows++;
            WARN("Send exception: %s", e.what());
        }
    }

    void SendAudio(const std::vector<uint8_t>& d, int64_t ts, int sp) {
        if (!conn || !authenticated || d.empty() || d.size() > 4000 || overflows >= 5) return;
        auto ch = dc; if (!ch || !ch->isOpen() || ch->bufferedAmount() > BT / 2) return;
        try {
            size_t tl = sizeof(AudioPktHdr) + d.size(); if (ab.size() < tl) ab.resize(tl);
            auto* h = (AudioPktHdr*)ab.data(); h->magic = MSG_AUDIO_DATA; h->ts = ts; h->samples = (uint16_t)sp; h->dataLen = (uint16_t)d.size();
            memcpy(ab.data() + sizeof(AudioPktHdr), d.data(), d.size());
            if (SafeSend(ab.data(), tl)) { bc += tl; asc++; }
        } catch (...) {}
    }

    void SendClipboard(const std::vector<uint8_t>& data) { if (!conn || !authenticated || data.empty()) return; auto ch = dc; if (!ch || !ch->isOpen() || ch->bufferedAmount() > BT / 2) return; SafeSend(data.data(), data.size()); }

    struct Stats { uint64_t sent, bytes, dropped; bool conn; };
    Stats GetStats() { return {sc.exchange(0), bc.exchange(0), dpc.exchange(0), conn.load()}; }
    uint64_t GetAudioSent() { return asc.exchange(0); }

    // Debug stats
    struct DebugStats {
        uint64_t bufferOverflows;
        uint64_t sendFails;
        uint64_t midFrameDrops;
        size_t maxBuffered;
    };
    DebugStats GetDebugStats() {
        return {
            bufferOverflowCount.exchange(0),
            sendFailCount.exchange(0),
            midFrameDropCount.exchange(0),
            maxBufferedAmount.exchange(0)
        };
    }
};
