#pragma once
#include "common.hpp"
#include <shellapi.h>
#include <shlobj.h>

constexpr size_t MAX_CLIPBOARD_SIZE = 10 * 1024 * 1024, MAX_CLIPBOARD_TEXT = 1 * 1024 * 1024;

#pragma pack(push, 1)
struct ClipboardTextMsg { uint32_t magic, length; };
struct ClipboardImageMsg { uint32_t magic, width, height, dataLength; };
#pragma pack(pop)

class ClipboardSync {
    HWND hwnd = nullptr; std::thread monitorThread; std::atomic<bool> running{false}, enabled{true}, ignoreNext{false};
    uint64_t lastHash = 0; std::function<void(const std::vector<uint8_t>&)> onChange;

    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM, LPARAM) {
        if (msg == WM_CLIPBOARDUPDATE) {
            auto* self = (ClipboardSync*)GetWindowLongPtr(hwnd, GWLP_USERDATA);
            if (self && self->enabled && !self->ignoreNext) self->OnChanged();
            self->ignoreNext = false;
        }
        return DefWindowProc(hwnd, msg, 0, 0);
    }

    uint64_t Hash(const void* data, size_t len) {
        uint64_t h = 0xcbf29ce484222325ULL;
        for (size_t i = 0; i < len; i++) { h ^= ((uint8_t*)data)[i]; h *= 0x100000001b3ULL; }
        return h;
    }

    void OnChanged() {
        if (!OpenClipboard(hwnd)) return;
        std::vector<uint8_t> packet;

        if (IsClipboardFormatAvailable(CF_UNICODETEXT)) {
            if (HANDLE h = GetClipboardData(CF_UNICODETEXT)) {
                if (const wchar_t* ws = (wchar_t*)GlobalLock(h)) {
                    int len = WideCharToMultiByte(CP_UTF8, 0, ws, -1, nullptr, 0, nullptr, nullptr);
                    if (len > 0 && len <= (int)MAX_CLIPBOARD_TEXT) {
                        std::string utf8(len, 0); WideCharToMultiByte(CP_UTF8, 0, ws, -1, utf8.data(), len, nullptr, nullptr);
                        utf8.resize(strlen(utf8.c_str()));
                        uint64_t hash = Hash(utf8.data(), utf8.size());
                        if (hash != lastHash) {
                            lastHash = hash;
                            packet.resize(sizeof(ClipboardTextMsg) + utf8.size());
                            auto* msg = (ClipboardTextMsg*)packet.data();
                            msg->magic = MSG_CLIPBOARD_TEXT; msg->length = (uint32_t)utf8.size();
                            memcpy(packet.data() + sizeof(ClipboardTextMsg), utf8.data(), utf8.size());
                        }
                    }
                    GlobalUnlock(h);
                }
            }
        } else if (IsClipboardFormatAvailable(CF_DIB)) {
            if (HANDLE h = GetClipboardData(CF_DIB)) {
                if (const BITMAPINFO* bmi = (BITMAPINFO*)GlobalLock(h)) {
                    int imgW = bmi->bmiHeader.biWidth, imgH = abs(bmi->bmiHeader.biHeight), bpp = bmi->bmiHeader.biBitCount;
                    if (imgW > 0 && imgH > 0 && (bpp == 24 || bpp == 32) && imgW * imgH * 4 <= (int)MAX_CLIPBOARD_SIZE) {
                        const uint8_t* pixels = (uint8_t*)bmi + bmi->bmiHeader.biSize + bmi->bmiHeader.biClrUsed * sizeof(RGBQUAD);
                        std::vector<uint8_t> rgba(imgW * imgH * 4);
                        int stride = ((imgW * bpp / 8) + 3) & ~3; bool flip = bmi->bmiHeader.biHeight > 0;
                        for (int y = 0; y < imgH; y++) {
                            const uint8_t* src = pixels + (flip ? imgH - 1 - y : y) * stride;
                            uint8_t* dst = rgba.data() + y * imgW * 4;
                            for (int x = 0; x < imgW; x++, dst += 4, src += bpp/8)
                                { dst[0] = src[2]; dst[1] = src[1]; dst[2] = src[0]; dst[3] = bpp == 32 ? src[3] : 255; }
                        }
                        auto png = EncodePNG(rgba.data(), imgW, imgH);
                        uint64_t hash = Hash(png.data(), png.size());
                        if (hash != lastHash && png.size() <= MAX_CLIPBOARD_SIZE) {
                            lastHash = hash;
                            packet.resize(sizeof(ClipboardImageMsg) + png.size());
                            auto* msg = (ClipboardImageMsg*)packet.data();
                            msg->magic = MSG_CLIPBOARD_IMAGE; msg->width = imgW; msg->height = imgH; msg->dataLength = (uint32_t)png.size();
                            memcpy(packet.data() + sizeof(ClipboardImageMsg), png.data(), png.size());
                        }
                    }
                    GlobalUnlock(h);
                }
            }
        }
        CloseClipboard();
        if (!packet.empty() && onChange) onChange(packet);
    }

    static std::vector<uint8_t> EncodePNG(const uint8_t* rgba, int w, int h) {
        std::vector<uint8_t> png = {0x89,'P','N','G',0x0D,0x0A,0x1A,0x0A};
        auto w32 = [&](uint32_t v) { png.insert(png.end(), {(uint8_t)(v>>24),(uint8_t)(v>>16),(uint8_t)(v>>8),(uint8_t)v}); };
        auto crc = [](const uint8_t* d, size_t l) { uint32_t c = ~0u; for (size_t i = 0; i < l; i++) { c ^= d[i]; for (int j = 0; j < 8; j++) c = (c >> 1) ^ (0xEDB88320U & (0U - (c & 1))); } return ~c; };
        auto chunk = [&](const char* t, const std::vector<uint8_t>& d) {
            w32((uint32_t)d.size()); size_t s = png.size();
            png.insert(png.end(), t, t + 4); png.insert(png.end(), d.begin(), d.end());
            w32(crc(png.data() + s, 4 + d.size()));
        };
        std::vector<uint8_t> ihdr = {(uint8_t)(w>>24),(uint8_t)(w>>16),(uint8_t)(w>>8),(uint8_t)w,
            (uint8_t)(h>>24),(uint8_t)(h>>16),(uint8_t)(h>>8),(uint8_t)h,8,6,0,0,0};
        chunk("IHDR", ihdr);
        std::vector<uint8_t> raw = {0x78, 0x01};
        for (int y = 0; y < h; y++) {
            uint16_t len = static_cast<uint16_t>(1 + w * 4), nlen = static_cast<uint16_t>(~len);
            raw.insert(raw.end(), {(uint8_t)(y == h-1), (uint8_t)len, (uint8_t)(len>>8), (uint8_t)nlen, (uint8_t)(nlen>>8), 0});
            raw.insert(raw.end(), rgba + y * w * 4, rgba + (y + 1) * w * 4);
        }
        uint32_t a = 1, b = 0;
        for (int y = 0; y < h; y++) { a = (a + 0) % 65521; b = (b + a) % 65521; for (int x = 0; x < w * 4; x++) { a = (a + rgba[y * w * 4 + x]) % 65521; b = (b + a) % 65521; } }
        uint32_t adler = (b << 16) | a;
        raw.insert(raw.end(), {(uint8_t)(adler>>24),(uint8_t)(adler>>16),(uint8_t)(adler>>8),(uint8_t)adler});
        chunk("IDAT", raw); chunk("IEND", {});
        return png;
    }

    static std::vector<uint8_t> DecodePNG(const uint8_t* data, size_t len, int w, int h) {
        if (len < 8 || memcmp(data, "\x89PNG\r\n\x1a\n", 8)) return {};
        std::vector<uint8_t> comp, rgba(w * h * 4);
        for (size_t p = 8; p + 12 <= len; ) {
            uint32_t cl = (data[p]<<24)|(data[p+1]<<16)|(data[p+2]<<8)|data[p+3];
            if (!memcmp(&data[p+4], "IDAT", 4) && p + 8 + cl <= len) comp.insert(comp.end(), data + p + 8, data + p + 8 + cl);
            p += 12 + cl;
        }
        if (comp.size() < 6) return {};
        std::vector<uint8_t> raw; size_t cp = 2;
        while (cp + 5 <= comp.size()) {
            uint16_t bl = comp[cp+1] | (comp[cp+2] << 8); bool isLast = comp[cp] & 1; cp += 5;
            if (cp + bl > comp.size()) break;
            raw.insert(raw.end(), comp.begin() + cp, comp.begin() + cp + bl); cp += bl;
            if (isLast) break;
        }
        size_t rs = 1 + w * 4;
        for (int y = 0; y < h && y * rs + rs <= raw.size(); y++)
            memcpy(rgba.data() + y * w * 4, raw.data() + y * rs + 1, w * 4);
        return rgba;
    }

public:
    ClipboardSync() {
        WNDCLASSEXW wc = {sizeof(wc), 0, WndProc, 0, 0, GetModuleHandle(0), 0, 0, 0, 0, L"ClipSync"};
        RegisterClassExW(&wc);
        hwnd = CreateWindowExW(0, L"ClipSync", L"", 0, 0, 0, 0, 0, HWND_MESSAGE, 0, wc.hInstance, 0);
        if (!hwnd || !AddClipboardFormatListener(hwnd)) { if (hwnd) DestroyWindow(hwnd); hwnd = nullptr; return; }
        SetWindowLongPtr(hwnd, GWLP_USERDATA, (LONG_PTR)this);
        running = true;
        monitorThread = std::thread([this] { MSG msg; while (running && GetMessage(&msg, hwnd, 0, 0)) { TranslateMessage(&msg); DispatchMessage(&msg); } });
        LOG("Clipboard sync initialized");
    }

    ~ClipboardSync() {
        running = false;
        if (hwnd) { RemoveClipboardFormatListener(hwnd); PostMessage(hwnd, WM_QUIT, 0, 0); }
        if (monitorThread.joinable()) monitorThread.join();
        if (hwnd) DestroyWindow(hwnd);
    }

    void SetOnChange(std::function<void(const std::vector<uint8_t>&)> cb) { onChange = cb; }
    void Enable() { enabled = true; }
    void Disable() { enabled = false; }

    bool HandleMessage(const uint8_t* data, size_t len) {
        if (len < 4) return false;
        uint32_t magic = *(uint32_t*)data;
        if (magic == MSG_CLIPBOARD_TEXT && len >= sizeof(ClipboardTextMsg)) {
            auto* msg = (ClipboardTextMsg*)data;
            if (len >= sizeof(ClipboardTextMsg) + msg->length && msg->length <= MAX_CLIPBOARD_TEXT) {
                std::string text((char*)data + sizeof(ClipboardTextMsg), msg->length);
                if (Hash(text.data(), text.size()) != lastHash) SetText(text);
                return true;
            }
        } else if (magic == MSG_CLIPBOARD_IMAGE && len >= sizeof(ClipboardImageMsg)) {
            auto* msg = (ClipboardImageMsg*)data;
            if (len >= sizeof(ClipboardImageMsg) + msg->dataLength && msg->dataLength <= MAX_CLIPBOARD_SIZE) {
                const uint8_t* png = data + sizeof(ClipboardImageMsg);
                if (Hash(png, msg->dataLength) != lastHash) SetImage(png, msg->dataLength, msg->width, msg->height);
                return true;
            }
        } else if (magic == MSG_CLIPBOARD_REQUEST) { ignoreNext = false; OnChanged(); return true; }
        return false;
    }

    void SetText(const std::string& text) {
        if (!OpenClipboard(hwnd)) return;
        EmptyClipboard(); ignoreNext = true; lastHash = Hash(text.data(), text.size());
        int wlen = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
        if (HGLOBAL hg = GlobalAlloc(GMEM_MOVEABLE, wlen * sizeof(wchar_t))) {
            MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, (wchar_t*)GlobalLock(hg), wlen);
            GlobalUnlock(hg); SetClipboardData(CF_UNICODETEXT, hg);
        }
        CloseClipboard();
    }

    void SetImage(const uint8_t* png, size_t len, int w, int h) {
        auto rgba = DecodePNG(png, len, w, h); if (rgba.empty()) return;
        if (!OpenClipboard(hwnd)) return;
        EmptyClipboard(); ignoreNext = true; lastHash = Hash(png, len);
        size_t dibSize = sizeof(BITMAPINFOHEADER) + w * h * 4;
        if (HGLOBAL hg = GlobalAlloc(GMEM_MOVEABLE, dibSize)) {
            auto* bih = (BITMAPINFOHEADER*)GlobalLock(hg);
            *bih = {sizeof(BITMAPINFOHEADER), w, -h, 1, 32, BI_RGB, (DWORD)(w * h * 4)};
            uint8_t* dst = (uint8_t*)bih + sizeof(BITMAPINFOHEADER);
            for (int i = 0; i < w * h; i++) { dst[i*4] = rgba[i*4+2]; dst[i*4+1] = rgba[i*4+1]; dst[i*4+2] = rgba[i*4]; dst[i*4+3] = rgba[i*4+3]; }
            GlobalUnlock(hg); SetClipboardData(CF_DIB, hg);
        }
        CloseClipboard();
    }

    void SendCurrentClipboard() { ignoreNext = false; OnChanged(); }
};
