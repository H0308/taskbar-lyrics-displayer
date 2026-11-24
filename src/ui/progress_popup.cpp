#include "progress_popup.h"

#include "graphics/composition_renderer.h"
#include "logging/runtime_logger.h"
#include "ui/fluent_theme.h"
#include "util/com_release.h"

#include <d2d1_1.h>
#include <dwrite.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cwchar>
#include <string>

namespace {

constexpr wchar_t kWndClassName[] = L"QQMusicLyricProgressPopup";
constexpr UINT_PTR kHideTimer = 1;
constexpr UINT kHideDelayMs = 260;

constexpr float kWidthDip = 256.0f;
constexpr float kHeightDip = 44.0f;
constexpr float kCurrentLeftDip = 8.0f;
constexpr float kCurrentRightDip = 56.0f;
constexpr float kTrackLeftDip = 64.0f;
constexpr float kTrackRightDip = 192.0f;
constexpr float kDurationLeftDip = 200.0f;
constexpr float kDurationRightDip = 248.0f;
constexpr float kKnobRadiusDip = 6.5f;
constexpr float kTrackHitPaddingDip = 8.0f;
constexpr float kTrackHalfHitDip = 16.0f;
constexpr int64_t kWheelStepMs = 5000;
constexpr ULONGLONG kPendingSeekHoldMs = 1500;
constexpr int64_t kPendingSeekConfirmToleranceMs = 2000;
constexpr ULONGLONG kDragRenderMinIntervalMs = 12;

std::wstring formatTime(int64_t ms) {
    const int64_t totalSec = std::max<int64_t>(0, ms) / 1000;
    const int64_t hours = totalSec / 3600;
    const int64_t minutes = (totalSec % 3600) / 60;
    const int64_t seconds = totalSec % 60;
    wchar_t buf[32]{};
    if (hours > 0)
        swprintf_s(buf, L"%lld:%02lld:%02lld", static_cast<long long>(hours),
                   static_cast<long long>(minutes), static_cast<long long>(seconds));
    else
        swprintf_s(buf, L"%lld:%02lld", static_cast<long long>(minutes),
                   static_cast<long long>(seconds));
    return buf;
}

} // namespace

struct ProgressPopup::Impl {
    HWND hwnd = nullptr;
    UINT dpi = 96;
    bool visible = false;
    bool anchorHover = false;
    bool popupHover = false;
    bool hoverSlider = false;
    bool dragging = false;

    uint64_t sessionGeneration = 0;
    int64_t positionMs = 0;
    int64_t previewPositionMs = 0;
    int64_t durationMs = 0;
    bool available = false;
    bool pendingSeek = false;
    int64_t pendingSeekTargetMs = 0;
    ULONGLONG pendingSeekUntilMs = 0;
    ULONGLONG lastDragRenderMs = 0;
    float lastRenderedFillRight = -1.0f;
    int64_t lastRenderedSecond = -1;
    std::function<void(uint64_t, int64_t)> onSeek;

    CompositionRenderer renderer;
    uint64_t resourceGeneration = 0;
    ID2D1SolidColorBrush* brushText = nullptr;
    ID2D1SolidColorBrush* brushSecondary = nullptr;
    ID2D1SolidColorBrush* brushTrack = nullptr;
    ID2D1SolidColorBrush* brushAccent = nullptr;
    ID2D1SolidColorBrush* brushKnob = nullptr;
    IDWriteTextFormat* fmtCurrent = nullptr;
    IDWriteTextFormat* fmtDuration = nullptr;
    uint64_t fontRevision = 0;

    float scale() const { return static_cast<float>(dpi) / 96.0f; }
    float dip(int px) const { return static_cast<float>(px) / scale(); }

    void releaseDrawingResources() {
        releaseCom(brushText);
        releaseCom(brushSecondary);
        releaseCom(brushTrack);
        releaseCom(brushAccent);
        releaseCom(brushKnob);
        releaseCom(fmtCurrent);
        releaseCom(fmtDuration);
        resourceGeneration = 0;
    }

    void releaseResources() {
        releaseDrawingResources();
        renderer.discard();
    }

    bool ensureResources() {
        const auto revision = fluent::uiFontRevision();
        if (resourceGeneration == renderer.deviceGeneration() && fmtCurrent && fmtDuration &&
            fontRevision == revision)
            return true;
        releaseDrawingResources();
        auto* rt = renderer.renderTarget();
        if (!rt || !renderer.dwrite())
            return false;
        const auto& p = fluent::palette(fluent::ThemeTarget::Window);
        if (FAILED(rt->CreateSolidColorBrush(p.text, &brushText)) ||
            FAILED(rt->CreateSolidColorBrush(p.textSecondary, &brushSecondary)) ||
            FAILED(rt->CreateSolidColorBrush(p.text, &brushTrack)) ||
            FAILED(rt->CreateSolidColorBrush(p.accent, &brushAccent)) ||
            FAILED(rt->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White), &brushKnob)))
            return false;
        if (FAILED(fluent::createUiTextFormat(renderer.dwrite(), fluent::font_sizes::caption,
                                              DWRITE_FONT_WEIGHT_NORMAL, &fmtCurrent)) ||
            FAILED(fluent::createUiTextFormat(renderer.dwrite(), fluent::font_sizes::caption,
                                              DWRITE_FONT_WEIGHT_NORMAL, &fmtDuration)))
            return false;
        fmtCurrent->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
        fmtCurrent->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        fluent::applyUiFontFallback(fmtCurrent);
        fmtDuration->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        fmtDuration->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        fluent::applyUiFontFallback(fmtDuration);
        resourceGeneration = renderer.deviceGeneration();
        fontRevision = revision;
        return true;
    }

    int64_t displayPositionMs() const {
        const int64_t raw = dragging      ? previewPositionMs
                            : pendingSeek ? pendingSeekTargetMs
                                          : positionMs;
        return durationMs > 0 ? std::clamp<int64_t>(raw, 0, durationMs) : 0;
    }

    float valueFraction() const {
        if (!available || durationMs <= 0)
            return 0.0f;
        return std::clamp(
            static_cast<float>(displayPositionMs()) / static_cast<float>(durationMs), 0.0f, 1.0f);
    }

    int64_t positionAt(float xDip) const {
        if (durationMs <= 0)
            return 0;
        const double fraction = std::clamp(
            static_cast<double>(xDip - kTrackLeftDip) / (kTrackRightDip - kTrackLeftDip), 0.0, 1.0);
        return std::clamp<int64_t>(static_cast<int64_t>(std::llround(fraction * durationMs)), 0,
                                   durationMs);
    }

    bool hitSlider(float xDip, float yDip) const {
        const float cy = kHeightDip * 0.5f;
        return xDip >= kTrackLeftDip - kTrackHitPaddingDip &&
               xDip <= kTrackRightDip + kTrackHitPaddingDip &&
               std::fabs(yDip - cy) <= kTrackHalfHitDip;
    }

    void clearPendingSeek() {
        pendingSeek = false;
        pendingSeekTargetMs = 0;
        pendingSeekUntilMs = 0;
    }

    void commitSeek(int64_t targetMs) {
        if (!available || durationMs <= 0)
            return;
        targetMs = std::clamp<int64_t>(targetMs, 0, durationMs);
        positionMs = targetMs;
        previewPositionMs = targetMs;
        pendingSeek = true;
        pendingSeekTargetMs = targetMs;
        pendingSeekUntilMs = GetTickCount64() + kPendingSeekHoldMs;
        if (visible)
            render();
        if (onSeek)
            onSeek(sessionGeneration, targetMs);
    }

    bool render() {
        if (!hwnd)
            return false;
        RECT rc{};
        GetClientRect(hwnd, &rc);
        const int pxW = rc.right - rc.left;
        const int pxH = rc.bottom - rc.top;
        if (pxW <= 0 || pxH <= 0)
            return false;
        renderer.setDpi(dpi);
        if (!renderer.bind(hwnd, pxW, pxH))
            return false;
        if (!ensureResources()) {
            releaseResources();
            return false;
        }

        auto* rt = renderer.renderTarget();
        const float h = dip(pxH);
        rt->BeginDraw();
        rt->SetTransform(D2D1::Matrix3x2F::Identity());
        rt->Clear(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f));

        const bool dark = fluent::isDarkMode(fluent::ThemeTarget::Window);
        const std::wstring currentText = available ? formatTime(displayPositionMs()) : L"--:--";
        const std::wstring totalText = available && durationMs > 0 ? formatTime(durationMs) : L"--:--";

        rt->DrawTextW(currentText.c_str(), static_cast<UINT32>(currentText.size()), fmtCurrent,
                      D2D1::RectF(kCurrentLeftDip, 0.0f, kCurrentRightDip, h),
                      available ? brushText : brushSecondary, D2D1_DRAW_TEXT_OPTIONS_CLIP);

        const float cy = h * 0.5f;
        const float trackTop = cy - 2.0f;
        const float trackBottom = cy + 2.0f;
        brushTrack->SetOpacity(!available ? 0.18f
                               : dark     ? (hoverSlider || dragging ? 0.65f : 0.55f)
                                          : (hoverSlider || dragging ? 0.38f : 0.30f));
        rt->FillRoundedRectangle(
            D2D1::RoundedRect(D2D1::RectF(kTrackLeftDip, trackTop, kTrackRightDip, trackBottom),
                              2.0f, 2.0f),
            brushTrack);
        const float fillRight =
            kTrackLeftDip + (kTrackRightDip - kTrackLeftDip) * valueFraction();
        lastRenderedFillRight = fillRight;
        lastRenderedSecond = displayPositionMs() / 1000;
        if (available && fillRight > kTrackLeftDip) {
            rt->FillRoundedRectangle(
                D2D1::RoundedRect(D2D1::RectF(kTrackLeftDip, trackTop, fillRight, trackBottom),
                                  2.0f, 2.0f),
                brushAccent);
        }
        if (available) {
            const auto center = D2D1::Point2F(fillRight, cy);
            rt->FillEllipse(D2D1::Ellipse(center, kKnobRadiusDip, kKnobRadiusDip), brushKnob);
            const float innerRadius = dragging ? 3.0f : hoverSlider ? 4.5f : 3.8f;
            rt->FillEllipse(D2D1::Ellipse(center, innerRadius, innerRadius), brushAccent);
        }

        rt->DrawTextW(totalText.c_str(), static_cast<UINT32>(totalText.size()), fmtDuration,
                      D2D1::RectF(kDurationLeftDip, 0.0f, kDurationRightDip, h), brushSecondary,
                      D2D1_DRAW_TEXT_OPTIONS_CLIP);

        HRESULT hr = rt->EndDraw();
        if (hr == D2DERR_RECREATE_TARGET || FAILED(hr)) {
            releaseResources();
            return false;
        }
        if (!renderer.present()) {
            releaseResources();
            return false;
        }
        return true;
    }

    void scheduleHide() {
        if (!hwnd || !visible || anchorHover || popupHover || dragging)
            return;
        SetTimer(hwnd, kHideTimer, kHideDelayMs, nullptr);
    }

    void showNear(const RECT& anchorRect, bool verticalTaskbar, bool popupOnRight) {
        if (!hwnd || !available || durationMs <= 0)
            return;
        if (!visible && !fluent::applyAcrylicBackdrop(hwnd)) {
            runtime_log::writef(L"[progress-popup] native Acrylic unavailable on show");
            return;
        }
        KillTimer(hwnd, kHideTimer);
        const float s = scale();
        const int popupW = static_cast<int>(std::lround(kWidthDip * s));
        const int popupH = static_cast<int>(std::lround(kHeightDip * s));
        const int gap = static_cast<int>(std::lround(6.0f * s));

        HMONITOR monitor = MonitorFromRect(&anchorRect, MONITOR_DEFAULTTONEAREST);
        MONITORINFO info{};
        info.cbSize = sizeof(info);
        GetMonitorInfoW(monitor, &info);
        const RECT work = info.rcWork;

        int x = 0;
        int y = 0;
        if (verticalTaskbar) {
            x = popupOnRight ? anchorRect.right + gap : anchorRect.left - gap - popupW;
            if (popupOnRight && x + popupW > work.right) {
                popupOnRight = false;
                x = anchorRect.left - gap - popupW;
            } else if (!popupOnRight && x < work.left) {
                popupOnRight = true;
                x = anchorRect.right + gap;
            }
            x = std::clamp(x, static_cast<int>(work.left),
                           std::max(static_cast<int>(work.left),
                                    static_cast<int>(work.right) - popupW));
            const int anchorCenterY = (anchorRect.top + anchorRect.bottom) / 2;
            y = anchorCenterY - popupH / 2;
            y = std::clamp(y, static_cast<int>(work.top),
                           std::max(static_cast<int>(work.top),
                                    static_cast<int>(work.bottom) - popupH));
        } else {
            const int anchorCenterX = (anchorRect.left + anchorRect.right) / 2;
            x = anchorCenterX - popupW / 2;
            x = std::clamp(x, static_cast<int>(work.left),
                           std::max(static_cast<int>(work.left),
                                    static_cast<int>(work.right) - popupW));
            y = anchorRect.top - gap - popupH;
            if (y < work.top)
                y = anchorRect.bottom + gap;
        }
        SetWindowPos(hwnd, HWND_TOPMOST, x, y, popupW, popupH, SWP_NOACTIVATE | SWP_NOOWNERZORDER);
        if (!visible) {
            if (!render())
                return;
            ShowWindow(hwnd, SW_SHOWNOACTIVATE);
            visible = true;
        }
    }

    void hide() {
        if (!hwnd)
            return;
        KillTimer(hwnd, kHideTimer);
        dragging = false;
        hoverSlider = false;
        if (visible) {
            visible = false;
            ShowWindow(hwnd, SW_HIDE);
        }
        if (GetCapture() == hwnd)
            ReleaseCapture();
    }

    void trackMouseLeave() {
        TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd, 0};
        TrackMouseEvent(&tme);
    }

    LRESULT handle(UINT msg, WPARAM wp, LPARAM lp) {
        switch (msg) {
        case WM_CREATE:
            return 0;
        case WM_NCCALCSIZE:
            if (wp)
                return 0;
            break;
        case WM_NCHITTEST:
            return HTCLIENT;
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;
        case WM_MOUSEMOVE: {
            const POINT point{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            RECT client{};
            GetClientRect(hwnd, &client);
            popupHover = PtInRect(&client, point) != FALSE;
            if (popupHover || dragging)
                KillTimer(hwnd, kHideTimer);
            if (popupHover)
                trackMouseLeave();
            const float x = dip(GET_X_LPARAM(lp));
            const float y = dip(GET_Y_LPARAM(lp));
            if (dragging) {
                const int64_t next = positionAt(x);
                if (next != previewPositionMs) {
                    previewPositionMs = next;
                    const float nextFillRight =
                        kTrackLeftDip + (kTrackRightDip - kTrackLeftDip) * valueFraction();
                    const int64_t nextSecond = next / 1000;
                    const ULONGLONG now = GetTickCount64();
                    if (nextSecond != lastRenderedSecond ||
                        (std::fabs(nextFillRight - lastRenderedFillRight) >= 0.5f &&
                         now - lastDragRenderMs >= kDragRenderMinIntervalMs)) {
                        lastDragRenderMs = now;
                        render();
                    }
                }
            } else {
                const bool hover = available && hitSlider(x, y);
                if (hover != hoverSlider) {
                    hoverSlider = hover;
                    render();
                }
            }
            return 0;
        }
        case WM_MOUSELEAVE:
            popupHover = false;
            hoverSlider = false;
            if (visible)
                render();
            scheduleHide();
            return 0;
        case WM_LBUTTONDOWN: {
            if (!available || durationMs <= 0)
                return 0;
            const float x = dip(GET_X_LPARAM(lp));
            const float y = dip(GET_Y_LPARAM(lp));
            if (hitSlider(x, y)) {
                KillTimer(hwnd, kHideTimer);
                dragging = true;
                previewPositionMs = positionAt(x);
                lastDragRenderMs = GetTickCount64();
                SetCapture(hwnd);
                render();
            }
            return 0;
        }
        case WM_LBUTTONUP:
            if (dragging) {
                dragging = false;
                if (GetCapture() == hwnd)
                    ReleaseCapture();
                commitSeek(positionAt(dip(GET_X_LPARAM(lp))));
                scheduleHide();
            }
            return 0;
        case WM_CAPTURECHANGED:
            if (dragging) {
                dragging = false;
                if (visible)
                    render();
                scheduleHide();
            }
            return 0;
        case WM_MOUSEWHEEL: {
            if (!available || durationMs <= 0)
                return 0;
            const int steps = GET_WHEEL_DELTA_WPARAM(wp) / WHEEL_DELTA;
            if (steps != 0)
                commitSeek(displayPositionMs() + static_cast<int64_t>(steps) * kWheelStepMs);
            return 0;
        }
        case WM_TIMER:
            if (wp == kHideTimer) {
                KillTimer(hwnd, kHideTimer);
                if (!anchorHover && !popupHover && !dragging)
                    hide();
            }
            return 0;
        case WM_DPICHANGED: {
            dpi = GetDpiForWindow(hwnd);
            releaseResources();
            const auto* suggested = reinterpret_cast<const RECT*>(lp);
            SetWindowPos(hwnd, nullptr, suggested->left, suggested->top,
                         static_cast<int>(std::lround(kWidthDip * scale())),
                         static_cast<int>(std::lround(kHeightDip * scale())),
                         SWP_NOACTIVATE | SWP_NOZORDER);
            if (visible)
                render();
            return 0;
        }
        case WM_SETTINGCHANGE:
        case WM_THEMECHANGED:
        case WM_DWMCOMPOSITIONCHANGED:
            if (!fluent::applyAcrylicBackdrop(hwnd)) {
                runtime_log::writef(L"[progress-popup] native Acrylic unavailable");
                hide();
            }
            releaseResources();
            if (visible)
                render();
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps{};
            BeginPaint(hwnd, &ps);
            render();
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_CLOSE:
            hide();
            return 0;
        case WM_DESTROY:
            KillTimer(hwnd, kHideTimer);
            releaseResources();
            renderer.releaseAll();
            hwnd = nullptr;
            return 0;
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    }

    static LRESULT CALLBACK wndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
        Impl* self = nullptr;
        if (msg == WM_NCCREATE) {
            auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
            self = static_cast<Impl*>(cs->lpCreateParams);
            self->hwnd = h;
            SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        } else {
            self = reinterpret_cast<Impl*>(GetWindowLongPtrW(h, GWLP_USERDATA));
        }
        if (self)
            return self->handle(msg, wp, lp);
        return DefWindowProcW(h, msg, wp, lp);
    }
};

ProgressPopup::ProgressPopup() : impl_(std::make_unique<Impl>()) {}

ProgressPopup::~ProgressPopup() {
    destroy();
}

bool ProgressPopup::create(HINSTANCE inst) {
    if (impl_->hwnd)
        return true;
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = Impl::wndProc;
    wc.hInstance = inst;
    wc.lpszClassName = kWndClassName;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassExW(&wc);

    const DWORD exStyle = WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE |
                          WS_EX_NOREDIRECTIONBITMAP;
    const DWORD style = WS_POPUP | WS_THICKFRAME;
    impl_->hwnd = CreateWindowExW(exStyle, kWndClassName, L"QQMusicLyricProgressPopup", style,
                                  0, 0, 1, 1, nullptr, nullptr, inst, impl_.get());
    if (!impl_->hwnd)
        return false;
    impl_->dpi = GetDpiForWindow(impl_->hwnd);
    if (impl_->dpi == 0)
        impl_->dpi = GetDpiForSystem();
    if (!fluent::applyAcrylicBackdrop(impl_->hwnd) || !impl_->renderer.initialize()) {
        runtime_log::writef(L"[progress-popup] native Acrylic initialization failed");
        destroy();
        return false;
    }
    return true;
}

void ProgressPopup::destroy() {
    if (!impl_)
        return;
    impl_->hide();
    if (impl_->hwnd) {
        DestroyWindow(impl_->hwnd);
        impl_->hwnd = nullptr;
    }
    impl_->renderer.releaseAll();
}

void ProgressPopup::setProgress(uint64_t sessionGeneration, int64_t positionMs, int64_t durationMs,
                                bool available) {
    if (!impl_)
        return;
    const bool nextAvailable = available && durationMs > 0;
    const int64_t nextDuration = nextAvailable ? durationMs : 0;
    const int64_t nextPosition = nextAvailable ? std::clamp<int64_t>(positionMs, 0, nextDuration) : 0;
    const bool sessionChanged = impl_->sessionGeneration != sessionGeneration;
    if (sessionChanged || !nextAvailable) {
        impl_->clearPendingSeek();
        if (impl_->dragging) {
            impl_->dragging = false;
            if (impl_->hwnd && GetCapture() == impl_->hwnd)
                ReleaseCapture();
        }
    } else if (impl_->pendingSeek) {
        const ULONGLONG now = GetTickCount64();
        const bool confirmed =
            std::llabs(nextPosition - impl_->pendingSeekTargetMs) <= kPendingSeekConfirmToleranceMs;
        if (confirmed || now >= impl_->pendingSeekUntilMs) {
            impl_->clearPendingSeek();
        } else {
            // SMTC 异步跳转尚未回读到新位置，忽略旧锚点推算帧以防滑块回弹抽搐。
            impl_->durationMs = nextDuration;
            return;
        }
    }
    const bool changed = sessionChanged || impl_->available != nextAvailable ||
                         impl_->durationMs != nextDuration ||
                         (!impl_->dragging && impl_->positionMs / 200 != nextPosition / 200);
    impl_->sessionGeneration = sessionGeneration;
    impl_->available = nextAvailable;
    impl_->durationMs = nextDuration;
    impl_->positionMs = nextPosition;
    if (!nextAvailable) {
        impl_->hide();
        return;
    }
    if (changed && impl_->visible && !impl_->dragging)
        impl_->render();
}

void ProgressPopup::seekBy(int64_t deltaMs) {
    if (!impl_ || !impl_->available || impl_->durationMs <= 0 || deltaMs == 0)
        return;
    impl_->commitSeek(impl_->displayPositionMs() + deltaMs);
}

void ProgressPopup::setCallback(std::function<void(uint64_t, int64_t)> cb) {
    if (impl_)
        impl_->onSeek = std::move(cb);
}

void ProgressPopup::showNear(const RECT& anchorRect, bool verticalTaskbar, bool popupOnRight) {
    if (impl_)
        impl_->showNear(anchorRect, verticalTaskbar, popupOnRight);
}

void ProgressPopup::hide() {
    if (impl_)
        impl_->hide();
}

bool ProgressPopup::isVisible() const {
    return impl_ && impl_->visible;
}

void ProgressPopup::onAnchorEnter() {
    if (!impl_)
        return;
    impl_->anchorHover = true;
    if (impl_->hwnd)
        KillTimer(impl_->hwnd, kHideTimer);
}

void ProgressPopup::onAnchorLeave() {
    if (!impl_)
        return;
    impl_->anchorHover = false;
    impl_->scheduleHide();
}
