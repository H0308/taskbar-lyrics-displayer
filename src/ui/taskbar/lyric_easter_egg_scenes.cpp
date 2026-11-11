#include "ui/taskbar/lyric_easter_eggs.h"

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <iterator>
#include <utility>

namespace {
constexpr float kPi = 3.14159265358979323846f;

// 固定种子决定一次彩蛋的构图，不在每次重绘时重新随机生成对象。
float variation(uint32_t seed, uint32_t index) {
    uint32_t value = seed ^ ((index + 1) * 0x9e3779b9u);
    value ^= value >> 16;
    value *= 0x7feb352du;
    value ^= value >> 15;
    return static_cast<float>(value & 65535u) / 65535.0f;
}

D2D1_POINT_2F cubic(D2D1_POINT_2F a, D2D1_POINT_2F b, D2D1_POINT_2F c,
                    D2D1_POINT_2F d, float t) {
    const float s = 1 - t;
    return {s * s * s * a.x + 3 * s * s * t * b.x + 3 * s * t * t * c.x + t * t * t * d.x,
            s * s * s * a.y + 3 * s * s * t * b.y + 3 * s * t * t * c.y + t * t * t * d.y};
}
} // namespace

bool LyricEasterEggs::ensureFlagBitmaps(ID2D1DeviceContext* target) {
    if (std::all_of(flagBitmaps_.begin(), flagBitmaps_.end(),
                    [](const auto& bitmap) { return bitmap.Get() != nullptr; }))
        return true;
    decltype(flagBitmaps_) bitmaps;
    const auto size = D2D1::SizeF(192.0f, 128.0f);
    const auto pixels = D2D1::SizeU(384, 256);
    for (size_t index = 0; index < bitmaps.size(); ++index) {
        const auto flag = static_cast<Flag>(index);
        Microsoft::WRL::ComPtr<ID2D1BitmapRenderTarget> canvas;
        if (FAILED(target->CreateCompatibleRenderTarget(
                &size, &pixels, nullptr, D2D1_COMPATIBLE_RENDER_TARGET_OPTIONS_NONE,
                canvas.GetAddressOf())))
            return false;
        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> ink;
        const auto red = D2D1::ColorF(0xDE2910);
        const auto gold = D2D1::ColorF(0xFFDE00);
        const auto green = D2D1::ColorF(0x00785E);
        if (FAILED(canvas->CreateSolidColorBrush(red, ink.GetAddressOf())))
            return false;
        const float scale = canvas->GetSize().height;
        const auto base = D2D1::Matrix3x2F::Scale(scale, scale);
        canvas->BeginDraw();
        canvas->Clear(flag == Flag::Macau ? green : red);
        auto star = [&](D2D1_POINT_2F center, float radius, float degrees,
                         const D2D1_MATRIX_3X2_F& transform, D2D1_COLOR_F color) {
            canvas->SetTransform(D2D1::Matrix3x2F::Scale(radius, radius) *
                D2D1::Matrix3x2F::Rotation(degrees) *
                D2D1::Matrix3x2F::Translation(center.x, center.y) * transform);
            ink->SetColor(color);
            canvas->FillGeometry(flagStar_.Get(), ink.Get());
        };
        if (flag == Flag::China) {
            star({0.25f, 0.25f}, 0.15f, 0.0f, base, gold);
            constexpr D2D1_POINT_2F stars[] = {
                {0.50f, 0.10f}, {0.60f, 0.20f}, {0.60f, 0.35f}, {0.50f, 0.45f},
            };
            for (const auto center : stars) {
                const float angle = std::atan2(0.25f - center.y, 0.25f - center.x);
                star(center, 0.05f, angle * 180.0f / kPi + 90.0f, base, gold);
            }
        } else if (flag == Flag::HongKong) {
            const auto flower = D2D1::Matrix3x2F::Scale(0.30f, 0.30f) *
                D2D1::Matrix3x2F::Translation(0.75f, 0.50f) * base;
            for (int petalIndex = 0; petalIndex < 5; ++petalIndex) {
                const auto petal = D2D1::Matrix3x2F::Rotation(petalIndex * 72.0f) * flower;
                canvas->SetTransform(petal);
                ink->SetColor(D2D1::ColorF(D2D1::ColorF::White));
                canvas->FillGeometry(bauhiniaPetal_.Get(), ink.Get());
                ink->SetColor(red);
                canvas->DrawGeometry(bauhiniaVein_.Get(), ink.Get(), 0.035f);
                star({-0.17f, -0.64f}, 0.105f, -15.0f, petal, red);
            }
        } else if (flag == Flag::Macau) {
            // 三瓣白莲花、弧形五星、大桥与由近向远变细的海水。
            star({0.75f, 0.19f}, 0.075f, 0.0f, base, gold);
            constexpr D2D1_POINT_2F smallStars[] = {
                {0.51f, 0.32f}, {0.61f, 0.23f}, {0.89f, 0.23f}, {0.99f, 0.32f},
            };
            for (const auto center : smallStars)
                star(center, 0.042f, 0.0f, base, gold);
            const auto lotus = D2D1::Matrix3x2F::Translation(0.75f, 0.62f) * base;
            ink->SetColor(D2D1::ColorF(D2D1::ColorF::White));
            for (const float angle : {-57.0f, 57.0f, 0.0f}) {
                canvas->SetTransform(D2D1::Matrix3x2F::Rotation(angle) * lotus);
                canvas->FillGeometry(lotusPetal_.Get(), ink.Get());
                // 细绿边保留三瓣之间的分界，随旗面一起变形。
                ink->SetColor(green);
                canvas->DrawGeometry(lotusPetal_.Get(), ink.Get(), 0.009f);
                ink->SetColor(D2D1::ColorF(D2D1::ColorF::White));
            }
            canvas->SetTransform(base);
            canvas->FillRectangle(D2D1::RectF(0.43f, 0.65f, 1.07f, 0.671f), ink.Get());
            canvas->DrawGeometry(macauBridge_.Get(), ink.Get(), 0.016f);
            for (const float x : {0.61f, 0.89f})
                canvas->DrawLine({x, 0.66f}, {x, 0.75f}, ink.Get(), 0.018f);
            constexpr float waterY[] = {0.77f, 0.805f, 0.85f, 0.905f};
            constexpr float waterWidth[] = {0.29f, 0.26f, 0.21f, 0.13f};
            for (size_t row = 0; row < std::size(waterY); ++row) {
                const float thickness = 0.012f + static_cast<float>(row) * 0.006f;
                canvas->FillRectangle(D2D1::RectF(0.75f - waterWidth[row], waterY[row],
                    0.75f + waterWidth[row], waterY[row] + thickness), ink.Get());
            }
        }
        if (FAILED(canvas->EndDraw()) || FAILED(canvas->GetBitmap(bitmaps[index].GetAddressOf())))
            return false;
    }
    flagBitmaps_ = std::move(bitmaps);
    return true;
}

void LyricEasterEggs::drawChinaRegionalFlags(ID2D1DeviceContext* target,
                                           ID2D1SolidColorBrush* brush,
                                           D2D1_RECT_F viewport, float age, float opacity,
                                           Flag regional) {
    if (!ensureFlagBitmaps(target))
        return;
    D2D1_MATRIX_3X2_F previous;
    target->GetTransform(&previous);
    const float h = viewport.bottom - viewport.top;
    const float w = viewport.right - viewport.left;
    const float unit = std::min(h, 72.0f);
    const float alpha = fade(age) * opacity;
    const float nationalH = std::min(unit * 0.62f, w / 3.25f);
    const float regionalH = nationalH * 0.84f;
    const float gap = nationalH * 0.25f;
    const float totalW = (nationalH + regionalH) * 1.5f + gap;
    const float left = (viewport.left + viewport.right - totalW) * 0.5f;
    const float top = viewport.top + (h - nationalH) * 0.5f - unit * 0.04f;
    BOOL motionEnabled = TRUE;
    SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &motionEnabled, 0);
    const float time = motionEnabled ? age / 1100.0f * 2.0f * kPi : 0.0f;

    const std::array<Flag, 2> flags = {Flag::China, regional};
    for (size_t index = 0; index < flags.size(); ++index) {
        auto* bitmap = flagBitmaps_[static_cast<size_t>(flags[index])].Get();
        const float flagH = index == 0 ? nationalH : regionalH;
        const float x = index == 0 ? left : left + nationalH * 1.5f + gap;
        const float y = top + nationalH - flagH;
        const float flagW = flagH * 1.5f;
        const float phase = time + static_cast<float>(index) * 0.35f;
        auto wave = [&](float u) {
            if (!motionEnabled)
                return 0.0f;
            return flagH * 0.075f * u *
                (std::sin(u * 2.0f * kPi * 1.15f - phase) +
                 0.28f * std::sin(u * 4.0f * kPi - phase * 1.4f));
        };
        // 旗杆固定，波峰由杆侧向自由端传播；两面旗帜保持同一风向。
        target->SetTransform(previous);
        brush->SetColor(D2D1::ColorF(0.30f, 0.32f, 0.36f, alpha * 0.88f));
        target->DrawLine({x - 0.6f, y - flagH * 0.07f},
                          {x - 0.6f, top + nationalH * 1.18f}, brush, 1.1f);
        brush->SetColor(D2D1::ColorF(0.96f, 0.82f, 0.43f, alpha));
        target->FillEllipse(D2D1::Ellipse({x - 0.6f, y - flagH * 0.08f}, 1.3f, 1.3f), brush);
        const auto imageSize = bitmap->GetSize();
        const float scale = flagH / imageSize.height;
        constexpr int strips = 32;
        for (int strip = 0; strip < strips; ++strip) {
            const float u0 = static_cast<float>(strip) / strips;
            const float u1 = static_cast<float>(strip + 1) / strips;
            const float sx0 = imageSize.width * u0;
            const float sx1 = imageSize.width * u1;
            const float dy0 = wave(u0);
            const float dy1 = wave(u1);
            const float slope = (dy1 - dy0) / (sx1 - sx0);
            const auto deformation = D2D1::Matrix3x2F(
                scale, slope, 0.0f, scale, x, y + dy0 - slope * sx0);
            target->SetTransform(deformation * previous);
            const auto band = D2D1::RectF(sx0, 0.0f, sx1, imageSize.height);
            target->DrawBitmap(bitmap, band, alpha,
                                D2D1_BITMAP_INTERPOLATION_MODE_LINEAR, band);
            // 褶皱的亮面与暗面沿同一波形移动，旗面图案随布料一起变形。
            const float fold = motionEnabled
                ? std::cos((u0 + u1) * kPi * 1.15f - phase) * u1 : 0.0f;
            brush->SetColor(D2D1::ColorF(fold > 0.0f ? D2D1::ColorF::White
                                                    : D2D1::ColorF::Black,
                                         std::fabs(fold) * 0.14f * alpha));
            target->FillRectangle(band, brush);
        }
        target->SetTransform(previous);
        brush->SetColor(D2D1::ColorF(1.0f, 0.88f, 0.73f, alpha * 0.36f));
        for (int strip = 0; strip < strips; ++strip) {
            const float u0 = static_cast<float>(strip) / strips;
            const float u1 = static_cast<float>(strip + 1) / strips;
            target->DrawLine({x + flagW * u0, y + wave(u0)},
                              {x + flagW * u1, y + wave(u1)}, brush, 0.5f);
        }
    }
    target->SetTransform(previous);
}

void LyricEasterEggs::drawRain(ID2D1DeviceContext* target, ID2D1SolidColorBrush* brush,
                             D2D1_RECT_F viewport, float age, float opacity, uint32_t seed) {
    const float h = viewport.bottom - viewport.top;
    const float w = viewport.right - viewport.left;
    const float unit = std::min(h, 72.0f);
    const float alpha = fade(age) * opacity;
    for (uint32_t i = 0; i < 14; ++i) {
        const float elapsed = age - i * 85.0f;
        if (elapsed < 0)
            continue;
        const float fallMs = 560.0f + variation(seed, i + 20) * 180.0f;
        const float x = viewport.left + w * (0.08f + variation(seed, i) * 0.84f);
        const float drift = unit * 0.22f;
        if (elapsed < fallMs) {
            const float t = elapsed / fallMs;
            const auto tip = D2D1::Point2F(x - drift * t, viewport.top + h * t);
            brush->SetColor(D2D1::ColorF(0.35f, 0.56f, 0.72f, alpha * 0.85f));
            target->DrawLine({tip.x + unit * 0.035f, tip.y - unit * 0.16f}, tip, brush, 1.15f);
            brush->SetColor(D2D1::ColorF(0.85f, 0.94f, 1.0f, alpha * 0.65f));
            target->DrawLine({tip.x + unit * 0.02f, tip.y - unit * 0.09f}, tip, brush, 0.55f);
        } else {
            const float t = (elapsed - fallMs) / 420.0f;
            if (t >= 1)
                continue;
            brush->SetColor(D2D1::ColorF(0.37f, 0.62f, 0.77f, alpha * (1 - t) * 0.85f));
            const float radius = unit * (0.025f + t * 0.17f);
            const auto center = D2D1::Point2F(x - drift, viewport.bottom - 2.5f);
            target->DrawEllipse(D2D1::Ellipse(center, radius, radius * 0.24f), brush, 0.85f);
            if (t < 0.35f) {
                target->DrawLine(center, {center.x - unit * 0.05f, center.y - unit * 0.07f},
                                 brush, 0.8f);
                target->DrawLine(center, {center.x + unit * 0.05f, center.y - unit * 0.05f},
                                 brush, 0.8f);
            }
        }
    }
}

void LyricEasterEggs::drawSnow(ID2D1DeviceContext* target, ID2D1SolidColorBrush* brush,
                             D2D1_RECT_F viewport, float age, float opacity, uint32_t seed) {
    D2D1_MATRIX_3X2_F previous;
    target->GetTransform(&previous);
    const float h = viewport.bottom - viewport.top;
    const float w = viewport.right - viewport.left;
    const float unit = std::min(h, 72.0f);
    for (uint32_t i = 0; i < 8; ++i) {
        const float elapsed = age - i * 60.0f;
        if (elapsed < 0)
            continue;
        const float t = elapsed / (1800.0f + variation(seed, i + 10) * 350.0f);
        if (t > 1)
            continue;
        const float x = viewport.left + w * (0.08f + variation(seed, i) * 0.84f) +
                        std::sin(t * kPi * 2 + i) * unit * 0.09f;
        const float y = viewport.top - unit * 0.12f + t * (h + unit * 0.24f);
        const float size = unit * (0.075f + variation(seed, i + 30) * 0.055f);
        const auto transform = D2D1::Matrix3x2F::Scale(size, size) *
            D2D1::Matrix3x2F::Rotation(elapsed * (i % 2 == 0 ? 0.025f : -0.018f) + i * 17) *
            D2D1::Matrix3x2F::Translation(x, y) * previous;
        target->SetTransform(transform);
        for (int layer = 0; layer < 2; ++layer) {
            brush->SetColor(layer == 0
                ? D2D1::ColorF(0.43f, 0.62f, 0.74f, fade(age) * opacity * 0.80f)
                : D2D1::ColorF(0.94f, 0.98f, 1.0f, fade(age) * opacity));
            const float stroke = (layer == 0 ? 1.6f : 0.75f) / size;
            for (int arm = 0; arm < 6; ++arm) {
                const float angle = arm * kPi / 3;
                const D2D1_POINT_2F tip{std::cos(angle), std::sin(angle)};
                target->DrawLine({0, 0}, tip, brush, stroke);
                if (i % 3 != 0) {
                    const float fork = i % 3 == 1 ? 0.55f : 0.72f;
                    const D2D1_POINT_2F root{tip.x * fork, tip.y * fork};
                    for (float side : {-1.0f, 1.0f}) {
                        const float branch = angle + side * kPi / 3;
                        target->DrawLine(root, {root.x - std::cos(branch) * 0.30f,
                                               root.y - std::sin(branch) * 0.30f}, brush, stroke);
                    }
                }
            }
        }
    }
    target->SetTransform(previous);
}

void LyricEasterEggs::drawWind(ID2D1DeviceContext* target, ID2D1SolidColorBrush* brush,
                             D2D1_RECT_F viewport, float age, float opacity, uint32_t seed) {
    D2D1_MATRIX_3X2_F previous;
    target->GetTransform(&previous);
    const float h = viewport.bottom - viewport.top;
    const float w = viewport.right - viewport.left;
    const float unit = std::min(h, 72.0f);
    const float length = std::clamp(w * 0.36f, 38.0f, 100.0f);
    constexpr D2D1_POINT_2F curves[3][4] = {
        {{0, 0}, {0.22f, -0.45f}, {0.42f, 0.45f}, {0.65f, 0}},
        {{0.65f, 0}, {0.82f, -0.42f}, {1.02f, -0.38f}, {0.99f, -0.07f}},
        {{0.99f, -0.07f}, {0.97f, 0.13f}, {0.81f, 0.10f}, {0.85f, -0.04f}},
    };
    for (uint32_t i = 0; i < 3; ++i) {
        const float elapsed = age - i * 70.0f;
        if (elapsed < 0)
            continue;
        const float t = elapsed / static_cast<float>(kDurationMs);
        const float x = viewport.left - length + t * (w + length);
        const float y = viewport.top + h * (0.25f + i * 0.23f);
        const float alpha = fade(elapsed) * opacity;
        target->SetTransform(previous);
        brush->SetColor(D2D1::ColorF(0.40f, 0.61f, 0.59f, alpha * 0.80f));
        // 在窗口 DIP 中采样曲线，保持横向风线和卷曲末端的描边一样粗细。
        for (const auto& curve : curves) {
            D2D1_POINT_2F from{x + curve[0].x * length, y + curve[0].y * unit * 0.20f};
            for (int sample = 1; sample <= 12; ++sample) {
                const auto point = cubic(curve[0], curve[1], curve[2], curve[3], sample / 12.0f);
                const D2D1_POINT_2F to{x + point.x * length, y + point.y * unit * 0.20f};
                target->DrawLine(from, to, brush, 1.15f);
                from = to;
            }
        }
        const float leafX = x + length * (0.72f + variation(seed, i) * 0.20f);
        const float leafY = y + std::sin(t * kPi * 3 + i) * unit * 0.08f;
        const float size = unit * 0.65f;
        target->SetTransform(D2D1::Matrix3x2F::Scale(size, size) *
            D2D1::Matrix3x2F::Rotation(30 + t * 200 + i * 40) *
            D2D1::Matrix3x2F::Translation(leafX, leafY) * previous);
        brush->SetColor(D2D1::ColorF(0.43f, 0.57f, 0.36f, alpha * 0.95f));
        target->FillGeometry(leaf_.Get(), brush);
        brush->SetColor(D2D1::ColorF(0.25f, 0.40f, 0.28f, alpha * 0.80f));
        target->DrawLine({0, 0}, {0.27f, -0.27f}, brush, 0.65f / size);
    }
    target->SetTransform(previous);
}

void LyricEasterEggs::drawStars(ID2D1DeviceContext* target, ID2D1SolidColorBrush* brush,
                              D2D1_RECT_F viewport, float age, float opacity, uint32_t seed) {
    D2D1_MATRIX_3X2_F previous;
    target->GetTransform(&previous);
    const float h = viewport.bottom - viewport.top;
    const float w = viewport.right - viewport.left;
    const float unit = std::min(h, 72.0f);
    for (uint32_t i = 0; i < 10; ++i) {
        const float t = (age - i * 55.0f) / 1600.0f;
        if (t < 0 || t > 1)
            continue;
        const float x = viewport.left + w * (0.06f + variation(seed, i) * 0.88f);
        const float y = viewport.top + h * (i % 2 == 0 ? 0.17f : 0.80f) +
                        (variation(seed, i + 20) - 0.5f) * unit * 0.12f;
        const float twinkle = std::sin(t * kPi);
        const float alpha = twinkle * twinkle * fade(age) * opacity;
        const float size = unit * (0.075f + variation(seed, i + 40) * 0.065f);
        target->SetTransform(D2D1::Matrix3x2F::Scale(size, size) *
            D2D1::Matrix3x2F::Rotation(i % 3 == 0 ? 20.0f : 0.0f) *
            D2D1::Matrix3x2F::Translation(x, y) * previous);
        brush->SetColor(D2D1::ColorF(0.68f, 0.48f, 0.24f, alpha * 0.85f));
        target->DrawGeometry(sparkle_.Get(), brush, 1.3f / size);
        brush->SetColor(D2D1::ColorF(0.99f, 0.87f, 0.56f, alpha));
        target->FillGeometry(sparkle_.Get(), brush);
        brush->SetColor(D2D1::ColorF(1.0f, 0.98f, 0.89f, alpha));
        target->FillEllipse(D2D1::Ellipse({0, 0}, 0.14f, 0.14f), brush);
    }
    target->SetTransform(previous);
}

void LyricEasterEggs::drawSea(ID2D1DeviceContext* target, ID2D1SolidColorBrush* brush,
                            D2D1_RECT_F viewport, float age, float opacity, uint32_t seed) {
    D2D1_MATRIX_3X2_F previous;
    target->GetTransform(&previous);
    const float unit = std::min(viewport.bottom - viewport.top, 72.0f);
    const float w = viewport.right - viewport.left;
    for (int layer = 0; layer < 3; ++layer) {
        const float t = age / static_cast<float>(kDurationMs);
        const float phase = std::fmod(t * 0.22f + layer * 0.11f + variation(seed, 0) * 0.10f, 0.5f);
        const float y = viewport.bottom - unit * (0.14f - layer * 0.035f);
        target->SetTransform(D2D1::Matrix3x2F::Scale(w * 2, unit * 0.12f) *
            D2D1::Matrix3x2F::Translation(viewport.left - w * phase, y) * previous);
        brush->SetColor(D2D1::ColorF(0.24f + layer * 0.08f, 0.53f + layer * 0.07f,
                                    0.67f + layer * 0.06f, fade(age) * opacity * 0.62f));
        target->FillGeometry(wave_.Get(), brush);
    }
    target->SetTransform(previous);
}

void LyricEasterEggs::drawButterflies(ID2D1DeviceContext* target, ID2D1SolidColorBrush* brush,
                                    D2D1_RECT_F viewport, float age, float opacity, uint32_t seed) {
    D2D1_MATRIX_3X2_F previous;
    target->GetTransform(&previous);
    const float h = viewport.bottom - viewport.top;
    const float w = viewport.right - viewport.left;
    const float unit = std::min(h, 72.0f);
    for (uint32_t i = 0; i < 2; ++i) {
        const float elapsed = age - i * 140.0f;
        if (elapsed < 0)
            continue;
        const float t = std::clamp(elapsed / 1500.0f, 0.0f, 1.0f);
        auto position = cubic(
            {viewport.left + unit * 0.20f, viewport.bottom - unit * 0.25f},
            {viewport.left + w * 0.28f, viewport.top - unit * 0.12f},
            {viewport.left + w * 0.64f, viewport.bottom - unit * 0.20f},
            {viewport.right - unit * (0.34f + i * 0.25f), viewport.top + unit * (0.30f + i * 0.20f)}, t);
        if (i == 1)
            position.x = viewport.left + viewport.right - position.x;
        const float exit = std::clamp((elapsed - 2050.0f) / 350.0f, 0.0f, 1.0f);
        position.x += (i == 0 ? 1 : -1) * exit * unit * 0.25f;
        position.y -= exit * unit * 0.18f;
        const float flap = 0.25f + 0.75f * std::abs(std::sin(elapsed / (t < 1 ? 65.0f : 120.0f)));
        const float size = unit * (i == 0 ? 0.145f : 0.12f);
        const float alpha = fade(elapsed) * opacity;
        const auto body = D2D1::Matrix3x2F::Rotation(i == 0 ? 16.0f : -20.0f) *
                          D2D1::Matrix3x2F::Translation(position.x, position.y);
        for (float side : {-1.0f, 1.0f}) {
            target->SetTransform(D2D1::Matrix3x2F::Scale(side * size * flap, size) * body * previous);
            const bool warm = (seed + i) % 2 == 0;
            brush->SetColor(warm ? D2D1::ColorF(0.81f, 0.50f, 0.27f, alpha * 0.96f)
                                 : D2D1::ColorF(0.45f, 0.56f, 0.77f, alpha * 0.96f));
            target->FillGeometry(butterflyWing_.Get(), brush);
            brush->SetColor(D2D1::ColorF(0.25f, 0.29f, 0.36f, alpha * 0.85f));
            target->DrawGeometry(butterflyWing_.Get(), brush, 0.65f / size);
            target->DrawLine({0, 0}, {0.88f, -0.52f}, brush, 0.5f / size);
            target->DrawLine({0, 0}, {0.48f, 0.56f}, brush, 0.5f / size);
        }
        target->SetTransform(body * previous);
        brush->SetColor(D2D1::ColorF(0.25f, 0.29f, 0.34f, alpha));
        target->FillEllipse(D2D1::Ellipse({0, 0}, 0.8f, size * 0.66f), brush);
        target->DrawLine({0, -size * 0.55f}, {-size * 0.24f, -size * 0.94f}, brush, 0.65f);
        target->DrawLine({0, -size * 0.55f}, {size * 0.24f, -size * 0.94f}, brush, 0.65f);
    }
    target->SetTransform(previous);
}
