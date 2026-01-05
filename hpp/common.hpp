#pragma once
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include <windows.h>
#include <d3d11_4.h>
#include <dxgi1_2.h>
#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Metadata.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <Windows.Graphics.Capture.Interop.h>
#include <Windows.Graphics.DirectX.Direct3D11.Interop.h>
#include <rtc/rtc.hpp>
#include <httplib.h>
#include <nlohmann/json.hpp>
#include <thread>
#include <atomic>
#include <vector>
#include <chrono>
#include <fstream>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <algorithm>
#include <unordered_map>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_d3d11va.h>
}

using json = nlohmann::json;
using namespace std::chrono;
namespace WGC = winrt::Windows::Graphics::Capture;
namespace WGD = winrt::Windows::Graphics::DirectX;

#define LOG(f, ...)  printf("[LOG] " f "\n", ##__VA_ARGS__)
#define WARN(f, ...) printf("\033[33m[WARN] " f "\033[0m\n", ##__VA_ARGS__)
#define ERR(f, ...)  fprintf(stderr, "\033[31m[ERR] " f "\033[0m\n", ##__VA_ARGS__)

constexpr uint32_t MSG_PING = 0x504E4750, MSG_FPS_SET = 0x46505343, MSG_HOST_INFO = 0x484F5354;
constexpr uint32_t MSG_FPS_ACK = 0x46505341, MSG_REQUEST_KEY = 0x4B455952;
constexpr uint32_t MSG_MONITOR_LIST = 0x4D4F4E4C, MSG_MONITOR_SET = 0x4D4F4E53, MSG_AUDIO_DATA = 0x41554449;

constexpr uint32_t MSG_CLIPBOARD_TEXT = 0x434C5054, MSG_CLIPBOARD_IMAGE = 0x434C5049;
constexpr uint32_t MSG_CLIPBOARD_REQUEST = 0x434C5052, MSG_CLIPBOARD_ACK = 0x434C5041;

// WebRTC-based authentication messages
constexpr uint32_t MSG_AUTH_REQUEST = 0x41555448;   // "AUTH"
constexpr uint32_t MSG_AUTH_RESPONSE = 0x41555452;  // "AUTR"

inline int64_t GetTimestamp() {
    FILETIME ft; GetSystemTimePreciseAsFileTime(&ft);
    ULARGE_INTEGER ul = {ft.dwLowDateTime, ft.dwHighDateTime};
    return (int64_t)((ul.QuadPart - 116444736000000000ULL) / 10);
}

template<typename... T> void SafeRelease(T*&... p) { ((p ? (p->Release(), p = nullptr) : nullptr), ...); }

struct MTLock {
    ID3D11Multithread* m;
    MTLock(ID3D11Multithread* m) : m(m) { if (m) m->Enter(); }
    ~MTLock() { if (m) m->Leave(); }
    MTLock(const MTLock&) = delete;
    MTLock& operator=(const MTLock&) = delete;
};
