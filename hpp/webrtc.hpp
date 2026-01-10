/**
 * @file webrtc.hpp
 * @brief WebRTC server implementation for peer-to-peer streaming
 * @copyright 2025-2026 Daniel Chrobak
 */

#pragma once
#include "common.hpp"
#include "encoder.hpp"
#include "input.hpp"

#pragma pack(push, 1)
struct PacketHeader { int64_t timestamp; uint32_t encodeTimeUs, frameId; uint16_t chunkIndex, totalChunks; uint8_t frameType; };
struct AudioPacketHeader { uint32_t magic; int64_t timestamp; uint16_t samples, dataLength; };
struct AuthRequestMsg { uint32_t magic; uint8_t usernameLength, pinLength; };
struct AuthResponseMsg { uint32_t magic; uint8_t success, errorLength; };
#pragma pack(pop)

class WebRTCServer {
private:
    std::shared_ptr<rtc::PeerConnection> peerConnection;
    std::shared_ptr<rtc::DataChannel> dataChannel;

    std::atomic<bool> connected{false}, needsKeyframe{true}, fpsReceived{false};
    std::atomic<bool> gatheringComplete{false}, authenticated{false};
    std::atomic<bool> hasLocalDescription{false};  // NEW: Track description availability separately
    std::string localDescription, authUsername, authPin;
    std::mutex descMutex, authMutex;
    std::condition_variable descCondition;
    rtc::Configuration rtcConfig;

    static constexpr size_t BUFFER_THRESHOLD = 32768, CHUNK_SIZE = 1400;
    static constexpr size_t HEADER_SIZE = sizeof(PacketHeader), DATA_CHUNK_SIZE = CHUNK_SIZE - HEADER_SIZE;

    std::vector<uint8_t> packetBuffer, audioBuffer;
    std::atomic<uint64_t> sentCount{0}, byteCount{0}, dropCount{0}, audioSentCount{0};
    std::atomic<uint32_t> frameId{0};
    std::atomic<int> currentFps{60}, overflowCount{0}, authAttempts{0};
    std::atomic<uint8_t> currentFpsMode{0};
    std::atomic<int64_t> lastPingTime{0};
    std::atomic<bool> pingTimeout{false};
    std::atomic<int> candidateCount{0};  // NEW: Track candidate count

    std::function<void(int, uint8_t)> onFpsChange;
    std::function<int()> getHostFps, getCurrentMonitor;
    std::function<bool(int)> onMonitorChange;
    std::function<void()> onDisconnect, onAuthenticated;
    InputHandler* inputHandler = nullptr;

    bool SafeSend(const void* data, size_t len) {
        auto ch = dataChannel;
        if (!ch || !ch->isOpen()) return false;
        try { ch->send(reinterpret_cast<const std::byte*>(data), len); return true; } catch (...) { return false; }
    }

    void SendAuthResponse(bool success, const std::string& error = "") {
        auto ch = dataChannel;
        if (!ch || !ch->isOpen()) return;

        std::vector<uint8_t> buf(sizeof(AuthResponseMsg) + (success ? 0 : error.size()));
        auto* msg = reinterpret_cast<AuthResponseMsg*>(buf.data());
        msg->magic = MSG_AUTH_RESPONSE;
        msg->success = success ? 1 : 0;
        msg->errorLength = success ? 0 : static_cast<uint8_t>(std::min(error.size(), size_t(255)));
        if (!success) memcpy(buf.data() + sizeof(AuthResponseMsg), error.c_str(), msg->errorLength);

        SafeSend(buf.data(), buf.size());

        if (success) { LOG("Client authenticated"); authAttempts = 0; }
        else {
            int att = ++authAttempts;
            WARN("Auth failed (%d/3): %s", att, error.c_str());
            if (att >= 3) std::thread([this] { std::this_thread::sleep_for(100ms); ForceDisconnect("Too many authentication failures"); }).detach();
        }
    }

    void SendHostInfo() {
        auto ch = dataChannel;
        if (!ch || !ch->isOpen()) return;
        uint8_t buf[6];
        *reinterpret_cast<uint32_t*>(buf) = MSG_HOST_INFO;
        *reinterpret_cast<uint16_t*>(buf + 4) = static_cast<uint16_t>(getHostFps ? getHostFps() : 60);
        SafeSend(buf, sizeof(buf));
    }

    void SendMonitorList() {
        auto ch = dataChannel;
        if (!ch || !ch->isOpen()) return;

        std::lock_guard<std::mutex> lock(g_monitorsMutex);
        std::vector<uint8_t> buf(6 + g_monitors.size() * 74);
        size_t off = 0;

        *reinterpret_cast<uint32_t*>(&buf[off]) = MSG_MONITOR_LIST; off += 4;
        buf[off++] = static_cast<uint8_t>(g_monitors.size());
        buf[off++] = static_cast<uint8_t>(getCurrentMonitor ? getCurrentMonitor() : 0);

        for (const auto& m : g_monitors) {
            buf[off++] = static_cast<uint8_t>(m.index);
            *reinterpret_cast<uint16_t*>(&buf[off]) = static_cast<uint16_t>(m.width);
            *reinterpret_cast<uint16_t*>(&buf[off + 2]) = static_cast<uint16_t>(m.height);
            *reinterpret_cast<uint16_t*>(&buf[off + 4]) = static_cast<uint16_t>(m.refreshRate);
            off += 6;
            buf[off++] = m.isPrimary ? 1 : 0;
            size_t nl = std::min(m.name.size(), size_t(63));
            buf[off++] = static_cast<uint8_t>(nl);
            memcpy(&buf[off], m.name.c_str(), nl);
            off += nl;
        }
        SafeSend(buf.data(), off);
    }

    void ForceDisconnect(const char* reason) {
        if (!connected) return;
        WARN("Disconnect: %s", reason);
        connected = fpsReceived = authenticated = false;
        overflowCount = 0; pingTimeout = false;
        try { if (dataChannel) dataChannel->close(); } catch (...) {}
        try { if (peerConnection) peerConnection->close(); } catch (...) {}
        if (onDisconnect) onDisconnect();
    }

    void HandleMessage(const rtc::binary& msg) {
        if (msg.size() < 4) return;
        uint32_t magic = *reinterpret_cast<const uint32_t*>(msg.data());

        if (magic == MSG_AUTH_REQUEST && msg.size() >= sizeof(AuthRequestMsg)) {
            auto* m = reinterpret_cast<const AuthRequestMsg*>(msg.data());
            if (msg.size() >= sizeof(AuthRequestMsg) + m->usernameLength + m->pinLength) {
                std::string user(reinterpret_cast<const char*>(msg.data()) + sizeof(AuthRequestMsg), m->usernameLength);
                std::string pin(reinterpret_cast<const char*>(msg.data()) + sizeof(AuthRequestMsg) + m->usernameLength, m->pinLength);

                std::lock_guard<std::mutex> lock(authMutex);
                if (user == authUsername && pin == authPin) {
                    authenticated = true;
                    SendAuthResponse(true);
                    SendHostInfo();
                    SendMonitorList();
                    if (onAuthenticated) onAuthenticated();
                } else SendAuthResponse(false, "Invalid credentials");
            }
            return;
        }

        if (!authenticated) return;

        if (inputHandler && (magic == MSG_MOUSE_MOVE || magic == MSG_MOUSE_BTN || magic == MSG_MOUSE_WHEEL || magic == MSG_KEY)) {
            inputHandler->HandleMessage(reinterpret_cast<const uint8_t*>(msg.data()), msg.size());
            return;
        }

        if (magic == MSG_PING && msg.size() == 16) {
            lastPingTime = GetTimestamp() / 1000; overflowCount = 0; pingTimeout = false;
            uint8_t resp[24]; memcpy(resp, msg.data(), 16);
            *reinterpret_cast<uint64_t*>(resp + 16) = GetTimestamp();
            SafeSend(resp, sizeof(resp));
        } else if (magic == MSG_FPS_SET && msg.size() == 7) {
            uint16_t fps = *reinterpret_cast<const uint16_t*>(reinterpret_cast<const uint8_t*>(msg.data()) + 4);
            uint8_t mode = static_cast<uint8_t>(msg[6]);
            if (fps >= 1 && fps <= 240 && mode <= 2) {
                int actual = (mode == 1 && getHostFps) ? getHostFps() : fps;
                currentFps = actual; currentFpsMode = mode; fpsReceived = true;
                if (onFpsChange) onFpsChange(actual, mode);
                uint8_t ack[7]; *reinterpret_cast<uint32_t*>(ack) = MSG_FPS_ACK;
                *reinterpret_cast<uint16_t*>(ack + 4) = static_cast<uint16_t>(actual); ack[6] = mode;
                SafeSend(ack, sizeof(ack));
            }
        } else if (magic == MSG_REQUEST_KEY) {
            needsKeyframe = true;
        } else if (magic == MSG_MONITOR_SET && msg.size() == 5) {
            int idx = static_cast<int>(static_cast<uint8_t>(msg[4]));
            if (onMonitorChange && onMonitorChange(idx)) { needsKeyframe = true; SendMonitorList(); SendHostInfo(); }
        }
    }

    void SetupPeerConnection() {
        if (peerConnection) {
            if (dataChannel && dataChannel->isOpen()) dataChannel->close();
            dataChannel.reset(); peerConnection->close();
        }

        connected = needsKeyframe = true;
        fpsReceived = gatheringComplete = authenticated = hasLocalDescription = false;
        overflowCount = 0; lastPingTime = 0; pingTimeout = false; authAttempts = 0;
        candidateCount = 0;
        { std::lock_guard<std::mutex> lock(descMutex); localDescription.clear(); }

        peerConnection = std::make_shared<rtc::PeerConnection>(rtcConfig);

        peerConnection->onLocalDescription([this](rtc::Description d) {
            std::lock_guard<std::mutex> lock(descMutex);
            localDescription = std::string(d);
            hasLocalDescription = true;
            descCondition.notify_all();
            LOG("Local description ready");
        });

        // OPTIMIZATION: Track candidates for faster gathering decision
        peerConnection->onLocalCandidate([this](rtc::Candidate candidate) {
            candidateCount++;
            // Notify after first few candidates for faster response
            if (candidateCount >= 2) {
                descCondition.notify_all();
            }
        });

        peerConnection->onStateChange([this](auto state) {
            bool was = connected.load();
            connected = (state == rtc::PeerConnection::State::Connected);
            if (connected && !was) { needsKeyframe = true; lastPingTime = GetTimestamp() / 1000; LOG("Peer connected"); }
            if (!connected && was) { fpsReceived = authenticated = false; overflowCount = 0; if (onDisconnect) onDisconnect(); }
        });

        peerConnection->onGatheringStateChange([this](auto state) {
            if (state == rtc::PeerConnection::GatheringState::Complete) {
                gatheringComplete = true;
                descCondition.notify_all();
                LOG("ICE gathering complete (%d candidates)", candidateCount.load());
            }
        });

        peerConnection->onDataChannel([this](auto ch) {
            if (ch->label() != "screen") return;
            dataChannel = ch;
            dataChannel->onOpen([this] { connected = needsKeyframe = true; authenticated = false; lastPingTime = GetTimestamp() / 1000; overflowCount = authAttempts = 0; LOG("Data channel opened"); });
            dataChannel->onClosed([this] { connected = fpsReceived = authenticated = false; overflowCount = 0; });
            dataChannel->onMessage([this](auto data) { if (auto* b = std::get_if<rtc::binary>(&data)) HandleMessage(*b); });
        });
    }

    bool IsConnectionStale() {
        if (!connected) return false;
        int64_t lastPing = lastPingTime.load(), now = GetTimestamp() / 1000;
        if (lastPing > 0 && (now - lastPing) > 3000) { if (!pingTimeout.exchange(true)) WARN("Ping timeout"); return true; }
        return overflowCount >= 10;
    }

public:
    WebRTCServer() {
        // OPTIMIZATION: For LAN, we can skip STUN entirely
        // But keep them for NAT traversal fallback
        rtcConfig.iceServers.push_back(rtc::IceServer("stun:stun.l.google.com:19302"));
        rtcConfig.iceServers.push_back(rtc::IceServer("stun:stun1.l.google.com:19302"));

        // OPTIMIZATION: Smaller port range = faster port allocation
        rtcConfig.portRangeBegin = 50000;
        rtcConfig.portRangeEnd = 50020;  // Reduced from 50100

        // OPTIMIZATION: Prefer UDP for lower latency on LAN
        rtcConfig.enableIceTcp = false;

        packetBuffer.resize(CHUNK_SIZE);
        audioBuffer.resize(4096);
        SetupPeerConnection();
        LOG("WebRTC initialized (optimized for fast connection)");
    }

    void SetAuthCredentials(const std::string& u, const std::string& p) { std::lock_guard<std::mutex> lock(authMutex); authUsername = u; authPin = p; }
    void SetInputHandler(InputHandler* h) { inputHandler = h; }
    void SetFpsChangeCallback(std::function<void(int, uint8_t)> cb) { onFpsChange = cb; }
    void SetGetHostFpsCallback(std::function<int()> cb) { getHostFps = cb; }
    void SetMonitorChangeCallback(std::function<bool(int)> cb) { onMonitorChange = cb; }
    void SetGetCurrentMonitorCallback(std::function<int()> cb) { getCurrentMonitor = cb; }
    void SetDisconnectCallback(std::function<void()> cb) { onDisconnect = cb; }
    void SetAuthenticatedCallback(std::function<void()> cb) { onAuthenticated = cb; }

    // OPTIMIZATION: Don't wait for complete ICE gathering!
    // Return as soon as we have the local description and some candidates
    std::string GetLocal() {
        std::unique_lock<std::mutex> lock(descMutex);

        // First, wait for local description (very fast, ~10-50ms)
        if (!descCondition.wait_for(lock, 200ms, [this] {
            return hasLocalDescription.load();
        })) {
            WARN("Timeout waiting for local description");
            return localDescription;
        }

        // OPTIMIZATION: For LAN, don't wait for gathering to complete!
        // Just wait for a few candidates or a short timeout
        // Host candidates are discovered almost instantly on LAN
        descCondition.wait_for(lock, 150ms, [this] {
            // Return early if we have enough candidates or gathering is done
            return gatheringComplete.load() || candidateCount.load() >= 2;
        });

        LOG("Returning answer with %d candidates (gathering %s)",
            candidateCount.load(),
            gatheringComplete.load() ? "complete" : "partial");

        return localDescription;
    }

    void SetRemote(const std::string& sdp, const std::string& type) {
        if (type == "offer") SetupPeerConnection();
        peerConnection->setRemoteDescription(rtc::Description(sdp, type));
        if (type == "offer") peerConnection->setLocalDescription();
    }

    bool IsConnected() const { return connected; }
    bool IsAuthenticated() const { return authenticated; }
    bool IsFpsReceived() const { return fpsReceived; }
    int GetCurrentFps() const { return currentFps; }
    bool NeedsKey() { return needsKeyframe.exchange(false); }

    void Send(const EncodedFrame& frame) {
        if (!connected || !authenticated) return;
        auto ch = dataChannel;
        if (!ch || !ch->isOpen()) { if (connected) ForceDisconnect("Channel closed"); return; }
        if (IsConnectionStale()) { ForceDisconnect("Stale connection"); return; }

        try {
            if (ch->bufferedAmount() > BUFFER_THRESHOLD) { overflowCount++; dropCount++; needsKeyframe = true; if (overflowCount >= 10) ForceDisconnect("Buffer overflow"); return; }
            overflowCount = 0;

            size_t dataSize = frame.data.size();
            size_t numChunks = (dataSize + DATA_CHUNK_SIZE - 1) / DATA_CHUNK_SIZE;
            if (numChunks > 65535 || !dataSize) return;

            PacketHeader hdr = {frame.ts, static_cast<uint32_t>(frame.encUs), frameId++, 0, static_cast<uint16_t>(numChunks), frame.isKey ? uint8_t(1) : uint8_t(0)};
            size_t sent = 0;

            for (size_t i = 0; i < numChunks; i++) {
                if (i > 0 && (i % 16) == 0 && ch->bufferedAmount() > BUFFER_THRESHOLD * 2) { overflowCount++; dropCount++; needsKeyframe = true; break; }
                hdr.chunkIndex = static_cast<uint16_t>(i);
                memcpy(packetBuffer.data(), &hdr, HEADER_SIZE);
                size_t off = i * DATA_CHUNK_SIZE, len = std::min(DATA_CHUNK_SIZE, dataSize - off);
                memcpy(packetBuffer.data() + HEADER_SIZE, frame.data.data() + off, len);
                if (!SafeSend(packetBuffer.data(), HEADER_SIZE + len)) { overflowCount++; dropCount++; needsKeyframe = true; break; }
                sent += HEADER_SIZE + len;
            }
            if (sent) { byteCount += sent; sentCount++; }
        } catch (...) { dropCount++; needsKeyframe = true; overflowCount++; }
    }

    void SendAudio(const std::vector<uint8_t>& data, int64_t ts, int samples) {
        if (!connected || !authenticated || data.empty() || data.size() > 4000 || overflowCount >= 5) return;
        auto ch = dataChannel;
        if (!ch || !ch->isOpen() || ch->bufferedAmount() > BUFFER_THRESHOLD / 2) return;

        try {
            size_t total = sizeof(AudioPacketHeader) + data.size();
            if (audioBuffer.size() < total) audioBuffer.resize(total);
            auto* hdr = reinterpret_cast<AudioPacketHeader*>(audioBuffer.data());
            hdr->magic = MSG_AUDIO_DATA; hdr->timestamp = ts;
            hdr->samples = static_cast<uint16_t>(samples); hdr->dataLength = static_cast<uint16_t>(data.size());
            memcpy(audioBuffer.data() + sizeof(AudioPacketHeader), data.data(), data.size());
            if (SafeSend(audioBuffer.data(), total)) { byteCount += total; audioSentCount++; }
        } catch (...) {}
    }

    struct Stats { uint64_t sent, bytes, dropped; bool connected; };
    Stats GetStats() { return {sentCount.exchange(0), byteCount.exchange(0), dropCount.exchange(0), connected.load()}; }
    uint64_t GetAudioSent() { return audioSentCount.exchange(0); }
};
