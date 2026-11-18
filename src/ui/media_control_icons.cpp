#include "media_control_icons.h"

#include <cmath>
#include <dwrite.h>

namespace media_control {
namespace {

bool createRoundedTriangle(ID2D1Factory* factory, int direction,
                           ID2D1PathGeometry** out) {
    if (!factory || !out)
        return false;
    *out = nullptr;
    if (FAILED(factory->CreatePathGeometry(out)) || !*out)
        return false;

    ID2D1GeometrySink* sink = nullptr;
    if (FAILED((*out)->Open(&sink)) || !sink) {
        (*out)->Release();
        *out = nullptr;
        return false;
    }

    const float tipX = direction * 0.55f;
    const float baseX = -direction * 0.35f;
    constexpr float radius = 0.18f;
    const float ux = direction * 0.8739f;
    constexpr float uy = 0.4856f;
    const D2D1_POINT_2F tip = {tipX, 0.0f};
    const D2D1_POINT_2F baseTop = {baseX, -0.5f};
    const D2D1_POINT_2F baseBottom = {baseX, 0.5f};

    sink->BeginFigure(D2D1::Point2F(baseTop.x + radius * ux, baseTop.y + radius * uy),
                      D2D1_FIGURE_BEGIN_FILLED);
    sink->AddQuadraticBezier(
        D2D1::QuadraticBezierSegment(baseTop, {baseTop.x, baseTop.y + radius}));
    sink->AddLine({baseBottom.x, baseBottom.y - radius});
    sink->AddQuadraticBezier(
        D2D1::QuadraticBezierSegment(baseBottom,
                                     {baseBottom.x + radius * ux,
                                      baseBottom.y - radius * uy}));
    sink->AddLine({tip.x - radius * ux, tip.y + radius * uy});
    sink->AddQuadraticBezier(
        D2D1::QuadraticBezierSegment(tip, {tip.x - radius * ux, tip.y - radius * uy}));
    sink->AddLine({baseTop.x + radius * ux, baseTop.y + radius * uy});
    sink->EndFigure(D2D1_FIGURE_END_CLOSED);
    const HRESULT closeHr = sink->Close();
    sink->Release();
    if (FAILED(closeHr)) {
        (*out)->Release();
        *out = nullptr;
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// 播放模式图标：直接取 Windows 自带图标字体里的真实字形，不再手绘路径。
//
// 码位来自微软官方 Segoe MDL2 Assets 图标表（Segoe Fluent Icons 与之兼容）：
//   RepeatAll  U+E8EE  —— 顺序播放 / 列表循环共用这一个字形
//   RepeatOne  U+E8ED  —— 单曲循环
//   Shuffle    U+E8B1  —— 随机播放
// ---------------------------------------------------------------------------
constexpr UINT32 kRepeatCodepoints[3] = {0xE8EE, 0xE8ED, 0xE8B1}; // All / One / Shuffle

// 按优先级在系统字体里找图标字体（Win11 是 Segoe Fluent Icons，Win10 是 MDL2 Assets）。
IDWriteFontFace* findIconFontFace(IDWriteFactory* dwrite) {
    static const wchar_t* kFamilies[] = {L"Segoe Fluent Icons", L"Segoe MDL2 Assets"};
    IDWriteFontCollection* collection = nullptr;
    if (FAILED(dwrite->GetSystemFontCollection(&collection, FALSE)) || !collection)
        return nullptr;

    IDWriteFontFace* face = nullptr;
    for (const wchar_t* name : kFamilies) {
        UINT32 index = 0;
        BOOL exists = FALSE;
        if (FAILED(collection->FindFamilyName(name, &index, &exists)) || !exists)
            continue;
        IDWriteFontFamily* family = nullptr;
        if (FAILED(collection->GetFontFamily(index, &family)) || !family)
            continue;
        IDWriteFont* font = nullptr;
        if (SUCCEEDED(family->GetFirstMatchingFont(DWRITE_FONT_WEIGHT_NORMAL,
                                                   DWRITE_FONT_STRETCH_NORMAL,
                                                   DWRITE_FONT_STYLE_NORMAL, &font)) &&
            font) {
            font->CreateFontFace(&face);
            font->Release();
        }
        family->Release();
        if (face)
            break;
    }
    collection->Release();
    return face;
}

// 把单个字形转成单位 em 坐标下的路径几何（笔形轮廓由 GetGlyphRunOutline 展平）。
bool buildGlyphGeometry(ID2D1Factory* factory, IDWriteFontFace* face, UINT32 codepoint,
                        ID2D1PathGeometry** out, D2D1_RECT_F* bounds) {
    if (!factory || !face || !out || !bounds)
        return false;
    UINT16 glyphIndex = 0;
    if (FAILED(face->GetGlyphIndices(&codepoint, 1, &glyphIndex)) || glyphIndex == 0)
        return false; // 该字体里没有这个码位

    ID2D1PathGeometry* path = nullptr;
    if (FAILED(factory->CreatePathGeometry(&path)) || !path)
        return false;
    ID2D1GeometrySink* sink = nullptr;
    if (FAILED(path->Open(&sink)) || !sink) {
        path->Release();
        return false;
    }
    // 字形轮廓按 nonzero winding 填充，PathGeometry 默认的 alternate 会让
    // 箭头这类多轮廓字形出现空洞。
    sink->SetFillMode(D2D1_FILL_MODE_WINDING);
    const HRESULT outlineHr =
        face->GetGlyphRunOutline(1.0f, &glyphIndex, nullptr, nullptr, 1, FALSE, FALSE, sink);
    const HRESULT closeHr = sink->Close();
    sink->Release();
    if (FAILED(outlineHr) || FAILED(closeHr)) {
        path->Release();
        return false;
    }
    D2D1_RECT_F rect{};
    if (FAILED(path->GetBounds(nullptr, &rect))) {
        path->Release();
        return false;
    }
    *out = path;
    *bounds = rect;
    return true;
}

void createRepeatGlyphs(ID2D1Factory* factory, Geometry& geometry) {
    if (!factory)
        return;
    IDWriteFactory* dwrite = nullptr;
    if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                   reinterpret_cast<IUnknown**>(&dwrite))) ||
        !dwrite) {
        return;
    }
    IDWriteFontFace* face = findIconFontFace(dwrite);
    if (face) {
        for (int i = 0; i < 3; ++i) {
            buildGlyphGeometry(factory, face, kRepeatCodepoints[i], &geometry.repeat[i],
                               &geometry.repeatBounds[i]);
        }
        face->Release();
    }
    dwrite->Release();
}

} // namespace

void release(Geometry& geometry) {
    auto releaseGeometry = [](ID2D1PathGeometry*& value) {
        if (value) {
            value->Release();
            value = nullptr;
        }
    };
    releaseGeometry(geometry.play);
    releaseGeometry(geometry.previous);
    releaseGeometry(geometry.next);
    for (int i = 0; i < 3; ++i) {
        releaseGeometry(geometry.repeat[i]);
        geometry.repeatBounds[i] = {};
    }
}


bool create(ID2D1Factory* factory, Geometry& geometry) {
    release(geometry);
    if (!factory || !createRoundedTriangle(factory, 1, &geometry.play) ||
        !createRoundedTriangle(factory, -1, &geometry.previous) ||
        !createRoundedTriangle(factory, 1, &geometry.next)) {
        release(geometry);
        return false;
    }
    createRepeatGlyphs(factory, geometry);
    return true;
}

void draw(ID2D1RenderTarget* target, const Geometry& geometry, int index, bool playing,
          const D2D1_POINT_2F& center, float radius, ID2D1Brush* brush) {
    if (!target || !brush || radius <= 0.0f || index < 0 || index > 2)
        return;

    if (index == 1) {
        if (playing) {
            const float width = radius * 0.22f;
            const float gap = radius * 0.16f;
            const float height = radius * 0.55f;
            const float barRadius = width * 0.5f;
            target->FillRoundedRectangle(
                D2D1::RoundedRect(D2D1::RectF(center.x - gap - width, center.y - height,
                                               center.x - gap, center.y + height),
                                  barRadius, barRadius),
                brush);
            target->FillRoundedRectangle(
                D2D1::RoundedRect(D2D1::RectF(center.x + gap, center.y - height,
                                               center.x + gap + width, center.y + height),
                                  barRadius, barRadius),
                brush);
        } else if (geometry.play) {
            D2D1_MATRIX_3X2_F previous{};
            target->GetTransform(&previous);
            target->SetTransform(
                D2D1::Matrix3x2F::Scale(radius * 1.3f, radius * 1.3f) *
                D2D1::Matrix3x2F::Translation(center.x + radius * 0.05f, center.y) *
                previous);
            target->FillGeometry(geometry.play, brush);
            target->SetTransform(previous);
        }
        return;
    }

    ID2D1PathGeometry* triangle = index == 0 ? geometry.previous : geometry.next;
    const float scale = radius * 1.16f;
    // 竖条+三角的组合字形边界为 ∓0.72 ~ ±0.55（单位），整体平移 0.085 使视觉中心对齐
    const float centerShift = (index == 0 ? 0.085f : -0.085f) * scale;
    const float barWidth = 0.16f * scale;
    const float barHeight = 1.0f * scale;
    const float barRadius = barWidth * 0.5f;
    const float barX = (index == 0 ? center.x - 0.72f * scale : center.x + 0.56f * scale) +
                       centerShift;
    target->FillRoundedRectangle(
        D2D1::RoundedRect(D2D1::RectF(barX, center.y - barHeight * 0.5f, barX + barWidth,
                                       center.y + barHeight * 0.5f),
                          barRadius, barRadius),
        brush);
    if (triangle) {
        D2D1_MATRIX_3X2_F previous{};
        target->GetTransform(&previous);
        target->SetTransform(D2D1::Matrix3x2F::Scale(scale, scale) *
                             D2D1::Matrix3x2F::Translation(center.x + centerShift, center.y) *
                             previous);
        target->FillGeometry(triangle, brush);
        target->SetTransform(previous);
    }
}

} // namespace media_control

namespace media_control {
namespace {

// 以 center 为原点、scale 缩放的单位坐标系下填充喇叭多边形。
void fillSpeaker(ID2D1RenderTarget* target, const D2D1_POINT_2F& center, float scale,
                 ID2D1Brush* brush) {
    ID2D1Factory* factory = nullptr;
    target->GetFactory(&factory);
    if (!factory)
        return;
    ID2D1PathGeometry* geometry = nullptr;
    if (FAILED(factory->CreatePathGeometry(&geometry)) || !geometry)
        return;
    ID2D1GeometrySink* sink = nullptr;
    if (FAILED(geometry->Open(&sink)) || !sink) {
        geometry->Release();
        return;
    }
    // 箱体、扩口与连接处均使用圆润转角，缩小时也避免尖锐的折线。
    sink->BeginFigure(D2D1::Point2F(-0.54f, -0.22f), D2D1_FIGURE_BEGIN_FILLED);
    sink->AddLine(D2D1::Point2F(-0.32f, -0.22f));
    sink->AddQuadraticBezier(D2D1::QuadraticBezierSegment({-0.28f, -0.22f}, {-0.24f, -0.26f}));
    sink->AddLine(D2D1::Point2F(-0.02f, -0.46f));
    sink->AddQuadraticBezier(D2D1::QuadraticBezierSegment({0.06f, -0.54f}, {0.06f, -0.42f}));
    sink->AddLine(D2D1::Point2F(0.06f, 0.42f));
    sink->AddQuadraticBezier(D2D1::QuadraticBezierSegment({0.06f, 0.54f}, {-0.02f, 0.46f}));
    sink->AddLine(D2D1::Point2F(-0.24f, 0.26f));
    sink->AddQuadraticBezier(D2D1::QuadraticBezierSegment({-0.28f, 0.22f}, {-0.32f, 0.22f}));
    sink->AddLine(D2D1::Point2F(-0.54f, 0.22f));
    sink->AddQuadraticBezier(D2D1::QuadraticBezierSegment({-0.62f, 0.22f}, {-0.62f, 0.14f}));
    sink->AddLine(D2D1::Point2F(-0.62f, -0.14f));
    sink->AddQuadraticBezier(D2D1::QuadraticBezierSegment({-0.62f, -0.22f}, {-0.54f, -0.22f}));
    sink->EndFigure(D2D1_FIGURE_END_CLOSED);
    const HRESULT hr = sink->Close();
    sink->Release();
    if (SUCCEEDED(hr)) {
        D2D1_MATRIX_3X2_F previous{};
        target->GetTransform(&previous);
        target->SetTransform(D2D1::Matrix3x2F::Scale(scale, scale) *
                             D2D1::Matrix3x2F::Translation(center.x, center.y) * previous);
        target->FillGeometry(geometry, brush);
        target->SetTransform(previous);
    }
    geometry->Release();
}

// 喇叭右侧的声波圆弧：radius 为弧半径（单位坐标），stroke 为线宽（DIP）。
void strokeWaveArc(ID2D1RenderTarget* target, const D2D1_POINT_2F& center, float scale,
                   float radius, float stroke, ID2D1Brush* brush) {
    ID2D1Factory* factory = nullptr;
    target->GetFactory(&factory);
    if (!factory)
        return;
    ID2D1PathGeometry* geometry = nullptr;
    if (FAILED(factory->CreatePathGeometry(&geometry)) || !geometry)
        return;
    ID2D1GeometrySink* sink = nullptr;
    if (FAILED(geometry->Open(&sink)) || !sink) {
        geometry->Release();
        return;
    }
    // 以喇叭口 (0.06, 0) 为圆心，向右张开 ±42°（D2D 坐标系 y 向下）
    constexpr float kAngle = 42.0f * 3.14159265f / 180.0f;
    const float cx = 0.06f;
    const float cosA = std::cos(kAngle);
    const float sinA = std::sin(kAngle);
    sink->BeginFigure(D2D1::Point2F(cx + radius * cosA, -radius * sinA),
                      D2D1_FIGURE_BEGIN_HOLLOW);
    D2D1_ARC_SEGMENT arc{};
    arc.point = D2D1::Point2F(cx + radius * cosA, radius * sinA);
    arc.size = D2D1::SizeF(radius, radius);
    arc.rotationAngle = 0.0f;
    arc.sweepDirection = D2D1_SWEEP_DIRECTION_CLOCKWISE;
    arc.arcSize = D2D1_ARC_SIZE_SMALL;
    sink->AddArc(arc);
    sink->EndFigure(D2D1_FIGURE_END_OPEN);
    const HRESULT hr = sink->Close();
    sink->Release();
    if (SUCCEEDED(hr)) {
        D2D1_MATRIX_3X2_F previous{};
        target->GetTransform(&previous);
        target->SetTransform(D2D1::Matrix3x2F::Scale(scale, scale) *
                             D2D1::Matrix3x2F::Translation(center.x, center.y) * previous);
        target->DrawGeometry(geometry, brush, stroke / scale);
        const float capRadius = stroke / (2.0f * scale);
        target->FillEllipse(D2D1::Ellipse(
            D2D1::Point2F(cx + radius * cosA, -radius * sinA), capRadius, capRadius), brush);
        target->FillEllipse(D2D1::Ellipse(
            D2D1::Point2F(cx + radius * cosA, radius * sinA), capRadius, capRadius), brush);
        target->SetTransform(previous);
    }
    geometry->Release();
}

// 图标字体不可用时的兜底画法，保证按钮不会变成一格空白。
void drawRepeatLegacy(ID2D1RenderTarget* target, const D2D1_POINT_2F& center, float radius,
                      ID2D1Brush* brush, int mode) {
    const float ring = radius * 0.76f;
    const float stroke = radius * 0.17f;
    if (mode == 3) {
        const float dx = radius * 0.80f;
        const float dy = radius * 0.52f;
        const D2D1_POINT_2F a0{center.x - dx, center.y + dy};
        const D2D1_POINT_2F a1{center.x + dx, center.y - dy};
        const D2D1_POINT_2F b0{center.x - dx, center.y - dy};
        const D2D1_POINT_2F b1{center.x + dx, center.y + dy};
        target->DrawLine(a0, a1, brush, stroke);
        target->DrawLine(b0, b1, brush, stroke);
        const float head = radius * 0.26f;
        target->DrawLine(a1, D2D1::Point2F(a1.x - head, a1.y - head * 0.55f), brush, stroke);
        target->DrawLine(a1, D2D1::Point2F(a1.x - head * 0.35f, a1.y + head * 0.75f), brush,
                         stroke);
        target->DrawLine(b1, D2D1::Point2F(b1.x - head, b1.y + head * 0.55f), brush, stroke);
        target->DrawLine(b1, D2D1::Point2F(b1.x - head * 0.35f, b1.y - head * 0.75f), brush,
                         stroke);
        return;
    }
    target->DrawEllipse(D2D1::Ellipse(center, ring, ring), brush, stroke);
    const D2D1_POINT_2F tip{center.x + ring * 0.06f, center.y - ring};
    target->DrawLine(tip, D2D1::Point2F(tip.x - ring * 0.34f, tip.y + ring * 0.02f), brush, stroke);
    target->DrawLine(tip, D2D1::Point2F(tip.x + ring * 0.02f, tip.y + ring * 0.34f), brush, stroke);
    if (mode == 1)
        target->FillEllipse(D2D1::Ellipse(center, radius * 0.13f, radius * 0.13f), brush);
}

} // namespace

void drawVolume(ID2D1RenderTarget* target, const D2D1_POINT_2F& center, float radius,
                ID2D1Brush* brush, int level) {
    if (!target || !brush || radius <= 0.0f)
        return;
    const float scale = radius * 1.3f;
    // 各档字形边界不同（喇叭 [-0.62,0.06]，一道波到 0.58，两道波/叉到 0.94），
    // 按档位平移使视觉中心对齐 center
    static constexpr float kCenterShift[4] = {-0.16f, 0.28f, 0.02f, -0.16f};
    const int lvl = level < 0 ? 0 : level > 3 ? 3 : level;
    const D2D1_POINT_2F c{center.x + kCenterShift[lvl] * scale, center.y};
    fillSpeaker(target, c, scale, brush);
    if (lvl <= 0) {
        // 静音：右侧画叉
        const float x0 = c.x + scale * 0.42f;
        const float y0 = c.y - scale * 0.30f;
        const float x1 = c.x + scale * 0.94f;
        const float y1 = c.y + scale * 0.30f;
        const float stroke = radius * 0.18f;
        target->DrawLine(D2D1::Point2F(x0, y0), D2D1::Point2F(x1, y1), brush, stroke);
        target->DrawLine(D2D1::Point2F(x0, y1), D2D1::Point2F(x1, y0), brush, stroke);
        const D2D1_POINT_2F endpoints[] = {
            D2D1::Point2F(x0, y0), D2D1::Point2F(x1, y1),
            D2D1::Point2F(x0, y1), D2D1::Point2F(x1, y0)};
        for (const auto point : endpoints)
            target->FillEllipse(D2D1::Ellipse(point, stroke * 0.5f, stroke * 0.5f), brush);
        return;
    }
    if (lvl >= 2)
        strokeWaveArc(target, c, scale, 0.52f, radius * 0.17f, brush);
    if (lvl >= 3)
        strokeWaveArc(target, c, scale, 0.88f, radius * 0.17f, brush);
}

void drawRepeat(ID2D1RenderTarget* target, const Geometry& geometry,
                const D2D1_POINT_2F& center, float radius, ID2D1Brush* brush, int mode) {
    if (!target || !brush || radius <= 0.0f)
        return;

    const int slot = (mode == 3) ? 2 : (mode == 2 ? 1 : 0); // All / One / Shuffle
    ID2D1PathGeometry* glyph = geometry.repeat[slot];
    const D2D1_RECT_F bounds = geometry.repeatBounds[slot];
    if (!glyph) {
        drawRepeatLegacy(target, center, radius, brush, mode);
        return;
    }

    const float glyphWidth = bounds.right - bounds.left;
    const float glyphHeight = bounds.bottom - bounds.top;
    if (glyphWidth <= 0.0f || glyphHeight <= 0.0f)
        return;

    // 三个图标在字体中原本按相同的 1em 宽度做光学平衡（Repeat 因上下箭头尖使
    // glyphHeight=1.0，而 Shuffle 的 glyphHeight=0.75）。按 glyphWidth 统一归一化，
    // 保证列表循环、单曲循环与随机播放在所有模式下视觉大小一致。
    const float scale = (radius * 1.75f) / glyphWidth;
    const float glyphCenterX = (bounds.left + bounds.right) * 0.5f;
    const float glyphCenterY = (bounds.top + bounds.bottom) * 0.5f;

    D2D1_MATRIX_3X2_F saved{};
    target->GetTransform(&saved);
    // 必须乘上已有变换：卡片/任务栏绘制带 DPI 与转场变换，直接覆盖会让图标逐帧漂移。
    target->SetTransform(D2D1::Matrix3x2F::Scale(scale, scale) *
                         D2D1::Matrix3x2F::Translation(center.x - glyphCenterX * scale,
                                                       center.y - glyphCenterY * scale) *
                         saved);
    target->FillGeometry(glyph, brush);
    // 图标字体是线性笔画，而上一首/播放/下一首是实心图块，等尺寸下会显得"细一档"。
    // 再描一圈同色轮廓把笔画加厚（strokeWidth 是 em 单位，会随 scale 一起放大），
    // 让这组按钮的视觉分量对得上。
    target->DrawGeometry(glyph, brush, 0.045f);
    target->SetTransform(saved);
}

} // namespace media_control
