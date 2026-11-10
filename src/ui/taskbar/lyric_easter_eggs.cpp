#include "ui/taskbar/lyric_easter_eggs.h"
#include "ui/taskbar/animation_clock.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <iterator>
#include <initializer_list>
#include <string_view>
#include <utility>

namespace {
constexpr float kPi = 3.14159265358979323846f;

wchar_t fold(wchar_t c) {
    if ((c >= L'A' && c <= L'Z') || (c >= L'А' && c <= L'Я'))
        return c + 32;
    return c == L'Ё' ? L'ё' : c;
}

bool wordCharacter(wchar_t c) {
    c = fold(c);
    return (c >= L'a' && c <= L'z') || (c >= L'а' && c <= L'я') || c == L'ё' ||
           (c >= L'0' && c <= L'9') || c == L'_' || (c >= 0x00c0 && c <= 0x02af) ||
           (c >= 0x0300 && c <= 0x036f) || (c >= 0x0400 && c <= 0x052f);
}

bool hangul(wchar_t c) { return c >= 0xac00 && c <= 0xd7a3; }

enum class Boundary { None, Word, Korean };
bool matches(std::wstring_view text, size_t offset, std::wstring_view keyword, Boundary boundary) {
    if (text.substr(offset, keyword.size()) != keyword)
        return false;
    const size_t end = offset + keyword.size();
    if (boundary == Boundary::Word)
        return (offset == 0 || !wordCharacter(text[offset - 1])) &&
               (end == text.size() || !wordCharacter(text[end]));
    if (boundary != Boundary::Korean)
        return true;
    if (offset != 0 && hangul(text[offset - 1]))
        return false;
    size_t suffixEnd = end;
    while (suffixEnd < text.size() && hangul(text[suffixEnd]))
        ++suffixEnd;
    const auto suffix = text.substr(end, suffixEnd - end);
    constexpr std::wstring_view suffixes[] = {
        L"", L"이", L"가", L"은", L"는", L"을", L"를", L"의", L"에", L"에서",
        L"에게", L"으로", L"로", L"과", L"와", L"도", L"만", L"처럼", L"같이",
        L"보다", L"까지", L"부터", L"들", L"들이", L"들은", L"들을", L"들의",
    };
    return std::find(std::begin(suffixes), std::end(suffixes), suffix) != std::end(suffixes);
}

Microsoft::WRL::ComPtr<ID2D1PathGeometry>
makeShape(ID2D1Factory* factory, D2D1_POINT_2F start,
          std::initializer_list<D2D1_BEZIER_SEGMENT> curves, bool closed = true) {
    Microsoft::WRL::ComPtr<ID2D1PathGeometry> shape;
    Microsoft::WRL::ComPtr<ID2D1GeometrySink> sink;
    if (FAILED(factory->CreatePathGeometry(shape.GetAddressOf())) ||
        FAILED(shape->Open(sink.GetAddressOf())))
        return {};
    sink->BeginFigure(start, closed ? D2D1_FIGURE_BEGIN_FILLED : D2D1_FIGURE_BEGIN_HOLLOW);
    for (const auto& curve : curves)
        sink->AddBezier(curve);
    sink->EndFigure(closed ? D2D1_FIGURE_END_CLOSED : D2D1_FIGURE_END_OPEN);
    if (FAILED(sink->Close()))
        return {};
    return shape;
}
} // namespace

float LyricEasterEggs::fade(float age) {
    return std::clamp(age / 180.0f, 0.0f, 1.0f) *
           std::clamp((static_cast<float>(kDurationMs) - age) / 250.0f, 0.0f, 1.0f);
}

void LyricEasterEggs::clear() {
    cues_.clear();
    cloud_.Reset();
    stem_.Reset();
    leaf_.Reset();
    petal_.Reset();
    tulip_.Reset();
    sparkle_.Reset();
    wave_.Reset();
    butterflyWing_.Reset();
}

bool LyricEasterEggs::ensureShapes(ID2D1DeviceContext* target) {
    if (cloud_ && stem_ && leaf_ && petal_ && tulip_ && sparkle_ && wave_ && butterflyWing_)
        return true;
    Microsoft::WRL::ComPtr<ID2D1Factory> factory;
    target->GetFactory(factory.GetAddressOf());
    if (!factory)
        return false;
    auto cloud = makeShape(factory.Get(), {-1.8f, 0.55f}, {
        {{-2.55f, 0.55f}, {-2.65f, -0.40f}, {-1.85f, -0.48f}},
        {{-1.93f, -1.16f}, {-1.00f, -1.40f}, {-0.63f, -0.88f}},
        {{-0.18f, -1.90f}, {1.10f, -1.65f}, {1.27f, -0.73f}},
        {{1.95f, -0.95f}, {2.55f, -0.32f}, {2.20f, 0.22f}},
        {{2.03f, 0.62f}, {1.30f, 0.62f}, {0.74f, 0.62f}},
        {{-0.15f, 0.62f}, {-1.10f, 0.62f}, {-1.8f, 0.55f}},
    });
    auto stem = makeShape(factory.Get(), {0, 0}, {
        {{0.10f, -0.30f}, {-0.10f, -0.72f}, {0, -1}},
    }, false);
    auto leaf = makeShape(factory.Get(), {0, 0}, {
        {{0.02f, -0.17f}, {0.17f, -0.31f}, {0.31f, -0.32f}},
        {{0.33f, -0.13f}, {0.17f, -0.01f}, {0, 0}},
    });
    auto petal = makeShape(factory.Get(), {0, 0.10f}, {
        {{-0.48f, -0.05f}, {-0.66f, -0.88f}, {-0.12f, -1.22f}},
        {{0.26f, -1.48f}, {0.68f, -0.86f}, {0.44f, -0.42f}},
        {{0.33f, -0.10f}, {0.12f, 0.04f}, {0, 0.10f}},
    });
    auto tulip = makeShape(factory.Get(), {0, 0.72f}, {
        {{-0.85f, 0.50f}, {-1.02f, -0.46f}, {-0.85f, -1.08f}},
        {{-0.57f, -0.91f}, {-0.37f, -0.63f}, {-0.28f, -0.40f}},
        {{-0.15f, -0.74f}, {0.03f, -1.08f}, {0.13f, -1.24f}},
        {{0.28f, -0.98f}, {0.43f, -0.68f}, {0.43f, -0.40f}},
        {{0.56f, -0.66f}, {0.77f, -0.89f}, {0.94f, -1.00f}},
        {{1.02f, -0.10f}, {0.81f, 0.60f}, {0, 0.72f}},
    });
    auto sparkle = makeShape(factory.Get(), {0, -1}, {
        {{0.12f, -0.18f}, {0.18f, -0.12f}, {0.68f, 0}},
        {{0.18f, 0.12f}, {0.12f, 0.18f}, {0, 1}},
        {{-0.12f, 0.18f}, {-0.18f, 0.12f}, {-0.68f, 0}},
        {{-0.18f, -0.12f}, {-0.12f, -0.18f}, {0, -1}},
    });
    auto wave = makeShape(factory.Get(), {0, 0}, {
        {{0.04f, -0.70f}, {0.08f, -0.70f}, {0.125f, 0}},
        {{0.17f, 0.70f}, {0.21f, 0.70f}, {0.25f, 0}},
        {{0.29f, -0.70f}, {0.33f, -0.70f}, {0.375f, 0}},
        {{0.42f, 0.70f}, {0.46f, 0.70f}, {0.50f, 0}},
        {{0.54f, -0.70f}, {0.58f, -0.70f}, {0.625f, 0}},
        {{0.67f, 0.70f}, {0.71f, 0.70f}, {0.75f, 0}},
        {{0.79f, -0.70f}, {0.83f, -0.70f}, {0.875f, 0}},
        {{0.92f, 0.70f}, {0.96f, 0.70f}, {1, 0}},
        {{1, 0.33f}, {1, 0.66f}, {1, 1}},
        {{0.66f, 1}, {0.33f, 1}, {0, 1}},
        {{0, 0.66f}, {0, 0.33f}, {0, 0}},
    });
    auto wing = makeShape(factory.Get(), {0, 0}, {
        {{0.18f, -0.72f}, {0.94f, -1.00f}, {1.13f, -0.58f}},
        {{1.32f, -0.18f}, {0.67f, 0.04f}, {0.37f, 0.08f}},
        {{0.95f, 0.13f}, {1.00f, 0.77f}, {0.49f, 0.75f}},
        {{0.13f, 0.76f}, {0.07f, 0.33f}, {0, 0}},
    });
    if (!cloud || !stem || !leaf || !petal || !tulip || !sparkle || !wave || !wing)
        return false;
    cloud_ = std::move(cloud);
    stem_ = std::move(stem);
    leaf_ = std::move(leaf);
    petal_ = std::move(petal);
    tulip_ = std::move(tulip);
    sparkle_ = std::move(sparkle);
    wave_ = std::move(wave);
    butterflyWing_ = std::move(wing);
    return true;
}

void LyricEasterEggs::resetPlayback(int64_t positionMs) {
    animations_ = {};
    cues_.clear();
    resetTriggerPosition(positionMs);
}

void LyricEasterEggs::resetTriggerPosition(int64_t positionMs) {
    emittedThroughMs_.fill(positionMs - 1);
}

bool LyricEasterEggs::active() const {
    const auto now = taskbar_detail::monotonicNowMs();
    return std::any_of(animations_.begin(), animations_.end(), [&](const auto& animation) {
        return animation && now - animation->startedMs < static_cast<uint64_t>(kDurationMs);
    });
}

const std::vector<LyricEasterEggs::Cue>& LyricEasterEggs::cues(const LyricLine& line,
                                                           int lineIndex) {
    struct Keyword {
        std::wstring_view text;
        Kind kind;
        Boundary boundary = Boundary::None;
    };
    // 跨逐字单元匹配，最长词只生成一次 cue；各种语言共用同一种视觉效果。
    static constexpr Keyword kKeywords[] = {
        {L"云朵", Kind::Cloud}, {L"雲朵", Kind::Cloud}, {L"白云", Kind::Cloud},
        {L"白雲", Kind::Cloud}, {L"云", Kind::Cloud}, {L"雲", Kind::Cloud},
        {L"花朵", Kind::Flowers}, {L"鲜花", Kind::Flowers}, {L"鮮花", Kind::Flowers},
        {L"花束", Kind::Flowers}, {L"花", Kind::Flowers},
        {L"雨滴", Kind::Rain}, {L"雨", Kind::Rain},
        {L"雪花", Kind::Snow}, {L"雪", Kind::Snow},
        {L"微风", Kind::Wind}, {L"微風", Kind::Wind}, {L"风", Kind::Wind}, {L"風", Kind::Wind},
        {L"星星", Kind::Stars}, {L"星", Kind::Stars},
        {L"海浪", Kind::Sea}, {L"海", Kind::Sea},
        {L"蝴蝶", Kind::Butterflies}, {L"蝶", Kind::Butterflies},
        {L"clouds", Kind::Cloud, Boundary::Word}, {L"cloud", Kind::Cloud, Boundary::Word},
        {L"flowers", Kind::Flowers, Boundary::Word}, {L"flower", Kind::Flowers, Boundary::Word},
        {L"rain", Kind::Rain, Boundary::Word}, {L"raindrop", Kind::Rain, Boundary::Word},
        {L"raindrops", Kind::Rain, Boundary::Word},
        {L"snow", Kind::Snow, Boundary::Word}, {L"snowflake", Kind::Snow, Boundary::Word},
        {L"snowflakes", Kind::Snow, Boundary::Word},
        {L"wind", Kind::Wind, Boundary::Word}, {L"breeze", Kind::Wind, Boundary::Word},
        {L"star", Kind::Stars, Boundary::Word}, {L"stars", Kind::Stars, Boundary::Word},
        {L"sea", Kind::Sea, Boundary::Word}, {L"ocean", Kind::Sea, Boundary::Word},
        {L"wave", Kind::Sea, Boundary::Word}, {L"waves", Kind::Sea, Boundary::Word},
        {L"butterfly", Kind::Butterflies, Boundary::Word},
        {L"butterflies", Kind::Butterflies, Boundary::Word},
        {L"구름", Kind::Cloud, Boundary::Korean}, {L"꽃다발", Kind::Flowers, Boundary::Korean},
        {L"꽃", Kind::Flowers, Boundary::Korean}, {L"빗방울", Kind::Rain, Boundary::Korean},
        {L"비", Kind::Rain, Boundary::Korean},
        {L"눈송이", Kind::Snow, Boundary::Korean}, {L"눈꽃", Kind::Snow, Boundary::Korean},
        {L"바람", Kind::Wind, Boundary::Korean}, {L"별", Kind::Stars, Boundary::Korean},
        {L"바다", Kind::Sea, Boundary::Korean}, {L"파도", Kind::Sea, Boundary::Korean},
        {L"나비", Kind::Butterflies, Boundary::Korean},
        {L"облако", Kind::Cloud, Boundary::Word}, {L"облака", Kind::Cloud, Boundary::Word},
        {L"облаку", Kind::Cloud, Boundary::Word}, {L"облаком", Kind::Cloud, Boundary::Word},
        {L"облаке", Kind::Cloud, Boundary::Word}, {L"облаков", Kind::Cloud, Boundary::Word},
        {L"облакам", Kind::Cloud, Boundary::Word}, {L"облаками", Kind::Cloud, Boundary::Word},
        {L"облаках", Kind::Cloud, Boundary::Word},
        {L"цветок", Kind::Flowers, Boundary::Word}, {L"цветка", Kind::Flowers, Boundary::Word},
        {L"цветку", Kind::Flowers, Boundary::Word}, {L"цветком", Kind::Flowers, Boundary::Word},
        {L"цветке", Kind::Flowers, Boundary::Word}, {L"цветы", Kind::Flowers, Boundary::Word},
        {L"цветов", Kind::Flowers, Boundary::Word}, {L"цветам", Kind::Flowers, Boundary::Word},
        {L"цветами", Kind::Flowers, Boundary::Word}, {L"цветах", Kind::Flowers, Boundary::Word},
        {L"дождь", Kind::Rain, Boundary::Word}, {L"дождя", Kind::Rain, Boundary::Word},
        {L"снег", Kind::Snow, Boundary::Word}, {L"снега", Kind::Snow, Boundary::Word},
        {L"снежинка", Kind::Snow, Boundary::Word}, {L"снежинки", Kind::Snow, Boundary::Word},
        {L"ветер", Kind::Wind, Boundary::Word}, {L"ветра", Kind::Wind, Boundary::Word},
        {L"звезда", Kind::Stars, Boundary::Word}, {L"звёзды", Kind::Stars, Boundary::Word},
        {L"звезды", Kind::Stars, Boundary::Word},
        {L"море", Kind::Sea, Boundary::Word}, {L"моря", Kind::Sea, Boundary::Word},
        {L"волна", Kind::Sea, Boundary::Word}, {L"волны", Kind::Sea, Boundary::Word},
        {L"бабочка", Kind::Butterflies, Boundary::Word}, {L"бабочки", Kind::Butterflies, Boundary::Word},
    };
    const auto found = cues_.find(lineIndex);
    if (found != cues_.end())
        return found->second;
    std::vector<Cue> result;
    std::wstring text;
    for (const auto& token : line.chars)
        text += token.text;
    std::transform(text.begin(), text.end(), text.begin(), fold);
    constexpr std::wstring_view exclusions[] = {
        L"花费", L"花費", L"花钱", L"花錢", L"花样", L"花樣", L"花招", L"花心",
        L"花哨", L"花火", L"花粉", L"花白", L"花纹", L"花紋", L"云计算", L"雲計算",
        L"风险", L"風險", L"风格", L"風格", L"风俗", L"風俗", L"星期", L"明星",
        L"海关", L"海關", L"海报", L"海報", L"蝶泳",
    };
    size_t tokenIndex = 0;
    size_t tokenEnd = line.chars.empty() ? 0 : line.chars.front().text.size();
    for (size_t offset = 0; offset < text.size();) {
        size_t excluded = 0;
        for (const auto word : exclusions)
            if (std::wstring_view(text).substr(offset, word.size()) == word)
                excluded = std::max(excluded, word.size());
        if (excluded != 0) {
            offset += excluded;
            continue;
        }
        const Keyword* best = nullptr;
        for (const auto& keyword : kKeywords)
            if ((!best || keyword.text.size() > best->text.size()) &&
                matches(text, offset, keyword.text, keyword.boundary))
                best = &keyword;
        if (!best) {
            ++offset;
            continue;
        }
        while (tokenIndex < line.chars.size() && offset >= tokenEnd) {
            ++tokenIndex;
            if (tokenIndex < line.chars.size())
                tokenEnd += line.chars[tokenIndex].text.size();
        }
        if (tokenIndex < line.chars.size()) {
            const auto& token = line.chars[tokenIndex];
            if (token.endMs > token.startMs) {
                result.push_back({best->kind, token.startMs, offset});
            }
        }
        offset += best->text.size();
    }
    return cues_.emplace(lineIndex, std::move(result)).first->second;
}

void LyricEasterEggs::draw(ID2D1DeviceContext* target, ID2D1SolidColorBrush* brush,
                         const LyricLine& line, int lineIndex, D2D1_RECT_F viewport,
                         int64_t positionMs, int64_t emitFromMs, bool playing) {
    if (!target || !brush || viewport.bottom <= viewport.top)
        return;
    const auto oldColor = brush->GetColor();
    const float unit = std::min(viewport.bottom - viewport.top, 72.0f);
    if (viewport.right - viewport.left < unit * 2.0f)
        return;
    const auto now = taskbar_detail::monotonicNowMs();
    for (auto& animation : animations_)
        if (animation && now - animation->startedMs >= static_cast<uint64_t>(kDurationMs))
            animation.reset();
    if (playing) {
        for (const auto& cue : cues(line, lineIndex)) {
            const int64_t elapsed = positionMs - cue.startMs;
            const size_t kind = static_cast<size_t>(cue.kind);
            if (cue.startMs < emitFromMs || elapsed < 0 || elapsed >= kDurationMs ||
                cue.startMs <= emittedThroughMs_[kind])
                continue;
            emittedThroughMs_[kind] = cue.startMs;
            if (!animations_[kind])
                animations_[kind] = Animation{
                    now, static_cast<uint32_t>(cue.textOffset * 31 + lineIndex * 17)};
        }
    }
    for (size_t kind = 0; kind < animations_.size(); ++kind) {
        const auto& animation = animations_[kind];
        if (!animation)
            continue;
        const float age = static_cast<float>(now - animation->startedMs);
        if (!ensureShapes(target))
            break;
        switch (static_cast<Kind>(kind)) {
        case Kind::Cloud:
            drawClouds(target, brush, viewport, age, 1.0f, animation->seed);
            break;
        case Kind::Flowers:
            drawFlowers(target, brush, viewport, age, 1.0f, animation->seed);
            break;
        case Kind::Rain:
            drawRain(target, brush, viewport, age, 1.0f, animation->seed);
            break;
        case Kind::Snow:
            drawSnow(target, brush, viewport, age, 1.0f, animation->seed);
            break;
        case Kind::Wind:
            drawWind(target, brush, viewport, age, 1.0f, animation->seed);
            break;
        case Kind::Stars:
            drawStars(target, brush, viewport, age, 1.0f, animation->seed);
            break;
        case Kind::Sea:
            drawSea(target, brush, viewport, age, 1.0f, animation->seed);
            break;
        case Kind::Butterflies:
            drawButterflies(target, brush, viewport, age, 1.0f, animation->seed);
            break;
        case Kind::Count:
            break;
        }
    }
    brush->SetColor(oldColor);
}

void LyricEasterEggs::drawClouds(ID2D1DeviceContext* target, ID2D1SolidColorBrush* brush,
                               D2D1_RECT_F viewport, float age, float opacity, uint32_t seed) {
    D2D1_MATRIX_3X2_F previous;
    target->GetTransform(&previous);
    const float unit = std::min(viewport.bottom - viewport.top, 72.0f);
    const float width = viewport.right - viewport.left;
    for (int i = 0; i < 3; ++i) {
        const float elapsed = age - i * 70.0f;
        if (elapsed < 0)
            continue;
        const float t = elapsed / static_cast<float>(kDurationMs);
        const float size = unit * (0.19f + i * 0.025f);
        const float cx = viewport.left + width * (0.16f + i * 0.31f) +
                         (t - 0.5f) * std::min(width * 0.25f, unit * 1.6f);
        const float cy = viewport.top + unit * (0.35f + (i % 2) * 0.23f) +
                         std::sin(t * kPi + i) * unit * 0.04f;
        const float alpha = fade(elapsed) * opacity;
        const auto transform = D2D1::Matrix3x2F::Scale(size, size * 0.76f) *
                               D2D1::Matrix3x2F::Translation(cx, cy);
        target->SetTransform(transform * previous);
        brush->SetColor(D2D1::ColorF(0.40f, 0.55f, 0.66f, alpha * 0.70f));
        target->FillGeometry(cloud_.Get(), brush);
        target->SetTransform(transform * D2D1::Matrix3x2F::Translation(0, -1.4f) * previous);
        const float tint = static_cast<float>((seed + i) % 3) * 0.02f;
        brush->SetColor(D2D1::ColorF(0.89f + tint, 0.94f, 0.98f, alpha * 0.92f));
        target->FillGeometry(cloud_.Get(), brush);
        brush->SetColor(D2D1::ColorF(0.55f, 0.67f, 0.77f, alpha * 0.65f));
        target->DrawGeometry(cloud_.Get(), brush, 0.65f / size);
    }
    target->SetTransform(previous);
}

void LyricEasterEggs::drawFlowers(ID2D1DeviceContext* target, ID2D1SolidColorBrush* brush,
                                D2D1_RECT_F viewport, float age, float opacity, uint32_t seed) {
    D2D1_MATRIX_3X2_F previous;
    target->GetTransform(&previous);
    const float unit = std::min(viewport.bottom - viewport.top, 72.0f);
    constexpr D2D1_COLOR_F palette[] = {
        {0.72f, 0.35f, 0.43f, 1}, {0.91f, 0.84f, 0.67f, 1},
        {0.61f, 0.48f, 0.64f, 1}, {0.85f, 0.60f, 0.51f, 1},
    };
    constexpr float angles[] = {24.0f, 47.0f, 67.0f};
    constexpr float lengths[] = {0.82f, 0.98f, 0.76f};
    for (int side = 0; side < 2; ++side) {
        const float direction = side == 0 ? 1.0f : -1.0f;
        const float baseX = side == 0 ? viewport.left + 2.0f : viewport.right - 2.0f;
        const float baseY = viewport.bottom + unit * 0.12f;
        for (int i = 0; i < 3; ++i) {
            const float elapsed = age - i * 60.0f - side * 35.0f;
            if (elapsed <= 0)
                continue;
            // mass=1、stiffness=100、damping=10；角度展开，花头保持完整尺寸。
            constexpr float frequency = 8.660254f;
            const float seconds = elapsed / 1000.0f;
            const float opening = 1.0f - std::exp(-5.0f * seconds) *
                (std::cos(frequency * seconds) + 5.0f / frequency * std::sin(frequency * seconds));
            const float sway = std::sin(age / 900.0f + i + side) * 1.5f;
            const float angle = direction * (88.0f + (angles[i] - 88.0f) * opening + sway);
            const float length = unit * lengths[i] * (side == 0 ? 1.0f : 0.92f);
            const float alpha = fade(elapsed) * opacity;
            const auto branch = D2D1::Matrix3x2F::Rotation(angle) *
                                D2D1::Matrix3x2F::Translation(baseX, baseY);
            target->SetTransform(D2D1::Matrix3x2F::Scale(length, length) * branch * previous);
            brush->SetColor(D2D1::ColorF(0.27f, 0.40f, 0.29f, alpha * 0.95f));
            target->DrawGeometry(stem_.Get(), brush, 1.25f / length);
            for (int j = 0; j < 2; ++j) {
                const auto leafTransform = D2D1::Matrix3x2F::Scale(
                    length * (j == 0 ? 0.65f : -0.55f), length * 0.65f) *
                    D2D1::Matrix3x2F::Translation(0, -length * (0.32f + j * 0.22f));
                target->SetTransform(leafTransform * branch * previous);
                brush->SetColor(D2D1::ColorF(0.39f, 0.51f, 0.37f, alpha * 0.92f));
                target->FillGeometry(leaf_.Get(), brush);
                brush->SetColor(D2D1::ColorF(0.24f, 0.36f, 0.26f, alpha * 0.65f));
                target->DrawGeometry(leaf_.Get(), brush, 0.7f / length);
            }
            const auto head = D2D1::Matrix3x2F::Translation(0, -length) * branch;
            const float radius = unit * (i == 1 ? 0.17f : 0.145f);
            const int style = static_cast<int>((seed + i + side) % 3);
            auto color = palette[(seed + i + side * 2) % 4];
            color.a = alpha * 0.97f;
            if (style == 1) {
                target->SetTransform(D2D1::Matrix3x2F::Scale(radius, radius) * head * previous);
                brush->SetColor(color);
                target->FillGeometry(tulip_.Get(), brush);
                brush->SetColor(D2D1::ColorF(color.r * 0.67f, color.g * 0.67f,
                                            color.b * 0.67f, alpha * 0.85f));
                target->DrawGeometry(tulip_.Get(), brush, 0.7f / radius);
                target->DrawLine({-0.28f, -0.40f}, {0, 0.55f}, brush, 0.65f / radius);
                target->DrawLine({0.43f, -0.40f}, {0, 0.55f}, brush, 0.65f / radius);
            } else {
                const int petals = style == 0 ? 7 : 9;
                for (int layer = 0; layer < 2; ++layer) {
                    const float scale = radius * (layer == 0 ? 1.0f : 0.64f);
                    for (int p = 0; p < petals; ++p) {
                        target->SetTransform(D2D1::Matrix3x2F::Scale(scale, scale) *
                            D2D1::Matrix3x2F::Rotation(p * 360.0f / petals + layer * 23.0f + i * 11) *
                            head * previous);
                        auto petalColor = color;
                        const float light = layer == 0 ? 0.0f : 0.16f;
                        petalColor.r += (1 - petalColor.r) * light;
                        petalColor.g += (1 - petalColor.g) * light;
                        petalColor.b += (1 - petalColor.b) * light;
                        brush->SetColor(petalColor);
                        target->FillGeometry(petal_.Get(), brush);
                        brush->SetColor(D2D1::ColorF(color.r * 0.70f, color.g * 0.70f,
                                                    color.b * 0.70f, alpha * 0.55f));
                        target->DrawGeometry(petal_.Get(), brush, 0.45f / scale);
                    }
                }
                target->SetTransform(head * previous);
                brush->SetColor(D2D1::ColorF(0.62f, 0.43f, 0.20f, alpha));
                target->FillEllipse(D2D1::Ellipse({0, 0}, radius * 0.28f, radius * 0.25f), brush);
                brush->SetColor(D2D1::ColorF(0.94f, 0.77f, 0.39f, alpha * 0.95f));
                for (int p = 0; p < 6; ++p) {
                    const float angleRad = p * kPi / 3;
                    target->FillEllipse(D2D1::Ellipse(
                        {std::cos(angleRad) * radius * 0.17f, std::sin(angleRad) * radius * 0.14f},
                        0.65f, 0.65f), brush);
                }
            }
        }
    }
    target->SetTransform(previous);
}
