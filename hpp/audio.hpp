/**
 * @file audio.hpp
 * @brief System audio capture and Opus encoding
 * @copyright 2025-2026 Daniel Chrobak
 */

#pragma once
#include "common.hpp"
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>

extern "C" {
#include <opus/opus.h>
}

/**
 * @brief Container for encoded audio packet data
 */
struct AudioPacket {
    std::vector<uint8_t> data;
    int64_t ts = 0;
    int samples = 0;
};

/**
 * @brief System audio capture using WASAPI loopback with Opus encoding
 *
 * Captures system audio output via WASAPI loopback mode and encodes it
 * using the Opus codec for low-latency streaming.
 */
class AudioCapture {
private:
    IMMDeviceEnumerator* enumerator = nullptr;
    IMMDevice* device = nullptr;
    IAudioClient* audioClient = nullptr;
    IAudioCaptureClient* captureClient = nullptr;
    OpusEncoder* opusEncoder = nullptr;
    WAVEFORMATEX* waveFormat = nullptr;

    int sampleRate = 48000;
    int channels = 2;
    int frameDurationMs = 20;
    int frameSamples = 0;
    int opusSampleRate = 48000;

    std::atomic<bool> running{false};
    std::atomic<bool> capturing{false};
    std::thread captureThread;

    std::queue<AudioPacket> packetQueue;
    std::mutex queueMutex;
    std::condition_variable queueCondition;

    std::vector<float> resampleBuffer;
    std::vector<int16_t> encodeBuffer;
    std::vector<uint8_t> outputBuffer;

    void CaptureLoop() {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);

        while (running) {
            if (!capturing) {
                std::this_thread::sleep_for(10ms);
                continue;
            }

            UINT32 packetLength = 0;
            if (FAILED(captureClient->GetNextPacketSize(&packetLength))) {
                std::this_thread::sleep_for(100ms);
                continue;
            }

            while (packetLength > 0 && running && capturing) {
                BYTE* data = nullptr;
                UINT32 numFrames = 0;
                DWORD flags = 0;
                UINT64 devicePosition = 0;
                UINT64 qpcPosition = 0;

                if (FAILED(captureClient->GetBuffer(&data, &numFrames, &flags, &devicePosition, &qpcPosition))) {
                    break;
                }

                if (!(flags & AUDCLNT_BUFFERFLAGS_SILENT) && data && numFrames > 0) {
                    ProcessAudio(data, numFrames, GetTimestamp());
                }

                captureClient->ReleaseBuffer(numFrames);

                if (FAILED(captureClient->GetNextPacketSize(&packetLength))) {
                    break;
                }
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(frameDurationMs / 2));
        }

        CoUninitialize();
    }

    void ProcessAudio(BYTE* data, UINT32 numFrames, int64_t timestamp) {
        auto* floatData = reinterpret_cast<float*>(data);
        resampleBuffer.insert(resampleBuffer.end(),
                              floatData,
                              floatData + numFrames * channels);

        const int opusFrameSamples = opusSampleRate * frameDurationMs / 1000;
        size_t consumed = 0;

        while (resampleBuffer.size() - consumed >= static_cast<size_t>(frameSamples * channels)) {
            const float* src = resampleBuffer.data() + consumed;

            if (sampleRate != opusSampleRate) {
                const double ratio = static_cast<double>(sampleRate) / opusSampleRate;

                for (int i = 0; i < opusFrameSamples * channels; i++) {
                    double srcIdx = (i / channels) * ratio * channels + (i % channels);
                    int idx0 = static_cast<int>(srcIdx);
                    float fraction = static_cast<float>(srcIdx - idx0);
                    int idx1 = std::min(idx0 + channels, frameSamples * channels - 1);

                    float sample = src[idx0] * (1.0f - fraction) + src[idx1] * fraction;
                    encodeBuffer[i] = static_cast<int16_t>(std::clamp(sample, -1.0f, 1.0f) * 32767.0f);
                }
            } else {
                for (int i = 0; i < frameSamples * channels; i++) {
                    encodeBuffer[i] = static_cast<int16_t>(std::clamp(src[i], -1.0f, 1.0f) * 32767.0f);
                }
            }

            consumed += frameSamples * channels;

            int encodedBytes = opus_encode(opusEncoder,
                                           encodeBuffer.data(),
                                           opusFrameSamples,
                                           outputBuffer.data(),
                                           static_cast<opus_int32>(outputBuffer.size()));

            if (encodedBytes > 0) {
                std::lock_guard<std::mutex> lock(queueMutex);
                if (packetQueue.size() < 50) {
                    packetQueue.push({
                        std::vector<uint8_t>(outputBuffer.begin(), outputBuffer.begin() + encodedBytes),
                        timestamp,
                        opusFrameSamples
                    });
                }
                queueCondition.notify_one();
            }
        }

        if (consumed > 0) {
            resampleBuffer.erase(resampleBuffer.begin(),
                                 resampleBuffer.begin() + consumed);
        }
    }

public:
    AudioCapture() {
        LOG("Initializing audio capture...");

        CoInitializeEx(nullptr, COINIT_MULTITHREADED);

        if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator),
                                    nullptr,
                                    CLSCTX_ALL,
                                    __uuidof(IMMDeviceEnumerator),
                                    reinterpret_cast<void**>(&enumerator)))) {
            throw std::runtime_error("Failed to create device enumerator");
        }

        if (FAILED(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device))) {
            throw std::runtime_error("Failed to get default audio endpoint");
        }

        if (FAILED(device->Activate(__uuidof(IAudioClient),
                                    CLSCTX_ALL,
                                    nullptr,
                                    reinterpret_cast<void**>(&audioClient)))) {
            throw std::runtime_error("Failed to activate audio client");
        }

        if (FAILED(audioClient->GetMixFormat(&waveFormat))) {
            throw std::runtime_error("Failed to get mix format");
        }

        sampleRate = waveFormat->nSamplesPerSec;
        channels = std::min(static_cast<int>(waveFormat->nChannels), 2);
        frameSamples = sampleRate * frameDurationMs / 1000;

        LOG("Audio: %dHz, %d channels", sampleRate, channels);

        if (FAILED(audioClient->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                           AUDCLNT_STREAMFLAGS_LOOPBACK,
                                           200000,
                                           0,
                                           waveFormat,
                                           nullptr))) {
            throw std::runtime_error("Failed to initialize audio client");
        }

        if (FAILED(audioClient->GetService(__uuidof(IAudioCaptureClient),
                                           reinterpret_cast<void**>(&captureClient)))) {
            throw std::runtime_error("Failed to get capture client");
        }

        opusSampleRate = (sampleRate == 8000 || sampleRate == 12000 ||
                          sampleRate == 16000 || sampleRate == 24000 ||
                          sampleRate == 48000) ? sampleRate : 48000;

        int opusError;
        opusEncoder = opus_encoder_create(opusSampleRate,
                                          channels,
                                          OPUS_APPLICATION_RESTRICTED_LOWDELAY,
                                          &opusError);

        if (opusError != OPUS_OK) {
            throw std::runtime_error("Failed to create Opus encoder");
        }

        opus_encoder_ctl(opusEncoder, OPUS_SET_BITRATE(128000));
        opus_encoder_ctl(opusEncoder, OPUS_SET_COMPLEXITY(5));
        opus_encoder_ctl(opusEncoder, OPUS_SET_SIGNAL(OPUS_SIGNAL_MUSIC));

        encodeBuffer.resize(opusSampleRate * frameDurationMs / 1000 * channels);
        outputBuffer.resize(4000);
        resampleBuffer.reserve(frameSamples * channels * 8);

        LOG("Audio initialized (Opus %dHz)", opusSampleRate);
        CoUninitialize();
    }

    ~AudioCapture() {
        Stop();

        if (opusEncoder) {
            opus_encoder_destroy(opusEncoder);
        }
        if (waveFormat) {
            CoTaskMemFree(waveFormat);
        }
        if (captureClient) {
            captureClient->Release();
        }
        if (audioClient) {
            audioClient->Release();
        }
        if (device) {
            device->Release();
        }
        if (enumerator) {
            enumerator->Release();
        }
    }

    void Start() {
        if (running) return;

        running = capturing = true;

        if (FAILED(audioClient->Start())) {
            running = capturing = false;
            return;
        }

        captureThread = std::thread(&AudioCapture::CaptureLoop, this);
        LOG("Audio capture started");
    }

    void Stop() {
        if (!running) return;

        running = capturing = false;
        queueCondition.notify_all();

        if (captureThread.joinable()) {
            captureThread.join();
        }

        if (audioClient) {
            audioClient->Stop();
        }
    }

    /**
     * @brief Retrieves the next encoded audio packet
     * @param out Output packet
     * @param timeoutMs Maximum wait time
     * @return True if a packet was retrieved
     */
    bool PopPacket(AudioPacket& out, int timeoutMs = 10) {
        std::unique_lock<std::mutex> lock(queueMutex);

        if (!queueCondition.wait_for(lock,
                                      std::chrono::milliseconds(timeoutMs),
                                      [this] { return !packetQueue.empty() || !running; })) {
            return false;
        }

        if (packetQueue.empty()) {
            return false;
        }

        out = std::move(packetQueue.front());
        packetQueue.pop();
        return true;
    }

    int GetSampleRate() const { return opusSampleRate; }
    int GetChannels() const { return channels; }
};
