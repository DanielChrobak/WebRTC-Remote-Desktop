/**
 * @file capture.hpp
 * @brief Screen capture implementation using Windows Graphics Capture API
 * @copyright 2025-2026 Daniel Chrobak
 */

#pragma once
#include "common.hpp"

/**
 * @brief Container for captured frame data
 */
struct FrameData {
    ID3D11Texture2D* tex = nullptr;
    int64_t ts = 0;
    uint64_t fence = 0;
    int poolIdx = -1;

    void Release() {
        SafeRelease(tex);
        poolIdx = -1;
    }
};

/**
 * @brief Thread-safe frame buffer for producer-consumer pattern
 *
 * Implements a triple-buffer scheme to minimize latency while preventing
 * frame tearing and ensuring smooth frame delivery to the encoder.
 */
class FrameSlot {
private:
    FrameData slots[3];
    int writeIdx = 0;
    int readIdx = -1;
    CRITICAL_SECTION cs;
    HANDLE event;
    uint64_t dropCount = 0;
    std::atomic<uint32_t> inFlightMask{0};

public:
    FrameSlot() {
        InitializeCriticalSection(&cs);
        event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    }

    ~FrameSlot() {
        DeleteCriticalSection(&cs);
        CloseHandle(event);
        for (auto& slot : slots) {
            slot.Release();
        }
    }

    /**
     * @brief Pushes a new frame into the buffer
     * @param tex Direct3D texture containing the frame
     * @param ts Capture timestamp in microseconds
     * @param fence GPU fence value for synchronization
     * @param poolIdx Index in the texture pool
     */
    void Push(ID3D11Texture2D* tex, int64_t ts, uint64_t fence, int poolIdx = -1) {
        EnterCriticalSection(&cs);

        int idx = writeIdx;
        writeIdx = (writeIdx + 1) % 3;
        if (writeIdx == readIdx) {
            writeIdx = (writeIdx + 1) % 3;
        }

        if (slots[idx].poolIdx >= 0) {
            inFlightMask.fetch_and(~(1u << slots[idx].poolIdx));
        }

        slots[idx].Release();
        tex->AddRef();
        slots[idx] = {tex, ts, fence, poolIdx};

        if (poolIdx >= 0) {
            inFlightMask.fetch_or(1u << poolIdx);
        }

        if (readIdx >= 0) {
            dropCount++;
        }

        readIdx = idx;
        SetEvent(event);
        LeaveCriticalSection(&cs);
    }

    /**
     * @brief Retrieves the latest frame from the buffer
     * @param out Output frame data
     * @param timeoutMs Maximum wait time in milliseconds
     * @return True if a frame was retrieved, false on timeout
     */
    bool Pop(FrameData& out, DWORD timeoutMs = 8) {
        if (WaitForSingleObject(event, timeoutMs) != WAIT_OBJECT_0) {
            return false;
        }

        EnterCriticalSection(&cs);
        ResetEvent(event);

        if (readIdx < 0) {
            LeaveCriticalSection(&cs);
            return false;
        }

        out = slots[readIdx];
        slots[readIdx].tex = nullptr;
        slots[readIdx].poolIdx = -1;
        readIdx = -1;

        LeaveCriticalSection(&cs);
        return true;
    }

    void MarkReleased(int idx) {
        if (idx >= 0) {
            inFlightMask.fetch_and(~(1u << idx));
        }
    }

    bool IsInFlight(int idx) const {
        return idx >= 0 && (inFlightMask.load() & (1u << idx));
    }

    void Reset() {
        EnterCriticalSection(&cs);
        inFlightMask = 0;
        for (auto& slot : slots) {
            slot.Release();
        }
        writeIdx = 0;
        readIdx = -1;
        ResetEvent(event);
        LeaveCriticalSection(&cs);
    }

    uint64_t GetDropped() {
        EnterCriticalSection(&cs);
        uint64_t count = dropCount;
        dropCount = 0;
        LeaveCriticalSection(&cs);
        return count;
    }

    HANDLE GetEvent() const { return event; }
};

/**
 * @brief GPU synchronization helper using D3D11 fences or queries
 *
 * Provides abstraction over D3D11.4 fences with fallback to event queries
 * for older hardware or drivers that don't support fences.
 */
class GPUSync {
private:
    ID3D11Device5* device5 = nullptr;
    ID3D11DeviceContext4* context4 = nullptr;
    ID3D11Fence* fence = nullptr;
    ID3D11Query* query = nullptr;
    HANDLE fenceEvent = nullptr;
    uint64_t fenceValue = 0;
    bool useFence = false;

public:
    /**
     * @brief Initializes GPU synchronization primitives
     * @param device D3D11 device
     * @param context D3D11 device context
     * @return True if initialization succeeded
     */
    bool Init(ID3D11Device* device, ID3D11DeviceContext* context) {
        if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&device5))) &&
            SUCCEEDED(context->QueryInterface(IID_PPV_ARGS(&context4))) &&
            SUCCEEDED(device5->CreateFence(0, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(&fence)))) {
            fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            useFence = true;
            LOG("GPU sync: Fence");
            return true;
        }

        SafeRelease(device5, context4, fence);

        D3D11_QUERY_DESC queryDesc = {D3D11_QUERY_EVENT, 0};
        if (SUCCEEDED(device->CreateQuery(&queryDesc, &query))) {
            LOG("GPU sync: Query");
            return true;
        }

        return false;
    }

    ~GPUSync() {
        if (fenceEvent) CloseHandle(fenceEvent);
        SafeRelease(fence, context4, device5, query);
    }

    uint64_t Signal(ID3D11DeviceContext* context) {
        if (useFence && context4 && fence) {
            context4->Signal(fence, ++fenceValue);
            return fenceValue;
        }
        if (query) {
            context->End(query);
        }
        return 0;
    }

    bool IsComplete(uint64_t value, ID3D11DeviceContext* context) {
        if (useFence) {
            return !fence || fence->GetCompletedValue() >= value;
        }
        return !query || context->GetData(query, nullptr, 0, 0) == S_OK;
    }

    bool Wait(uint64_t value, ID3D11DeviceContext* context, DWORD timeoutMs = 5) {
        if (IsComplete(value, context)) {
            return true;
        }

        if (useFence && fence && fenceEvent) {
            fence->SetEventOnCompletion(value, fenceEvent);
            return WaitForSingleObject(fenceEvent, timeoutMs) == WAIT_OBJECT_0 ||
                   IsComplete(value, context);
        }

        for (DWORD i = 0; i < 200 && !IsComplete(value, context); i++) {
            YieldProcessor();
        }

        return IsComplete(value, context);
    }
};

/**
 * @brief Windows Graphics Capture-based screen capture implementation
 *
 * Captures the desktop using the Windows.Graphics.Capture API with
 * hardware-accelerated texture copies and GPU synchronization.
 */
class ScreenCapture {
private:
    static constexpr int TEX_POOL_SIZE = 8;

    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    ID3D11Multithread* multithread = nullptr;
    WGD::Direct3D11::IDirect3DDevice winrtDevice{nullptr};

    WGC::GraphicsCaptureItem captureItem{nullptr};
    WGC::Direct3D11CaptureFramePool framePool{nullptr};
    WGC::GraphicsCaptureSession captureSession{nullptr};

    ID3D11Texture2D* texturePool[TEX_POOL_SIZE] = {};
    int textureIndex = 0;
    int width = 0;
    int height = 0;
    int hostFps = 60;

    std::atomic<int> targetFps{60};
    std::atomic<int> currentMonitorIdx{0};
    GPUSync gpuSync;
    FrameSlot* frameSlot;

    std::atomic<bool> running{true};
    std::atomic<bool> capturing{false};
    std::atomic<bool> forceSync{true};
    std::atomic<bool> sessionStarted{false};

    bool supportsMinInterval = false;
    int64_t nextFrameTime = 0;
    HMONITOR currentMonitor = nullptr;
    std::mutex captureMutex;
    std::function<void(int, int, int)> onResolutionChange;
    std::atomic<uint64_t> textureConflicts{0};

    int FindAvailableTexture() {
        for (int i = 0; i < TEX_POOL_SIZE; i++) {
            int idx = (textureIndex + i) % TEX_POOL_SIZE;
            if (!frameSlot->IsInFlight(idx)) {
                textureIndex = idx + 1;
                return idx;
            }
        }
        textureConflicts++;
        return textureIndex++ % TEX_POOL_SIZE;
    }

    void OnFrameArrived(WGC::Direct3D11CaptureFramePool const& sender,
                        winrt::Windows::Foundation::IInspectable const&) {
        auto frame = sender.TryGetNextFrame();
        if (!frame || !running || !capturing) {
            return;
        }

        int64_t timestamp = GetTimestamp();
        int64_t interval = 1000000 / targetFps.load();

        if (forceSync.exchange(false)) {
            nextFrameTime = timestamp + interval;
        } else if (timestamp < nextFrameTime) {
            return;
        }

        while (nextFrameTime <= timestamp) {
            nextFrameTime += interval;
        }

        auto surface = frame.Surface();
        if (!surface) {
            return;
        }

        winrt::com_ptr<ID3D11Texture2D> sourceTexture;
        auto access = surface.as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
        if (FAILED(access->GetInterface(IID_PPV_ARGS(sourceTexture.put()))) || !sourceTexture) {
            return;
        }

        int texIdx = FindAvailableTexture();
        if (!texturePool[texIdx]) {
            return;
        }

        {
            MTLock lock(multithread);
            context->CopyResource(texturePool[texIdx], sourceTexture.get());
            context->Flush();
        }

        frameSlot->Push(texturePool[texIdx], timestamp, gpuSync.Signal(context), texIdx);
    }

    void InitializeMonitor(HMONITOR monitor) {
        MONITORINFOEXW monitorInfo{sizeof(monitorInfo)};
        DEVMODEW displayMode{.dmSize = sizeof(displayMode)};

        if (GetMonitorInfoW(monitor, &monitorInfo) &&
            EnumDisplaySettingsW(monitorInfo.szDevice, ENUM_CURRENT_SETTINGS, &displayMode)) {
            hostFps = targetFps = displayMode.dmDisplayFrequency;
        }

        auto interop = winrt::get_activation_factory<WGC::GraphicsCaptureItem,
                                                      IGraphicsCaptureItemInterop>();
        if (FAILED(interop->CreateForMonitor(
                monitor,
                winrt::guid_of<ABI::Windows::Graphics::Capture::IGraphicsCaptureItem>(),
                winrt::put_abi(captureItem)))) {
            throw std::runtime_error("Failed to create capture item for monitor");
        }

        width = captureItem.Size().Width;
        height = captureItem.Size().Height;

        for (auto& tex : texturePool) {
            SafeRelease(tex);
        }

        D3D11_TEXTURE2D_DESC texDesc = {
            static_cast<UINT>(width),
            static_cast<UINT>(height),
            1, 1,
            DXGI_FORMAT_B8G8R8A8_UNORM,
            {1, 0},
            D3D11_USAGE_DEFAULT,
            D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET,
            0,
            D3D11_RESOURCE_MISC_SHARED
        };

        for (int i = 0; i < TEX_POOL_SIZE; i++) {
            if (FAILED(device->CreateTexture2D(&texDesc, nullptr, &texturePool[i]))) {
                throw std::runtime_error("Failed to create texture pool");
            }
        }

        framePool = WGC::Direct3D11CaptureFramePool::CreateFreeThreaded(
            winrtDevice,
            WGD::DirectXPixelFormat::B8G8R8A8UIntNormalized,
            2,
            {width, height}
        );

        framePool.FrameArrived({this, &ScreenCapture::OnFrameArrived});
        captureSession = framePool.CreateCaptureSession(captureItem);
        captureSession.IsCursorCaptureEnabled(true);

        try {
            captureSession.IsBorderRequired(false);
        } catch (...) {}

        sessionStarted = false;
        currentMonitor = monitor;
    }

    void ApplyMinUpdateInterval() {
        if (supportsMinInterval) {
            try {
                captureSession.MinUpdateInterval(
                    duration<int64_t, std::ratio<1, 10000000>>(10000000 / hostFps));
            } catch (...) {}
        }
    }

public:
    /**
     * @brief Constructs a screen capture instance
     * @param slot Frame buffer for captured frames
     */
    explicit ScreenCapture(FrameSlot* slot) : frameSlot(slot) {
        LOG("Initializing screen capture...");

        winrt::init_apartment(winrt::apartment_type::multi_threaded);

        UINT createFlags = D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
        D3D_FEATURE_LEVEL featureLevels[] = {
            D3D_FEATURE_LEVEL_12_1,
            D3D_FEATURE_LEVEL_12_0,
            D3D_FEATURE_LEVEL_11_1,
            D3D_FEATURE_LEVEL_11_0
        };
        D3D_FEATURE_LEVEL actualLevel;

        if (FAILED(D3D11CreateDevice(
                nullptr,
                D3D_DRIVER_TYPE_HARDWARE,
                nullptr,
                createFlags,
                featureLevels,
                _countof(featureLevels),
                D3D11_SDK_VERSION,
                &device,
                &actualLevel,
                &context))) {
            throw std::runtime_error("Failed to create D3D11 device");
        }

        if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&multithread)))) {
            multithread->SetMultithreadProtected(TRUE);
        }

        if (!gpuSync.Init(device, context)) {
            throw std::runtime_error("Failed to initialize GPU synchronization");
        }

        winrt::com_ptr<IDXGIDevice> dxgiDevice;
        winrt::com_ptr<::IInspectable> inspectable;

        if (FAILED(device->QueryInterface(IID_PPV_ARGS(dxgiDevice.put()))) ||
            FAILED(CreateDirect3D11DeviceFromDXGIDevice(dxgiDevice.get(), inspectable.put()))) {
            throw std::runtime_error("Failed to create WinRT device");
        }

        winrtDevice = inspectable.as<WGD::Direct3D11::IDirect3DDevice>();

        RefreshMonitorList();

        supportsMinInterval = winrt::Windows::Foundation::Metadata::ApiInformation::IsPropertyPresent(
            L"Windows.Graphics.Capture.GraphicsCaptureSession",
            L"MinUpdateInterval"
        );

        InitializeMonitor(MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY));

        LOG("Capture initialized: %dx%d @ %dHz", width, height, hostFps);
    }

    ~ScreenCapture() {
        running = capturing = false;

        try { if (captureSession) captureSession.Close(); } catch (...) {}
        try { if (framePool) framePool.Close(); } catch (...) {}

        for (auto& tex : texturePool) {
            SafeRelease(tex);
        }

        SafeRelease(multithread, context, device);
        winrt::uninit_apartment();
    }

    void SetResolutionChangeCallback(std::function<void(int, int, int)> callback) {
        onResolutionChange = callback;
    }

    void StartCapture() {
        std::lock_guard<std::mutex> lock(captureMutex);
        if (capturing) return;

        frameSlot->Reset();
        textureIndex = 0;
        forceSync = true;
        ApplyMinUpdateInterval();

        if (!sessionStarted.exchange(true)) {
            captureSession.StartCapture();
        }

        capturing = true;
        LOG("Capture started at %dHz", hostFps);
    }

    void PauseCapture() {
        if (capturing) {
            capturing = false;
            LOG("Capture paused");
        }
    }

    /**
     * @brief Switches capture to a different monitor
     * @param index Monitor index from global monitor list
     * @return True if switch succeeded
     */
    bool SwitchMonitor(int index) {
        std::lock_guard<std::mutex> lock(captureMutex);
        std::lock_guard<std::mutex> monitorLock(g_monitorsMutex);

        if (index < 0 || index >= static_cast<int>(g_monitors.size())) {
            return false;
        }

        if (currentMonitorIdx == index && currentMonitor == g_monitors[index].hMon) {
            return true;
        }

        bool wasCapturing = capturing.load();
        capturing = false;

        try { if (captureSession) captureSession.Close(); } catch (...) {}
        try { if (framePool) framePool.Close(); } catch (...) {}

        captureSession = nullptr;
        framePool = nullptr;
        captureItem = nullptr;
        frameSlot->Reset();
        textureIndex = 0;

        try {
            InitializeMonitor(g_monitors[index].hMon);
            currentMonitorIdx = index;

            LOG("Switched to monitor %d: %dx%d @ %dHz", index, width, height, hostFps);

            if (onResolutionChange) {
                onResolutionChange(width, height, hostFps);
            }

            if (wasCapturing) {
                forceSync = true;
                ApplyMinUpdateInterval();
                captureSession.StartCapture();
                sessionStarted = true;
                capturing = true;
            }

            return true;
        } catch (const std::exception& e) {
            ERR("Monitor switch failed: %s", e.what());
            return false;
        }
    }

    bool SetFPS(int fps) {
        if (fps < 1 || fps > 240) return false;
        int oldFps = targetFps.exchange(fps);
        if (oldFps != fps) forceSync = true;
        return true;
    }

    int RefreshHostFPS() {
        if (currentMonitor) {
            MONITORINFOEXW monitorInfo{sizeof(monitorInfo)};
            DEVMODEW displayMode{.dmSize = sizeof(displayMode)};
            if (GetMonitorInfoW(currentMonitor, &monitorInfo) &&
                EnumDisplaySettingsW(monitorInfo.szDevice, ENUM_CURRENT_SETTINGS, &displayMode)) {
                hostFps = displayMode.dmDisplayFrequency;
            }
        }
        return hostFps;
    }

    int GetCurrentMonitorIndex() const { return currentMonitorIdx; }
    int GetHostFPS() const { return hostFps; }
    int GetCurrentFPS() const { return targetFps; }
    bool IsCapturing() const { return capturing; }
    bool IsReady(uint64_t fence) { return gpuSync.IsComplete(fence, context); }
    bool WaitReady(uint64_t fence) { return gpuSync.Wait(fence, context); }
    uint64_t GetTexConflicts() { return textureConflicts.exchange(0); }
    ID3D11Device* GetDev() const { return device; }
    ID3D11DeviceContext* GetCtx() const { return context; }
    ID3D11Multithread* GetMT() const { return multithread; }
    int GetW() const { return width; }
    int GetH() const { return height; }
};
