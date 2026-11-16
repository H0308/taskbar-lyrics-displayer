#include "ui/manual_search_preview.h"

#include "ui/fluent_theme.h"
#include "ui/font_sizes.h"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <utility>

namespace {

constexpr DWORD kResumeDelayMs = 2000;
constexpr int kWheelLinesPerNotch = 3;
constexpr float kLineScrollFrequency = 18.0f;
constexpr float kResumeScrollFrequency = 14.0f;
constexpr float kLyricTextWidthDip = 420.0f;
constexpr float kHeaderHeight = 62.0f;
constexpr float kLyricRowHeightDip = 50.0f;
constexpr float kSecondaryLineHeight = 18.0f;
constexpr float kSecondaryGap = 2.0f;
constexpr float kToggleSize = 28.0f;
constexpr float kToggleGap = 6.0f;
constexpr float kToggleMargin = 10.0f;
constexpr float kToggleTextGap = 10.0f;

} // namespace

bool ManualSearchPreview::create(HWND hwnd, std::function<void()> onInvalidate) {
    return scrollView_.create(hwnd, 0x6402,
                              [cb = std::move(onInvalidate)](float, bool redraw) {
                                  if (redraw && cb)
                                      cb();
                              });
}

void ManualSearchPreview::resetScroll() {
    scrollView_.reset();
}

void ManualSearchPreview::setState(ManualSearchPreviewState state, HWND hwnd) {
    if (state == state_)
        return;
    state_ = state;
    if (state == ManualSearchPreviewState::Loading)
        loadingSince_ = GetTickCount64();
    updateLoadingTimer(hwnd);
}

void ManualSearchPreview::updateLoadingTimer(HWND hwnd) {
    if (!hwnd)
        return;
    if (state_ == ManualSearchPreviewState::Loading && animations_ && !IsIconic(hwnd))
        SetTimer(hwnd, kLoadingTimerId, 33, nullptr);
    else
        KillTimer(hwnd, kLoadingTimerId);
}

void ManualSearchPreview::refreshAnimations(HWND hwnd) {
    BOOL enabled = TRUE;
    SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &enabled, 0);
    animations_ = enabled != FALSE;
    updateLoadingTimer(hwnd);
}

void ManualSearchPreview::updateCapability() {
    hasTranslation_ = false;
    hasRomanization_ = false;
    for (const auto& line : lines_) {
        hasTranslation_ = hasTranslation_ || !line.translation.empty();
        hasRomanization_ = hasRomanization_ || !line.romanization.empty();
    }
    showTranslation_ = showTranslation_ && hasTranslation_;
    showRomanization_ = showRomanization_ && hasRomanization_;
    if (hasTranslation_ && hasRomanization_)
        capabilityLabel_ = L"[支持翻译 + 罗马音]";
    else if (hasTranslation_)
        capabilityLabel_ = L"[支持翻译]";
    else if (hasRomanization_)
        capabilityLabel_ = L"[支持罗马音]";
    else
        capabilityLabel_.clear();
}

float ManualSearchPreview::contentTop() const {
    return kHeaderHeight + (capabilityLabel_.empty() ? 0.0f : 24.0f);
}

int ManualSearchPreview::secondaryMask() const {
    return (showRomanization_ ? 1 : 0) | (showTranslation_ ? 2 : 0);
}

int ManualSearchPreview::secondaryLineCount(const LyricLine& line) const {
    return ((showRomanization_ && !line.romanization.empty()) ? 1 : 0) +
           ((showTranslation_ && !line.translation.empty()) ? 1 : 0);
}

float ManualSearchPreview::secondaryHeight(int secondaryCount) const {
    return secondaryCount * (kSecondaryLineHeight + kSecondaryGap);
}

float ManualSearchPreview::defaultLyricRowHeight() const {
    const int count = (showRomanization_ ? 1 : 0) + (showTranslation_ ? 1 : 0);
    return kLyricRowHeightDip + secondaryHeight(count);
}

float ManualSearchPreview::measureMainHeight(fluent::FluentDialogSurface::Painter& painter,
                                             const std::wstring& text, float width) const {
    if (text.empty())
        return kLyricRowHeightDip;

    auto* format = painter.textFormat(fluent::font_sizes::emphasis, 700, true);
    auto* layout = painter.textLayout(text, format, width, 10000.0f);
    if (!layout)
        return kLyricRowHeightDip;

    DWRITE_TEXT_METRICS metrics{};
    if (FAILED(layout->GetMetrics(&metrics)))
        return kLyricRowHeightDip;
    return std::max(kLyricRowHeightDip,
                    static_cast<float>(std::ceil(std::max(0.0f, metrics.height))));
}

void ManualSearchPreview::resetView(int64_t playbackPositionMs) {
    scrollView_.setOffset(0.0f);
    positionMs_ = playbackPositionMs;
    currentLine_ = LyricProvider::findLine(lines_, positionMs_);
    manualScroll_ = false;
    snapNextSync_ = true;
    autoResumePending_ = false;
    contentDirty_ = true;
    lastScrollTick_ = 0;
}

void ManualSearchPreview::setLines(const std::vector<LyricLine>& lines,
                                   int64_t playbackPositionMs) {
    lines_ = lines;
    measuredWidth_ = -1.0f;
    mainHeights_.clear();
    rowHeights_.clear();
    rowOffsets_.clear();
    updateCapability();
    resetView(playbackPositionMs);
}

bool ManualSearchPreview::setPosition(int64_t positionMs) {
    const int nextLine = LyricProvider::findLine(lines_, positionMs);
    const bool lineChanged = nextLine != currentLine_;
    const bool karaokeActive =
        nextLine >= 0 && nextLine < static_cast<int>(lines_.size()) &&
        !lines_[nextLine].chars.empty() && positionMs != positionMs_;
    positionMs_ = positionMs;
    currentLine_ = nextLine;
    if (!lineChanged && !karaokeActive)
        return false;
    contentDirty_ = true;
    return true;
}

void ManualSearchPreview::shiftTimes(int64_t deltaMs, int64_t playbackPositionMs) {
    if (lines_.empty())
        return;
    info_.timingOffsetMs += deltaMs;
    for (auto& line : lines_) {
        line.ms += deltaMs;
        for (auto& character : line.chars) {
            character.startMs += deltaMs;
            character.endMs += deltaMs;
        }
    }
    resetView(playbackPositionMs);
}

void ManualSearchPreview::toggleRomanization() {
    showRomanization_ = !showRomanization_;
    manualScroll_ = false;
    snapNextSync_ = true;
    autoResumePending_ = false;
    contentDirty_ = true;
}

void ManualSearchPreview::toggleTranslation() {
    showTranslation_ = !showTranslation_;
    manualScroll_ = false;
    snapNextSync_ = true;
    autoResumePending_ = false;
    contentDirty_ = true;
}

void ManualSearchPreview::onPointerDown() {
    if (manualScroll_) {
        scrollView_.stop();
        lastScrollTick_ = GetTickCount();
    }
}

void ManualSearchPreview::scrollByWheel(int delta) {
    if (lines_.empty())
        return;
    manualScroll_ = true;
    autoResumePending_ = false;
    snapNextSync_ = false;
    lastScrollTick_ = GetTickCount();
    scrollView_.scrollBy(-static_cast<float>(delta) / WHEEL_DELTA *
                         kWheelLinesPerNotch * defaultLyricRowHeight());
}

bool ManualSearchPreview::onResumeTimer() {
    if (!manualScroll_ || GetTickCount() - lastScrollTick_ < kResumeDelayMs)
        return false;
    manualScroll_ = false;
    autoResumePending_ = true;
    contentDirty_ = true;
    return true;
}

void ManualSearchPreview::onContainerResizeOrDpiChange() {
    scrollView_.stop();
    if (!manualScroll_)
        snapNextSync_ = true;
    contentDirty_ = true;
}

void ManualSearchPreview::onThemeChange() {
    measuredWidth_ = -1.0f;
    onContainerResizeOrDpiChange();
}

void ManualSearchPreview::layoutToggleButtons(const D2D1_RECT_F& previewRect,
                                              D2D1_RECT_F& romanizationRect,
                                              D2D1_RECT_F& translationRect) const {
    romanizationRect = {};
    translationRect = {};
    const int toggleCount = (hasRomanization_ ? 1 : 0) + (hasTranslation_ ? 1 : 0);
    if (toggleCount <= 0)
        return;

    const float toggleRailHeight = toggleCount * kToggleSize + (toggleCount - 1) * kToggleGap;
    const float toggleBottom = previewRect.bottom - kToggleMargin;
    const float toggleTop =
        std::max(previewRect.top + contentTop(), toggleBottom - toggleRailHeight);
    const float toggleRight = previewRect.right - kToggleMargin;
    float nextTop = toggleTop;
    if (hasRomanization_) {
        romanizationRect = D2D1::RectF(toggleRight - kToggleSize, nextTop, toggleRight,
                                       nextTop + kToggleSize);
        nextTop += kToggleSize + kToggleGap;
    }
    if (hasTranslation_) {
        translationRect = D2D1::RectF(toggleRight - kToggleSize, nextTop, toggleRight,
                                      nextTop + kToggleSize);
    }
}

std::vector<D2D1_RECT_F> ManualSearchPreview::karaokeHighlightClips(
    IDWriteTextLayout* layout, const LyricLine& line) const {
    std::vector<D2D1_RECT_F> clips;
    if (!layout || line.chars.empty() || line.text.empty())
        return clips;

    const UINT32 textLength = static_cast<UINT32>(line.text.size());
    UINT32 textOffset = 0;
    auto appendClip = [&clips](const DWRITE_HIT_TEST_METRICS& metrics, float width) {
        if (width <= 0.0f || metrics.height <= 0.0f)
            return;
        const D2D1_RECT_F clip =
            D2D1::RectF(metrics.left, metrics.top, metrics.left + width,
                        metrics.top + metrics.height);
        // 同一视觉行的已唱片段合并成一块，避免每个字都 Push/Pop 一次裁剪区域。
        for (auto& existing : clips) {
            if (std::fabs(existing.top - clip.top) < 0.5f &&
                std::fabs(existing.bottom - clip.bottom) < 0.5f) {
                existing.left = std::min(existing.left, clip.left);
                existing.right = std::max(existing.right, clip.right);
                return;
            }
        }
        clips.push_back(clip);
    };

    std::vector<DWRITE_HIT_TEST_METRICS> metrics;
    for (const auto& character : line.chars) {
        const UINT32 tokenLength = static_cast<UINT32>(character.text.size());
        if (tokenLength == 0)
            continue;
        if (textOffset >= textLength || character.startMs > positionMs_)
            break;

        const UINT32 rangeLength = std::min(tokenLength, textLength - textOffset);
        const int64_t durationMs = std::max<int64_t>(character.endMs - character.startMs, 1);
        const float fraction = static_cast<float>(std::clamp(
            static_cast<double>(positionMs_ - character.startMs) /
                static_cast<double>(durationMs),
            0.0, 1.0));
        if (fraction > 0.0f && rangeLength > 0) {
            metrics.resize(rangeLength + 1);
            UINT32 actualCount = 0;
            if (SUCCEEDED(layout->HitTestTextRange(
                    textOffset, rangeLength, 0.0f, 0.0f, metrics.data(),
                    static_cast<UINT32>(metrics.size()), &actualCount))) {
                float totalWidth = 0.0f;
                for (UINT32 i = 0; i < actualCount; ++i)
                    totalWidth += std::max(0.0f, metrics[i].width);

                float remainingWidth = totalWidth * fraction;
                for (UINT32 i = 0; i < actualCount && remainingWidth > 0.0f; ++i) {
                    const float metricWidth = std::max(0.0f, metrics[i].width);
                    if (metricWidth <= 0.0f)
                        continue;
                    const float highlightWidth = std::min(metricWidth, remainingWidth);
                    appendClip(metrics[i], highlightWidth);
                    remainingWidth -= metricWidth;
                }
            }
        }
        textOffset += tokenLength;
    }
    return clips;
}

void ManualSearchPreview::drawCurrentLyric(fluent::FluentDialogSurface::Painter& painter,
                                           const LyricLine& line, const D2D1_RECT_F& rect,
                                           float dipScale) const {
    const auto& p = fluent::palette();
    auto* format = painter.textFormat(fluent::font_sizes::emphasis, 700, true);
    if (!format)
        return;

    if (line.chars.empty()) {
        painter.drawText(line.text, format, rect, fluent::toD2D(RGB(49, 194, 124)));
        return;
    }

    auto* layout = painter.textLayout(line.text, format, rect.right - rect.left,
                                      rect.bottom - rect.top);
    if (!layout)
        return;

    const D2D1_POINT_2F origin = D2D1::Point2F(
        std::round(rect.left * dipScale) / dipScale,
        std::round(rect.top * dipScale) / dipScale);
    std::vector<D2D1_RECT_F> playedClips;
    std::vector<D2D1_RECT_F> unplayedClips{
        D2D1::RectF(rect.left, rect.top, rect.right, rect.bottom)};
    for (const auto& clip : karaokeHighlightClips(layout, line)) {
        const D2D1_RECT_F played = D2D1::RectF(
            std::max(rect.left, origin.x + clip.left),
            std::max(rect.top, origin.y + clip.top),
            std::min(rect.right, origin.x + clip.right),
            std::min(rect.bottom, origin.y + clip.bottom));
        if (played.right <= played.left || played.bottom <= played.top)
            continue;
        playedClips.push_back(played);
        std::vector<D2D1_RECT_F> remaining;
        for (const auto& area : unplayedClips) {
            const D2D1_RECT_F overlap = D2D1::RectF(
                std::max(area.left, played.left), std::max(area.top, played.top),
                std::min(area.right, played.right), std::min(area.bottom, played.bottom));
            if (overlap.right <= overlap.left || overlap.bottom <= overlap.top) {
                remaining.push_back(area);
                continue;
            }
            if (area.top < overlap.top)
                remaining.push_back(D2D1::RectF(area.left, area.top, area.right, overlap.top));
            if (overlap.bottom < area.bottom)
                remaining.push_back(D2D1::RectF(area.left, overlap.bottom, area.right, area.bottom));
            if (area.left < overlap.left)
                remaining.push_back(D2D1::RectF(area.left, overlap.top, overlap.left, overlap.bottom));
            if (overlap.right < area.right)
                remaining.push_back(D2D1::RectF(overlap.right, overlap.top, area.right, overlap.bottom));
        }
        unplayedClips = std::move(remaining);
    }

    auto drawClippedText = [&](const D2D1_RECT_F& clip, D2D1_COLOR_F color) {
        painter.target()->PushAxisAlignedClip(clip, D2D1_ANTIALIAS_MODE_ALIASED);
        painter.drawTextLayout(layout, origin, color);
        painter.target()->PopAxisAlignedClip();
    };
    for (const auto& clip : unplayedClips)
        drawClippedText(clip, p.textSecondary);
    for (const auto& clip : playedClips)
        drawClippedText(clip, fluent::toD2D(RGB(49, 194, 124)));
}

void ManualSearchPreview::drawLyricLine(fluent::FluentDialogSurface::Painter& painter,
                                        const LyricLine& line, const D2D1_RECT_F& rowRect,
                                        float mainHeight, bool current,
                                        float dipScale) const {
    const auto& p = fluent::palette();
    const int secondaryCount = secondaryLineCount(line);
    const float contentHeight = mainHeight + secondaryHeight(secondaryCount);
    const float top = rowRect.top +
                      std::max(0.0f, (rowRect.bottom - rowRect.top - contentHeight) * 0.5f);
    const D2D1_RECT_F mainRect =
        D2D1::RectF(rowRect.left, top, rowRect.right, top + mainHeight);
    if (current) {
        drawCurrentLyric(painter, line, mainRect, dipScale);
    } else {
        painter.drawText(line.text, painter.textFormat(fluent::font_sizes::body, 400, true),
                         mainRect, p.text);
    }

    float secondaryTop = mainRect.bottom + kSecondaryGap;
    auto drawSecondary = [&](const std::wstring& text) {
        if (text.empty())
            return;
        painter.drawTrimmedText(
            text, painter.textFormat(fluent::font_sizes::caption, 400, true, true),
            D2D1::RectF(rowRect.left, secondaryTop, rowRect.right,
                        secondaryTop + kSecondaryLineHeight),
            p.textSecondary);
        secondaryTop += kSecondaryLineHeight + kSecondaryGap;
    };
    if (showRomanization_)
        drawSecondary(line.romanization);
    if (showTranslation_)
        drawSecondary(line.translation);
}

void ManualSearchPreview::drawToggleButton(fluent::FluentDialogSurface::Painter& painter,
                                           const D2D1_RECT_F& rect, const wchar_t* label,
                                           bool checked, int id,
                                           const ManualSearchPreviewDrawContext& ctx) const {
    const auto& p = fluent::palette();
    const bool hovered = ctx.hoverId == id;
    const bool pressed = ctx.pressedId == id;
    const bool showFill = hovered || pressed;
    const D2D1_COLOR_F border = checked || hovered || pressed ? p.accent : p.controlStroke;
    const D2D1_COLOR_F textColor = checked ? p.accent : (hovered ? p.text : p.textSecondary);
    if (showFill) {
        painter.fillRoundRect(pressed ? p.controlPressed : p.controlHover, rect,
                              fluent::metrics::controlRadius);
    }
    painter.strokeRoundRect(border, rect, 1.0f, fluent::metrics::controlRadius);
    if (ctx.focusedId == id && ctx.focusVisible) {
        painter.strokeRoundRect(
            p.accent,
            D2D1::RectF(rect.left + 1.5f, rect.top + 1.5f, rect.right - 1.5f,
                        rect.bottom - 1.5f),
            1.5f, std::max(1.0f, fluent::metrics::controlRadius - 1.0f));
    }
    painter.drawText(label, painter.textFormat(fluent::font_sizes::label, 600, true, true),
                     rect, textColor);
}

void ManualSearchPreview::drawPlaceholder(fluent::FluentDialogSurface::Painter& painter,
                                          const D2D1_RECT_F& rect) const {
    const auto& p = fluent::palette();
    const float centerX = (rect.left + rect.right) * 0.5f;
    const float centerY = (rect.top + kHeaderHeight + rect.bottom) * 0.5f;
    const bool loading = state_ == ManualSearchPreviewState::Loading;
    const wchar_t* text = loading ? L"正在加载预览…"
        : state_ == ManualSearchPreviewState::Unavailable ? L"该候选没有可用歌词"
                                                          : L"选择左侧歌曲预览歌词";

    if (loading) {
        // 五根静止的节拍柱错开亮度，避免持续改变图形位置或尺寸。
        constexpr float heights[] = {10.0f, 18.0f, 26.0f, 18.0f, 10.0f};
        constexpr ULONGLONG beatMs = 500;
        const auto elapsed = GetTickCount64() - loadingSince_;
        for (size_t i = 0; i < _countof(heights); ++i) {
            const float x = centerX + (static_cast<float>(i) - 2.0f) * 9.0f;
            const float halfHeight = heights[i] * 0.5f;
            const float phase = static_cast<float>((elapsed + beatMs - i * 60) % beatMs) /
                                static_cast<float>(beatMs);
            const float pulse = 0.5f - 0.5f * std::cos(phase * 2.0f * std::numbers::pi_v<float>);
            auto color = p.accent;
            color.a *= animations_ ? 0.30f + 0.65f * pulse : 0.75f;
            painter.fillRoundRect(color, D2D1::RectF(x - 2.0f, centerY - 14.0f - halfHeight,
                                                   x + 2.0f, centerY - 14.0f + halfHeight),
                                  2.0f);
        }
    }
    const float textY = centerY + (loading ? 18.0f : 0.0f);
    painter.drawText(text, painter.textFormat(fluent::font_sizes::emphasis, 400, true),
                     D2D1::RectF(rect.left + 12.0f, textY - 14.0f,
                                 rect.right - 12.0f, textY + 14.0f),
                     p.textSecondary);
}

void ManualSearchPreview::draw(fluent::FluentDialogSurface::Painter& painter,
                               const ManualSearchPreviewDrawContext& ctx) {
    const auto& p = fluent::palette();
    const auto& previewRect = ctx.rect;
    if (ctx.focusedId == ctx.previewId && ctx.focusVisible) {
        painter.strokeRoundRect(
            p.accent,
            D2D1::RectF(previewRect.left + 1.5f, previewRect.top + 1.5f,
                        previewRect.right - 1.5f, previewRect.bottom - 1.5f),
            1.5f, fluent::metrics::cardRadius - 1.0f);
    }

    if (ctx.selectedCandidate) {
        const float left = previewRect.left + 18.0f;
        const float right = previewRect.right - 18.0f;
        const float top = previewRect.top;
        painter.drawTrimmedText(
            ctx.selectedCandidate->name,
            painter.textFormat(fluent::font_sizes::emphasis, 600, false, true),
            D2D1::RectF(left, top + 7.0f, right, top + 31.0f), p.text);
        painter.drawTrimmedText(
            ctx.selectedCandidate->singer,
            painter.textFormat(fluent::font_sizes::caption, 400, false, true),
            D2D1::RectF(left, top + 31.0f, right, top + 51.0f), p.textSecondary);
        if (auto* brush = painter.brush(p.cardStroke)) {
            const float y = top + kHeaderHeight - 6.0f;
            painter.target()->FillRectangle(D2D1::RectF(left, y, right, y + 1.0f), brush);
        }
    }

    if (lines_.empty()) {
        scrollView_.layout({}, 0.0f, ctx.dpi);
        drawPlaceholder(painter, previewRect);
        return;
    }

    const float topOffset = contentTop();
    if (!capabilityLabel_.empty()) {
        painter.drawText(capabilityLabel_,
                         painter.textFormat(fluent::font_sizes::smallText, 400, true),
                         D2D1::RectF(previewRect.left + 12.0f, previewRect.top + kHeaderHeight,
                                     previewRect.right - 12.0f,
                                     previewRect.top + kHeaderHeight + 24.0f),
                         p.textSecondary);
    }

    // 左右对称预留切换按钮安全边距，保证歌词列始终对齐容器中心且不与「音/译」按钮重叠。
    constexpr float kLyricSideMargin = kToggleMargin + kToggleSize + kToggleTextGap;
    const float lyricLeftLimit = previewRect.left + kLyricSideMargin;
    const float lyricRightLimit = previewRect.right - kLyricSideMargin;
    const float lyricAreaWidth = std::max(0.0f, lyricRightLimit - lyricLeftLimit);
    const float lyricWidth = std::min(kLyricTextWidthDip, lyricAreaWidth);
    const float lyricLeft = lyricLeftLimit + (lyricAreaWidth - lyricWidth) * 0.5f;
    const float lyricRight = lyricLeft + lyricWidth;

    const int total = static_cast<int>(lines_.size());
    const float bodyTop = previewRect.top + topOffset;
    const float bodyBottom = std::max(bodyTop, previewRect.bottom - 2.0f);
    const float availableHeight = std::max(0.0f, bodyBottom - bodyTop);

    const int mask = secondaryMask();
    if (measuredWidth_ != lyricWidth || measuredSecondaryMask_ != mask ||
        measuredDpi_ != ctx.dpi) {
        mainHeights_.resize(total);
        rowHeights_.resize(total);
        rowOffsets_.assign(total + 1, 0.0f);
        for (int index = 0; index < total; ++index) {
            mainHeights_[index] = measureMainHeight(painter, lines_[index].text, lyricWidth);
            rowHeights_[index] =
                mainHeights_[index] + secondaryHeight(secondaryLineCount(lines_[index]));
            rowOffsets_[index + 1] = rowOffsets_[index] + rowHeights_[index];
        }
        measuredWidth_ = lyricWidth;
        measuredSecondaryMask_ = mask;
        measuredDpi_ = ctx.dpi;
        contentDirty_ = true;
    }

    float totalScrollContentHeight = availableHeight;
    if (rowOffsets_[total] <= availableHeight) {
        scrollPadding_ = std::max(0.0f, (availableHeight - rowOffsets_[total]) * 0.5f);
    } else {
        float firstPageHeight = 0.0f;
        for (int index = 0; index < total; ++index) {
            if (index > 0 && firstPageHeight + rowHeights_[index] > availableHeight)
                break;
            firstPageHeight += rowHeights_[index];
        }
        scrollPadding_ = std::max(6.0f, (availableHeight - firstPageHeight) * 0.5f);
        totalScrollContentHeight = rowOffsets_[total] + scrollPadding_ * 2.0f;
    }
    const float maxScrollOffset = std::max(0.0f, totalScrollContentHeight - availableHeight);

    scrollView_.layout(D2D1::RectF(lyricLeft, bodyTop, lyricRight, bodyBottom),
                       totalScrollContentHeight, ctx.dpi);
    if (!manualScroll_) {
        float desiredOffset = 0.0f;
        if (currentLine_ >= 0 && currentLine_ < total) {
            const float lineCenterY =
                scrollPadding_ + rowOffsets_[currentLine_] + rowHeights_[currentLine_] * 0.5f;
            desiredOffset = std::clamp(lineCenterY - availableHeight * 0.5f,
                                       0.0f, maxScrollOffset);
        }
        if (snapNextSync_) {
            snapNextSync_ = false;
            autoResumePending_ = false;
            scrollView_.setOffset(desiredOffset);
        } else if (autoResumePending_) {
            autoResumePending_ = false;
            scrollView_.scrollTo(desiredOffset, true, kResumeScrollFrequency, false);
        } else if (std::fabs(scrollView_.targetOffset() - desiredOffset) > 0.5f) {
            scrollView_.scrollTo(desiredOffset, true, kLineScrollFrequency, false);
        }
    }

    const bool forceRedraw = contentDirty_;
    contentDirty_ = false;
    scrollView_.paint(
        [&](fluent::FluentDialogSurface::Painter& scrollPainter,
            const D2D1_RECT_F& paintBounds, float position) {
            scrollPainter.fillRoundRect(p.cardFill, paintBounds, 0.0f);
            for (int index = 0; index < total; ++index) {
                const float rowTop = bodyTop + scrollPadding_ + rowOffsets_[index] - position;
                const D2D1_RECT_F rect = D2D1::RectF(lyricLeft, rowTop, lyricRight,
                                                     rowTop + rowHeights_[index]);
                if (rect.bottom < paintBounds.top || rect.top > paintBounds.bottom)
                    continue;
                drawLyricLine(scrollPainter, lines_[index], rect, mainHeights_[index],
                              index == currentLine_, ctx.dipScale);
            }
        },
        false, forceRedraw);

    if (hasRomanization_)
        drawToggleButton(painter, ctx.romanizationRect, L"音", showRomanization_,
                         ctx.romanizationId, ctx);
    if (hasTranslation_)
        drawToggleButton(painter, ctx.translationRect, L"译", showTranslation_,
                         ctx.translationId, ctx);
}
