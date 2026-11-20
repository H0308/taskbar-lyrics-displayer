#include "ui/taskbar/interaction.h"
#include <commctrl.h>

void TaskbarTooltip::mouseMove(HWND owner, const RECT& rect, WPARAM keys, LPARAM point,
                              const wchar_t* label) {
    if (!hwnd_) {
        hwnd_ = CreateWindowExW(WS_EX_TOPMOST | WS_EX_NOACTIVATE, TOOLTIPS_CLASSW, nullptr,
                                WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX,
                                CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
                                owner, nullptr, nullptr, nullptr);
        if (!hwnd_)
            return;
        TOOLINFOW tool{sizeof(tool)};
        tool.hwnd = owner;
        tool.uId = 1;
        tool.rect = rect;
        tool.lpszText = const_cast<wchar_t*>(label);
        if (!SendMessageW(hwnd_, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&tool))) {
            reset();
            return;
        }
        rect_ = rect;
        label_ = label;
        SendMessageW(hwnd_, TTM_SETDELAYTIME, TTDT_INITIAL, 500);
    }
    if (label_ != label) {
        hide();
        TOOLINFOW tool{sizeof(tool)};
        tool.hwnd = owner;
        tool.uId = 1;
        tool.lpszText = const_cast<wchar_t*>(label);
        SendMessageW(hwnd_, TTM_UPDATETIPTEXTW, 0, reinterpret_cast<LPARAM>(&tool));
        label_ = label;
    }
    if (!EqualRect(&rect_, &rect)) {
        TOOLINFOW tool{sizeof(tool)};
        tool.hwnd = owner;
        tool.uId = 1;
        tool.rect = rect;
        SendMessageW(hwnd_, TTM_NEWTOOLRECTW, 0, reinterpret_cast<LPARAM>(&tool));
        rect_ = rect;
    }
    if (!active_) {
        SendMessageW(hwnd_, TTM_ACTIVATE, TRUE, 0);
        active_ = true;
    }
    MSG message{};
    message.hwnd = owner;
    message.message = WM_MOUSEMOVE;
    message.wParam = keys;
    message.lParam = point;
    message.time = GetMessageTime();
    GetCursorPos(&message.pt);
    SendMessageW(hwnd_, TTM_RELAYEVENT, 0, reinterpret_cast<LPARAM>(&message));
}

void TaskbarTooltip::hide() {
    if (hwnd_ && active_) {
        SendMessageW(hwnd_, TTM_ACTIVATE, FALSE, 0);
        active_ = false;
    }
}

void TaskbarTooltip::reset() {
    if (hwnd_ && IsWindow(hwnd_))
        DestroyWindow(hwnd_);
    hwnd_ = nullptr;
    active_ = false;
    rect_ = {};
    label_ = nullptr;
}

float TaskbarPresence::opacity(ULONGLONG now) const {
    if (phase_ == Phase::Shown)
        return 1.0f;
    if (phase_ == Phase::Hidden)
        return 0.0f;
    if (startMs_ == 0)
        return from_;
    const bool hiding = phase_ == Phase::Hiding;
    const float duration = static_cast<float>(hiding ? kHideMs : kShowMs);
    const float t = std::clamp(static_cast<float>(now - startMs_) / duration, 0.0f, 1.0f);
    const float eased = t * t * (3.0f - 2.0f * t);
    const float target = hiding ? 0.0f : 1.0f;
    return from_ + (target - from_) * eased;
}

TaskbarPresence::Action TaskbarPresence::advance(ULONGLONG now, bool cursorAway) {
    switch (phase_) {
    case Phase::Hiding:
        return now - startMs_ >= kHideMs ? Action::Hide : Action::None;
    case Phase::Hidden:
        if (!cursorAway)
            awaySince_ = 0;
        else if (awaySince_ == 0)
            awaySince_ = now;
        else if (now - awaySince_ >= 180)
            return Action::Show;
        return Action::None;
    case Phase::Showing:
        if (now - startMs_ < kShowMs)
            return Action::None;
        phase_ = Phase::Shown;
        return Action::StopTimer;
    case Phase::Shown:
        return Action::StopTimer;
    }
    return Action::None;
}
