#pragma once

#include <windows.h>
#include <d2d1_1.h>
#include <wrl/client.h>

namespace taskbar_detail {

/// 绘制单帧任务栏黑胶唱片封面时由宿主借出的只读设备资源与状态。
struct VinylDrawContext {
    ID2D1DeviceContext* rt = nullptr;
    ID2D1Bitmap* coverBitmap = nullptr;
    ID2D1Layer* coverLayer = nullptr;
    ID2D1SolidColorBrush* fallbackDimBrush = nullptr;
    float coverX = 0.0f;
    float coverY = 0.0f;
    float coverSize = 0.0f;
    float discAngle = 0.0f;
    float armAngle = 0.0f;
    bool lightTheme = false;
    bool hasDominantColor = false;
    COLORREF dominantColor = RGB(0, 0, 0);
};

/// 管理任务栏黑胶唱片封面的画刷、圆形裁剪几何与绘制。
class TaskbarVinylRenderer {
public:
    static float innerRadius(float coverSize);

    void createBrushes(ID2D1DeviceContext* rt);
    void createClipGeometry(ID2D1Factory* factory, float coverSize);
    void releaseClipGeometry();
    void releaseBrushes();
    void releaseResources();

    void draw(const VinylDrawContext& ctx) const;

private:
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brushBase_;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brushGroove_;
    Microsoft::WRL::ComPtr<ID2D1LinearGradientBrush> brushSurface_;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brushTurntable_;
    Microsoft::WRL::ComPtr<ID2D1EllipseGeometry> coverClip_;
};

} // namespace taskbar_detail
