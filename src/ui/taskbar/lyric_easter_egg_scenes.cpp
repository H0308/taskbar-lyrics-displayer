#include "ui/taskbar/lyric_easter_eggs.h"

#include <algorithm>
#include <cmath>
#include <initializer_list>

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
