#pragma once
#include "common.hpp"
#include <shellapi.h>
#include <shlobj.h>

// MSG_CLIPBOARD_* constants are defined in common.hpp

constexpr size_t MAX_CLIPBOARD_SIZE = 10 * 1024 * 1024; // 10MB max
constexpr size_t MAX_CLIPBOARD_TEXT_SIZE = 1 * 1024 * 1024;  // 1MB text max

#pragma pack(push, 1)
struct ClipboardTextMsg {
    uint32_t magic;
    uint32_t length;
    // UTF-8 text follows
};

struct ClipboardImageMsg {
    uint32_t magic;
    uint32_t width;
    uint32_t height;
    uint32_t dataLength;
    // PNG data follows
};
#pragma pack(pop)

class ClipboardSync {
    HWND hwnd = nullptr;
    std::thread monitorThread;
    std::atomic<bool> running{false};
    std::atomic<bool> enabled{true};
    std::mutex mtx;
    std::string lastSentText;
    std::vector<uint8_t> lastSentImage;
    uint64_t lastSentHash = 0;
    std::function<void(const std::vector<uint8_t>&)> onClipboardChange;
    std::atomic<bool> ignoreNextChange{false};

    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
        if (msg == WM_CLIPBOARDUPDATE) {
            auto* self = (ClipboardSync*)GetWindowLongPtr(hwnd, GWLP_USERDATA);
            if (self && self->enabled && !self->ignoreNextChange) {
                self->OnClipboardChanged();
            }
            self->ignoreNextChange = false;
            return 0;
        }
        return DefWindowProc(hwnd, msg, wp, lp);
    }

    uint64_t HashData(const void* data, size_t len) {
        uint64_t hash = 0xcbf29ce484222325ULL;
        const uint8_t* p = (const uint8_t*)data;
        for (size_t i = 0; i < len; i++) {
            hash ^= p[i];
            hash *= 0x100000001b3ULL;
        }
        return hash;
    }

    void OnClipboardChanged() {
        if (!OpenClipboard(hwnd)) return;

        std::vector<uint8_t> packet;

        // Try text first (most common)
        if (IsClipboardFormatAvailable(CF_UNICODETEXT)) {
            HANDLE hText = GetClipboardData(CF_UNICODETEXT);
            if (hText) {
                const wchar_t* wstr = (const wchar_t*)GlobalLock(hText);
                if (wstr) {
                    int len = WideCharToMultiByte(CP_UTF8, 0, wstr, -1, nullptr, 0, nullptr, nullptr);
                    if (len > 0 && len <= (int)MAX_CLIPBOARD_TEXT_SIZE) {
                        std::string utf8(len, 0);
                        WideCharToMultiByte(CP_UTF8, 0, wstr, -1, utf8.data(), len, nullptr, nullptr);
                        utf8.resize(strlen(utf8.c_str())); // Remove null terminator from length

                        uint64_t hash = HashData(utf8.data(), utf8.size());
                        if (hash != lastSentHash) {
                            lastSentHash = hash;
                            lastSentText = utf8;

                            packet.resize(sizeof(ClipboardTextMsg) + utf8.size());
                            auto* msg = (ClipboardTextMsg*)packet.data();
                            msg->magic = MSG_CLIPBOARD_TEXT;
                            msg->length = (uint32_t)utf8.size();
                            memcpy(packet.data() + sizeof(ClipboardTextMsg), utf8.data(), utf8.size());

                            LOG("Clipboard: sending %zu bytes text", utf8.size());
                        }
                    }
                    GlobalUnlock(hText);
                }
            }
        }
        // Try DIB (image) if no text
        else if (IsClipboardFormatAvailable(CF_DIB)) {
            HANDLE hDib = GetClipboardData(CF_DIB);
            if (hDib) {
                const BITMAPINFO* bmi = (const BITMAPINFO*)GlobalLock(hDib);
                if (bmi) {
                    int imgW = bmi->bmiHeader.biWidth;
                    int imgH = abs(bmi->bmiHeader.biHeight);
                    int bpp = bmi->bmiHeader.biBitCount;

                    if (imgW > 0 && imgH > 0 && (bpp == 24 || bpp == 32) && imgW * imgH * 4 <= (int)MAX_CLIPBOARD_SIZE) {
                        const uint8_t* pixels = (const uint8_t*)bmi + bmi->bmiHeader.biSize;
                        if (bmi->bmiHeader.biClrUsed > 0) {
                            pixels += bmi->bmiHeader.biClrUsed * sizeof(RGBQUAD);
                        }

                        // Convert to RGBA
                        std::vector<uint8_t> rgba(imgW * imgH * 4);
                        int srcStride = ((imgW * bpp / 8) + 3) & ~3;
                        bool bottomUp = bmi->bmiHeader.biHeight > 0;

                        for (int y = 0; y < imgH; y++) {
                            int srcY = bottomUp ? (imgH - 1 - y) : y;
                            const uint8_t* src = pixels + srcY * srcStride;
                            uint8_t* dst = rgba.data() + y * imgW * 4;

                            for (int x = 0; x < imgW; x++) {
                                if (bpp == 32) {
                                    dst[0] = src[2]; dst[1] = src[1]; dst[2] = src[0]; dst[3] = src[3];
                                    src += 4;
                                } else {
                                    dst[0] = src[2]; dst[1] = src[1]; dst[2] = src[0]; dst[3] = 255;
                                    src += 3;
                                }
                                dst += 4;
                            }
                        }

                        // Simple PNG encoding (uncompressed for speed)
                        std::vector<uint8_t> png = EncodePNG(rgba.data(), imgW, imgH);

                        uint64_t hash = HashData(png.data(), png.size());
                        if (hash != lastSentHash && png.size() <= MAX_CLIPBOARD_SIZE) {
                            lastSentHash = hash;
                            lastSentImage = png;

                            packet.resize(sizeof(ClipboardImageMsg) + png.size());
                            auto* msg = (ClipboardImageMsg*)packet.data();
                            msg->magic = MSG_CLIPBOARD_IMAGE;
                            msg->width = imgW;
                            msg->height = imgH;
                            msg->dataLength = (uint32_t)png.size();
                            memcpy(packet.data() + sizeof(ClipboardImageMsg), png.data(), png.size());

                            LOG("Clipboard: sending %dx%d image (%zu bytes)", imgW, imgH, png.size());
                        }
                    }
                    GlobalUnlock(hDib);
                }
            }
        }

        CloseClipboard();

        if (!packet.empty() && onClipboardChange) {
            onClipboardChange(packet);
        }
    }

    // Simple PNG encoder (uncompressed)
    static std::vector<uint8_t> EncodePNG(const uint8_t* rgba, int w, int h) {
        std::vector<uint8_t> png;

        // PNG signature
        const uint8_t sig[] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
        png.insert(png.end(), sig, sig + 8);

        auto writeBE32 = [](std::vector<uint8_t>& v, uint32_t val) {
            v.push_back((val >> 24) & 0xFF);
            v.push_back((val >> 16) & 0xFF);
            v.push_back((val >> 8) & 0xFF);
            v.push_back(val & 0xFF);
        };

        auto crc32 = [](const uint8_t* data, size_t len) -> uint32_t {
            uint32_t crc = 0xFFFFFFFF;
            for (size_t i = 0; i < len; i++) {
                crc ^= data[i];
                for (int j = 0; j < 8; j++)
                    crc = (crc >> 1) ^ (0xEDB88320U & (0U - (crc & 1)));
            }
            return ~crc;
        };

        auto writeChunk = [&](const char* type, const std::vector<uint8_t>& data) {
            writeBE32(png, (uint32_t)data.size());
            size_t typeStart = png.size();
            png.insert(png.end(), type, type + 4);
            png.insert(png.end(), data.begin(), data.end());
            uint32_t c = crc32(png.data() + typeStart, 4 + data.size());
            writeBE32(png, c);
        };

        // IHDR
        std::vector<uint8_t> ihdr(13);
        ihdr[0] = (w >> 24) & 0xFF; ihdr[1] = (w >> 16) & 0xFF;
        ihdr[2] = (w >> 8) & 0xFF; ihdr[3] = w & 0xFF;
        ihdr[4] = (h >> 24) & 0xFF; ihdr[5] = (h >> 16) & 0xFF;
        ihdr[6] = (h >> 8) & 0xFF; ihdr[7] = h & 0xFF;
        ihdr[8] = 8;  // bit depth
        ihdr[9] = 6;  // RGBA
        ihdr[10] = 0; // compression
        ihdr[11] = 0; // filter
        ihdr[12] = 0; // interlace
        writeChunk("IHDR", ihdr);

        // IDAT - use zlib store (no compression for speed)
        std::vector<uint8_t> raw;
        raw.reserve(h * (1 + w * 4) + 100);

        // zlib header
        raw.push_back(0x78); raw.push_back(0x01);

        size_t rowSize = 1 + w * 4;
        for (int y = 0; y < h; y++) {
            bool isLast = (y == h - 1);
            uint16_t len = (uint16_t)rowSize;
            uint16_t nlen = ~len;

            raw.push_back(isLast ? 0x01 : 0x00);
            raw.push_back(len & 0xFF); raw.push_back(len >> 8);
            raw.push_back(nlen & 0xFF); raw.push_back(nlen >> 8);

            raw.push_back(0); // filter none
            raw.insert(raw.end(), rgba + y * w * 4, rgba + (y + 1) * w * 4);
        }

        // Adler-32
        uint32_t a = 1, b = 0;
        for (int y = 0; y < h; y++) {
            a = (a + 0) % 65521; b = (b + a) % 65521;
            for (int x = 0; x < w * 4; x++) {
                a = (a + rgba[y * w * 4 + x]) % 65521;
                b = (b + a) % 65521;
            }
        }
        uint32_t adler = (b << 16) | a;
        raw.push_back((adler >> 24) & 0xFF);
        raw.push_back((adler >> 16) & 0xFF);
        raw.push_back((adler >> 8) & 0xFF);
        raw.push_back(adler & 0xFF);

        writeChunk("IDAT", raw);

        // IEND
        writeChunk("IEND", {});

        return png;
    }

public:
    ClipboardSync() {
        LOG("Initializing clipboard sync...");

        // Create hidden window for clipboard monitoring
        WNDCLASSEXW wc = {sizeof(wc)};
        wc.lpfnWndProc = WndProc;
        wc.hInstance = GetModuleHandle(nullptr);
        wc.lpszClassName = L"ClipboardSyncWindow";
        RegisterClassExW(&wc);

        hwnd = CreateWindowExW(0, L"ClipboardSyncWindow", L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, wc.hInstance, nullptr);
        if (!hwnd) {
            ERR("Failed to create clipboard window");
            return;
        }

        SetWindowLongPtr(hwnd, GWLP_USERDATA, (LONG_PTR)this);

        if (!AddClipboardFormatListener(hwnd)) {
            ERR("Failed to add clipboard listener");
            DestroyWindow(hwnd);
            hwnd = nullptr;
            return;
        }

        running = true;
        monitorThread = std::thread([this] {
            MSG msg;
            while (running && GetMessage(&msg, hwnd, 0, 0)) {
                TranslateMessage(&msg);
                DispatchMessage(&msg);
            }
        });

        LOG("Clipboard sync initialized");
    }

    ~ClipboardSync() {
        running = false;
        if (hwnd) {
            RemoveClipboardFormatListener(hwnd);
            PostMessage(hwnd, WM_QUIT, 0, 0);
        }
        if (monitorThread.joinable()) {
            monitorThread.join();
        }
        if (hwnd) {
            DestroyWindow(hwnd);
        }
    }

    void SetOnChange(std::function<void(const std::vector<uint8_t>&)> cb) {
        onClipboardChange = cb;
    }

    void Enable() { enabled = true; LOG("Clipboard sync enabled"); }
    void Disable() { enabled = false; LOG("Clipboard sync disabled"); }
    bool IsEnabled() const { return enabled; }

    // Handle incoming clipboard data from client
    bool HandleMessage(const uint8_t* data, size_t len) {
        if (len < 4) return false;

        uint32_t magic = *(uint32_t*)data;

        if (magic == MSG_CLIPBOARD_TEXT && len >= sizeof(ClipboardTextMsg)) {
            auto* msg = (ClipboardTextMsg*)data;
            if (len >= sizeof(ClipboardTextMsg) + msg->length && msg->length <= MAX_CLIPBOARD_TEXT_SIZE) {
                std::string text((char*)data + sizeof(ClipboardTextMsg), msg->length);

                // Check if same as what we sent
                uint64_t hash = HashData(text.data(), text.size());
                if (hash == lastSentHash) return true;

                SetTextToClipboard(text);
                LOG("Clipboard: received %u bytes text from client", msg->length);
                return true;
            }
        }
        else if (magic == MSG_CLIPBOARD_IMAGE && len >= sizeof(ClipboardImageMsg)) {
            auto* msg = (ClipboardImageMsg*)data;
            if (len >= sizeof(ClipboardImageMsg) + msg->dataLength && msg->dataLength <= MAX_CLIPBOARD_SIZE) {
                const uint8_t* pngData = data + sizeof(ClipboardImageMsg);

                uint64_t hash = HashData(pngData, msg->dataLength);
                if (hash == lastSentHash) return true;

                SetImageToClipboard(pngData, msg->dataLength, msg->width, msg->height);
                LOG("Clipboard: received %dx%d image from client", msg->width, msg->height);
                return true;
            }
        }
        else if (magic == MSG_CLIPBOARD_REQUEST) {
            // Client requesting current clipboard
            ignoreNextChange = false;
            OnClipboardChanged();
            return true;
        }

        return false;
    }

    void SetTextToClipboard(const std::string& text) {
        if (!OpenClipboard(hwnd)) return;

        EmptyClipboard();
        ignoreNextChange = true;
        lastSentHash = HashData(text.data(), text.size());

        int wlen = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
        HGLOBAL hg = GlobalAlloc(GMEM_MOVEABLE, wlen * sizeof(wchar_t));
        if (hg) {
            wchar_t* wstr = (wchar_t*)GlobalLock(hg);
            MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, wstr, wlen);
            GlobalUnlock(hg);
            SetClipboardData(CF_UNICODETEXT, hg);
        }

        CloseClipboard();
    }

    void SetImageToClipboard(const uint8_t* pngData, size_t pngLen, int w, int h) {
        // Decode PNG and set as DIB
        std::vector<uint8_t> rgba = DecodePNG(pngData, pngLen, w, h);
        if (rgba.empty()) return;

        if (!OpenClipboard(hwnd)) return;

        EmptyClipboard();
        ignoreNextChange = true;
        lastSentHash = HashData(pngData, pngLen);

        // Create DIB
        size_t dibSize = sizeof(BITMAPINFOHEADER) + w * h * 4;
        HGLOBAL hg = GlobalAlloc(GMEM_MOVEABLE, dibSize);
        if (hg) {
            uint8_t* dib = (uint8_t*)GlobalLock(hg);
            BITMAPINFOHEADER* bih = (BITMAPINFOHEADER*)dib;

            bih->biSize = sizeof(BITMAPINFOHEADER);
            bih->biWidth = w;
            bih->biHeight = -h; // top-down
            bih->biPlanes = 1;
            bih->biBitCount = 32;
            bih->biCompression = BI_RGB;
            bih->biSizeImage = w * h * 4;
            bih->biXPelsPerMeter = 0;
            bih->biYPelsPerMeter = 0;
            bih->biClrUsed = 0;
            bih->biClrImportant = 0;

            // Convert RGBA to BGRA
            uint8_t* dst = dib + sizeof(BITMAPINFOHEADER);
            for (int i = 0; i < w * h; i++) {
                dst[i * 4 + 0] = rgba[i * 4 + 2]; // B
                dst[i * 4 + 1] = rgba[i * 4 + 1]; // G
                dst[i * 4 + 2] = rgba[i * 4 + 0]; // R
                dst[i * 4 + 3] = rgba[i * 4 + 3]; // A
            }

            GlobalUnlock(hg);
            SetClipboardData(CF_DIB, hg);
        }

        CloseClipboard();
    }

    // Simple PNG decoder for incoming images
    static std::vector<uint8_t> DecodePNG(const uint8_t* data, size_t len, int expectedW, int expectedH) {
        // Validate PNG signature
        if (len < 8 || data[0] != 0x89 || data[1] != 'P' || data[2] != 'N' || data[3] != 'G') {
            return {};
        }

        std::vector<uint8_t> rgba(expectedW * expectedH * 4);

        // Find IDAT chunk and decompress
        size_t pos = 8;
        std::vector<uint8_t> compressed;

        while (pos + 12 <= len) {
            uint32_t chunkLen = (data[pos] << 24) | (data[pos+1] << 16) | (data[pos+2] << 8) | data[pos+3];
            const char* type = (const char*)&data[pos + 4];

            if (memcmp(type, "IDAT", 4) == 0 && pos + 8 + chunkLen <= len) {
                compressed.insert(compressed.end(), data + pos + 8, data + pos + 8 + chunkLen);
            }

            pos += 12 + chunkLen;
        }

        if (compressed.size() < 6) return {};

        // Skip zlib header (2 bytes) and decompress stored blocks
        size_t cpos = 2;
        std::vector<uint8_t> raw;
        raw.reserve(expectedH * (1 + expectedW * 4));

        while (cpos + 5 <= compressed.size()) {
            bool isLastBlock = (compressed[cpos] & 0x01) != 0;
            uint16_t blockLen = compressed[cpos + 1] | (compressed[cpos + 2] << 8);
            cpos += 5;

            if (cpos + blockLen > compressed.size()) break;
            raw.insert(raw.end(), compressed.begin() + cpos, compressed.begin() + cpos + blockLen);
            cpos += blockLen;

            if (isLastBlock) break;
        }

        // Parse filter bytes and copy pixels
        size_t rowSize = 1 + expectedW * 4;
        for (int y = 0; y < expectedH; y++) {
            size_t rowStart = y * rowSize;
            if (rowStart + rowSize > raw.size()) break;

            // Skip filter byte (0 = none)
            memcpy(rgba.data() + y * expectedW * 4, raw.data() + rowStart + 1, expectedW * 4);
        }

        return rgba;
    }

    // Send current clipboard to client on connect
    void SendCurrentClipboard() {
        ignoreNextChange = false;
        OnClipboardChanged();
    }
};
