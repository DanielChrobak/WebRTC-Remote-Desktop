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
struct PacketHeader {
    int64_t timestamp;
    uint32_t encodeTimeUs;
    uint32_t frameId;
    uint16_t chunkIndex;
    uint16_t totalChunks;
    uint8_t frameType;
};

struct AudioPacketHeader {
    uint32_t magic;
    int64_t timestamp;
    uint16_t samples;
    uint16_t dataLength;
};

struct AuthRequestMsg {
    uint32_t magic;
    uint8_t usernameLength;
    uint8_t pinLength;
};

struct AuthResponseMsg {
    uint32_t magic;
    uint8_t success;
    uint8_t errorLength;
};
#pragma pack(pop)

/**
 * @brief WebRTC server for real-time video streaming and input handling
 *
 * Manages peer connections, data channels, authentication, and handles
 * encoding/transmission of video frames and audio packets.
 */
class WebRTCServer {
private:
    std::shared_ptr<rtc::PeerConnection> peerConnection;
    std::shared_ptr<rtc::DataChannel> dataChannel;

    std::atomic<bool> connected{false};
    std::atomic<bool> needsKeyframe{true};
    std::atomic<bool> fpsReceived{false};
    std::atomic<bool> gatheringComplete{false};
    std::atomic<bool> authenticated{false};

    std::string localDescription;
    std::string authUsername;
    std::string authPin;

    std::mutex descMutex;
    std::mutex authMutex;
    std::condition_variable descCondition;

    rtc::Configuration rtcConfig;

    static constexpr size_t BUFFER_THRESHOLD = 32768;
    static constexpr size_t CHUNK_SIZE = 1400;
    static constexpr size_t HEADER_SIZE = sizeof(PacketHeader);
    static constexpr size_t DATA_CHUNK_SIZE = CHUNK_SIZE - HEADER_SIZE;

    std::vector<uint8_t> packetBuffer;
    std::vector<uint8_t> audioBuffer;

    std::atomic<uint64_t> sentCount{0};
    std::atomic<uint64_t> byteCount{0};
    std::atomic<uint64_t> dropCount{0};
    std::atomic<uint64_t> audioSentCount{0};
    std::atomic<uint32_t> frameId{0};

    std::atomic<int> currentFps{60};
    std::atomic<uint8_t> currentFpsMode{0};

    std::atomic<int> overflowCount{0};
    std::atomic<int64_t> lastPingTime{0};
    std::atomic<bool> pingTimeout{false};
    std::atomic<int> authAttempts{0};

    std::function<void(int, uint8_t)> onFpsChange;
    std::function<int()> getHostFps;
    std::function<int()> getCurrentMonitor;
    std::function<bool(int)> onMonitorChange;
    std::function<void()> onDisconnect;
    std::function<void()> onAuthenticated;

    InputHandler* inputHandler = nullptr;

    bool SafeSend(const void* data, size_t length) {
        auto channel = dataChannel;
        if (!channel || !channel->isOpen()) {
            return false;
        }
        try {
            channel->send(reinterpret_cast<const std::byte*>(data), length);
            return true;
        } catch (...) {
            return false;
        }
    }

    void SendAuthResponse(bool success, const std::string& error = "") {
        auto channel = dataChannel;
        if (!channel || !channel->isOpen()) {
            return;
        }

        std::vector<uint8_t> buffer(sizeof(AuthResponseMsg) + (success ? 0 : error.size()));
        auto* msg = reinterpret_cast<AuthResponseMsg*>(buffer.data());

        msg->magic = MSG_AUTH_RESPONSE;
        msg->success = success ? 1 : 0;
        msg->errorLength = success ? 0 : static_cast<uint8_t>(std::min(error.size(), size_t(255)));

        if (!success) {
            memcpy(buffer.data() + sizeof(AuthResponseMsg), error.c_str(), msg->errorLength);
        }

        SafeSend(buffer.data(), buffer.size());

        if (success) {
            LOG("Client authenticated");
            authAttempts = 0;
        } else {
            int attempts = ++authAttempts;
            WARN("Auth failed (%d/3): %s", attempts, error.c_str());

            if (attempts >= 3) {
                std::thread([this] {
                    std::this_thread::sleep_for(100ms);
                    ForceDisconnect("Too many authentication failures");
                }).detach();
            }
        }
    }

    void SendHostInfo() {
        auto channel = dataChannel;
        if (!channel || !channel->isOpen()) {
            return;
        }

        int fps = getHostFps ? getHostFps() : 60;
        uint8_t buffer[6];

        *reinterpret_cast<uint32_t*>(buffer) = MSG_HOST_INFO;
        *reinterpret_cast<uint16_t*>(buffer + 4) = static_cast<uint16_t>(fps);

        SafeSend(buffer, sizeof(buffer));
    }

    void SendMonitorList() {
        auto channel = dataChannel;
        if (!channel || !channel->isOpen()) {
            return;
        }

        std::lock_guard<std::mutex> lock(g_monitorsMutex);

        std::vector<uint8_t> buffer(6 + g_monitors.size() * 74);
        size_t offset = 0;

        *reinterpret_cast<uint32_t*>(&buffer[offset]) = MSG_MONITOR_LIST;
        offset += 4;

        buffer[offset++] = static_cast<uint8_t>(g_monitors.size());
        buffer[offset++] = static_cast<uint8_t>(getCurrentMonitor ? getCurrentMonitor() : 0);

        for (const auto& monitor : g_monitors) {
            buffer[offset++] = static_cast<uint8_t>(monitor.index);
            *reinterpret_cast<uint16_t*>(&buffer[offset]) = static_cast<uint16_t>(monitor.width);
            *reinterpret_cast<uint16_t*>(&buffer[offset + 2]) = static_cast<uint16_t>(monitor.height);
            *reinterpret_cast<uint16_t*>(&buffer[offset + 4]) = static_cast<uint16_t>(monitor.refreshRate);
            offset += 6;

            buffer[offset++] = monitor.isPrimary ? 1 : 0;

            size_t nameLength = std::min(monitor.name.size(), size_t(63));
            buffer[offset++] = static_cast<uint8_t>(nameLength);
            memcpy(&buffer[offset], monitor.name.c_str(), nameLength);
            offset += nameLength;
        }

        SafeSend(buffer.data(), offset);
    }

    void ForceDisconnect(const char* reason) {
        if (!connected) return;

        WARN("Disconnect: %s", reason);

        connected = fpsReceived = authenticated = false;
        overflowCount = 0;
        pingTimeout = false;

        try {
            if (dataChannel) dataChannel->close();
        } catch (...) {}

        try {
            if (peerConnection) peerConnection->close();
        } catch (...) {}

        if (onDisconnect) {
            onDisconnect();
        }
    }

    void HandleMessage(const rtc::binary& message) {
        if (message.size() < 4) return;

        uint32_t magic = *reinterpret_cast<const uint32_t*>(message.data());

        if (magic == MSG_AUTH_REQUEST && message.size() >= sizeof(AuthRequestMsg)) {
            auto* msg = reinterpret_cast<const AuthRequestMsg*>(message.data());

            if (message.size() >= sizeof(AuthRequestMsg) + msg->usernameLength + msg->pinLength) {
                std::string username(
                    reinterpret_cast<const char*>(message.data()) + sizeof(AuthRequestMsg),
                    msg->usernameLength
                );
                std::string pin(
                    reinterpret_cast<const char*>(message.data()) + sizeof(AuthRequestMsg) + msg->usernameLength,
                    msg->pinLength
                );

                std::lock_guard<std::mutex> lock(authMutex);

                if (username == authUsername && pin == authPin) {
                    authenticated = true;
                    SendAuthResponse(true);
                    SendHostInfo();
                    SendMonitorList();
                    if (onAuthenticated) {
                        onAuthenticated();
                    }
                } else {
                    SendAuthResponse(false, "Invalid credentials");
                }
            }
            return;
        }

        if (!authenticated) return;

        if (inputHandler && (magic == MSG_MOUSE_MOVE || magic == MSG_MOUSE_BTN ||
                             magic == MSG_MOUSE_WHEEL || magic == MSG_KEY)) {
            inputHandler->HandleMessage(
                reinterpret_cast<const uint8_t*>(message.data()),
                message.size()
            );
            return;
        }

        if (magic == MSG_PING && message.size() == 16) {
            lastPingTime = GetTimestamp() / 1000;
            overflowCount = 0;
            pingTimeout = false;

            uint8_t response[24];
            memcpy(response, message.data(), 16);
            *reinterpret_cast<uint64_t*>(response + 16) = GetTimestamp();

            SafeSend(response, sizeof(response));
        } else if (magic == MSG_FPS_SET && message.size() == 7) {
            uint16_t fps = *reinterpret_cast<const uint16_t*>(
                reinterpret_cast<const uint8_t*>(message.data()) + 4
            );
            uint8_t mode = static_cast<uint8_t>(message[6]);

            if (fps >= 1 && fps <= 240 && mode <= 2) {
                int actualFps = (mode == 1 && getHostFps) ? getHostFps() : fps;
                currentFps = actualFps;
                currentFpsMode = mode;
                fpsReceived = true;

                if (onFpsChange) {
                    onFpsChange(actualFps, mode);
                }

                uint8_t ack[7];
                *reinterpret_cast<uint32_t*>(ack) = MSG_FPS_ACK;
                *reinterpret_cast<uint16_t*>(ack + 4) = static_cast<uint16_t>(actualFps);
                ack[6] = mode;

                SafeSend(ack, sizeof(ack));
            }
        } else if (magic == MSG_REQUEST_KEY) {
            needsKeyframe = true;
        } else if (magic == MSG_MONITOR_SET && message.size() == 5) {
            int monitorIndex = static_cast<int>(static_cast<uint8_t>(message[4]));

            if (onMonitorChange && onMonitorChange(monitorIndex)) {
                needsKeyframe = true;
                SendMonitorList();
                SendHostInfo();
            }
        }
    }

    void SetupPeerConnection() {
        if (peerConnection) {
            if (dataChannel && dataChannel->isOpen()) {
                dataChannel->close();
            }
            dataChannel.reset();
            peerConnection->close();
        }

        connected = needsKeyframe = true;
        fpsReceived = gatheringComplete = authenticated = false;
        overflowCount = 0;
        lastPingTime = 0;
        pingTimeout = false;
        authAttempts = 0;

        {
            std::lock_guard<std::mutex> lock(descMutex);
            localDescription.clear();
        }

        peerConnection = std::make_shared<rtc::PeerConnection>(rtcConfig);

        peerConnection->onLocalDescription([this](rtc::Description desc) {
            std::lock_guard<std::mutex> lock(descMutex);
            localDescription = std::string(desc);
            descCondition.notify_all();
        });

        peerConnection->onStateChange([this](auto state) {
            bool wasConnected = connected.load();
            connected = (state == rtc::PeerConnection::State::Connected);

            if (connected && !wasConnected) {
                needsKeyframe = true;
                lastPingTime = GetTimestamp() / 1000;
            }

            if (!connected && wasConnected) {
                fpsReceived = authenticated = false;
                overflowCount = 0;
                if (onDisconnect) {
                    onDisconnect();
                }
            }
        });

        peerConnection->onGatheringStateChange([this](auto state) {
            if (state == rtc::PeerConnection::GatheringState::Complete) {
                gatheringComplete = true;
                descCondition.notify_all();
            }
        });

        peerConnection->onDataChannel([this](auto channel) {
            if (channel->label() != "screen") return;

            dataChannel = channel;

            dataChannel->onOpen([this] {
                connected = needsKeyframe = true;
                authenticated = false;
                lastPingTime = GetTimestamp() / 1000;
                overflowCount = 0;
                authAttempts = 0;
            });

            dataChannel->onClosed([this] {
                connected = fpsReceived = authenticated = false;
                overflowCount = 0;
            });

            dataChannel->onMessage([this](auto data) {
                if (auto* binary = std::get_if<rtc::binary>(&data)) {
                    HandleMessage(*binary);
                }
            });
        });
    }

    bool IsConnectionStale() {
        if (!connected) return false;

        int64_t lastPing = lastPingTime.load();
        int64_t now = GetTimestamp() / 1000;

        if (lastPing > 0 && (now - lastPing) > 3000) {
            if (!pingTimeout.exchange(true)) {
                WARN("Ping timeout");
            }
            return true;
        }

        return overflowCount >= 10;
    }

public:
    WebRTCServer() {
        rtcConfig.iceServers.push_back(rtc::IceServer("stun:stun.l.google.com:19302"));
        rtcConfig.iceServers.push_back(rtc::IceServer("stun:stun1.l.google.com:19302"));
        rtcConfig.portRangeBegin = 50000;
        rtcConfig.portRangeEnd = 50100;
        rtcConfig.enableIceTcp = true;

        packetBuffer.resize(CHUNK_SIZE);
        audioBuffer.resize(4096);

        SetupPeerConnection();

        LOG("WebRTC initialized with STUN");
    }

    void SetAuthCredentials(const std::string& username, const std::string& pin) {
        std::lock_guard<std::mutex> lock(authMutex);
        authUsername = username;
        authPin = pin;
    }

    void SetInputHandler(InputHandler* handler) {
        inputHandler = handler;
    }

    void SetFpsChangeCallback(std::function<void(int, uint8_t)> callback) {
        onFpsChange = callback;
    }

    void SetGetHostFpsCallback(std::function<int()> callback) {
        getHostFps = callback;
    }

    void SetMonitorChangeCallback(std::function<bool(int)> callback) {
        onMonitorChange = callback;
    }

    void SetGetCurrentMonitorCallback(std::function<int()> callback) {
        getCurrentMonitor = callback;
    }

    void SetDisconnectCallback(std::function<void()> callback) {
        onDisconnect = callback;
    }

    void SetAuthenticatedCallback(std::function<void()> callback) {
        onAuthenticated = callback;
    }

    std::string GetLocal() {
        std::unique_lock<std::mutex> lock(descMutex);
        descCondition.wait_for(lock, 5s, [this] {
            return !localDescription.empty() && gatheringComplete.load();
        });
        return localDescription;
    }

    void SetRemote(const std::string& sdp, const std::string& type) {
        if (type == "offer") {
            SetupPeerConnection();
        }

        peerConnection->setRemoteDescription(rtc::Description(sdp, type));

        if (type == "offer") {
            peerConnection->setLocalDescription();
        }
    }

    bool IsConnected() const { return connected; }
    bool IsAuthenticated() const { return authenticated; }
    bool IsFpsReceived() const { return fpsReceived; }
    int GetCurrentFps() const { return currentFps; }

    bool NeedsKey() {
        return needsKeyframe.exchange(false);
    }

    /**
     * @brief Sends an encoded video frame over the data channel
     * @param frame Encoded frame to send
     */
    void Send(const EncodedFrame& frame) {
        if (!connected || !authenticated) return;

        auto channel = dataChannel;
        if (!channel || !channel->isOpen()) {
            if (connected) {
                ForceDisconnect("Channel closed");
            }
            return;
        }

        if (IsConnectionStale()) {
            ForceDisconnect("Stale connection");
            return;
        }

        try {
            if (channel->bufferedAmount() > BUFFER_THRESHOLD) {
                overflowCount++;
                dropCount++;
                needsKeyframe = true;

                if (overflowCount >= 10) {
                    ForceDisconnect("Buffer overflow");
                }
                return;
            }

            overflowCount = 0;

            size_t dataSize = frame.data.size();
            size_t numChunks = (dataSize + DATA_CHUNK_SIZE - 1) / DATA_CHUNK_SIZE;

            if (numChunks > 65535 || !dataSize) return;

            PacketHeader header = {
                frame.ts,
                static_cast<uint32_t>(frame.encUs),
                frameId++,
                0,
                static_cast<uint16_t>(numChunks),
                frame.isKey ? uint8_t(1) : uint8_t(0)
            };

            size_t bytesSent = 0;

            for (size_t i = 0; i < numChunks; i++) {
                if (i > 0 && (i % 16) == 0 && channel->bufferedAmount() > BUFFER_THRESHOLD * 2) {
                    overflowCount++;
                    dropCount++;
                    needsKeyframe = true;
                    break;
                }

                header.chunkIndex = static_cast<uint16_t>(i);
                memcpy(packetBuffer.data(), &header, HEADER_SIZE);

                size_t offset = i * DATA_CHUNK_SIZE;
                size_t chunkLen = std::min(DATA_CHUNK_SIZE, dataSize - offset);
                memcpy(packetBuffer.data() + HEADER_SIZE, frame.data.data() + offset, chunkLen);

                if (!SafeSend(packetBuffer.data(), HEADER_SIZE + chunkLen)) {
                    overflowCount++;
                    dropCount++;
                    needsKeyframe = true;
                    break;
                }

                bytesSent += HEADER_SIZE + chunkLen;
            }

            if (bytesSent) {
                byteCount += bytesSent;
                sentCount++;
            }
        } catch (...) {
            dropCount++;
            needsKeyframe = true;
            overflowCount++;
        }
    }

    /**
     * @brief Sends an audio packet over the data channel
     * @param data Encoded audio data
     * @param timestamp Audio timestamp
     * @param samples Number of samples
     */
    void SendAudio(const std::vector<uint8_t>& data, int64_t timestamp, int samples) {
        if (!connected || !authenticated || data.empty() || data.size() > 4000 || overflowCount >= 5) {
            return;
        }

        auto channel = dataChannel;
        if (!channel || !channel->isOpen() || channel->bufferedAmount() > BUFFER_THRESHOLD / 2) {
            return;
        }

        try {
            size_t totalLength = sizeof(AudioPacketHeader) + data.size();

            if (audioBuffer.size() < totalLength) {
                audioBuffer.resize(totalLength);
            }

            auto* header = reinterpret_cast<AudioPacketHeader*>(audioBuffer.data());
            header->magic = MSG_AUDIO_DATA;
            header->timestamp = timestamp;
            header->samples = static_cast<uint16_t>(samples);
            header->dataLength = static_cast<uint16_t>(data.size());

            memcpy(audioBuffer.data() + sizeof(AudioPacketHeader), data.data(), data.size());

            if (SafeSend(audioBuffer.data(), totalLength)) {
                byteCount += totalLength;
                audioSentCount++;
            }
        } catch (...) {}
    }

    struct Stats {
        uint64_t sent;
        uint64_t bytes;
        uint64_t dropped;
        bool connected;
    };

    Stats GetStats() {
        return {
            sentCount.exchange(0),
            byteCount.exchange(0),
            dropCount.exchange(0),
            connected.load()
        };
    }

    uint64_t GetAudioSent() {
        return audioSentCount.exchange(0);
    }
};
