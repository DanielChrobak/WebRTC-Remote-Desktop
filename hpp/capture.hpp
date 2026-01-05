#pragma once
#include "common.hpp"

struct FrameData {
    ID3D11Texture2D* tex = nullptr;
    int64_t ts = 0;
    uint64_t fence = 0;
    int poolIdx = -1;  // Track which pool texture this came from
    void Release() { SafeRelease(tex); poolIdx = -1; }
};

class FrameSlot {
    FrameData s[3];
    int wi = 0, ri = -1;
    CRITICAL_SECTION cs;
    HANDLE ev;
    uint64_t dc = 0;
    std::atomic<uint32_t> inFlightMask{0};  // Track which pool textures are in-flight

public:
    FrameSlot() { InitializeCriticalSection(&cs); ev = CreateEventW(0, 1, 0, 0); }
    ~FrameSlot() { DeleteCriticalSection(&cs); CloseHandle(ev); for (auto& x : s) x.Release(); }

    void Push(ID3D11Texture2D* t, int64_t ts, uint64_t f, int poolIdx = -1) {
        EnterCriticalSection(&cs);
        int i = wi; wi = (wi + 1) % 3;
        if (wi == ri) wi = (wi + 1) % 3;

        // Release old slot and clear its in-flight bit
        if (s[i].poolIdx >= 0) {
            inFlightMask.fetch_and(~(1u << s[i].poolIdx), std::memory_order_release);
        }
        s[i].Release();

        t->AddRef();
        s[i] = {t, ts, f, poolIdx};

        // Mark new texture as in-flight
        if (poolIdx >= 0) {
            inFlightMask.fetch_or(1u << poolIdx, std::memory_order_release);
        }

        if (ri >= 0) dc++;
        ri = i; SetEvent(ev);
        LeaveCriticalSection(&cs);
    }

    bool Pop(FrameData& o, DWORD ms = 8) {
        if (WaitForSingleObject(ev, ms) != WAIT_OBJECT_0) return false;
        EnterCriticalSection(&cs);
        ResetEvent(ev);
        if (ri < 0) { LeaveCriticalSection(&cs); return false; }
        o = s[ri]; s[ri].tex = nullptr; s[ri].poolIdx = -1; ri = -1;
        LeaveCriticalSection(&cs);
        return true;
    }

    void MarkReleased(int poolIdx) {
        if (poolIdx >= 0) {
            inFlightMask.fetch_and(~(1u << poolIdx), std::memory_order_release);
        }
    }

    bool IsInFlight(int poolIdx) const {
        return poolIdx >= 0 && (inFlightMask.load(std::memory_order_acquire) & (1u << poolIdx)) != 0;
    }

    uint64_t GetDropped() { EnterCriticalSection(&cs); uint64_t d = dc; dc = 0; LeaveCriticalSection(&cs); return d; }
    HANDLE GetEvent() const { return ev; }
};

class GPUSync {
    ID3D11Device5* d5 = nullptr;
    ID3D11DeviceContext4* c4 = nullptr;
    ID3D11Fence* fn = nullptr;
    ID3D11Query* qy = nullptr;
    HANDLE ev = nullptr;
    uint64_t fv = 0;
    bool uf = false;
public:
    bool Init(ID3D11Device* d, ID3D11DeviceContext* c) {
        if (SUCCEEDED(d->QueryInterface(IID_PPV_ARGS(&d5))) &&
            SUCCEEDED(c->QueryInterface(IID_PPV_ARGS(&c4))) &&
            SUCCEEDED(d5->CreateFence(0, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(&fn)))) {
            ev = CreateEventW(0, 0, 0, 0); uf = true;
            LOG("GPU sync: Fence"); return true;
        }
        SafeRelease(d5, c4, fn);
        D3D11_QUERY_DESC qd = {D3D11_QUERY_EVENT, 0};
        if (SUCCEEDED(d->CreateQuery(&qd, &qy))) { LOG("GPU sync: Query"); return true; }
        ERR("GPU sync init failed"); return false;
    }
    ~GPUSync() { if (ev) CloseHandle(ev); SafeRelease(fn, c4, d5, qy); }

    uint64_t Signal(ID3D11DeviceContext* c) {
        if (uf && c4 && fn) { c4->Signal(fn, ++fv); return fv; }
        if (qy) c->End(qy); return 0;
    }
    bool IsComplete(uint64_t v, ID3D11DeviceContext* c) {
        return uf ? (!fn || fn->GetCompletedValue() >= v) : (!qy || c->GetData(qy, 0, 0, 0) == S_OK);
    }
    bool Wait(uint64_t v, ID3D11DeviceContext* c, DWORD ms = 5) {
        if (IsComplete(v, c)) return true;
        if (uf && fn && ev) {
            fn->SetEventOnCompletion(v, ev);
            return WaitForSingleObject(ev, ms) == WAIT_OBJECT_0 || IsComplete(v, c);
        }
        for (DWORD i = 0; i < 200 && !IsComplete(v, c); i++) YieldProcessor();
        return IsComplete(v, c);
    }
};

struct MonitorInfo { HMONITOR hMon; int index, width, height, refreshRate; bool isPrimary; std::string name; };
extern std::vector<MonitorInfo> g_monitors;
extern std::mutex g_monitorsMutex;
void RefreshMonitorList();

class ScreenCapture {
    static constexpr int TEX_POOL_SIZE = 8;  // Increased from 4 for high FPS scenarios

    ID3D11Device* dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    ID3D11Multithread* mt = nullptr;
    WGD::Direct3D11::IDirect3DDevice wdev{nullptr};
    WGC::GraphicsCaptureItem item{nullptr};
    WGC::Direct3D11CaptureFramePool pool{nullptr};
    WGC::GraphicsCaptureSession sess{nullptr};
    ID3D11Texture2D* tp[TEX_POOL_SIZE] = {};
    int ti = 0, w = 0, h = 0, hfps = 60;
    std::atomic<int> tfps{60}, cmi{0};
    GPUSync gs;
    FrameSlot* sl;
    std::atomic<bool> run{true}, cap{false}, fsr{true};
    bool smi = false;
    int64_t nft = 0;
    HMONITOR chm = nullptr;
    std::mutex cmx;
    std::function<void(int, int, int)> onRes;
    std::atomic<uint64_t> texConflicts{0};

    int FindAvailableTexture() {
        // Find a texture that's not currently in-flight
        int startIdx = ti % TEX_POOL_SIZE;
        for (int i = 0; i < TEX_POOL_SIZE; i++) {
            int idx = (startIdx + i) % TEX_POOL_SIZE;
            if (!sl->IsInFlight(idx)) {
                ti = idx + 1;
                return idx;
            }
        }
        // All textures in-flight - use next in sequence anyway (will cause conflict)
        texConflicts.fetch_add(1, std::memory_order_relaxed);
        int idx = ti++ % TEX_POOL_SIZE;
        return idx;
    }

    void OnFrame(WGC::Direct3D11CaptureFramePool const& snd, winrt::Windows::Foundation::IInspectable const&) {
        if (!run.load(std::memory_order_acquire) || !cap.load(std::memory_order_acquire)) return;
        int64_t ts = GetTimestamp(), iv = 1000000 / tfps.load(std::memory_order_relaxed);
        auto fr = snd.TryGetNextFrame();
        if (!fr) return;
        if (fsr.exchange(false, std::memory_order_acq_rel)) nft = ts + iv;
        else if (ts < nft) return;
        while (nft <= ts) nft += iv;
        auto sf = fr.Surface();
        if (!sf) return;
        winrt::com_ptr<ID3D11Texture2D> src;
        if (FAILED(sf.as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>()
                ->GetInterface(IID_PPV_ARGS(src.put()))) || !src) return;

        int texIdx = FindAvailableTexture();
        if (!tp[texIdx]) return;

        { MTLock lk(mt); ctx->CopyResource(tp[texIdx], src.get()); ctx->Flush(); }
        sl->Push(tp[texIdx], ts, gs.Signal(ctx), texIdx);
    }

    void InitMon(HMONITOR hm) {
        MONITORINFOEXW mi{sizeof(mi)}; DEVMODEW dm{.dmSize = sizeof(dm)};
        if (GetMonitorInfoW(hm, &mi) && EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm))
            hfps = tfps = dm.dmDisplayFrequency;
        auto ip = winrt::get_activation_factory<WGC::GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
        if (FAILED(ip->CreateForMonitor(hm, winrt::guid_of<ABI::Windows::Graphics::Capture::IGraphicsCaptureItem>(), winrt::put_abi(item))))
            throw std::runtime_error("CreateForMonitor failed");
        w = item.Size().Width; h = item.Size().Height;
        for (auto& t : tp) SafeRelease(t);
        D3D11_TEXTURE2D_DESC td = {(UINT)w, (UINT)h, 1, 1, DXGI_FORMAT_B8G8R8A8_UNORM, {1, 0},
            D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET, 0, D3D11_RESOURCE_MISC_SHARED};
        for (int i = 0; i < TEX_POOL_SIZE; i++)
            if (FAILED(dev->CreateTexture2D(&td, 0, &tp[i]))) throw std::runtime_error("CreateTexture2D failed");
        pool = WGC::Direct3D11CaptureFramePool::CreateFreeThreaded(wdev, WGD::DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, {w, h});
        pool.FrameArrived({this, &ScreenCapture::OnFrame});
        sess = pool.CreateCaptureSession(item);
        sess.IsCursorCaptureEnabled(true);
        try { sess.IsBorderRequired(false); } catch (...) {}
        chm = hm;
    }

    void ApplyMinInterval() {
        if (smi) try { sess.MinUpdateInterval(duration<int64_t, std::ratio<1, 10000000>>(10000000 / hfps)); } catch (...) {}
    }

public:
    ScreenCapture(FrameSlot* s) : sl(s) {
        LOG("Initializing capture...");
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        UINT fl = D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
        #ifdef _DEBUG
        fl |= D3D11_CREATE_DEVICE_DEBUG;
        #endif
        D3D_FEATURE_LEVEL lv[] = {D3D_FEATURE_LEVEL_12_1, D3D_FEATURE_LEVEL_12_0, D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0}, ol;
        if (FAILED(D3D11CreateDevice(0, D3D_DRIVER_TYPE_HARDWARE, 0, fl, lv, 4, D3D11_SDK_VERSION, &dev, &ol, &ctx)))
            throw std::runtime_error("D3D11CreateDevice failed");

        winrt::com_ptr<IDXGIDevice> dd; winrt::com_ptr<IDXGIAdapter> ad;
        if (SUCCEEDED(dev->QueryInterface(IID_PPV_ARGS(dd.put()))) && SUCCEEDED(dd->GetAdapter(ad.put()))) {
            DXGI_ADAPTER_DESC ds; char nm[256];
            if (SUCCEEDED(ad->GetDesc(&ds))) {
                WideCharToMultiByte(CP_UTF8, 0, ds.Description, -1, nm, sizeof(nm), 0, 0);
                LOG("GPU: %s (VRAM: %llu MB)", nm, ds.DedicatedVideoMemory >> 20);
            }
        }
        if (SUCCEEDED(dev->QueryInterface(IID_PPV_ARGS(&mt)))) mt->SetMultithreadProtected(TRUE);
        if (!gs.Init(dev, ctx)) throw std::runtime_error("GPU sync init failed");

        winrt::com_ptr<IDXGIDevice> dx; winrt::com_ptr<::IInspectable> ip;
        if (FAILED(dev->QueryInterface(IID_PPV_ARGS(dx.put()))) || FAILED(CreateDirect3D11DeviceFromDXGIDevice(dx.get(), ip.put())))
            throw std::runtime_error("WinRT device creation failed");
        wdev = ip.as<WGD::Direct3D11::IDirect3DDevice>();
        RefreshMonitorList();
        smi = winrt::Windows::Foundation::Metadata::ApiInformation::IsPropertyPresent(
            L"Windows.Graphics.Capture.GraphicsCaptureSession", L"MinUpdateInterval");
        InitMon(MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY));
        LOG("Capture initialized: %dx%d @ %dHz (pool: %d textures)", w, h, hfps, TEX_POOL_SIZE);
    }

    ~ScreenCapture() {
        run.store(false, std::memory_order_release);
        cap.store(false, std::memory_order_release);
        try { if (sess) sess.Close(); } catch (...) {}
        try { if (pool) pool.Close(); } catch (...) {}
        for (auto& t : tp) SafeRelease(t);
        SafeRelease(mt, ctx, dev);
        winrt::uninit_apartment();
    }

    void SetResolutionChangeCallback(std::function<void(int, int, int)> cb) { onRes = cb; }

    void StartCapture() {
        std::lock_guard<std::mutex> lk(cmx);
        if (cap.load(std::memory_order_relaxed)) return;
        fsr.store(true, std::memory_order_release);
        ApplyMinInterval(); sess.StartCapture();
        cap.store(true, std::memory_order_release);
        LOG("Capture started at %dHz, target %d FPS", hfps, tfps.load());
    }

    void PauseCapture() {
        if (!cap.load(std::memory_order_relaxed)) return;
        cap.store(false, std::memory_order_release);
        LOG("Capture paused");
    }

    bool SwitchMonitor(int idx) {
        std::lock_guard<std::mutex> lk(cmx);
        std::lock_guard<std::mutex> ml(g_monitorsMutex);
        if (idx < 0 || idx >= (int)g_monitors.size()) { WARN("Invalid monitor index: %d", idx); return false; }
        if (cmi == idx && chm == g_monitors[idx].hMon) { LOG("Already on monitor %d", idx); return true; }
        bool wc = cap.load(std::memory_order_relaxed);
        cap.store(false, std::memory_order_release);
        try { if (sess) sess.Close(); } catch (...) {}
        try { if (pool) pool.Close(); } catch (...) {}
        sess = nullptr; pool = nullptr; item = nullptr;
        try {
            InitMon(g_monitors[idx].hMon); cmi = idx;
            LOG("Switched to monitor %d: %s (%dx%d @ %dHz)", idx, g_monitors[idx].name.c_str(), w, h, hfps);
            if (onRes) onRes(w, h, hfps);
            if (wc) { fsr.store(true, std::memory_order_release); ApplyMinInterval(); sess.StartCapture(); cap.store(true, std::memory_order_release); }
            return true;
        } catch (const std::exception& e) { ERR("Failed to switch monitor: %s", e.what()); return false; }
    }

    bool SetFPS(int fps) {
        if (fps < 1 || fps > 240) return false;
        int of = tfps.exchange(fps, std::memory_order_acq_rel);
        if (of != fps) { fsr.store(true, std::memory_order_release); LOG("FPS: %d -> %d", of, fps); }
        return true;
    }

    int RefreshHostFPS() {
        if (chm) {
            MONITORINFOEXW mi{sizeof(mi)}; DEVMODEW dm{.dmSize = sizeof(dm)};
            if (GetMonitorInfoW(chm, &mi) && EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm))
                hfps = dm.dmDisplayFrequency;
        }
        return hfps;
    }

    int GetCurrentMonitorIndex() const { return cmi.load(std::memory_order_relaxed); }
    int GetHostFPS() const { return hfps; }
    int GetCurrentFPS() const { return tfps.load(std::memory_order_relaxed); }
    bool IsCapturing() const { return cap.load(std::memory_order_relaxed); }
    bool IsReady(uint64_t f) { return gs.IsComplete(f, ctx); }
    bool WaitReady(uint64_t f) { return gs.Wait(f, ctx); }
    uint64_t GetTexConflicts() { return texConflicts.exchange(0); }
    ID3D11Device* GetDev() const { return dev; }
    ID3D11DeviceContext* GetCtx() const { return ctx; }
    ID3D11Multithread* GetMT() const { return mt; }
    int GetW() const { return w; }
    int GetH() const { return h; }
};
