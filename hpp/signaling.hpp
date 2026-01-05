#pragma once
#include "common.hpp"
#include <random>
#include <fstream>

class SignalingClient {
    std::string workerUrl;
    std::string hostId;
    std::atomic<bool> running{true};
    std::thread pollThread;
    int lastIceIndex = 0;
    std::string currentSessionId;

    std::function<void(const json&, const std::vector<json>&, const std::string&)> onOffer;
    std::function<void(const std::vector<json>&)> onClientIce;

public:
    SignalingClient(const std::string& url, const std::string& id) : workerUrl(url), hostId(id) {
        LOG("Signaling client: Host ID = %s", hostId.c_str());
    }

    ~SignalingClient() {
        running = false;
        if (pollThread.joinable()) pollThread.join();
    }

    void SetOnOffer(std::function<void(const json&, const std::vector<json>&, const std::string&)> cb) {
        onOffer = cb;
    }

    void SetOnClientIce(std::function<void(const std::vector<json>&)> cb) {
        onClientIce = cb;
    }

    std::string GetHostId() const { return hostId; }
    std::string GetWorkerUrl() const { return workerUrl; }

    void Start() {
        pollThread = std::thread([this] {
            // Use httplib::Client with full URL (supports https:// when built with OpenSSL)
            httplib::Client cli(workerUrl);
            cli.set_connection_timeout(10);
            cli.set_read_timeout(15);
            cli.enable_server_certificate_verification(true);

            LOG("Polling signaling server: %s", workerUrl.c_str());

            while (running) {
                try {
                    std::string path = "/api/host/" + hostId + "/poll";
                    if (lastIceIndex > 0) {
                        path += "?lastIce=" + std::to_string(lastIceIndex);
                    }

                    auto res = cli.Get(path);

                    if (res && res->status == 200) {
                        auto j = json::parse(res->body);
                        std::string status = j.value("status", "");

                        if (status == "offer" && onOffer) {
                            LOG("Received offer from client");
                            auto offer = j["offer"];
                            std::vector<json> ice;
                            if (j.contains("clientIce")) {
                                ice = j["clientIce"].get<std::vector<json>>();
                            }
                            currentSessionId = j.value("sessionId", "");
                            lastIceIndex = (int)ice.size();
                            onOffer(offer, ice, currentSessionId);
                        }
                        else if (status == "ice" && onClientIce) {
                            std::vector<json> ice = j["clientIce"].get<std::vector<json>>();
                            lastIceIndex = j.value("iceIndex", lastIceIndex);
                            if (!ice.empty()) {
                                LOG("Received %zu new ICE candidates from client", ice.size());
                                onClientIce(ice);
                            }
                        }
                    } else if (res) {
                        if (res->status != 200) {
                            // Log non-200 responses occasionally
                        }
                    } else {
                        WARN("Signaling poll failed - no response");
                    }
                } catch (const std::exception& e) {
                    WARN("Signaling poll error: %s", e.what());
                }

                std::this_thread::sleep_for(1s);
            }
        });
    }

    bool SendAnswer(const std::string& sdp, const std::vector<json>& ice = {}) {
        try {
            httplib::Client cli(workerUrl);
            cli.set_connection_timeout(10);
            cli.set_read_timeout(15);
            cli.enable_server_certificate_verification(true);

            json body = {
                {"answer", {{"sdp", sdp}, {"type", "answer"}}},
                {"ice", ice}
            };

            auto res = cli.Post(
                ("/api/host/" + hostId + "/answer"),
                body.dump(),
                "application/json"
            );

            if (res && res->status == 200) {
                LOG("Sent answer to signaling server");
                return true;
            }
            WARN("Failed to send answer: %s", res ? std::to_string(res->status).c_str() : "no response");
            return false;
        } catch (const std::exception& e) {
            ERR("SendAnswer error: %s", e.what());
            return false;
        }
    }

    bool SendIce(const std::vector<json>& ice) {
        if (ice.empty()) return true;
        try {
            httplib::Client cli(workerUrl);
            cli.set_connection_timeout(5);
            cli.enable_server_certificate_verification(true);

            auto res = cli.Post(
                ("/api/host/" + hostId + "/ice"),
                json{{"ice", ice}}.dump(),
                "application/json"
            );
            return res && res->status == 200;
        } catch (...) {
            return false;
        }
    }

    void ResetSession() {
        lastIceIndex = 0;
        currentSessionId.clear();
    }
};
