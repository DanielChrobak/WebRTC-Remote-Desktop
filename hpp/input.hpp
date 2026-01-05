#pragma once
#include "common.hpp"

#pragma pack(push, 1)
struct MouseMoveMsg { uint32_t magic; float x, y; };
struct MouseBtnMsg { uint32_t magic; uint8_t button, action; };
struct MouseWheelMsg { uint32_t magic; int16_t deltaX, deltaY; float x, y; };
struct KeyMsg { uint32_t magic; uint16_t keyCode, scanCode; uint8_t action, modifiers; };
#pragma pack(pop)

inline WORD JsKeyToVK(uint16_t k) {
    if ((k >= 65 && k <= 90) || (k >= 48 && k <= 57)) return (WORD)k;
    static const std::unordered_map<uint16_t, WORD> m = {
        {8,VK_BACK},{9,VK_TAB},{13,VK_RETURN},{16,VK_SHIFT},{17,VK_CONTROL},{18,VK_MENU},
        {19,VK_PAUSE},{20,VK_CAPITAL},{27,VK_ESCAPE},{32,VK_SPACE},{33,VK_PRIOR},{34,VK_NEXT},
        {35,VK_END},{36,VK_HOME},{37,VK_LEFT},{38,VK_UP},{39,VK_RIGHT},{40,VK_DOWN},
        {44,VK_SNAPSHOT},{45,VK_INSERT},{46,VK_DELETE},{91,VK_LWIN},{92,VK_RWIN},
        {96,VK_NUMPAD0},{97,VK_NUMPAD1},{98,VK_NUMPAD2},{99,VK_NUMPAD3},{100,VK_NUMPAD4},
        {101,VK_NUMPAD5},{102,VK_NUMPAD6},{103,VK_NUMPAD7},{104,VK_NUMPAD8},{105,VK_NUMPAD9},
        {106,VK_MULTIPLY},{107,VK_ADD},{109,VK_SUBTRACT},{110,VK_DECIMAL},{111,VK_DIVIDE},
        {112,VK_F1},{113,VK_F2},{114,VK_F3},{115,VK_F4},{116,VK_F5},{117,VK_F6},
        {118,VK_F7},{119,VK_F8},{120,VK_F9},{121,VK_F10},{122,VK_F11},{123,VK_F12},
        {144,VK_NUMLOCK},{145,VK_SCROLL},{173,VK_VOLUME_MUTE},{174,VK_VOLUME_DOWN},
        {175,VK_VOLUME_UP},{176,VK_MEDIA_NEXT_TRACK},{177,VK_MEDIA_PREV_TRACK},
        {178,VK_MEDIA_STOP},{179,VK_MEDIA_PLAY_PAUSE},{186,VK_OEM_1},{187,VK_OEM_PLUS},
        {188,VK_OEM_COMMA},{189,VK_OEM_MINUS},{190,VK_OEM_PERIOD},{191,VK_OEM_2},
        {192,VK_OEM_3},{219,VK_OEM_4},{220,VK_OEM_5},{221,VK_OEM_6},{222,VK_OEM_7},
    };
    auto it = m.find(k); return it != m.end() ? it->second : 0;
}

class InputHandler {
    std::atomic<int> monX{0}, monY{0}, monW{1920}, monH{1080};
    std::atomic<bool> enabled{false};
    std::atomic<uint64_t> moveCount{0}, clickCount{0}, keyCount{0};

    void ToAbsolute(float nx, float ny, LONG& ax, LONG& ay) {
        int px = monX + (int)(std::clamp(nx, 0.f, 1.f) * monW);
        int py = monY + (int)(std::clamp(ny, 0.f, 1.f) * monH);
        int vsX = GetSystemMetrics(SM_XVIRTUALSCREEN), vsY = GetSystemMetrics(SM_YVIRTUALSCREEN);
        ax = (LONG)((px - vsX) * 65535 / GetSystemMetrics(SM_CXVIRTUALSCREEN));
        ay = (LONG)((py - vsY) * 65535 / GetSystemMetrics(SM_CYVIRTUALSCREEN));
    }

    static bool IsExtendedKey(WORD vk) {
        static const WORD ext[] = {VK_INSERT,VK_DELETE,VK_HOME,VK_END,VK_PRIOR,VK_NEXT,
            VK_LEFT,VK_RIGHT,VK_UP,VK_DOWN,VK_LWIN,VK_RWIN,VK_APPS,VK_DIVIDE,VK_NUMLOCK};
        return std::find(std::begin(ext), std::end(ext), vk) != std::end(ext);
    }

public:
    void SetMonitorBounds(int x, int y, int w, int h) { monX=x; monY=y; monW=w; monH=h; }
    void UpdateFromMonitorInfo(const MonitorInfo& mi) {
        MONITORINFO info{sizeof(info)};
        if (GetMonitorInfo(mi.hMon, &info))
            SetMonitorBounds(info.rcMonitor.left, info.rcMonitor.top,
                info.rcMonitor.right - info.rcMonitor.left, info.rcMonitor.bottom - info.rcMonitor.top);
    }
    void Enable() { enabled = true; LOG("Input enabled"); }
    void Disable() { enabled = false; }
    bool IsEnabled() const { return enabled; }

    void WiggleCenter() {
        if (!enabled) return;
        // Move to center
        LONG ax, ay;
        ToAbsolute(0.5f, 0.5f, ax, ay);
        INPUT inp[3] = {};

        // Move to center
        inp[0].type = INPUT_MOUSE;
        inp[0].mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
        inp[0].mi.dx = ax;
        inp[0].mi.dy = ay;

        // Move 1 pixel right
        inp[1].type = INPUT_MOUSE;
        inp[1].mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
        inp[1].mi.dx = ax + 1;
        inp[1].mi.dy = ay;

        // Move back to center
        inp[2].type = INPUT_MOUSE;
        inp[2].mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
        inp[2].mi.dx = ax;
        inp[2].mi.dy = ay;

        SendInput(3, inp, sizeof(INPUT));
        LOG("Cursor wiggled at center to trigger keyframe");
    }

    void MouseMove(float nx, float ny) {
        if (!enabled) return;
        LONG ax, ay; ToAbsolute(nx, ny, ax, ay);
        INPUT inp{INPUT_MOUSE}; inp.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
        inp.mi.dx = ax; inp.mi.dy = ay;
        SendInput(1, &inp, sizeof(INPUT)); moveCount++;
    }

    void MouseButton(uint8_t btn, bool down) {
        if (!enabled || btn > 4) return;
        static const DWORD flags[5][2] = {{MOUSEEVENTF_LEFTUP,MOUSEEVENTF_LEFTDOWN},{MOUSEEVENTF_RIGHTUP,MOUSEEVENTF_RIGHTDOWN},
            {MOUSEEVENTF_MIDDLEUP,MOUSEEVENTF_MIDDLEDOWN},{MOUSEEVENTF_XUP,MOUSEEVENTF_XDOWN},{MOUSEEVENTF_XUP,MOUSEEVENTF_XDOWN}};
        INPUT inp{INPUT_MOUSE}; inp.mi.dwFlags = flags[btn][down];
        if (btn >= 3) inp.mi.mouseData = btn == 3 ? XBUTTON1 : XBUTTON2;
        SendInput(1, &inp, sizeof(INPUT)); clickCount++;
    }

    void MouseWheel(int16_t dx, int16_t dy) {
        if (!enabled) return;
        auto send = [](DWORD fl, int d) { INPUT inp{INPUT_MOUSE}; inp.mi.dwFlags=fl; inp.mi.mouseData=(DWORD)d; SendInput(1,&inp,sizeof(INPUT)); };
        if (dy) send(MOUSEEVENTF_WHEEL, -dy * WHEEL_DELTA / 100);
        if (dx) send(MOUSEEVENTF_HWHEEL, dx * WHEEL_DELTA / 100);
    }

    void Key(uint16_t jsKey, uint16_t scan, bool down, uint8_t) {
        if (!enabled) return;
        WORD vk = JsKeyToVK(jsKey);
        if (!vk) { WARN("Unknown keyCode: %d", jsKey); return; }
        INPUT inp{INPUT_KEYBOARD}; inp.ki.wVk = vk;
        inp.ki.wScan = scan ? scan : (WORD)MapVirtualKey(vk, MAPVK_VK_TO_VSC);
        inp.ki.dwFlags = (down ? 0 : KEYEVENTF_KEYUP) | (IsExtendedKey(vk) ? KEYEVENTF_EXTENDEDKEY : 0);
        SendInput(1, &inp, sizeof(INPUT)); keyCount++;
    }

    bool HandleMessage(const uint8_t* d, size_t len) {
        if (len < 4) return false;
        uint32_t mg = *(uint32_t*)d;
        switch (mg) {
            case MSG_MOUSE_MOVE: if (len >= 12) { auto* m=(MouseMoveMsg*)d; MouseMove(m->x,m->y); return true; } break;
            case MSG_MOUSE_BTN: if (len >= 6) { auto* m=(MouseBtnMsg*)d; MouseButton(m->button,m->action); return true; } break;
            case MSG_MOUSE_WHEEL: if (len >= 8) { auto* m=(MouseWheelMsg*)d; MouseWheel(m->deltaX,m->deltaY); return true; } break;
            case MSG_KEY: if (len >= 10) { auto* m=(KeyMsg*)d; Key(m->keyCode,m->scanCode,m->action,m->modifiers); return true; } break;
        }
        return false;
    }

    struct Stats { uint64_t moves, clicks, keys; };
    Stats GetStats() { return {moveCount.exchange(0), clickCount.exchange(0), keyCount.exchange(0)}; }
};
