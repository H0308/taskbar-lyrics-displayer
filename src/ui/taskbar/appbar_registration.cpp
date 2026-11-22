#include "ui/taskbar/appbar_registration.h"
#include "logging/runtime_logger.h"
#include <shellapi.h>

bool AppBarRegistration::registerWindow(HWND window, UINT callbackMessage) {
    if (registered_)
        return true;
    APPBARDATA data{};
    data.cbSize = sizeof(data);
    data.hWnd = window;
    data.uCallbackMessage = callbackMessage;
    if (!SHAppBarMessage(ABM_NEW, &data))
        return false;
    registered_ = true;
    return true;
}
void AppBarRegistration::unregisterWindow(HWND window) {
    if (!registered_ || !window)
        return;
    registered_ = false;
    APPBARDATA data{};
    data.cbSize = sizeof(data);
    data.hWnd = window;
    SHAppBarMessage(ABM_REMOVE, &data);
    runtime_log::writef(L"[appbar] unregistered");
}
void AppBarRegistration::updateZOrder(HWND window) {
    if (!window || !registered_)
        return;
    SetWindowPos(window, fullscreen_ ? HWND_BOTTOM : HWND_TOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
}
bool AppBarRegistration::positionWindow(HWND window, HMONITOR monitor, AppBarEdge edge,
                                        LONG thickness) {
    if (!window || !registered_ || positioning_)
        return false;
    struct PositionScope {
        bool& active;
        explicit PositionScope(bool& value) : active(value) { active = true; }
        ~PositionScope() { active = false; }
    } scope(positioning_);
    MONITORINFO monitorInfo{};
    monitorInfo.cbSize = sizeof(monitorInfo);
    if (!GetMonitorInfoW(monitor, &monitorInfo))
        return false;
    APPBARDATA data{};
    data.cbSize = sizeof(data);
    data.hWnd = window;
    switch (edge) {
    case AppBarEdge::Top: data.uEdge = ABE_TOP; break;
    case AppBarEdge::Bottom: data.uEdge = ABE_BOTTOM; break;
    case AppBarEdge::Left: data.uEdge = ABE_LEFT; break;
    case AppBarEdge::Right: data.uEdge = ABE_RIGHT; break;
    }
    data.rc = monitorInfo.rcMonitor;
    const auto applyThickness = [&] {
        switch (edge) {
        case AppBarEdge::Top: data.rc.bottom = data.rc.top + thickness; break;
        case AppBarEdge::Bottom: data.rc.top = data.rc.bottom - thickness; break;
        case AppBarEdge::Left: data.rc.right = data.rc.left + thickness; break;
        case AppBarEdge::Right: data.rc.left = data.rc.right - thickness; break;
        }
    };

    applyThickness();
    SHAppBarMessage(ABM_QUERYPOS, &data);
    if (!registered_)
        return false;

    applyThickness();
    SHAppBarMessage(ABM_SETPOS, &data);
    if (!registered_)
        return false;
    if (data.rc.right <= data.rc.left || data.rc.bottom <= data.rc.top)
        return false;

    SetWindowPos(window, fullscreen_ ? HWND_BOTTOM : HWND_TOPMOST, data.rc.left, data.rc.top,
                 data.rc.right - data.rc.left, data.rc.bottom - data.rc.top,
                 SWP_NOACTIVATE | SWP_FRAMECHANGED);

    return true;
}
