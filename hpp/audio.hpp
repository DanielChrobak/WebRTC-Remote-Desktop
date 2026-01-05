#pragma once
#include "common.hpp"
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>
#include <queue>

extern "C" {
#include <opus/opus.h>
}

struct AudioPacket {
    std::vector<uint8_t> data;
    int64_t ts = 0;
    int samples = 0;
    AudioPacket() { data.reserve(4096); }
};

class AudioCapture {
    IMMDeviceEnumerator* en = nullptr;
    IMMDevice* dv = nullptr;
    IAudioClient* ac = nullptr;
    IAudioCaptureClient* cc = nullptr;
    OpusEncoder* oe = nullptr;
    WAVEFORMATEX* wf = nullptr;
    int sr = 48000, ch = 2, fsm = 20, fs = 0, opusRate = 48000;
    std::atomic<bool> run{false}, cap{false};
    std::thread ct;
    std::queue<AudioPacket> pq;
    std::mutex qm;
    std::condition_variable qcv;
    std::atomic<uint64_t> pc{0}, dc{0};
    std::vector<float> rb;
    std::vector<int16_t> eb;
    std::vector<uint8_t> ob;

    void Loop() {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
        if (FAILED(CoInitializeEx(0, COINIT_MULTITHREADED))) { ERR("Audio: CoInitializeEx failed"); return; }
        while (run) {
            if (!cap) { std::this_thread::sleep_for(10ms); continue; }
            UINT32 pl = 0;
            if (FAILED(cc->GetNextPacketSize(&pl))) { WARN("Audio: GetNextPacketSize failed"); std::this_thread::sleep_for(100ms); continue; }
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
        const float* fd = (const float*)d;
        const size_t newSamples = nf * ch, oldSize = rb.size();
        rb.resize(oldSize + newSamples);
        memcpy(rb.data() + oldSize, fd, newSamples * sizeof(float));

        const int opusFrameSamples = opusRate * fsm / 1000;
        size_t consumed = 0;

        while (rb.size() - consumed >= (size_t)(fs * ch)) {
            const float* src = rb.data() + consumed;
            if (sr != opusRate) {
                const double ratio = (double)sr / opusRate;
                for (int i = 0; i < opusFrameSamples * ch; i++) {
                    const double srcIdx = (i / ch) * ratio * ch + (i % ch);
                    const int idx0 = (int)srcIdx, idx1 = std::min(idx0 + ch, fs * ch - 1);
                    eb[i] = (int16_t)(std::clamp(src[idx0] * (1.0f - (float)(srcIdx - idx0)) + src[idx1] * (float)(srcIdx - idx0), -1.f, 1.f) * 32767.f);
                }
            } else {
                for (int i = 0; i < fs * ch; i++) eb[i] = (int16_t)(std::clamp(src[i], -1.f, 1.f) * 32767.f);
            }
            consumed += fs * ch;

            int nb = opus_encode(oe, eb.data(), opusFrameSamples, ob.data(), (opus_int32)ob.size());
            if (nb > 0) {
                AudioPacket p;
                p.data.assign(ob.begin(), ob.begin() + nb);
                p.ts = ts; p.samples = opusFrameSamples;
                std::lock_guard<std::mutex> lk(qm);
                if (pq.size() < 50) { pq.push(std::move(p)); pc++; } else dc++;
                qcv.notify_one();
            }
        }

        if (consumed > 0) {
            size_t remaining = rb.size() - consumed;
            if (remaining > 0) memmove(rb.data(), rb.data() + consumed, remaining * sizeof(float));
            rb.resize(remaining);
        }
    }

public:
    AudioCapture() {
        LOG("Initializing audio capture...");
        bool ci = SUCCEEDED(CoInitializeEx(0, COINIT_MULTITHREADED));
        if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), 0, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void**)&en)))
            throw std::runtime_error("Failed to create MMDeviceEnumerator");
        if (FAILED(en->GetDefaultAudioEndpoint(eRender, eConsole, &dv)))
            throw std::runtime_error("Failed to get default audio endpoint");

        IPropertyStore* ps = nullptr;
        if (SUCCEEDED(dv->OpenPropertyStore(STGM_READ, &ps))) {
            PROPVARIANT vn; PropVariantInit(&vn);
            if (SUCCEEDED(ps->GetValue(PKEY_Device_FriendlyName, &vn))) {
                char nm[256]; WideCharToMultiByte(CP_UTF8, 0, vn.pwszVal, -1, nm, sizeof(nm), 0, 0);
                LOG("Audio device: %s", nm); PropVariantClear(&vn);
            }
            ps->Release();
        }

        if (FAILED(dv->Activate(__uuidof(IAudioClient), CLSCTX_ALL, 0, (void**)&ac)))
            throw std::runtime_error("Failed to activate audio client");
        if (FAILED(ac->GetMixFormat(&wf))) throw std::runtime_error("Failed to get mix format");
        sr = wf->nSamplesPerSec; ch = std::min((int)wf->nChannels, 2); fs = sr * fsm / 1000;
        LOG("Audio format: %dHz, %d channels, %d bits", sr, ch, wf->wBitsPerSample);

        if (FAILED(ac->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK, 200000, 0, wf, 0)))
            throw std::runtime_error("Failed to initialize audio client");
        if (FAILED(ac->GetService(__uuidof(IAudioCaptureClient), (void**)&cc)))
            throw std::runtime_error("Failed to get capture client");

        opusRate = (sr == 8000 || sr == 12000 || sr == 16000 || sr == 24000 || sr == 48000) ? sr : 48000;

        int oe_err;
        oe = opus_encoder_create(opusRate, ch, OPUS_APPLICATION_RESTRICTED_LOWDELAY, &oe_err);
        if (oe_err != OPUS_OK) throw std::runtime_error("Failed to create Opus encoder");
        opus_encoder_ctl(oe, OPUS_SET_BITRATE(128000));
        opus_encoder_ctl(oe, OPUS_SET_COMPLEXITY(5));
        opus_encoder_ctl(oe, OPUS_SET_SIGNAL(OPUS_SIGNAL_MUSIC));

        if (sr != opusRate) WARN("Audio sample rate %d will be resampled to %dHz for Opus", sr, opusRate);

        eb.resize(opusRate * fsm / 1000 * ch);
        ob.resize(4000);
        rb.reserve(fs * ch * 8);

        LOG("Audio capture initialized (Opus %dHz, frame size: %d samples)", opusRate, opusRate * fsm / 1000);
        if (ci) CoUninitialize();
    }

    ~AudioCapture() {
        Stop();
        if (oe) opus_encoder_destroy(oe);
        if (wf) CoTaskMemFree(wf);
        if (cc) cc->Release(); if (ac) ac->Release();
        if (dv) dv->Release(); if (en) en->Release();
    }

    void Start() {
        if (run) return;
        run = cap = true;
        if (FAILED(ac->Start())) { ERR("Failed to start audio client"); run = cap = false; return; }
        ct = std::thread(&AudioCapture::Loop, this);
        LOG("Audio capture started");
    }

    void Stop() {
        if (!run) return;
        run = cap = false; qcv.notify_all();
        if (ct.joinable()) ct.join();
        if (ac) ac->Stop();
        LOG("Audio capture stopped");
    }

    bool PopPacket(AudioPacket& o, int ms = 10) {
        std::unique_lock<std::mutex> lk(qm);
        if (!qcv.wait_for(lk, std::chrono::milliseconds(ms), [this] { return !pq.empty() || !run; })) return false;
        if (pq.empty()) return false;
        o = std::move(pq.front()); pq.pop(); return true;
    }

    bool IsCapturing() const { return cap.load(); }
    int GetSampleRate() const { return opusRate; }
    int GetChannels() const { return ch; }
};
