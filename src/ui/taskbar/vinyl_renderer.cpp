#include "ui/taskbar/vinyl_renderer.h"

#include "ui/fluent_theme.h"

#include <algorithm>
#include <cstdlib>

namespace taskbar_detail {
namespace {

constexpr float kVinylHaloWidth = 0.5f;
constexpr float kVinylOuterRatio = 0.40f; // 唱片直径约为底座边长的 80%，四边各留约 10%
constexpr float kVinylInnerRatio = 0.25f; // 圆形专辑封面半径 / 封面槽边长

} // namespace

float TaskbarVinylRenderer::innerRadius(float coverSize) {
    const float outerRadius = std::max(1.0f, coverSize * kVinylOuterRatio);
    const float recordRadius =
        std::max(0.5f, outerRadius - std::min(kVinylHaloWidth, outerRadius * 0.25f));
    return std::min(coverSize * kVinylInnerRatio, std::max(1.0f, recordRadius - 1.0f));
}

void TaskbarVinylRenderer::createBrushes(ID2D1DeviceContext* rt) {
    if (!rt)
        return;
    releaseBrushes();
    rt->CreateSolidColorBrush(D2D1::ColorF(0.055f, 0.060f, 0.068f),
                              brushBase_.ReleaseAndGetAddressOf());
    rt->CreateSolidColorBrush(D2D1::ColorF(0.70f, 0.72f, 0.75f, 0.16f),
                              brushGroove_.ReleaseAndGetAddressOf());
    const D2D1_GRADIENT_STOP stops[] = {
        {0.00f, D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.20f)},
        {0.22f, D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.26f)},
        {0.38f, D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.10f)},
        {0.52f, D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.45f)},
        {0.76f, D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.20f)},
        {1.00f, D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.15f)},
    };
    Microsoft::WRL::ComPtr<ID2D1GradientStopCollection> collection;
    if (SUCCEEDED(rt->CreateGradientStopCollection(stops, _countof(stops), D2D1_GAMMA_2_2,
                                                   D2D1_EXTEND_MODE_CLAMP, &collection)) &&
        collection) {
        rt->CreateLinearGradientBrush(
            D2D1::LinearGradientBrushProperties(D2D1::Point2F(0.0f, 0.0f),
                                                D2D1::Point2F(1.0f, 1.0f)),
            collection.Get(), brushSurface_.ReleaseAndGetAddressOf());
    }
    rt->CreateSolidColorBrush(D2D1::ColorF(0.07f, 0.08f, 0.09f),
                              brushTurntable_.ReleaseAndGetAddressOf());
}

void TaskbarVinylRenderer::createClipGeometry(ID2D1Factory* factory, float coverSize) {
    releaseClipGeometry();
    if (!factory)
        return;
    const float radius = innerRadius(coverSize);
    const D2D1_ELLIPSE coverEllipse{D2D1::Point2F(coverSize * 0.5f, coverSize * 0.5f),
                                    radius, radius};
    factory->CreateEllipseGeometry(coverEllipse, coverClip_.ReleaseAndGetAddressOf());
}

void TaskbarVinylRenderer::releaseClipGeometry() {
    coverClip_.Reset();
}

void TaskbarVinylRenderer::releaseBrushes() {
    brushBase_.Reset();
    brushGroove_.Reset();
    brushSurface_.Reset();
    brushTurntable_.Reset();
}

void TaskbarVinylRenderer::releaseResources() {
    releaseBrushes();
    releaseClipGeometry();
}

void TaskbarVinylRenderer::draw(const VinylDrawContext& ctx) const {
    auto* rt = ctx.rt;
    const float s = ctx.coverSize;
    if (!rt || s <= 0.0f)
        return;

    // 使用任务栏内容主题，与普通设置窗口的主题来源独立。
    const auto deckFill =
        ctx.lightTheme ? D2D1::ColorF(0.97f, 0.975f, 0.98f) : D2D1::ColorF(0.07f, 0.08f, 0.09f);
    const auto deckStroke =
        ctx.lightTheme ? D2D1::ColorF(0.77f, 0.79f, 0.81f) : D2D1::ColorF(0.27f, 0.28f, 0.30f);
    const auto platterRim =
        ctx.lightTheme ? D2D1::ColorF(0.82f, 0.84f, 0.86f) : D2D1::ColorF(0.37f, 0.39f, 0.42f);
    const auto armShadow = ctx.lightTheme ? D2D1::ColorF(0.45f, 0.47f, 0.49f, 0.60f)
                                          : D2D1::ColorF(0.02f, 0.025f, 0.03f);
    const auto metal =
        ctx.lightTheme ? D2D1::ColorF(0.86f, 0.88f, 0.90f) : D2D1::ColorF(0.65f, 0.67f, 0.69f);
    const auto pivotFill = ctx.lightTheme ? D2D1::ColorF(0.99f, 0.99f, 0.99f) : metal;
    const auto cartridge =
        ctx.lightTheme ? D2D1::ColorF(0.95f, 0.96f, 0.97f) : D2D1::ColorF(0.12f, 0.13f, 0.15f);

    const D2D1_POINT_2F center = D2D1::Point2F(ctx.coverX + s * 0.5f, ctx.coverY + s * 0.5f);
    const float outerRadius = std::max(1.0f, s * kVinylOuterRatio);
    const float haloWidth = std::min(kVinylHaloWidth, outerRadius * 0.25f);
    const float recordRadius = std::max(0.5f, outerRadius - haloWidth);
    const float coverRadius = innerRadius(s);
    const D2D1_ELLIPSE outer{center, outerRadius, outerRadius};
    const D2D1_ELLIPSE record{center, recordRadius, recordRadius};
    const D2D1_ELLIPSE inner{center, coverRadius, coverRadius};

    // 固定唱机底座始终位于原封面槽内，不随盘面旋转，也不挤占歌曲文字。
    if (brushTurntable_.Get()) {
        const auto deck = D2D1::RoundedRect(
            D2D1::RectF(ctx.coverX, ctx.coverY, ctx.coverX + s, ctx.coverY + s), 4.0f, 4.0f);
        brushTurntable_->SetColor(deckFill);
        rt->FillRoundedRectangle(deck, brushTurntable_.Get());
        brushTurntable_->SetColor(deckStroke);
        rt->DrawRoundedRectangle(deck, brushTurntable_.Get(), 0.6f);
    }

    // 光照固定在唱机上，盘面和封面在光照下旋转。
    D2D1_MATRIX_3X2_F previous{};
    rt->GetTransform(&previous);
    if (brushTurntable_.Get()) {
        brushTurntable_->SetColor(platterRim);
        rt->FillEllipse(outer, brushTurntable_.Get());
    }
    if (brushBase_.Get()) {
        brushBase_->SetColor(ctx.hasDominantColor
                                 ? fluent::toD2D(ctx.dominantColor)
                                 : (ctx.lightTheme ? D2D1::ColorF(0.72f, 0.73f, 0.74f)
                                                   : D2D1::ColorF(0.055f, 0.060f, 0.068f)));
        rt->FillEllipse(record, brushBase_.Get());
    }
    if (brushSurface_.Get()) {
        brushSurface_->SetStartPoint(
            D2D1::Point2F(center.x - recordRadius * 0.8f, center.y - recordRadius * 0.6f));
        brushSurface_->SetEndPoint(
            D2D1::Point2F(center.x + recordRadius * 0.8f, center.y + recordRadius * 0.6f));
        rt->FillEllipse(record, brushSurface_.Get());
    }
    rt->SetTransform(D2D1::Matrix3x2F::Rotation(ctx.discAngle, center) * previous);

    if (brushGroove_.Get()) {
        brushGroove_->SetColor(ctx.lightTheme ? D2D1::ColorF(0.25f, 0.27f, 0.29f, 0.20f)
                                              : D2D1::ColorF(0.70f, 0.72f, 0.75f, 0.16f));
        // 细窄的低对比唱纹，避免小尺寸下粗同心圆看起来像一圈圈边框。
        for (float radius = coverRadius + 0.9f; radius < recordRadius - 0.4f; radius += 1.1f) {
            rt->DrawEllipse(D2D1_ELLIPSE{center, radius, radius}, brushGroove_.Get(), 0.35f);
        }
    }

    const D2D1_RECT_F innerRect = D2D1::RectF(center.x - coverRadius, center.y - coverRadius,
                                              center.x + coverRadius, center.y + coverRadius);
    if (ctx.coverBitmap && coverClip_.Get() && ctx.coverLayer) {
        rt->PushLayer(D2D1::LayerParameters1(D2D1::InfiniteRect(), coverClip_.Get(),
                                             D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                                             D2D1::Matrix3x2F::Translation(ctx.coverX, ctx.coverY)),
                      ctx.coverLayer);
        rt->DrawBitmap(ctx.coverBitmap, innerRect, 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
        rt->PopLayer();
    } else if (ctx.fallbackDimBrush) {
        rt->FillEllipse(inner, ctx.fallbackDimBrush);
    }
    if (brushGroove_.Get())
        rt->DrawEllipse(inner, brushGroove_.Get(), 0.8f);
    if (brushBase_.Get()) {
        brushBase_->SetColor(ctx.lightTheme ? D2D1::ColorF(0.78f, 0.80f, 0.82f)
                                            : D2D1::ColorF(0.035f, 0.04f, 0.05f));
        rt->FillEllipse(
            D2D1_ELLIPSE{center, std::max(0.8f, s * 0.025f), std::max(0.8f, s * 0.025f)},
            brushBase_.Get());
    }
    rt->SetTransform(previous);

    // 唱臂绕固定支点收放，盘面旋转与唱臂动作分别计时。
    if (brushTurntable_.Get()) {
        const auto point = [&](float x, float y) {
            return D2D1::Point2F(ctx.coverX + s * x, ctx.coverY + s * y);
        };
        const auto pivot = point(0.85f, 0.17f);
        const auto elbow = point(0.81f, 0.55f);
        const auto stylus = point(0.73f, 0.75f);
        rt->SetTransform(D2D1::Matrix3x2F::Rotation(ctx.armAngle, pivot) * previous);
        brushTurntable_->SetColor(armShadow);
        rt->FillEllipse(D2D1::Ellipse(pivot, s * 0.075f, s * 0.075f), brushTurntable_.Get());
        rt->DrawLine(pivot, elbow, brushTurntable_.Get(), std::max(1.2f, s * 0.045f));
        rt->DrawLine(elbow, stylus, brushTurntable_.Get(), std::max(1.2f, s * 0.045f));
        brushTurntable_->SetColor(metal);
        rt->DrawLine(pivot, elbow, brushTurntable_.Get(), std::max(0.65f, s * 0.023f));
        rt->DrawLine(elbow, stylus, brushTurntable_.Get(), std::max(0.65f, s * 0.023f));
        brushTurntable_->SetColor(pivotFill);
        rt->FillEllipse(D2D1::Ellipse(pivot, s * 0.035f, s * 0.035f), brushTurntable_.Get());
        brushTurntable_->SetColor(cartridge);
        rt->DrawLine(point(0.75f, 0.70f), point(0.71f, 0.79f), brushTurntable_.Get(),
                     std::max(1.5f, s * 0.07f));
        rt->SetTransform(previous);
    }
}

} // namespace taskbar_detail
