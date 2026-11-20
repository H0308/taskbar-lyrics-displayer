#pragma once
#include <windows.h>

// 独占 WS_CHILD / WS_POPUP 与父窗口关系的变更及验证。
class TaskbarWindowAttachment {
public:
    static constexpr unsigned kMaxAttachAttempts = 12;

    bool attach(HWND window, HWND taskbar);
    bool detach(HWND window);
    bool embedded() const { return embedded_; }
    bool retriesExhausted() const { return consecutiveFailures_ >= kMaxAttachAttempts; }
    void reset() {
        embedded_ = false;
        consecutiveFailures_ = 0;
    }

private:
    bool embedded_ = false;
    unsigned consecutiveFailures_ = 0;
};
