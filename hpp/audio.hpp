#pragma once
#include "common.hpp"
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>

extern "C" {
#include <opus/opus.h>
}

struct AudioPacket { std::vector<uint8_t> data; int64_t ts = 0; int samples = 0; };

class AudioCapture {
    IMMDeviceEnumerator* en = nullptr; IMMDevice* dv = nullptr;
    IAudioClient* ac = nullptr; IAudioCaptureClient* cc = nullptr;
    OpusEncoder* oe = nullptr; WAVEFORMATEX* wf = nullptr;
    int sr = 48000, ch = 2, fsm = 20, fs = 0, opusRate = 48000;
    std::atomic<bool> run{false}, cap{false};
    std::thread ct;
    std::queue<AudioPacket> pq; std::mutex qm; std::condition_variable qcv;
    std::vector<float> rb; std::vector<int16_t> eb; std::vector<uint8_t> ob;

    void Loop() {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
        CoInitializeEx(0, COINIT_MULTITHREADED);
        while (run) {
            if (!cap) { std::this_thread::sleep_for(10ms); continue; }
            UINT32 pl = 0;
            if (FAILED(cc->GetNextPacketSize(&pl))) { std::this_thread::sleep_for(100ms); continue; }
            while (pl > 0 && run && cap) {
                BYTE* d = nullptr; UINT32 nf = 0; DWORD fl = 0; UINT64 dp = 0, qp = 0;
                if (FAILED(cc->GetBuffer(&d, &nf, &fl, &dp, &qp))) break;
                if (!(fl & AUDCLNT_BUFFERFLAGS_SILENT) && d && nf > 0) Process(d, nf, GetTimestamp());
                cc->ReleaseBuffer(nf);
                if (FAILED(cc->GetNextPacketSize(&pl))) break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(fsm / 2));
        }
        CoUninitialize();
    }

    void Process(BYTE* d, UINT32 nf, int64_t ts) {
        rb.insert(rb.end(), (float*)d, (float*)d + nf * ch);
        const int opusFrameSamples = opusRate * fsm / 1000;
        size_t consumed = 0;
        while (rb.size() - consumed >= (size_t)(fs * ch)) {
            const float* src = rb.data() + consumed;
            if (sr != opusRate) {
                const double ratio = (double)sr / opusRate;
                for (int i = 0; i < opusFrameSamples * ch; i++) {
                    double srcIdx = (i / ch) * ratio * ch + (i % ch); int idx0 = (int)srcIdx;
                    eb[i] = (int16_t)(std::clamp(src[idx0] * (1.f - (float)(srcIdx - idx0)) + src[std::min(idx0 + ch, fs * ch - 1)] * (float)(srcIdx - idx0), -1.f, 1.f) * 32767.f);
                }
            } else for (int i = 0; i < fs * ch; i++) eb[i] = (int16_t)(std::clamp(src[i], -1.f, 1.f) * 32767.f);
            consumed += fs * ch;
            int nb = opus_encode(oe, eb.data(), opusFrameSamples, ob.data(), (opus_int32)ob.size());
            if (nb > 0) { std::lock_guard<std::mutex> lk(qm); if (pq.size() < 50) pq.push({std::vector<uint8_t>(ob.begin(), ob.begin() + nb), ts, opusFrameSamples}); qcv.notify_one(); }
        }
        if (consumed > 0) rb.erase(rb.begin(), rb.begin() + consumed);
    }

public:
    AudioCapture() {
        LOG("Initializing audio capture...");
        CoInitializeEx(0, COINIT_MULTITHREADED);
        if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), 0, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void**)&en)) ||
            FAILED(en->GetDefaultAudioEndpoint(eRender, eConsole, &dv)) || FAILED(dv->Activate(__uuidof(IAudioClient), CLSCTX_ALL, 0, (void**)&ac)) || FAILED(ac->GetMixFormat(&wf)))
            throw std::runtime_error("Audio init failed");
        sr = wf->nSamplesPerSec; ch = std::min((int)wf->nChannels, 2); fs = sr * fsm / 1000;
        LOG("Audio: %dHz, %d ch", sr, ch);
        if (FAILED(ac->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK, 200000, 0, wf, 0)) || FAILED(ac->GetService(__uuidof(IAudioCaptureClient), (void**)&cc)))
            throw std::runtime_error("Audio client init failed");
        opusRate = (sr == 8000 || sr == 12000 || sr == 16000 || sr == 24000 || sr == 48000) ? sr : 48000;
        int err; oe = opus_encoder_create(opusRate, ch, OPUS_APPLICATION_RESTRICTED_LOWDELAY, &err);
        if (err != OPUS_OK) throw std::runtime_error("Opus encoder failed");
        opus_encoder_ctl(oe, OPUS_SET_BITRATE(128000)); opus_encoder_ctl(oe, OPUS_SET_COMPLEXITY(5)); opus_encoder_ctl(oe, OPUS_SET_SIGNAL(OPUS_SIGNAL_MUSIC));
        eb.resize(opusRate * fsm / 1000 * ch); ob.resize(4000); rb.reserve(fs * ch * 8);
        LOG("Audio initialized (Opus %dHz)", opusRate);
        CoUninitialize();
    }

    ~AudioCapture() { Stop(); if (oe) opus_encoder_destroy(oe); if (wf) CoTaskMemFree(wf); if (cc) cc->Release(); if (ac) ac->Release(); if (dv) dv->Release(); if (en) en->Release(); }
    void Start() { if (run) return; run = cap = true; if (FAILED(ac->Start())) { run = cap = false; return; } ct = std::thread(&AudioCapture::Loop, this); LOG("Audio started"); }
    void Stop() { if (!run) return; run = cap = false; qcv.notify_all(); if (ct.joinable()) ct.join(); if (ac) ac->Stop(); }
    bool PopPacket(AudioPacket& o, int ms = 10) { std::unique_lock<std::mutex> lk(qm); if (!qcv.wait_for(lk, std::chrono::milliseconds(ms), [this] { return !pq.empty() || !run; }) || pq.empty()) return false; o = std::move(pq.front()); pq.pop(); return true; }
    int GetSampleRate() const { return opusRate; }
    int GetChannels() const { return ch; }
};
