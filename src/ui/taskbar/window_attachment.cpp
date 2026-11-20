#include "ui/taskbar/window_attachment.h"
#include "logging/runtime_logger.h"
bool TaskbarWindowAttachment::attach(HWND window, HWND taskbar) {
    embedded_ = false;
    if (!window || !taskbar || !IsWindow(taskbar))
        return false;

    SetLastError(ERROR_SUCCESS);
    const LONG_PTR originalStyle = GetWindowLongPtrW(window, GWL_STYLE);
    if (originalStyle == 0 && GetLastError() != ERROR_SUCCESS)
        return false;

    // SetParent 不会替窗口切换 WS_POPUP/WS_CHILD；先切成真正的子窗口，
    // 否则后续 SetWindowPos 仍可能按顶层窗口的屏幕坐标解释。
    const LONG_PTR childStyle =
        (originalStyle & ~static_cast<LONG_PTR>(WS_POPUP)) | static_cast<LONG_PTR>(WS_CHILD);
    SetLastError(ERROR_SUCCESS);
    if (SetWindowLongPtrW(window, GWL_STYLE, childStyle) == 0 && GetLastError() != ERROR_SUCCESS)
        return false;

    // 初始窗口没有父窗口时，SetParent 成功也会返回 nullptr（返回的是旧父窗口），
    // 必须结合 GetLastError 和实际父窗口判断，不能直接判断返回值。
    SetLastError(ERROR_SUCCESS);
    const HWND previousParent = SetParent(window, taskbar);
    const DWORD error = GetLastError();
    if (!previousParent && error != ERROR_SUCCESS) {
        SetParent(window, nullptr);
        SetWindowLongPtrW(window, GWL_STYLE, originalStyle);
        runtime_log::writef(L"[taskbar] SetParent failed: error=%lu hwnd=%p parent=%p",
                            static_cast<unsigned long>(error), window, taskbar);
        return false;
    }

    if (GetParent(window) != taskbar) {
        SetParent(window, nullptr);
        SetWindowLongPtrW(window, GWL_STYLE, originalStyle);
        // 部分环境（如 Windows 11 24H2+ 收紧了 Shell 窗口的外来子窗口限制）下
        // SetParent 会静默失败（返回 NULL 且 GetLastError 为 0），宿主会按 250ms
        // 周期重试并停留在顶层悬浮回退模式；这里限制日志频率避免刷爆日志文件。
        ++attachFailCount_;
        if (attachFailCount_ <= 4 || attachFailCount_ % 400 == 0)
            runtime_log::writef(L"[taskbar] SetParent parent mismatch: hwnd=%p parent=%p attempts=%u",
                                window, taskbar, attachFailCount_);
        return false;
    }

    embedded_ = true;
    attachFailCount_ = 0;
    return true;
}

bool TaskbarWindowAttachment::detach(HWND window) {
    if (!window)
        return false;
    const HWND originalParent = GetParent(window);
    const LONG_PTR originalStyle = GetWindowLongPtrW(window, GWL_STYLE);
    if (originalParent) {
        SetLastError(ERROR_SUCCESS);
        const HWND previousParent = SetParent(window, nullptr);
        const DWORD error = GetLastError();
        if (!previousParent && error != ERROR_SUCCESS) {
            runtime_log::writef(L"[appbar] detach failed: error=%lu",
                                static_cast<unsigned long>(error));
            return false;
        }
    }

    // SetParent 不会自动切换 WS_CHILD/WS_POPUP。仍带 WS_CHILD 时，桌面窗口会被
    // GetParent 返回为父窗口，因此必须先完成顶层样式切换，再校验是否真正脱离。
    SetLastError(ERROR_SUCCESS);
    const LONG_PTR popupStyle =
        (originalStyle & ~static_cast<LONG_PTR>(WS_CHILD)) | static_cast<LONG_PTR>(WS_POPUP);
    if (SetWindowLongPtrW(window, GWL_STYLE, popupStyle) == 0 && GetLastError() != ERROR_SUCCESS) {
        runtime_log::writef(L"[appbar] popup style failed: error=%lu",
                            static_cast<unsigned long>(GetLastError()));
        return false;
    }
    SetWindowPos(window, nullptr, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    if (GetParent(window) != nullptr) {
        runtime_log::writef(L"[appbar] detach verification failed: parent=%p", GetParent(window));
        return false;
    }
    embedded_ = false;
    return true;
}
