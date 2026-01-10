/**
 * @file input.hpp
 * @brief Remote input injection for mouse, keyboard, and touch events
 * @copyright 2025-2026 Daniel Chrobak
 */

#pragma once
#include "common.hpp"

#pragma pack(push, 1)
struct MouseMoveMsg { uint32_t magic; float x, y; };
struct MouseBtnMsg { uint32_t magic; uint8_t button, action; };
struct MouseWheelMsg { uint32_t magic; int16_t deltaX, deltaY; float x, y; };
struct KeyMsg { uint32_t magic; uint16_t keyCode, scanCode; uint8_t action, modifiers; };
#pragma pack(pop)

inline WORD JsKeyToVK(uint16_t keyCode) {
    if ((keyCode >= 65 && keyCode <= 90) || (keyCode >= 48 && keyCode <= 57))
        return static_cast<WORD>(keyCode);

    static const std::unordered_map<uint16_t, WORD> keyMap = {
        {8, VK_BACK}, {9, VK_TAB}, {13, VK_RETURN}, {16, VK_SHIFT}, {17, VK_CONTROL}, {18, VK_MENU},
        {19, VK_PAUSE}, {20, VK_CAPITAL}, {27, VK_ESCAPE}, {32, VK_SPACE}, {33, VK_PRIOR}, {34, VK_NEXT},
        {35, VK_END}, {36, VK_HOME}, {37, VK_LEFT}, {38, VK_UP}, {39, VK_RIGHT}, {40, VK_DOWN},
        {44, VK_SNAPSHOT}, {45, VK_INSERT}, {46, VK_DELETE}, {91, VK_LWIN}, {92, VK_RWIN},
        {96, VK_NUMPAD0}, {97, VK_NUMPAD1}, {98, VK_NUMPAD2}, {99, VK_NUMPAD3}, {100, VK_NUMPAD4},
        {101, VK_NUMPAD5}, {102, VK_NUMPAD6}, {103, VK_NUMPAD7}, {104, VK_NUMPAD8}, {105, VK_NUMPAD9},
        {106, VK_MULTIPLY}, {107, VK_ADD}, {109, VK_SUBTRACT}, {110, VK_DECIMAL}, {111, VK_DIVIDE},
        {112, VK_F1}, {113, VK_F2}, {114, VK_F3}, {115, VK_F4}, {116, VK_F5}, {117, VK_F6},
        {118, VK_F7}, {119, VK_F8}, {120, VK_F9}, {121, VK_F10}, {122, VK_F11}, {123, VK_F12},
        {144, VK_NUMLOCK}, {145, VK_SCROLL}, {173, VK_VOLUME_MUTE}, {174, VK_VOLUME_DOWN}, {175, VK_VOLUME_UP},
        {176, VK_MEDIA_NEXT_TRACK}, {177, VK_MEDIA_PREV_TRACK}, {178, VK_MEDIA_STOP}, {179, VK_MEDIA_PLAY_PAUSE},
        {186, VK_OEM_1}, {187, VK_OEM_PLUS}, {188, VK_OEM_COMMA}, {189, VK_OEM_MINUS}, {190, VK_OEM_PERIOD},
        {191, VK_OEM_2}, {192, VK_OEM_3}, {219, VK_OEM_4}, {220, VK_OEM_5}, {221, VK_OEM_6}, {222, VK_OEM_7}
    };
    auto it = keyMap.find(keyCode);
    return it != keyMap.end() ? it->second : 0;
}

class InputHandler {
private:
    std::atomic<int> monitorX{0}, monitorY{0}, monitorWidth{1920}, monitorHeight{1080};
    std::atomic<bool> enabled{false};
    std::atomic<uint64_t> moveCount{0}, clickCount{0}, keyCount{0};

    void ToAbsolute(float nx, float ny, LONG& ax, LONG& ay) {
        int px = monitorX + static_cast<int>(std::clamp(nx, 0.f, 1.f) * monitorWidth);
        int py = monitorY + static_cast<int>(std::clamp(ny, 0.f, 1.f) * monitorHeight);
        ax = static_cast<LONG>((px - GetSystemMetrics(SM_XVIRTUALSCREEN)) * 65535 / GetSystemMetrics(SM_CXVIRTUALSCREEN));
        ay = static_cast<LONG>((py - GetSystemMetrics(SM_YVIRTUALSCREEN)) * 65535 / GetSystemMetrics(SM_CYVIRTUALSCREEN));
    }

    static bool IsExtendedKey(WORD vk) {
        return vk == VK_INSERT || vk == VK_DELETE || vk == VK_HOME || vk == VK_END ||
               vk == VK_PRIOR || vk == VK_NEXT || vk == VK_LEFT || vk == VK_RIGHT ||
               vk == VK_UP || vk == VK_DOWN || vk == VK_LWIN || vk == VK_RWIN ||
               vk == VK_APPS || vk == VK_DIVIDE || vk == VK_NUMLOCK;
    }

public:
    void SetMonitorBounds(int x, int y, int w, int h) { monitorX = x; monitorY = y; monitorWidth = w; monitorHeight = h; }

    void UpdateFromMonitorInfo(const MonitorInfo& info) {
        MONITORINFO mi{sizeof(mi)};
        if (GetMonitorInfo(info.hMon, &mi))
            SetMonitorBounds(mi.rcMonitor.left, mi.rcMonitor.top,
                             mi.rcMonitor.right - mi.rcMonitor.left, mi.rcMonitor.bottom - mi.rcMonitor.top);
    }

    void Enable() { enabled = true; LOG("Input enabled"); }
    void Disable() { enabled = false; }
    bool IsEnabled() const { return enabled; }

    void WiggleCenter() {
        if (!enabled) return;
        LONG ax, ay; ToAbsolute(0.5f, 0.5f, ax, ay);
        INPUT inputs[3] = {};
        DWORD flags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
        for (int i = 0; i < 3; i++) {
            inputs[i].type = INPUT_MOUSE;
            inputs[i].mi.dwFlags = flags;
            inputs[i].mi.dx = ax + (i == 1 ? 1 : 0);
            inputs[i].mi.dy = ay;
        }
        SendInput(3, inputs, sizeof(INPUT));
        LOG("Cursor wiggled at center to trigger keyframe");
    }

    void MouseMove(float nx, float ny) {
        if (!enabled) return;
        LONG ax, ay; ToAbsolute(nx, ny, ax, ay);
        INPUT in{INPUT_MOUSE};
        in.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
        in.mi.dx = ax; in.mi.dy = ay;
        SendInput(1, &in, sizeof(INPUT));
        moveCount++;
    }

    void MouseButton(uint8_t button, bool down) {
        if (!enabled || button > 4) return;
        static const DWORD flags[5][2] = {
            {MOUSEEVENTF_LEFTUP, MOUSEEVENTF_LEFTDOWN}, {MOUSEEVENTF_RIGHTUP, MOUSEEVENTF_RIGHTDOWN},
            {MOUSEEVENTF_MIDDLEUP, MOUSEEVENTF_MIDDLEDOWN}, {MOUSEEVENTF_XUP, MOUSEEVENTF_XDOWN},
            {MOUSEEVENTF_XUP, MOUSEEVENTF_XDOWN}
        };
        INPUT in{INPUT_MOUSE};
        in.mi.dwFlags = flags[button][down ? 1 : 0];
        if (button >= 3) in.mi.mouseData = button == 3 ? XBUTTON1 : XBUTTON2;
        SendInput(1, &in, sizeof(INPUT));
        clickCount++;
    }

    void MouseWheel(int16_t dx, int16_t dy) {
        if (!enabled) return;
        if (dy) { INPUT in{INPUT_MOUSE}; in.mi.dwFlags = MOUSEEVENTF_WHEEL; in.mi.mouseData = static_cast<DWORD>(-dy * WHEEL_DELTA / 100); SendInput(1, &in, sizeof(INPUT)); }
        if (dx) { INPUT in{INPUT_MOUSE}; in.mi.dwFlags = MOUSEEVENTF_HWHEEL; in.mi.mouseData = static_cast<DWORD>(dx * WHEEL_DELTA / 100); SendInput(1, &in, sizeof(INPUT)); }
    }

    void Key(uint16_t jsKey, uint16_t scanCode, bool down, uint8_t) {
        if (!enabled) return;
        WORD vk = JsKeyToVK(jsKey);
        if (!vk) { WARN("Unknown keyCode: %d", jsKey); return; }
        INPUT in{INPUT_KEYBOARD};
        in.ki.wVk = vk;
        in.ki.wScan = scanCode ? scanCode : static_cast<WORD>(MapVirtualKey(vk, MAPVK_VK_TO_VSC));
        in.ki.dwFlags = (down ? 0 : KEYEVENTF_KEYUP) | (IsExtendedKey(vk) ? KEYEVENTF_EXTENDEDKEY : 0);
        SendInput(1, &in, sizeof(INPUT));
        keyCount++;
    }

    bool HandleMessage(const uint8_t* data, size_t len) {
        if (len < 4) return false;
        uint32_t magic = *reinterpret_cast<const uint32_t*>(data);

        if (magic == MSG_MOUSE_MOVE && len >= sizeof(MouseMoveMsg)) {
            auto* m = reinterpret_cast<const MouseMoveMsg*>(data); MouseMove(m->x, m->y); return true;
        }
        if (magic == MSG_MOUSE_BTN && len >= sizeof(MouseBtnMsg)) {
            auto* m = reinterpret_cast<const MouseBtnMsg*>(data); MouseButton(m->button, m->action != 0); return true;
        }
        if (magic == MSG_MOUSE_WHEEL && len >= 8) {
            auto* m = reinterpret_cast<const MouseWheelMsg*>(data); MouseWheel(m->deltaX, m->deltaY); return true;
        }
        if (magic == MSG_KEY && len >= sizeof(KeyMsg)) {
            auto* m = reinterpret_cast<const KeyMsg*>(data); Key(m->keyCode, m->scanCode, m->action != 0, m->modifiers); return true;
        }
        return false;
    }

    struct Stats { uint64_t moves, clicks, keys; };
    Stats GetStats() { return {moveCount.exchange(0), clickCount.exchange(0), keyCount.exchange(0)}; }
};
