/**
 * @file encoder.hpp
 * @brief AV1 video encoder with hardware acceleration support
 * @copyright 2025-2026 Daniel Chrobak
 */

#pragma once
#include "common.hpp"

/**
 * @brief Container for encoded video frame data
 */
struct EncodedFrame {
    std::vector<uint8_t> data;
    int64_t ts = 0;
    int64_t encUs = 0;
    bool isKey = false;

    void Clear() {
        data.clear();
        ts = 0;
        encUs = 0;
        isKey = false;
    }
};

/**
 * @brief AV1 video encoder with support for NVENC, QSV, AMF, and software fallback
 *
 * Automatically detects and uses the best available hardware encoder,
 * falling back to software encoding (libsvtav1 or libaom) when necessary.
 */
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

    int width;
    int height;
    int frameNumber = 0;
    UINT stagingWidth = 0;
    UINT stagingHeight = 0;
    bool useHardware = false;
    steady_clock::time_point lastKeyframe;

    static constexpr auto KEYFRAME_INTERVAL = 2000ms;

    EncodedFrame outputFrames[2];
    int outputIndex = 0;
    std::atomic<uint64_t> encodedCount{0};
    std::atomic<uint64_t> failedCount{0};

    static inline const int64_t queryFrequency = [] {
        LARGE_INTEGER freq;
        QueryPerformanceFrequency(&freq);
        return freq.QuadPart;
    }();

    bool InitHardwareContext(const AVCodec* codec) {
        if (strcmp(codec->name, "av1_nvenc") && strcmp(codec->name, "av1_qsv")) {
            return false;
        }

        hwDevice = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_D3D11VA);
        if (!hwDevice) {
            return false;
        }

        auto* deviceCtx = reinterpret_cast<AVD3D11VADeviceContext*>(
            reinterpret_cast<AVHWDeviceContext*>(hwDevice->data)->hwctx);
        deviceCtx->device = device;
        deviceCtx->device_context = context;

        if (av_hwdevice_ctx_init(hwDevice) < 0) {
            av_buffer_unref(&hwDevice);
            return false;
        }

        codecContext->hw_device_ctx = av_buffer_ref(hwDevice);

        hwFrameCtx = av_hwframe_ctx_alloc(hwDevice);
        if (!hwFrameCtx) {
            av_buffer_unref(&hwDevice);
            return false;
        }

        auto* framesCtx = reinterpret_cast<AVHWFramesContext*>(hwFrameCtx->data);
        framesCtx->format = AV_PIX_FMT_D3D11;
        framesCtx->sw_format = AV_PIX_FMT_BGRA;
        framesCtx->width = width;
        framesCtx->height = height;
        framesCtx->initial_pool_size = 4;

        if (av_hwframe_ctx_init(hwFrameCtx) < 0) {
            av_buffer_unref(&hwFrameCtx);
            av_buffer_unref(&hwDevice);
            return false;
        }

        codecContext->hw_frames_ctx = av_buffer_ref(hwFrameCtx);
        codecContext->pix_fmt = AV_PIX_FMT_D3D11;
        return true;
    }

    bool WaitForGPU(DWORD timeoutMs = 16) {
        if (!encodeQuery) {
            return true;
        }

        context->End(encodeQuery);

        LARGE_INTEGER start, now, freq;
        QueryPerformanceCounter(&start);
        QueryPerformanceFrequency(&freq);
        int64_t timeout = (freq.QuadPart * timeoutMs) / 1000;

        while (context->GetData(encodeQuery, nullptr, 0, 0) == S_FALSE) {
            QueryPerformanceCounter(&now);
            if ((now.QuadPart - start.QuadPart) > timeout) {
                return false;
            }
            YieldProcessor();
        }

        return true;
    }

    void ConfigureEncoder(const AVCodec* codec) {
        auto setOpt = [this](const char* key, const char* value) {
            av_opt_set(codecContext->priv_data, key, value, 0);
        };

        if (!strcmp(codec->name, "av1_nvenc")) {
            setOpt("preset", "p1");
            setOpt("tune", "ull");
            setOpt("rc", "cbr");
            setOpt("cq", "23");
            setOpt("delay", "0");
            setOpt("zerolatency", "1");
            setOpt("lookahead", "0");
            setOpt("rc-lookahead", "0");
            setOpt("forced-idr", "1");
            setOpt("b_adapt", "0");
            setOpt("spatial-aq", "0");
            setOpt("temporal-aq", "0");
            setOpt("nonref_p", "1");
            setOpt("strict_gop", "1");
            setOpt("multipass", "disabled");
            setOpt("ldkfs", "1");
            setOpt("surfaces", "8");
            setOpt("aud", "0");
            setOpt("bluray-compat", "0");
        } else if (!strcmp(codec->name, "av1_qsv")) {
            setOpt("preset", "veryfast");
            setOpt("async_depth", "1");
            setOpt("look_ahead", "0");
            setOpt("look_ahead_depth", "0");
            setOpt("forced_idr", "1");
            setOpt("low_power", "1");
            setOpt("low_delay_brc", "1");
            setOpt("max_frame_size", "0");
            setOpt("b_strategy", "0");
            setOpt("extbrc", "0");
            setOpt("global_quality", "23");
        } else if (!strcmp(codec->name, "av1_amf")) {
            setOpt("usage", "ultralowlatency");
            setOpt("quality", "speed");
            setOpt("rc", "vbr_latency");
            setOpt("qp_i", "23");
            setOpt("qp_p", "25");
            setOpt("preanalysis", "0");
            setOpt("enforce_hrd", "0");
            setOpt("filler_data", "0");
            setOpt("frame_skipping", "0");
            setOpt("header_insertion_mode", "idr");
        } else if (!strcmp(codec->name, "libsvtav1")) {
            setOpt("preset", "12");
            setOpt("crf", "28");
            setOpt("svtav1-params",
                   "tune=0:fast-decode=1:enable-overlays=0:scd=0:lookahead=0:"
                   "lp=1:tile-rows=0:tile-columns=1:enable-tf=0:enable-cdef=0:"
                   "enable-restoration=0:rmv=0:film-grain=0");
        } else {
            setOpt("cpu-used", "10");
            setOpt("usage", "realtime");
            setOpt("crf", "28");
            setOpt("lag-in-frames", "0");
            setOpt("row-mt", "1");
            setOpt("tile-columns", "2");
            setOpt("tile-rows", "0");
            setOpt("error-resilient", "1");
            setOpt("frame-parallel", "1");
            setOpt("aq-mode", "0");
            setOpt("tune", "ssim");
            setOpt("enable-cdef", "0");
            setOpt("enable-global-motion", "0");
            setOpt("deltaq-mode", "0");
            setOpt("enable-ref-frame-mvs", "0");
            setOpt("reduced-reference-set", "1");
        }
    }

public:
    /**
     * @brief Constructs an AV1 encoder
     * @param w Frame width
     * @param h Frame height
     * @param fps Target frame rate
     * @param dev D3D11 device for hardware encoding
     * @param ctx D3D11 device context
     * @param mt D3D11 multithread interface
     */
    AV1Encoder(int w, int h, int fps, ID3D11Device* dev, ID3D11DeviceContext* ctx, ID3D11Multithread* mt)
        : width(w), height(h), device(dev), context(ctx), multithread(mt) {

        device->AddRef();
        if (context) {
            context->AddRef();
        } else {
            device->GetImmediateContext(&context);
        }
        if (multithread) {
            multithread->AddRef();
        }

        lastKeyframe = steady_clock::now() - KEYFRAME_INTERVAL;

        D3D11_QUERY_DESC queryDesc = {D3D11_QUERY_EVENT, 0};
        device->CreateQuery(&queryDesc, &encodeQuery);

        const AVCodec* codec = nullptr;
        const char* encoderNames[] = {"av1_nvenc", "av1_qsv", "av1_amf", "libsvtav1", "libaom-av1"};

        for (const auto& name : encoderNames) {
            codec = avcodec_find_encoder_by_name(name);
            if (codec) {
                LOG("Encoder: %s", name);
                break;
            }
        }

        if (!codec) {
            throw std::runtime_error("No AV1 encoder available");
        }

        codecContext = avcodec_alloc_context3(codec);
        if (!codecContext) {
            throw std::runtime_error("Failed to allocate codec context");
        }

        useHardware = strcmp(codec->name, "libaom-av1") &&
                      strcmp(codec->name, "libsvtav1") &&
                      InitHardwareContext(codec);

        if (!useHardware) {
            codecContext->pix_fmt = AV_PIX_FMT_BGRA;
        }

        codecContext->width = width;
        codecContext->height = height;
        codecContext->time_base = {1, fps};
        codecContext->framerate = {fps, 1};
        codecContext->bit_rate = 20000000;
        codecContext->rc_max_rate = 40000000;
        codecContext->rc_buffer_size = 40000000;
        codecContext->gop_size = fps * 2;
        codecContext->max_b_frames = 0;
        codecContext->flags |= AV_CODEC_FLAG_LOW_DELAY;
        codecContext->flags2 |= AV_CODEC_FLAG2_FAST;
        codecContext->delay = 0;
        codecContext->has_b_frames = 0;
        codecContext->thread_count = useHardware ? 1 :
            std::min(4, std::max(1, static_cast<int>(std::thread::hardware_concurrency()) / 2));

        ConfigureEncoder(codec);

        if (avcodec_open2(codecContext, codec, nullptr) < 0) {
            throw std::runtime_error("Failed to open encoder");
        }

        hwFrame = av_frame_alloc();
        packet = av_packet_alloc();

        if (!hwFrame || !packet) {
            throw std::runtime_error("Failed to allocate frame or packet");
        }

        hwFrame->format = codecContext->pix_fmt;
        hwFrame->width = width;
        hwFrame->height = height;

        if (!useHardware && av_frame_get_buffer(hwFrame, 32) < 0) {
            throw std::runtime_error("Failed to allocate frame buffer");
        }

        LOG("Encoder mode: %s", useHardware ? "Hardware" : "Software");
    }

    ~AV1Encoder() {
        av_packet_free(&packet);
        av_frame_free(&hwFrame);
        av_buffer_unref(&hwFrameCtx);
        av_buffer_unref(&hwDevice);
        if (codecContext) {
            avcodec_free_context(&codecContext);
        }
        SafeRelease(encodeQuery, stagingTexture, multithread, context, device);
    }

    /**
     * @brief Flushes the encoder pipeline
     */
    void Flush() {
        avcodec_send_frame(codecContext, nullptr);
        while (avcodec_receive_packet(codecContext, packet) == 0) {
            av_packet_unref(packet);
        }
        avcodec_flush_buffers(codecContext);
        lastKeyframe = steady_clock::now() - KEYFRAME_INTERVAL;
    }

    /**
     * @brief Encodes a single frame
     * @param texture Source D3D11 texture
     * @param timestamp Frame timestamp in microseconds
     * @param forceKeyframe Force generation of a keyframe
     * @return Pointer to encoded frame data, or nullptr on failure
     */
    EncodedFrame* Encode(ID3D11Texture2D* texture, int64_t timestamp, bool forceKeyframe = false) {
        LARGE_INTEGER startTime, endTime;
        QueryPerformanceCounter(&startTime);

        EncodedFrame* output = &outputFrames[outputIndex++ % 2];
        output->Clear();

        bool needsKeyframe = forceKeyframe || (steady_clock::now() - lastKeyframe >= KEYFRAME_INTERVAL);

        if (useHardware) {
            if (av_hwframe_get_buffer(codecContext->hw_frames_ctx, hwFrame, 0) < 0) {
                failedCount++;
                return nullptr;
            }

            MTLock lock(multithread);
            context->CopySubresourceRegion(
                reinterpret_cast<ID3D11Texture2D*>(hwFrame->data[0]),
                static_cast<UINT>(reinterpret_cast<intptr_t>(hwFrame->data[1])),
                0, 0, 0,
                texture,
                0,
                nullptr
            );

            if (!WaitForGPU(16)) {
                failedCount++;
                av_frame_unref(hwFrame);
                return nullptr;
            }
        } else {
            D3D11_TEXTURE2D_DESC texDesc;
            texture->GetDesc(&texDesc);

            if (!stagingTexture || stagingWidth != texDesc.Width || stagingHeight != texDesc.Height) {
                SafeRelease(stagingTexture);
                texDesc.Usage = D3D11_USAGE_STAGING;
                texDesc.BindFlags = 0;
                texDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                texDesc.MiscFlags = 0;
                device->CreateTexture2D(&texDesc, nullptr, &stagingTexture);
                stagingWidth = texDesc.Width;
                stagingHeight = texDesc.Height;
            }

            {
                MTLock lock(multithread);
                context->CopyResource(stagingTexture, texture);
                context->Flush();
            }

            D3D11_MAPPED_SUBRESOURCE mapped;
            {
                MTLock lock(multithread);
                if (FAILED(context->Map(stagingTexture, 0, D3D11_MAP_READ, 0, &mapped))) {
                    failedCount++;
                    return nullptr;
                }
            }

            if (av_frame_make_writable(hwFrame) < 0) {
                MTLock lock(multithread);
                context->Unmap(stagingTexture, 0);
                failedCount++;
                return nullptr;
            }

            auto* srcData = static_cast<uint8_t*>(mapped.pData);
            for (int y = 0; y < height; y++) {
                memcpy(hwFrame->data[0] + y * hwFrame->linesize[0],
                       srcData + y * mapped.RowPitch,
                       width * 4);
            }

            MTLock lock(multithread);
            context->Unmap(stagingTexture, 0);
        }

        hwFrame->pts = frameNumber++;
        hwFrame->pict_type = needsKeyframe ? AV_PICTURE_TYPE_I : AV_PICTURE_TYPE_NONE;
        hwFrame->flags = needsKeyframe ?
            (hwFrame->flags | AV_FRAME_FLAG_KEY) :
            (hwFrame->flags & ~AV_FRAME_FLAG_KEY);

        if (needsKeyframe) {
            lastKeyframe = steady_clock::now();
        }

        int ret = avcodec_send_frame(codecContext, hwFrame);

        if (ret == AVERROR(EAGAIN)) {
            bool gotKeyframe = false;
            while (avcodec_receive_packet(codecContext, packet) == 0) {
                if (packet->flags & AV_PKT_FLAG_KEY) {
                    gotKeyframe = true;
                }
                output->data.insert(output->data.end(),
                                    packet->data,
                                    packet->data + packet->size);
                av_packet_unref(packet);
            }
            ret = avcodec_send_frame(codecContext, hwFrame);
        }

        if (ret < 0 && ret != AVERROR_EOF) {
            failedCount++;
            if (useHardware) {
                av_frame_unref(hwFrame);
            }
            return nullptr;
        }

        bool gotKeyframe = false;
        while (avcodec_receive_packet(codecContext, packet) == 0) {
            if (packet->flags & AV_PKT_FLAG_KEY) {
                gotKeyframe = true;
            }
            output->data.insert(output->data.end(),
                                packet->data,
                                packet->data + packet->size);
            av_packet_unref(packet);
        }

        if (useHardware) {
            av_frame_unref(hwFrame);
        }

        if (output->data.empty()) {
            return nullptr;
        }

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
