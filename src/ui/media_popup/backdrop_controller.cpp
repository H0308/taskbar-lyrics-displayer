#include "ui/media_popup/backdrop_controller.h"

#include "graphics/pixel_luminance.h"

#include <d2d1effects.h>
#include <dwmapi.h>

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace media_popup_detail {
namespace {

constexpr float kBackdropBlurDip = 16.0f;
constexpr float kBackdropCapturePaddingDip = kBackdropBlurDip * 3.0f;

// d2d1effects.h 只声明这个 GUID；当前工程的链接配置不提供其外部定义，
// 这里保留 Direct2D 标准 Gaussian Blur CLSID 的内部定义。
constexpr CLSID kGaussianBlurClsid = {
    0x1feb6d69, 0x2fe6, 0x4ac9, {0x8c, 0x58, 0x1d, 0x7f, 0x93, 0xe7, 0xa6, 0xa5}};

} // namespace

void MediaPopupBackdrop::releaseCaptureResources() {
    bmp_.Reset();
    blur_.Reset();
    sourceRect_ = {};
    lastLuminance_ = -1.0f;
    dirty_ = true;
}

void MediaPopupBackdrop::releaseGeometryAndLayer() {
    clip_.Reset();
    layer_.Reset();
}

void MediaPopupBackdrop::releaseDynamicBackground() {
    dynamicTintBrush_.Reset();
    dynamicBackgroundDirty_ = true;
}

void MediaPopupBackdrop::releaseAll() {
    releaseCaptureResources();
    releaseGeometryAndLayer();
    releaseDynamicBackground();
}

bool MediaPopupBackdrop::createGeometryAndLayer(ID2D1Factory* factory, ID2D1DeviceContext* rt,
                                                float widthDip, float heightDip,
                                                float cornerDip) {
    if (!factory || !rt)
        return false;
    clip_.Reset();
    layer_.Reset();
    if (FAILED(factory->CreateRoundedRectangleGeometry(
            D2D1::RoundedRect(D2D1::RectF(0.0f, 0.0f, widthDip, heightDip),
                              cornerDip, cornerDip),
            &clip_)) ||
        FAILED(rt->CreateLayer(&layer_))) {
        releaseGeometryAndLayer();
        return false;
    }
    return true;
}

bool MediaPopupBackdrop::ensureDynamicTintBrush(ID2D1DeviceContext* rt, D2D1_COLOR_F tintColor) {
    if (!dynamicBackgroundDirty_ && dynamicTintBrush_)
        return true;

    dynamicTintBrush_.Reset();
    if (!rt)
        return false;

    if (FAILED(rt->CreateSolidColorBrush(tintColor, &dynamicTintBrush_)) || !dynamicTintBrush_) {
        dynamicTintBrush_.Reset();
        return false;
    }
    dynamicBackgroundDirty_ = false;
    return true;
}

bool MediaPopupBackdrop::capture(HWND hwnd, HWND anchor, const RECT& cardRect, UINT dpi,
                                   ID2D1DeviceContext* rt, bool categoryTransitionActive,
                                   bool frostedActive, bool force) {
    if (!dirty_)
        return true;
    // 页面互切不改变窗口后方的场景；已有快照时直接复用，即使其他设置
    // 把 dirty 标记置上，也不能在可见转场中通过隐藏窗口重新抓屏。
    if (categoryTransitionActive && bmp_ && lastLuminance_ >= 0.0f) {
        dirty_ = false;
        return true;
    }

    dirty_ = false;
    blur_.Reset();
    bmp_.Reset();
    sourceRect_ = {};
    lastLuminance_ = -1.0f;
    if ((!force && !frostedActive) || !hwnd)
        return true;

    const int cardWidthPx = cardRect.right - cardRect.left;
    const int cardHeightPx = cardRect.bottom - cardRect.top;
    if (cardWidthPx <= 0 || cardHeightPx <= 0)
        return false;

    const float dipScale = static_cast<float>(dpi) / 96.0f;
    auto toDip = [dipScale](int px) { return static_cast<float>(px) / dipScale; };

    const int padding = static_cast<int>(std::ceil(kBackdropCapturePaddingDip * dipScale));
    const LONG desktopLeft = GetSystemMetrics(SM_XVIRTUALSCREEN);
    const LONG desktopTop = GetSystemMetrics(SM_YVIRTUALSCREEN);
    const LONG desktopRight = desktopLeft + GetSystemMetrics(SM_CXVIRTUALSCREEN);
    const LONG desktopBottom = desktopTop + GetSystemMetrics(SM_CYVIRTUALSCREEN);
    // 在实际桌面范围内多采集三倍模糊半径，最终只显示中央的卡片区域。
    RECT captureRect{
        std::max(cardRect.left - padding, desktopLeft),
        std::max(cardRect.top - padding, desktopTop),
        std::min(cardRect.right + padding, desktopRight),
        std::min(cardRect.bottom + padding, desktopBottom)};
    RECT anchorRect{};
    const bool anchorVisible = anchor && anchor != hwnd && IsWindow(anchor) &&
                               IsWindowVisible(anchor) && GetWindowRect(anchor, &anchorRect);
    const bool anchorOverlapsCard = anchorVisible &&
                                    anchorRect.left < cardRect.right &&
                                    anchorRect.right > cardRect.left &&
                                    anchorRect.top < cardRect.bottom &&
                                    anchorRect.bottom > cardRect.top;
    // 额外采样只取卡片所在的一侧，不能为了取得外围像素而隐藏歌词窗口。
    // 靠近任务栏的一边由模糊的硬边界延伸，其余方向保留原有采样余量。
    if (anchorVisible && !anchorOverlapsCard &&
        anchorRect.left < captureRect.right && anchorRect.right > captureRect.left &&
        anchorRect.top < captureRect.bottom && anchorRect.bottom > captureRect.top) {
        if (anchorRect.top >= cardRect.bottom)
            captureRect.bottom = std::min(captureRect.bottom, anchorRect.top);
        else if (anchorRect.bottom <= cardRect.top)
            captureRect.top = std::max(captureRect.top, anchorRect.bottom);
        else if (anchorRect.left >= cardRect.right)
            captureRect.right = std::min(captureRect.right, anchorRect.left);
        else if (anchorRect.right <= cardRect.left)
            captureRect.left = std::max(captureRect.left, anchorRect.right);
    }

    const int width = captureRect.right - captureRect.left;
    const int height = captureRect.bottom - captureRect.top;
    const int cardLeft = cardRect.left - captureRect.left;
    const int cardTop = cardRect.top - captureRect.top;
    if (cardLeft < 0 || cardTop < 0 || cardLeft + cardWidthPx > width ||
        cardTop + cardHeightPx > height)
        return false;

    // 首次显示时窗口本来是隐藏的；切换设置或主题时可能已经可见，
    // 临时隐藏它再抓取，避免把自己的卡片拍进背景图形成递归灰层。
    const bool restoreVisible = IsWindowVisible(hwnd) != FALSE;
    if (restoreVisible)
        ShowWindow(hwnd, SW_HIDE);

    // 仅卡片本体确实覆盖歌词时才沿用原有的避让采样，外围采样不改变歌词显隐。
    if (anchorOverlapsCard)
        ShowWindow(anchor, SW_HIDE);

    // 只有实际隐藏过窗口才需要等合成器移除旧画面，避免无条件等待刷新周期。
    if (restoreVisible || anchorOverlapsCard)
        DwmFlush();

    auto restoreWindows = [&]() {
        if (anchorOverlapsCard)
            ShowWindow(anchor, SW_SHOWNA);
        if (restoreVisible)
            ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    };

    HDC screenDc = GetDC(nullptr);
    if (!screenDc) {
        restoreWindows();
        return false;
    }

    BITMAPINFO bitmapInfo{};
    bitmapInfo.bmiHeader.biSize = sizeof(bitmapInfo.bmiHeader);
    bitmapInfo.bmiHeader.biWidth = width;
    bitmapInfo.bmiHeader.biHeight = -height;
    bitmapInfo.bmiHeader.biPlanes = 1;
    bitmapInfo.bmiHeader.biBitCount = 32;
    bitmapInfo.bmiHeader.biCompression = BI_RGB;

    void* pixels = nullptr;
    HBITMAP dib = CreateDIBSection(screenDc, &bitmapInfo, DIB_RGB_COLORS, &pixels, nullptr, 0);
    HDC memoryDc = dib ? CreateCompatibleDC(screenDc) : nullptr;
    HGDIOBJ oldBitmap = nullptr;
    bool copied = false;
    if (memoryDc && dib) {
        oldBitmap = SelectObject(memoryDc, dib);
        if (oldBitmap && oldBitmap != HGDI_ERROR)
            copied = BitBlt(memoryDc, 0, 0, width, height, screenDc, captureRect.left,
                            captureRect.top, SRCCOPY | CAPTUREBLT) != FALSE;
    }
    if (oldBitmap && oldBitmap != HGDI_ERROR)
        SelectObject(memoryDc, oldBitmap);
    if (memoryDc)
        DeleteDC(memoryDc);
    ReleaseDC(nullptr, screenDc);
    restoreWindows();
    if (!copied || !pixels) {
        if (dib)
            DeleteObject(dib);
        return false;
    }

    const auto* cardPixels = static_cast<const uint8_t*>(pixels) +
        (static_cast<size_t>(cardTop) * width + cardLeft) * 4;
    lastLuminance_ = graphics::backdropLuminance(cardPixels, cardWidthPx, cardHeightPx, width);

    if (!rt) {
        DeleteObject(dib);
        return false;
    }
    const auto props = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_NONE,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE),
        static_cast<float>(dpi), static_cast<float>(dpi));
    Microsoft::WRL::ComPtr<ID2D1Bitmap1> captured;
    const HRESULT bitmapHr = rt->CreateBitmap(
        D2D1::SizeU(static_cast<UINT>(width), static_cast<UINT>(height)),
        pixels, static_cast<UINT>(width * 4), &props, &captured);
    DeleteObject(dib);
    if (FAILED(bitmapHr) || !captured)
        return false;

    bmp_ = std::move(captured);
    sourceRect_ = D2D1::RectF(toDip(cardLeft), toDip(cardTop),
                              toDip(cardLeft + cardWidthPx), toDip(cardTop + cardHeightPx));

    Microsoft::WRL::ComPtr<ID2D1Effect> blur;
    HRESULT hr = rt->CreateEffect(kGaussianBlurClsid, &blur);
    if (SUCCEEDED(hr) && blur) {
        blur->SetInput(0, bmp_.Get());
        hr = blur->SetValue(D2D1_GAUSSIANBLUR_PROP_STANDARD_DEVIATION, kBackdropBlurDip);
        if (SUCCEEDED(hr))
            hr = blur->SetValue(D2D1_GAUSSIANBLUR_PROP_OPTIMIZATION,
                                D2D1_GAUSSIANBLUR_OPTIMIZATION_BALANCED);
        if (SUCCEEDED(hr))
            hr = blur->SetValue(D2D1_GAUSSIANBLUR_PROP_BORDER_MODE, D2D1_BORDER_MODE_HARD);
        if (SUCCEEDED(hr))
            blur_ = std::move(blur);
    }
    return true;
}

void MediaPopupBackdrop::drawBackdrop(ID2D1DeviceContext* rt, float widthDip, float heightDip,
                                      bool enabled, bool useCachedLayer) const {
    if (!enabled || !rt || (!blur_ && !bmp_))
        return;

    Microsoft::WRL::ComPtr<ID2D1Layer> temporaryLayer;
    ID2D1Layer* clipLayer = useCachedLayer ? layer_.Get() : nullptr;
    if (clip_ && !clipLayer && FAILED(rt->CreateLayer(&temporaryLayer)))
        return;
    clipLayer = clipLayer ? clipLayer : temporaryLayer.Get();
    const bool clipped = clip_ && clipLayer;
    if (clipped) {
        rt->PushLayer(D2D1::LayerParameters1(
                          D2D1::InfiniteRect(), clip_.Get(),
                          D2D1_ANTIALIAS_MODE_PER_PRIMITIVE),
                      clipLayer);
    }
    if (blur_) {
        const D2D1_POINT_2F offset = D2D1::Point2F(0.0f, 0.0f);
        rt->DrawImage(blur_.Get(), &offset, &sourceRect_, D2D1_INTERPOLATION_MODE_LINEAR,
                      D2D1_COMPOSITE_MODE_SOURCE_OVER);
    } else {
        rt->DrawBitmap(bmp_.Get(), D2D1::RectF(0.0f, 0.0f, widthDip, heightDip), 1.0f,
                       D2D1_BITMAP_INTERPOLATION_MODE_LINEAR, sourceRect_);
    }
    if (clipped)
        rt->PopLayer();
}

void MediaPopupBackdrop::drawDynamicTint(ID2D1DeviceContext* rt, float widthDip, float heightDip,
                                         float cornerDip, bool enabled) const {
    if (!enabled || !rt || !dynamicTintBrush_)
        return;

    const auto card = D2D1::RoundedRect(D2D1::RectF(0.0f, 0.0f, widthDip, heightDip),
                                         cornerDip, cornerDip);
    rt->FillRoundedRectangle(card, dynamicTintBrush_.Get());
}

} // namespace media_popup_detail
