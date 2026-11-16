#include "ui/taskbar/idle_background_renderer.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace taskbar_detail {

void drawIdleQuoteBackground(const IdleQuoteBackgroundContext& ctx) {
    auto* rt = ctx.rt;
    const float w = ctx.width;
    const float h = ctx.height;
    if (!rt || w <= 0.0f || h <= 0.0f)
        return;

    const float effectW = w;
    const float time = static_cast<float>(ctx.frameNowMs % 600000ULL) / 1000.0f;
    auto* warm = ctx.brushWarm ? ctx.brushWarm : ctx.fallbackTextBrush;
    auto* cool = ctx.brushCool ? ctx.brushCool : ctx.fallbackTextBrush;
    auto* accent = ctx.brushAccent ? ctx.brushAccent : ctx.fallbackTextBrush;
    if (!warm && !cool && !accent)
        return;

    auto drawWithOpacity = [](ID2D1SolidColorBrush* brush, float opacity, const auto& draw) {
        if (!brush)
            return;
        const float previous = brush->GetOpacity();
        brush->SetOpacity(std::clamp(opacity, 0.0f, 1.0f));
        draw();
        brush->SetOpacity(previous);
    };

    rt->PushAxisAlignedClip(ctx.region, D2D1_ANTIALIAS_MODE_ALIASED);
    switch (ctx.style) {
    case IdleQuoteBackground::FallingLeaves: {
        struct Leaf {
            float x;
            float y;
            float size;
            float fallSpeed;
            float drift;
            float phase;
            float angle;
        };
        static constexpr Leaf leaves[] = {
            {0.03f, -0.22f, 3.6f, 9.4f, 0.12f, 0.4f, -24.0f},
            {0.10f, 0.46f, 3.0f, 7.2f, 0.16f, 2.1f, 18.0f},
            {0.17f, 0.06f, 4.3f, 12.0f, 0.10f, 4.0f, 42.0f},
            {0.24f, 0.78f, 2.8f, 8.6f, 0.14f, 1.2f, -12.0f},
            {0.31f, -0.04f, 3.8f, 10.8f, 0.17f, 3.5f, 28.0f},
            {0.38f, 0.30f, 3.1f, 7.8f, 0.11f, 5.1f, -36.0f},
            {0.45f, 0.93f, 4.6f, 11.2f, 0.15f, 0.9f, 12.0f},
            {0.52f, 0.14f, 2.9f, 8.2f, 0.18f, 2.8f, -48.0f},
            {0.59f, 0.61f, 3.7f, 10.0f, 0.12f, 4.6f, 34.0f},
            {0.66f, -0.28f, 3.2f, 7.0f, 0.16f, 1.6f, -8.0f},
            {0.73f, 0.38f, 4.1f, 12.6f, 0.10f, 3.9f, 52.0f},
            {0.80f, 0.84f, 2.7f, 8.8f, 0.14f, 5.4f, -30.0f},
            {0.87f, 0.18f, 3.5f, 9.8f, 0.17f, 2.6f, 20.0f},
            {0.95f, 0.56f, 4.4f, 11.8f, 0.11f, 0.7f, -42.0f},
        };
        const float loopH = h + 16.0f;
        for (size_t i = 0; i < _countof(leaves); ++i) {
            float y = std::fmod(leaves[i].y * h + time * leaves[i].fallSpeed, loopH);
            if (y < 0.0f)
                y += loopH;
            y -= 8.0f;
            float x = leaves[i].x * effectW +
                      std::sin(time * 0.62f + leaves[i].phase) * leaves[i].drift * effectW;
            x = std::fmod(x + effectW, effectW);
            if (x < 0.0f)
                x += effectW;
            const D2D1_POINT_2F center = D2D1::Point2F(x, y);
            const float angle =
                leaves[i].angle + std::sin(time * 0.88f + leaves[i].phase) * 24.0f;
            drawWithOpacity(warm, 0.22f + (i % 4) * 0.045f, [&] {
                D2D1_MATRIX_3X2_F previous{};
                rt->GetTransform(&previous);
                rt->SetTransform(D2D1::Matrix3x2F::Rotation(angle, center));
                rt->FillEllipse(D2D1::Ellipse(center, leaves[i].size, leaves[i].size * 0.46f),
                                warm);
                rt->DrawLine(D2D1::Point2F(x - leaves[i].size * 0.52f, y),
                             D2D1::Point2F(x + leaves[i].size * 0.52f, y), warm, 0.70f);
                rt->SetTransform(previous);
            });
        }
        break;
    }
    case IdleQuoteBackground::TwinklingStars: {
        struct Star {
            float x;
            float y;
            float radius;
            float phase;
            float speed;
        };
        static constexpr Star stars[] = {
            {0.03f, 0.18f, 0.92f, 0.2f, 1.25f}, {0.07f, 0.68f, 0.78f, 2.4f, 0.92f},
            {0.12f, 0.42f, 1.18f, 4.1f, 1.46f}, {0.16f, 0.86f, 0.84f, 1.1f, 0.78f},
            {0.21f, 0.08f, 0.76f, 3.2f, 1.12f}, {0.25f, 0.56f, 1.06f, 5.0f, 1.36f},
            {0.30f, 0.30f, 0.88f, 0.6f, 0.86f}, {0.34f, 0.76f, 0.74f, 2.9f, 1.58f},
            {0.39f, 0.16f, 1.10f, 4.7f, 1.04f}, {0.43f, 0.92f, 0.82f, 1.8f, 1.32f},
            {0.48f, 0.46f, 0.96f, 3.7f, 0.74f}, {0.52f, 0.12f, 0.76f, 5.6f, 1.18f},
            {0.56f, 0.70f, 1.24f, 1.5f, 1.42f}, {0.60f, 0.34f, 0.80f, 3.0f, 0.88f},
            {0.64f, 0.84f, 1.02f, 4.4f, 1.24f}, {0.68f, 0.22f, 0.72f, 0.9f, 1.60f},
            {0.72f, 0.52f, 1.14f, 2.1f, 1.02f}, {0.76f, 0.06f, 0.86f, 5.2f, 1.38f},
            {0.80f, 0.76f, 1.00f, 3.4f, 0.80f}, {0.84f, 0.38f, 0.78f, 1.7f, 1.50f},
            {0.88f, 0.16f, 1.20f, 4.8f, 1.14f}, {0.92f, 0.62f, 0.82f, 2.6f, 0.96f},
            {0.96f, 0.30f, 1.08f, 0.7f, 1.28f}, {0.985f, 0.90f, 0.74f, 5.8f, 0.72f},
        };
        for (size_t i = 0; i < _countof(stars); ++i) {
            const float twinkle =
                0.5f + 0.5f * std::sin(time * stars[i].speed + stars[i].phase);
            const D2D1_POINT_2F center = D2D1::Point2F(stars[i].x * effectW, stars[i].y * h);
            const float radius = stars[i].radius * (0.72f + twinkle * 0.52f);
            drawWithOpacity(cool, 0.14f + twinkle * 0.30f, [&] {
                rt->FillEllipse(D2D1::Ellipse(center, radius, radius), cool);
                if (i % 4 == 0 && twinkle > 0.52f) {
                    const float ray = radius * (2.4f + twinkle * 1.5f);
                    rt->DrawLine(D2D1::Point2F(center.x - ray, center.y),
                                 D2D1::Point2F(center.x + ray, center.y), cool, 0.70f);
                    rt->DrawLine(D2D1::Point2F(center.x, center.y - ray),
                                 D2D1::Point2F(center.x, center.y + ray), cool, 0.70f);
                }
            });
        }
        break;
    }
    case IdleQuoteBackground::BinaryRain: {
        if (ctx.binaryFontFormat) {
            const int columns = std::clamp(static_cast<int>(effectW / 23.0f), 5, 10);
            const float columnW = effectW / static_cast<float>(columns);
            const float rowGap = std::max(9.0f, h / 3.6f);
            const ULONGLONG bitTick = ctx.frameNowMs / 190ULL;
            for (int col = 0; col < columns; ++col) {
                const float head =
                    std::fmod(time * (7.0f + static_cast<float>(col % 3) * 2.0f) +
                                  static_cast<float>(col) * 8.0f,
                              h + rowGap * 4.0f) -
                    rowGap * 2.0f;
                for (int row = -1; row < 5; ++row) {
                    const float y = head + static_cast<float>(row) * rowGap;
                    if (y < -rowGap || y > h)
                        continue;
                    const bool one = ((bitTick + static_cast<ULONGLONG>(col * 13 + row * 7 +
                                                                        col * row * 3)) &
                                      1ULL) != 0;
                    wchar_t digit[2] = {one ? L'1' : L'0', L'\0'};
                    const float trail =
                        std::clamp(1.0f - std::fabs(y - head) / (rowGap * 3.5f), 0.0f, 1.0f);
                    const float opacity = 0.045f + trail * 0.16f;
                    const D2D1_RECT_F rect = D2D1::RectF(
                        columnW * (static_cast<float>(col) + 0.5f) - 4.0f, y,
                        columnW * (static_cast<float>(col) + 0.5f) + 4.0f, y + rowGap + 2.0f);
                    drawWithOpacity(accent, opacity, [&] {
                        rt->DrawTextW(digit, 1, ctx.binaryFontFormat, rect, accent,
                                      D2D1_DRAW_TEXT_OPTIONS_CLIP, DWRITE_MEASURING_MODE_NATURAL);
                    });
                }
            }
        }
        break;
    }
    case IdleQuoteBackground::FloatingParticles: {
        struct Particle {
            float x;
            float y;
            float radius;
            float speed;
            float amplitude;
            float phase;
        };
        static constexpr Particle particles[] = {
            {0.02f, 0.28f, 1.18f, 4.0f, 0.18f, 0.4f}, {0.07f, 0.74f, 0.92f, 5.5f, 0.14f, 2.3f},
            {0.12f, 0.48f, 1.48f, 2.8f, 0.20f, 4.2f}, {0.17f, 0.12f, 1.00f, 4.8f, 0.13f, 1.2f},
            {0.22f, 0.86f, 1.26f, 3.3f, 0.17f, 3.5f}, {0.27f, 0.38f, 0.86f, 6.2f, 0.22f, 5.0f},
            {0.32f, 0.66f, 1.38f, 3.7f, 0.16f, 0.9f}, {0.37f, 0.22f, 0.96f, 5.2f, 0.19f, 2.8f},
            {0.42f, 0.80f, 1.10f, 3.1f, 0.14f, 4.8f}, {0.47f, 0.52f, 1.56f, 4.6f, 0.18f, 1.7f},
            {0.52f, 0.08f, 0.90f, 5.8f, 0.21f, 3.9f}, {0.57f, 0.92f, 1.32f, 2.6f, 0.15f, 5.4f},
            {0.62f, 0.34f, 1.16f, 4.3f, 0.20f, 1.5f}, {0.67f, 0.70f, 0.88f, 6.0f, 0.13f, 3.1f},
            {0.72f, 0.18f, 1.44f, 3.4f, 0.17f, 4.5f}, {0.77f, 0.84f, 1.02f, 4.9f, 0.22f, 0.7f},
            {0.82f, 0.44f, 1.30f, 2.9f, 0.16f, 2.0f}, {0.87f, 0.06f, 0.94f, 5.6f, 0.19f, 3.6f},
            {0.92f, 0.62f, 1.50f, 3.9f, 0.14f, 5.1f}, {0.97f, 0.30f, 1.08f, 4.5f, 0.20f, 1.0f},
        };
        for (size_t i = 0; i < _countof(particles); ++i) {
            float x = std::fmod(particles[i].x * effectW + time * particles[i].speed, effectW);
            if (x < 0.0f)
                x += effectW;
            const float y = particles[i].y * h +
                            std::sin(time * 0.72f + particles[i].phase) * particles[i].amplitude * h;
            const float pulse =
                0.5f + 0.5f * std::sin(time * (1.1f + i * 0.04f) + particles[i].phase);
            auto* brush = i % 3 == 0 ? accent : cool;
            drawWithOpacity(brush, 0.15f + pulse * 0.26f, [&] {
                const float radius = particles[i].radius * (0.72f + pulse * 0.48f);
                rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(x, y), radius, radius), brush);
                if (i % 2 == 0) {
                    rt->DrawLine(D2D1::Point2F(x - particles[i].speed * 0.7f, y),
                                 D2D1::Point2F(x - radius, y), brush, 0.65f);
                }
            });
        }
        break;
    }
    case IdleQuoteBackground::None:
    default:
        break;
    }
    rt->PopAxisAlignedClip();
}

} // namespace taskbar_detail
