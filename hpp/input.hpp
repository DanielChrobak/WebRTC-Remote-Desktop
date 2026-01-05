#pragma once
#include "common.hpp"

constexpr uint32_t MSG_MOUSE_MOVE = 0x4D4F5645, MSG_MOUSE_BTN = 0x4D42544E;
constexpr uint32_t MSG_MOUSE_WHEEL = 0x4D57484C, MSG_KEY = 0x4B455920;

#pragma pack(push, 1)
struct MouseMoveMsg { uint32_t magic; float x, y; };
struct MouseBtnMsg { uint32_t magic; uint8_t button, action; };
struct MouseWheelMsg { uint32_t magic; int16_t deltaX, deltaY; float x, y; };
struct KeyMsg { uint32_t magic; uint16_t keyCode, scanCode; uint8_t action, modifiers; };
#pragma pack(pop)

inline WORD JsKeyToVK(uint16_t k) {
    if ((k >= 65 && k <= 90) || (k >= 48 && k <= 57)) return (WORD)k;
    static const std::unordered_map<uint16_t, WORD> m = {
        {112,VK_F1},{113,VK_F2},{114,VK_F3},{115,VK_F4},{116,VK_F5},{117,VK_F6},
        {118,VK_F7},{119,VK_F8},{120,VK_F9},{121,VK_F10},{122,VK_F11},{123,VK_F12},
        {37,VK_LEFT},{38,VK_UP},{39,VK_RIGHT},{40,VK_DOWN},{33,VK_PRIOR},{34,VK_NEXT},
        {35,VK_END},{36,VK_HOME},{45,VK_INSERT},{46,VK_DELETE},{16,VK_SHIFT},{17,VK_CONTROL},
        {18,VK_MENU},{91,VK_LWIN},{92,VK_RWIN},{8,VK_BACK},{9,VK_TAB},{13,VK_RETURN},
        {27,VK_ESCAPE},{32,VK_SPACE},{20,VK_CAPITAL},{144,VK_NUMLOCK},{145,VK_SCROLL},
        {19,VK_PAUSE},{44,VK_SNAPSHOT},{186,VK_OEM_1},{187,VK_OEM_PLUS},{188,VK_OEM_COMMA},
        {189,VK_OEM_MINUS},{190,VK_OEM_PERIOD},{191,VK_OEM_2},{192,VK_OEM_3},{219,VK_OEM_4},
        {220,VK_OEM_5},{221,VK_OEM_6},{222,VK_OEM_7},{96,VK_NUMPAD0},{97,VK_NUMPAD1},
        {98,VK_NUMPAD2},{99,VK_NUMPAD3},{100,VK_NUMPAD4},{101,VK_NUMPAD5},{102,VK_NUMPAD6},
        {103,VK_NUMPAD7},{104,VK_NUMPAD8},{105,VK_NUMPAD9},{106,VK_MULTIPLY},{107,VK_ADD},
        {109,VK_SUBTRACT},{110,VK_DECIMAL},{111,VK_DIVIDE},{173,VK_VOLUME_MUTE},
        {174,VK_VOLUME_DOWN},{175,VK_VOLUME_UP},{176,VK_MEDIA_NEXT_TRACK},
        {177,VK_MEDIA_PREV_TRACK},{178,VK_MEDIA_STOP},{179,VK_MEDIA_PLAY_PAUSE},
    };
    auto it = m.find(k); return it != m.end() ? it->second : 0;
}

class InputHandler {
    std::atomic<int> monX{0}, monY{0}, monW{1920}, monH{1080};
    std::atomic<bool> enabled{false};
    std::atomic<uint64_t> moveCount{0}, clickCount{0}, keyCount{0};

    void ToAbsolute(float nx, float ny, LONG& ax, LONG& ay) {
        int mx = monX.load(), my = monY.load(), mw = monW.load(), mh = monH.load();
        int px = mx + (int)(std::clamp(nx, 0.f, 1.f) * mw);
        int py = my + (int)(std::clamp(ny, 0.f, 1.f) * mh);
        int vsX = GetSystemMetrics(SM_XVIRTUALSCREEN), vsY = GetSystemMetrics(SM_YVIRTUALSCREEN);
        int vsW = GetSystemMetrics(SM_CXVIRTUALSCREEN), vsH = GetSystemMetrics(SM_CYVIRTUALSCREEN);
        ax = (LONG)((px - vsX) * 65535 / vsW); ay = (LONG)((py - vsY) * 65535 / vsH);
    }

    static bool IsExtendedKey(WORD vk) {
        return vk == VK_INSERT || vk == VK_DELETE || vk == VK_HOME || vk == VK_END || vk == VK_PRIOR ||
            vk == VK_NEXT || vk == VK_LEFT || vk == VK_RIGHT || vk == VK_UP || vk == VK_DOWN ||
            vk == VK_LWIN || vk == VK_RWIN || vk == VK_APPS || vk == VK_DIVIDE || vk == VK_NUMLOCK;
    }

public:
    InputHandler() { LOG("Input handler initialized"); }

    void SetMonitorBounds(int x, int y, int w, int h) {
        monX = x; monY = y; monW = w; monH = h;
        LOG("Input bounds: %d,%d %dx%d", x, y, w, h);
    }

    void UpdateFromMonitorInfo(const struct MonitorInfo& mi) {
        MONITORINFO info{sizeof(info)};
        if (GetMonitorInfo(mi.hMon, &info))
            SetMonitorBounds(info.rcMonitor.left, info.rcMonitor.top,
                info.rcMonitor.right - info.rcMonitor.left, info.rcMonitor.bottom - info.rcMonitor.top);
    }

    void Enable() { enabled = true; LOG("Input enabled"); }
    void Disable() { enabled = false; LOG("Input disabled"); }
    bool IsEnabled() const { return enabled.load(); }

    void MouseMove(float nx, float ny) {
        if (!enabled) return;
        LONG ax, ay; ToAbsolute(nx, ny, ax, ay);
        INPUT inp{}; inp.type = INPUT_MOUSE;
        inp.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
        inp.mi.dx = ax; inp.mi.dy = ay;
        SendInput(1, &inp, sizeof(INPUT)); moveCount++;
    }

    void MouseButton(uint8_t btn, bool down) {
        if (!enabled || btn > 4) return;
        static const DWORD flags[5][2] = {
            {MOUSEEVENTF_LEFTUP, MOUSEEVENTF_LEFTDOWN}, {MOUSEEVENTF_RIGHTUP, MOUSEEVENTF_RIGHTDOWN},
            {MOUSEEVENTF_MIDDLEUP, MOUSEEVENTF_MIDDLEDOWN}, {MOUSEEVENTF_XUP, MOUSEEVENTF_XDOWN},
            {MOUSEEVENTF_XUP, MOUSEEVENTF_XDOWN}
        };
        INPUT inp{}; inp.type = INPUT_MOUSE;
        inp.mi.dwFlags = flags[btn][down ? 1 : 0];
        if (btn >= 3) inp.mi.mouseData = btn == 3 ? XBUTTON1 : XBUTTON2;
        SendInput(1, &inp, sizeof(INPUT)); clickCount++;
    }

    void MouseWheel(int16_t dx, int16_t dy) {
        if (!enabled) return;
        if (dy) { INPUT inp{}; inp.type = INPUT_MOUSE; inp.mi.dwFlags = MOUSEEVENTF_WHEEL;
                  inp.mi.mouseData = (DWORD)(-dy * WHEEL_DELTA / 100); SendInput(1, &inp, sizeof(INPUT)); }
        if (dx) { INPUT inp{}; inp.type = INPUT_MOUSE; inp.mi.dwFlags = MOUSEEVENTF_HWHEEL;
                  inp.mi.mouseData = (DWORD)(dx * WHEEL_DELTA / 100); SendInput(1, &inp, sizeof(INPUT)); }
    }

    void Key(uint16_t jsKey, uint16_t scan, bool down, uint8_t) {
        if (!enabled) return;
        WORD vk = JsKeyToVK(jsKey);
        if (!vk) { WARN("Unknown keyCode: %d", jsKey); return; }
        INPUT inp{}; inp.type = INPUT_KEYBOARD;
        inp.ki.wVk = vk; inp.ki.wScan = scan ? scan : (WORD)MapVirtualKey(vk, MAPVK_VK_TO_VSC);
        inp.ki.dwFlags = (down ? 0 : KEYEVENTF_KEYUP) | (IsExtendedKey(vk) ? KEYEVENTF_EXTENDEDKEY : 0);
        SendInput(1, &inp, sizeof(INPUT)); keyCount++;
    }

    bool HandleMessage(const uint8_t* d, size_t len) {
        if (len < 4) return false;
        uint32_t mg = *(uint32_t*)d;
        if (mg == MSG_MOUSE_MOVE && len >= 12) { auto* m = (MouseMoveMsg*)d; MouseMove(m->x, m->y); return true; }
        if (mg == MSG_MOUSE_BTN && len >= 6) { auto* m = (MouseBtnMsg*)d; MouseButton(m->button, m->action); return true; }
        if (mg == MSG_MOUSE_WHEEL && len >= 8) { auto* m = (MouseWheelMsg*)d; MouseWheel(m->deltaX, m->deltaY); return true; }
        if (mg == MSG_KEY && len >= 10) { auto* m = (KeyMsg*)d; Key(m->keyCode, m->scanCode, m->action, m->modifiers); return true; }
        return false;
    }

    struct Stats { uint64_t moves, clicks, keys; };
    Stats GetStats() { return {moveCount.exchange(0), clickCount.exchange(0), keyCount.exchange(0)}; }
};
