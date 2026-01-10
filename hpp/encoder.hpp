/**
 * @file encoder.hpp
 * @brief AV1 video encoder with hardware acceleration support
 * @copyright 2025-2026 Daniel Chrobak
 */

#pragma once
#include "common.hpp"

struct EncodedFrame {
    std::vector<uint8_t> data;
    int64_t ts = 0, encUs = 0;
    bool isKey = false;
    void Clear() { data.clear(); ts = encUs = 0; isKey = false; }
};

class AV1Encoder {
private:
    AVCodecContext* codecContext = nullptr;
    AVFrame* hwFrame = nullptr;
    AVPacket* packet = nullptr;
    AVBufferRef* hwDevice = nullptr;
    AVBufferRef* hwFrameCtx = nullptr;

    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    ID3D11Multithread* multithread = nullptr;
    ID3D11Texture2D* stagingTexture = nullptr;
    ID3D11Query* encodeQuery = nullptr;

    int width, height, frameNumber = 0;
    UINT stagingWidth = 0, stagingHeight = 0;
    bool useHardware = false;
    steady_clock::time_point lastKeyframe;
    static constexpr auto KEYFRAME_INTERVAL = 2000ms;

    EncodedFrame outputFrames[2];
    int outputIndex = 0;
    std::atomic<uint64_t> encodedCount{0}, failedCount{0};

    static inline const int64_t queryFrequency = [] {
        LARGE_INTEGER f; QueryPerformanceFrequency(&f); return f.QuadPart;
    }();

    bool InitHardwareContext(const AVCodec* codec) {
        if (strcmp(codec->name, "av1_nvenc") && strcmp(codec->name, "av1_qsv")) return false;

        hwDevice = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_D3D11VA);
        if (!hwDevice) return false;

        auto* dc = reinterpret_cast<AVD3D11VADeviceContext*>(
            reinterpret_cast<AVHWDeviceContext*>(hwDevice->data)->hwctx);
        dc->device = device;
        dc->device_context = context;

        if (av_hwdevice_ctx_init(hwDevice) < 0) { av_buffer_unref(&hwDevice); return false; }

        codecContext->hw_device_ctx = av_buffer_ref(hwDevice);
        hwFrameCtx = av_hwframe_ctx_alloc(hwDevice);
        if (!hwFrameCtx) { av_buffer_unref(&hwDevice); return false; }

        auto* fc = reinterpret_cast<AVHWFramesContext*>(hwFrameCtx->data);
        fc->format = AV_PIX_FMT_D3D11; fc->sw_format = AV_PIX_FMT_BGRA;
        fc->width = width; fc->height = height; fc->initial_pool_size = 4;

        if (av_hwframe_ctx_init(hwFrameCtx) < 0) { av_buffer_unref(&hwFrameCtx); av_buffer_unref(&hwDevice); return false; }

        codecContext->hw_frames_ctx = av_buffer_ref(hwFrameCtx);
        codecContext->pix_fmt = AV_PIX_FMT_D3D11;
        return true;
    }

    bool WaitForGPU(DWORD timeoutMs = 16) {
        if (!encodeQuery) return true;
        context->End(encodeQuery);
        LARGE_INTEGER start, now, freq;
        QueryPerformanceCounter(&start); QueryPerformanceFrequency(&freq);
        int64_t timeout = (freq.QuadPart * timeoutMs) / 1000;
        while (context->GetData(encodeQuery, nullptr, 0, 0) == S_FALSE) {
            QueryPerformanceCounter(&now);
            if ((now.QuadPart - start.QuadPart) > timeout) return false;
            YieldProcessor();
        }
        return true;
    }

    void ConfigureEncoder(const AVCodec* codec) {
        auto set = [this](const char* k, const char* v) { av_opt_set(codecContext->priv_data, k, v, 0); };
        const char* name = codec->name;

        if (!strcmp(name, "av1_nvenc")) {
            for (auto& [k, v] : std::initializer_list<std::pair<const char*, const char*>>{
                {"preset", "p1"}, {"tune", "ull"}, {"rc", "cbr"}, {"cq", "23"}, {"delay", "0"},
                {"zerolatency", "1"}, {"lookahead", "0"}, {"rc-lookahead", "0"}, {"forced-idr", "1"},
                {"b_adapt", "0"}, {"spatial-aq", "0"}, {"temporal-aq", "0"}, {"nonref_p", "1"},
                {"strict_gop", "1"}, {"multipass", "disabled"}, {"ldkfs", "1"}, {"surfaces", "8"},
                {"aud", "0"}, {"bluray-compat", "0"}}) set(k, v);
        } else if (!strcmp(name, "av1_qsv")) {
            for (auto& [k, v] : std::initializer_list<std::pair<const char*, const char*>>{
                {"preset", "veryfast"}, {"async_depth", "1"}, {"look_ahead", "0"}, {"look_ahead_depth", "0"},
                {"forced_idr", "1"}, {"low_power", "1"}, {"low_delay_brc", "1"}, {"max_frame_size", "0"},
                {"b_strategy", "0"}, {"extbrc", "0"}, {"global_quality", "23"}}) set(k, v);
        } else if (!strcmp(name, "av1_amf")) {
            for (auto& [k, v] : std::initializer_list<std::pair<const char*, const char*>>{
                {"usage", "ultralowlatency"}, {"quality", "speed"}, {"rc", "vbr_latency"},
                {"qp_i", "23"}, {"qp_p", "25"}, {"preanalysis", "0"}, {"enforce_hrd", "0"},
                {"filler_data", "0"}, {"frame_skipping", "0"}, {"header_insertion_mode", "idr"}}) set(k, v);
        } else if (!strcmp(name, "libsvtav1")) {
            set("preset", "12"); set("crf", "28");
            set("svtav1-params", "tune=0:fast-decode=1:enable-overlays=0:scd=0:lookahead=0:"
                "lp=1:tile-rows=0:tile-columns=1:enable-tf=0:enable-cdef=0:enable-restoration=0:rmv=0:film-grain=0");
        } else {
            for (auto& [k, v] : std::initializer_list<std::pair<const char*, const char*>>{
                {"cpu-used", "10"}, {"usage", "realtime"}, {"crf", "28"}, {"lag-in-frames", "0"},
                {"row-mt", "1"}, {"tile-columns", "2"}, {"tile-rows", "0"}, {"error-resilient", "1"},
                {"frame-parallel", "1"}, {"aq-mode", "0"}, {"tune", "ssim"}, {"enable-cdef", "0"},
                {"enable-global-motion", "0"}, {"deltaq-mode", "0"}, {"enable-ref-frame-mvs", "0"},
                {"reduced-reference-set", "1"}}) set(k, v);
        }
    }

public:
    AV1Encoder(int w, int h, int fps, ID3D11Device* dev, ID3D11DeviceContext* ctx, ID3D11Multithread* mt)
        : width(w), height(h), device(dev), context(ctx), multithread(mt) {

        device->AddRef();
        if (context) context->AddRef();
        else device->GetImmediateContext(&context);
        if (multithread) multithread->AddRef();

        lastKeyframe = steady_clock::now() - KEYFRAME_INTERVAL;
        D3D11_QUERY_DESC qd = {D3D11_QUERY_EVENT, 0};
        device->CreateQuery(&qd, &encodeQuery);

        const AVCodec* codec = nullptr;
        for (auto name : {"av1_nvenc", "av1_qsv", "av1_amf", "libsvtav1", "libaom-av1"})
            if ((codec = avcodec_find_encoder_by_name(name))) { LOG("Encoder: %s", name); break; }

        if (!codec) throw std::runtime_error("No AV1 encoder available");

        codecContext = avcodec_alloc_context3(codec);
        if (!codecContext) throw std::runtime_error("Failed to allocate codec context");

        useHardware = strcmp(codec->name, "libaom-av1") && strcmp(codec->name, "libsvtav1") && InitHardwareContext(codec);
        if (!useHardware) codecContext->pix_fmt = AV_PIX_FMT_BGRA;

        codecContext->width = width; codecContext->height = height;
        codecContext->time_base = {1, fps}; codecContext->framerate = {fps, 1};
        codecContext->bit_rate = 20000000; codecContext->rc_max_rate = 40000000; codecContext->rc_buffer_size = 40000000;
        codecContext->gop_size = fps * 2; codecContext->max_b_frames = 0;
        codecContext->flags |= AV_CODEC_FLAG_LOW_DELAY; codecContext->flags2 |= AV_CODEC_FLAG2_FAST;
        codecContext->delay = 0; codecContext->has_b_frames = 0;
        codecContext->thread_count = useHardware ? 1 : std::min(4, std::max(1, static_cast<int>(std::thread::hardware_concurrency()) / 2));

        ConfigureEncoder(codec);
        if (avcodec_open2(codecContext, codec, nullptr) < 0) throw std::runtime_error("Failed to open encoder");

        hwFrame = av_frame_alloc(); packet = av_packet_alloc();
        if (!hwFrame || !packet) throw std::runtime_error("Failed to allocate frame or packet");

        hwFrame->format = codecContext->pix_fmt; hwFrame->width = width; hwFrame->height = height;
        if (!useHardware && av_frame_get_buffer(hwFrame, 32) < 0) throw std::runtime_error("Failed to allocate frame buffer");

        LOG("Encoder mode: %s", useHardware ? "Hardware" : "Software");
    }

    ~AV1Encoder() {
        av_packet_free(&packet); av_frame_free(&hwFrame);
        av_buffer_unref(&hwFrameCtx); av_buffer_unref(&hwDevice);
        if (codecContext) avcodec_free_context(&codecContext);
        SafeRelease(encodeQuery, stagingTexture, multithread, context, device);
    }

    void Flush() {
        avcodec_send_frame(codecContext, nullptr);
        while (avcodec_receive_packet(codecContext, packet) == 0) av_packet_unref(packet);
        avcodec_flush_buffers(codecContext);
        lastKeyframe = steady_clock::now() - KEYFRAME_INTERVAL;
    }

    EncodedFrame* Encode(ID3D11Texture2D* texture, int64_t timestamp, bool forceKeyframe = false) {
        LARGE_INTEGER startTime, endTime;
        QueryPerformanceCounter(&startTime);

        EncodedFrame* output = &outputFrames[outputIndex++ % 2];
        output->Clear();

        bool needsKeyframe = forceKeyframe || (steady_clock::now() - lastKeyframe >= KEYFRAME_INTERVAL);

        if (useHardware) {
            if (av_hwframe_get_buffer(codecContext->hw_frames_ctx, hwFrame, 0) < 0) { failedCount++; return nullptr; }
            MTLock lock(multithread);
            context->CopySubresourceRegion(reinterpret_cast<ID3D11Texture2D*>(hwFrame->data[0]),
                static_cast<UINT>(reinterpret_cast<intptr_t>(hwFrame->data[1])), 0, 0, 0, texture, 0, nullptr);
            if (!WaitForGPU(16)) { failedCount++; av_frame_unref(hwFrame); return nullptr; }
        } else {
            D3D11_TEXTURE2D_DESC td; texture->GetDesc(&td);
            if (!stagingTexture || stagingWidth != td.Width || stagingHeight != td.Height) {
                SafeRelease(stagingTexture);
                td.Usage = D3D11_USAGE_STAGING; td.BindFlags = 0; td.CPUAccessFlags = D3D11_CPU_ACCESS_READ; td.MiscFlags = 0;
                device->CreateTexture2D(&td, nullptr, &stagingTexture);
                stagingWidth = td.Width; stagingHeight = td.Height;
            }
            { MTLock lock(multithread); context->CopyResource(stagingTexture, texture); context->Flush(); }

            D3D11_MAPPED_SUBRESOURCE mapped;
            { MTLock lock(multithread); if (FAILED(context->Map(stagingTexture, 0, D3D11_MAP_READ, 0, &mapped))) { failedCount++; return nullptr; } }
            if (av_frame_make_writable(hwFrame) < 0) { MTLock lock(multithread); context->Unmap(stagingTexture, 0); failedCount++; return nullptr; }

            auto* src = static_cast<uint8_t*>(mapped.pData);
            for (int y = 0; y < height; y++)
                memcpy(hwFrame->data[0] + y * hwFrame->linesize[0], src + y * mapped.RowPitch, width * 4);
            MTLock lock(multithread); context->Unmap(stagingTexture, 0);
        }

        hwFrame->pts = frameNumber++;
        hwFrame->pict_type = needsKeyframe ? AV_PICTURE_TYPE_I : AV_PICTURE_TYPE_NONE;
        hwFrame->flags = needsKeyframe ? (hwFrame->flags | AV_FRAME_FLAG_KEY) : (hwFrame->flags & ~AV_FRAME_FLAG_KEY);
        if (needsKeyframe) lastKeyframe = steady_clock::now();

        int ret = avcodec_send_frame(codecContext, hwFrame);
        if (ret == AVERROR(EAGAIN)) {
            while (avcodec_receive_packet(codecContext, packet) == 0) {
                output->data.insert(output->data.end(), packet->data, packet->data + packet->size);
                av_packet_unref(packet);
            }
            ret = avcodec_send_frame(codecContext, hwFrame);
        }

        if (ret < 0 && ret != AVERROR_EOF) { failedCount++; if (useHardware) av_frame_unref(hwFrame); return nullptr; }

        bool gotKeyframe = false;
        while (avcodec_receive_packet(codecContext, packet) == 0) {
            if (packet->flags & AV_PKT_FLAG_KEY) gotKeyframe = true;
            output->data.insert(output->data.end(), packet->data, packet->data + packet->size);
            av_packet_unref(packet);
        }

        if (useHardware) av_frame_unref(hwFrame);
        if (output->data.empty()) return nullptr;

        QueryPerformanceCounter(&endTime);
        output->ts = timestamp;
        output->encUs = ((endTime.QuadPart - startTime.QuadPart) * 1000000) / queryFrequency;
        output->isKey = gotKeyframe;
        encodedCount++;
        return output;
    }

    uint64_t GetEncoded() { return encodedCount.exchange(0); }
    uint64_t GetFailed() { return failedCount.exchange(0); }
    int GetWidth() const { return width; }
    int GetHeight() const { return height; }
};
