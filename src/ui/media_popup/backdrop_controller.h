#pragma once

#include <windows.h>
#include <d2d1_1.h>
#include <wrl/client.h>

namespace media_popup_detail {

/// 管理媒体弹窗磨砂背景抓屏、高斯模糊、动态底色画刷以及背景亮度采样状态。
class MediaPopupBackdrop {
public:
    void markDirty() { dirty_ = true; }
    void markDynamicBackgroundDirty() { dynamicBackgroundDirty_ = true; }

    bool hasBackdrop() const { return bmp_ != nullptr; }
    float lastLuminance() const { return lastLuminance_; }
    bool hasContrastReady() const { return bmp_ != nullptr && lastLuminance_ >= 0.0f; }

    /// 判断本次捕获是否需要重置文字对比度（非脏或正在使用历史快照互切时跳过）。
    bool needsRecapture(bool categoryTransitionActive) const {
        if (!dirty_)
            return false;
        if (categoryTransitionActive && bmp_ && lastLuminance_ >= 0.0f)
            return false;
        return true;
    }

    void releaseCaptureResources();
    void releaseGeometryAndLayer();
    void releaseDynamicBackground();
    void releaseAll();

    bool createGeometryAndLayer(ID2D1Factory* factory, ID2D1DeviceContext* rt,
                                float widthDip, float heightDip, float cornerDip);
    bool ensureDynamicTintBrush(ID2D1DeviceContext* rt, D2D1_COLOR_F tintColor);

    bool capture(HWND hwnd, HWND anchor, const RECT& cardScreenRect, UINT dpi,
                 ID2D1DeviceContext* rt, bool categoryTransitionActive,
                 bool frostedActive, bool force);

    void drawBackdrop(ID2D1DeviceContext* rt, float widthDip, float heightDip,
                      bool enabled, bool useCachedLayer) const;
    void drawDynamicTint(ID2D1DeviceContext* rt, float widthDip, float heightDip,
                         float cornerDip, bool enabled) const;

private:
    bool dirty_ = true;
    bool dynamicBackgroundDirty_ = true;
    float lastLuminance_ = -1.0f;
    D2D1_RECT_F sourceRect_{};

    Microsoft::WRL::ComPtr<ID2D1Bitmap1> bmp_;
    Microsoft::WRL::ComPtr<ID2D1Effect> blur_;
    Microsoft::WRL::ComPtr<ID2D1RoundedRectangleGeometry> clip_;
    Microsoft::WRL::ComPtr<ID2D1Layer> layer_;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> dynamicTintBrush_;
};

} // namespace media_popup_detail
