#pragma once
#include "common.hpp"

struct EncodedFrame { std::vector<uint8_t> data; int64_t ts = 0, encUs = 0; bool isKey = false; void Clear() { data.clear(); ts = encUs = 0; isKey = false; } };

class AV1Encoder {
    AVCodecContext* cc = nullptr; AVFrame* hf = nullptr; AVPacket* pk = nullptr;
    AVBufferRef* hd = nullptr; AVBufferRef* hfr = nullptr;
    ID3D11Device* dev = nullptr; ID3D11DeviceContext* ctx = nullptr; ID3D11Multithread* mt = nullptr;
    ID3D11Texture2D* stg = nullptr; ID3D11Query* encQuery = nullptr;
    int w, h, fn = 0; UINT stgW = 0, stgH = 0; bool hw = false;
    steady_clock::time_point lk;
    static constexpr auto KI = 2000ms;
    EncodedFrame out[2]; int oi = 0;
    std::atomic<uint64_t> ec{0}, fc{0};
    static inline const int64_t qf = []{ LARGE_INTEGER f; QueryPerformanceFrequency(&f); return f.QuadPart; }();

    bool InitHW(const AVCodec* c) {
        if (strcmp(c->name, "av1_nvenc") && strcmp(c->name, "av1_qsv")) return false;
        if (!(hd = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_D3D11VA))) return false;
        auto* d = (AVD3D11VADeviceContext*)((AVHWDeviceContext*)hd->data)->hwctx; d->device = dev; d->device_context = ctx;
        if (av_hwdevice_ctx_init(hd) < 0) { av_buffer_unref(&hd); return false; }
        cc->hw_device_ctx = av_buffer_ref(hd);
        if (!(hfr = av_hwframe_ctx_alloc(hd))) { av_buffer_unref(&hd); return false; }
        auto* fctx = (AVHWFramesContext*)hfr->data; fctx->format = AV_PIX_FMT_D3D11; fctx->sw_format = AV_PIX_FMT_BGRA; fctx->width = w; fctx->height = h; fctx->initial_pool_size = 4;
        if (av_hwframe_ctx_init(hfr) < 0) { av_buffer_unref(&hfr); av_buffer_unref(&hd); return false; }
        cc->hw_frames_ctx = av_buffer_ref(hfr); cc->pix_fmt = AV_PIX_FMT_D3D11; return true;
    }

    bool WaitGPU(DWORD ms = 16) {
        if (!encQuery) return true;
        ctx->End(encQuery);
        LARGE_INTEGER start, now, freq; QueryPerformanceCounter(&start); QueryPerformanceFrequency(&freq);
        int64_t timeout = (freq.QuadPart * ms) / 1000;
        while (ctx->GetData(encQuery, nullptr, 0, 0) == S_FALSE) { QueryPerformanceCounter(&now); if ((now.QuadPart - start.QuadPart) > timeout) return false; YieldProcessor(); }
        return true;
    }

public:
    AV1Encoder(int W, int H, int fps, ID3D11Device* d, ID3D11DeviceContext* c, ID3D11Multithread* m) : w(W), h(H), dev(d), ctx(c), mt(m) {
        dev->AddRef(); if (ctx) ctx->AddRef(); else dev->GetImmediateContext(&ctx); if (mt) mt->AddRef(); lk = steady_clock::now() - KI;
        D3D11_QUERY_DESC qd = {D3D11_QUERY_EVENT, 0}; dev->CreateQuery(&qd, &encQuery);
        const AVCodec* cd = nullptr;
        for (auto n : {"av1_nvenc", "av1_qsv", "av1_amf", "libsvtav1", "libaom-av1"}) if ((cd = avcodec_find_encoder_by_name(n))) { LOG("Encoder: %s", n); break; }
        if (!cd) throw std::runtime_error("No AV1 encoder");
        if (!(cc = avcodec_alloc_context3(cd))) throw std::runtime_error("avcodec_alloc_context3 failed");
        hw = strcmp(cd->name, "libaom-av1") && strcmp(cd->name, "libsvtav1") && InitHW(cd);
        if (!hw) cc->pix_fmt = AV_PIX_FMT_BGRA;
        cc->width = w; cc->height = h; cc->time_base = {1, fps}; cc->framerate = {fps, 1};
        cc->bit_rate = 20000000; cc->rc_max_rate = cc->rc_buffer_size = 40000000; cc->gop_size = fps * 2; cc->max_b_frames = 0;
        cc->flags |= AV_CODEC_FLAG_LOW_DELAY; cc->flags2 |= AV_CODEC_FLAG2_FAST; cc->delay = cc->has_b_frames = 0;
        cc->thread_count = hw ? 1 : std::min(4, std::max(1, (int)std::thread::hardware_concurrency() / 2));

        auto set = [&](const char* k, const char* v) { av_opt_set(cc->priv_data, k, v, 0); };
        if (!strcmp(cd->name, "av1_nvenc")) { set("preset","p1"); set("tune","ull"); set("rc","cbr"); set("cq","23"); set("delay","0"); set("zerolatency","1"); set("lookahead","0"); set("rc-lookahead","0"); set("forced-idr","1"); set("b_adapt","0"); set("spatial-aq","0"); set("temporal-aq","0"); set("nonref_p","1"); set("strict_gop","1"); set("multipass","disabled"); set("ldkfs","1"); set("surfaces","8"); set("aud","0"); set("bluray-compat","0"); }
        else if (!strcmp(cd->name, "av1_qsv")) { set("preset","veryfast"); set("async_depth","1"); set("look_ahead","0"); set("look_ahead_depth","0"); set("forced_idr","1"); set("low_power","1"); set("low_delay_brc","1"); set("max_frame_size","0"); set("b_strategy","0"); set("extbrc","0"); set("global_quality","23"); }
        else if (!strcmp(cd->name, "av1_amf")) { set("usage","ultralowlatency"); set("quality","speed"); set("rc","vbr_latency"); set("qp_i","23"); set("qp_p","25"); set("preanalysis","0"); set("enforce_hrd","0"); set("filler_data","0"); set("frame_skipping","0"); set("header_insertion_mode","idr"); }
        else if (!strcmp(cd->name, "libsvtav1")) { set("preset","12"); set("crf","28"); set("svtav1-params","tune=0:fast-decode=1:enable-overlays=0:scd=0:lookahead=0:lp=1:tile-rows=0:tile-columns=1:enable-tf=0:enable-cdef=0:enable-restoration=0:rmv=0:film-grain=0"); }
        else { set("cpu-used","10"); set("usage","realtime"); set("crf","28"); set("lag-in-frames","0"); set("row-mt","1"); set("tile-columns","2"); set("tile-rows","0"); set("error-resilient","1"); set("frame-parallel","1"); set("aq-mode","0"); set("tune","ssim"); set("enable-cdef","0"); set("enable-global-motion","0"); set("deltaq-mode","0"); set("enable-ref-frame-mvs","0"); set("reduced-reference-set","1"); }

        if (avcodec_open2(cc, cd, 0) < 0) throw std::runtime_error("avcodec_open2 failed");
        hf = av_frame_alloc(); pk = av_packet_alloc();
        if (!hf || !pk) throw std::runtime_error("Alloc failed");
        hf->format = cc->pix_fmt; hf->width = w; hf->height = h;
        if (!hw && av_frame_get_buffer(hf, 32) < 0) throw std::runtime_error("av_frame_get_buffer failed");
        LOG("Encoder: %s", hw ? "Hardware" : "Software");
    }

    ~AV1Encoder() { av_packet_free(&pk); av_frame_free(&hf); av_buffer_unref(&hfr); av_buffer_unref(&hd); if (cc) avcodec_free_context(&cc); SafeRelease(encQuery, stg, mt, ctx, dev); }
    void Flush() { avcodec_send_frame(cc, 0); while (avcodec_receive_packet(cc, pk) == 0) av_packet_unref(pk); avcodec_flush_buffers(cc); lk = steady_clock::now() - KI; }

    EncodedFrame* Encode(ID3D11Texture2D* tx, int64_t ts, bool fk = false) {
        LARGE_INTEGER t1, t2; QueryPerformanceCounter(&t1);
        EncodedFrame* o = &out[oi++ % 2]; o->Clear();
        bool nk = fk || (steady_clock::now() - lk >= KI);

        if (hw) {
            if (av_hwframe_get_buffer(cc->hw_frames_ctx, hf, 0) < 0) { fc++; return nullptr; }
            MTLock l(mt); ctx->CopySubresourceRegion((ID3D11Texture2D*)hf->data[0], (UINT)(intptr_t)hf->data[1], 0, 0, 0, tx, 0, 0);
            if (!WaitGPU(16)) { fc++; av_frame_unref(hf); return nullptr; }
        } else {
            D3D11_TEXTURE2D_DESC td; tx->GetDesc(&td);
            if (!stg || stgW != td.Width || stgH != td.Height) { SafeRelease(stg); td.Usage = D3D11_USAGE_STAGING; td.BindFlags = 0; td.CPUAccessFlags = D3D11_CPU_ACCESS_READ; td.MiscFlags = 0; dev->CreateTexture2D(&td, 0, &stg); stgW = td.Width; stgH = td.Height; }
            { MTLock l(mt); ctx->CopyResource(stg, tx); ctx->Flush(); }
            D3D11_MAPPED_SUBRESOURCE mp; { MTLock l(mt); if (FAILED(ctx->Map(stg, 0, D3D11_MAP_READ, 0, &mp))) { fc++; return nullptr; } }
            if (av_frame_make_writable(hf) < 0) { MTLock l(mt); ctx->Unmap(stg, 0); fc++; return nullptr; }
            auto* sr = (uint8_t*)mp.pData; for (int y = 0; y < h; y++) memcpy(hf->data[0] + y * hf->linesize[0], sr + y * mp.RowPitch, w * 4);
            MTLock l(mt); ctx->Unmap(stg, 0);
        }

        hf->pts = fn++; hf->pict_type = nk ? AV_PICTURE_TYPE_I : AV_PICTURE_TYPE_NONE;
        hf->flags = nk ? (hf->flags | AV_FRAME_FLAG_KEY) : (hf->flags & ~AV_FRAME_FLAG_KEY);
        if (nk) lk = steady_clock::now();

        int ret = avcodec_send_frame(cc, hf);
        if (ret == AVERROR(EAGAIN)) { bool gk = false; while (avcodec_receive_packet(cc, pk) == 0) { if (pk->flags & AV_PKT_FLAG_KEY) gk = true; o->data.insert(o->data.end(), pk->data, pk->data + pk->size); av_packet_unref(pk); } ret = avcodec_send_frame(cc, hf); }
        if (ret < 0 && ret != AVERROR_EOF) { fc++; if (hw) av_frame_unref(hf); return nullptr; }

        bool gk = false;
        while (avcodec_receive_packet(cc, pk) == 0) { if (pk->flags & AV_PKT_FLAG_KEY) gk = true; o->data.insert(o->data.end(), pk->data, pk->data + pk->size); av_packet_unref(pk); }
        if (hw) av_frame_unref(hf);
        if (o->data.empty()) return nullptr;

        QueryPerformanceCounter(&t2);
        o->ts = ts; o->encUs = ((t2.QuadPart - t1.QuadPart) * 1000000) / qf; o->isKey = gk; ec++; return o;
    }

    uint64_t GetEncoded() { return ec.exchange(0); }
};
