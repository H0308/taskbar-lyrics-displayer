#include "ui/settings/row_painters.h"

#include "ui/fluent_controls.h"
#include "ui/fluent_theme.h"
#include "ui/font_sizes.h"
#include "ui/media_control_icons.h"
#include "ui/settings/ids.h"
#include "ui/settings/section_metrics.h"

#include <wrl/client.h>

#include <algorithm>
#include <string>

namespace settings_ui::row_painters {
namespace {

bool isOptionEnabled(const Row& row, size_t option) {
    return option >= row.optionEnabled.size() || row.optionEnabled[option];
}

void drawRadioCardBackground(fluent::FluentDialogSurface::Painter& painter,
                             const RowPaintContext& ctx, const Row& row,
                             const D2D1_RECT_F& card, bool selected, bool hovered,
                             bool pressed) {
    const auto& p = painter.colors();
    D2D1_COLOR_F fill = p.controlFill;
    if (!row.enabled)
        fill = p.listHover;
    else if (pressed)
        fill = p.controlPressed;
    else if (selected)
        fill = p.listSelected;
    else if (hovered)
        fill = p.controlHover;
    painter.fillRoundRect(fill, card, fluent::metrics::controlRadius);

    const bool focused = ctx.focusedId == row.id && ctx.focusVisible && row.enabled;
    if (selected) {
        painter.strokeRoundRect(row.enabled ? (hovered ? p.accentHover : p.accent)
                                            : p.disabled,
                                card, focused ? 2.0f : 1.5f,
                                fluent::metrics::controlRadius);
    } else {
        painter.strokeRoundRect(p.cardStroke, card, 1.0f,
                                fluent::metrics::controlRadius);
    }
}

void drawRadioIndicator(fluent::FluentDialogSurface::Painter& painter, const Row& row,
                        const D2D1_RECT_F& card, bool selected, bool hovered, bool pressed,
                        float rightInset, float topInset) {
    const auto& p = painter.colors();
    const D2D1_POINT_2F radioCenter =
        D2D1::Point2F(card.right - rightInset, card.top + topInset);
    if (!row.enabled) {
        if (auto* brush = painter.brush(p.disabled))
            painter.target()->DrawEllipse(D2D1::Ellipse(radioCenter, 6.0f, 6.0f), brush,
                                          1.0f);
        if (selected) {
            if (auto* brush = painter.brush(p.disabled))
                painter.target()->FillEllipse(D2D1::Ellipse(radioCenter, 2.0f, 2.0f),
                                              brush);
        }
    } else if (selected) {
        if (auto* brush = painter.brush(hovered || pressed ? p.accentHover : p.accent))
            painter.target()->FillEllipse(D2D1::Ellipse(radioCenter, 6.0f, 6.0f), brush);
        if (auto* brush = painter.brush(p.textOnAccent))
            painter.target()->FillEllipse(D2D1::Ellipse(radioCenter, 2.0f, 2.0f), brush);
    } else if (auto* brush = painter.brush(hovered ? p.text : p.textSecondary)) {
        painter.target()->DrawEllipse(D2D1::Ellipse(radioCenter, 6.0f, 6.0f), brush,
                                      pressed ? 1.5f : 1.0f);
    }
}

void drawSpectrumStyleRadio(fluent::FluentDialogSurface::Painter& painter,
                            const RowPaintContext& ctx, Row& row) {
    const auto& p = painter.colors();
    auto* titleFormat = painter.textFormat(fluent::font_sizes::label, 500, true, true);
    if (!titleFormat)
        return;

    const float gridW = row.controlRect.right - row.controlRect.left;
    row.optionRects.clear();
    const size_t count = row.options.size();
    if (count == 0)
        return;
    const size_t columns = std::min<size_t>(count, 3);
    const float cardW = (gridW - kSpectrumStyleCardGap * static_cast<float>(columns - 1)) /
                        static_cast<float>(columns);
    for (size_t i = 0; i < count; ++i) {
        const float left = row.controlRect.left + static_cast<float>(i % columns) *
                           (cardW + kSpectrumStyleCardGap);
        const float top = row.controlRect.top + static_cast<float>(i / columns) *
                          (kSpectrumStyleCardH + kSpectrumStyleCardGap);
        const D2D1_RECT_F card = D2D1::RectF(left, top, left + cardW,
                                             top + kSpectrumStyleCardH);
        row.optionRects.push_back(card);

        const bool selected = static_cast<int>(i) == row.selected;
        const bool hovered = row.enabled && ctx.hoverId == row.id &&
                             ctx.hoverOption == static_cast<int>(i);
        const bool pressed = row.enabled && ctx.pressedId == row.id &&
                             ctx.pressedOption == static_cast<int>(i);
        drawRadioCardBackground(painter, ctx, row, card, selected, hovered, pressed);

        const D2D1_RECT_F artwork =
            D2D1::RectF(card.left + 8.0f, card.top + 8.0f, card.right - 24.0f,
                        card.top + 60.0f);
        ctx.artwork.drawSpectrumStyleArtwork(painter, artwork, static_cast<int>(i), row.enabled);

        painter.drawText(row.options[i], titleFormat,
                         D2D1::RectF(card.left + 6.0f, card.bottom - 25.0f,
                                     card.right - 6.0f, card.bottom - 6.0f),
                         row.enabled ? p.text : p.disabled);

        // 示意图和文字先画，单选框最后画，避免缩放时被内容盖住。
        drawRadioIndicator(painter, row, card, selected, hovered, pressed, 14.0f, 14.0f);
    }
}

void drawCoverEffectRadio(fluent::FluentDialogSurface::Painter& painter,
                          const RowPaintContext& ctx, Row& row) {
    const auto& p = painter.colors();
    auto* titleFormat = painter.textFormat(fluent::font_sizes::label, 500, false, true);
    if (!titleFormat)
        return;

    const float gridW = row.controlRect.right - row.controlRect.left;
    const float cardW = (gridW - kCoverEffectCardGap) * 0.5f;
    row.optionRects.clear();
    const size_t count = std::min<size_t>(row.options.size(), 2);
    for (size_t i = 0; i < count; ++i) {
        const float left = row.controlRect.left + static_cast<float>(i) *
                           (cardW + kCoverEffectCardGap);
        const D2D1_RECT_F card = D2D1::RectF(left, row.controlRect.top, left + cardW,
                                             row.controlRect.bottom);
        row.optionRects.push_back(card);

        const bool selected = static_cast<int>(i) == row.selected;
        const bool hovered = row.enabled && ctx.hoverId == row.id &&
                             ctx.hoverOption == static_cast<int>(i);
        const bool pressed = row.enabled && ctx.pressedId == row.id &&
                             ctx.pressedOption == static_cast<int>(i);
        drawRadioCardBackground(painter, ctx, row, card, selected, hovered, pressed);

        drawRadioIndicator(painter, row, card, selected, hovered, pressed, 16.0f, 16.0f);

        const D2D1_RECT_F artwork =
            D2D1::RectF(card.left + 12.0f, card.top + 14.0f, card.left + 56.0f,
                        card.top + 58.0f);
        ctx.artwork.drawCoverEffectArtwork(painter, artwork, i == 1, row.enabled);
        painter.drawText(row.options[i], titleFormat,
                         D2D1::RectF(card.left + 68.0f, card.top + 20.0f,
                                     card.right - 28.0f, card.bottom - 14.0f),
                         row.enabled ? p.text : p.disabled);
    }
}

void drawSongToastPositionRadio(fluent::FluentDialogSurface::Painter& painter,
                                const RowPaintContext& ctx, Row& row) {
    const auto& p = painter.colors();
    auto* titleFormat = painter.textFormat(fluent::font_sizes::label, 500, true, true);
    if (!titleFormat)
        return;

    const float gridW = row.controlRect.right - row.controlRect.left;
    row.optionRects.clear();
    const size_t count = std::min<size_t>(row.options.size(), 2);
    if (count == 0)
        return;
    const float cardW = std::min(
        kSongToastPositionCardW,
        (gridW - kSongToastPositionCardGap * static_cast<float>(count - 1)) /
            static_cast<float>(count));
    const float cardsW = cardW * static_cast<float>(count) +
                         kSongToastPositionCardGap * static_cast<float>(count - 1);
    const float cardsLeft = row.controlRect.left + (gridW - cardsW) * 0.5f;
    for (size_t i = 0; i < count; ++i) {
        const float left = cardsLeft + static_cast<float>(i) *
                           (cardW + kSongToastPositionCardGap);
        const D2D1_RECT_F card = D2D1::RectF(left, row.controlRect.top, left + cardW,
                                             row.controlRect.bottom);
        row.optionRects.push_back(card);

        const bool selected = static_cast<int>(i) == row.selected;
        const bool hovered = row.enabled && ctx.hoverId == row.id &&
                             ctx.hoverOption == static_cast<int>(i);
        const bool pressed = row.enabled && ctx.pressedId == row.id &&
                             ctx.pressedOption == static_cast<int>(i);
        drawRadioCardBackground(painter, ctx, row, card, selected, hovered, pressed);

        const float artworkW = std::min(100.0f, cardW - 20.0f);
        const D2D1_RECT_F artwork = D2D1::RectF(
            card.left + (cardW - artworkW) * 0.5f, card.top + 8.0f,
            card.left + (cardW + artworkW) * 0.5f, card.top + 58.0f);
        ctx.artwork.drawSongToastPositionArtwork(painter, artwork, i == 0, row.enabled);

        // 示意图先画，单选框最后画，避免位置指示被单选框遮住。
        drawRadioIndicator(painter, row, card, selected, hovered, pressed, 16.0f, 16.0f);

        painter.drawText(row.options[i], titleFormat,
                         D2D1::RectF(card.left + 10.0f, card.top + 65.0f,
                                     card.right - 10.0f, card.bottom - 8.0f),
                         row.enabled ? p.text : p.disabled);
    }
}

void drawIdleQuoteBackgroundRadio(fluent::FluentDialogSurface::Painter& painter,
                                  const RowPaintContext& ctx, Row& row) {
    const auto& p = painter.colors();
    auto* titleFormat = painter.textFormat(fluent::font_sizes::settings::option, 600, true, true);
    auto* noneFormat = painter.textFormat(fluent::font_sizes::caption, 600, true, true);
    if (!titleFormat || !noneFormat)
        return;

    const float gridW = row.controlRect.right - row.controlRect.left;
    const float cardW = (gridW - kIdleQuoteBackgroundCardGap) * 0.5f;
    row.optionRects.clear();

    const D2D1_RECT_F noneRect =
        D2D1::RectF(row.cardRect.right - 72.0f, row.cardRect.top + 10.0f,
                    row.cardRect.right - 16.0f, row.cardRect.top + 36.0f);
    row.optionRects.push_back(noneRect);

    const bool noneSelected = row.selected == 0;
    const bool noneHovered = row.enabled && ctx.hoverId == row.id && ctx.hoverOption == 0;
    const bool nonePressed = row.enabled && ctx.pressedId == row.id && ctx.pressedOption == 0;
    D2D1_COLOR_F noneFill = p.controlFill;
    if (!row.enabled)
        noneFill = p.listHover;
    else if (nonePressed)
        noneFill = p.controlPressed;
    else if (noneSelected)
        noneFill = p.listSelected;
    else if (noneHovered)
        noneFill = p.controlHover;
    painter.fillRoundRect(noneFill, noneRect, 13.0f);
    if (noneSelected) {
        painter.strokeRoundRect(row.enabled ? (noneHovered ? p.accentHover : p.accent)
                                            : p.disabled,
                                noneRect,
                                ctx.focusedId == row.id && ctx.focusVisible ? 2.0f : 1.0f, 13.0f);
    } else {
        painter.strokeRoundRect(p.cardStroke, noneRect, 1.0f, 13.0f);
    }
    const std::wstring noneText = row.options.empty() ? std::wstring(L"无") : row.options[0];
    painter.drawText(noneText, noneFormat, noneRect,
                     row.enabled ? p.text : p.disabled);

    const size_t count = row.options.size() > 1
                             ? std::min<size_t>(row.options.size() - 1, 4)
                             : 0;
    for (size_t i = 0; i < count; ++i) {
        const int optionIndex = static_cast<int>(i + 1);
        const int column = static_cast<int>(i % 2);
        const int line = static_cast<int>(i / 2);
        const float left = row.controlRect.left +
                           column * (cardW + kIdleQuoteBackgroundCardGap);
        const float top = row.controlRect.top +
                          line * (kIdleQuoteBackgroundCardH + kIdleQuoteBackgroundCardGap);
        const D2D1_RECT_F card =
            D2D1::RectF(left, top, left + cardW, top + kIdleQuoteBackgroundCardH);
        row.optionRects.push_back(card);

        const bool selected = optionIndex == row.selected;
        const bool hovered = row.enabled && ctx.hoverId == row.id &&
                             ctx.hoverOption == optionIndex;
        const bool pressed = row.enabled && ctx.pressedId == row.id &&
                             ctx.pressedOption == optionIndex;
        drawRadioCardBackground(painter, ctx, row, card, selected, hovered, pressed);

        const D2D1_RECT_F artwork =
            D2D1::RectF(card.left + 10.0f, card.top + 10.0f, card.right - 10.0f,
                        card.bottom - 31.0f);
        ctx.artwork.drawIdleQuoteBackgroundArtwork(painter, artwork, optionIndex, row.enabled);

        drawRadioIndicator(painter, row, card, selected, hovered, pressed, 16.0f, 14.0f);

        painter.drawText(row.options[optionIndex], titleFormat,
                         D2D1::RectF(card.left + 10.0f, card.bottom - 24.0f,
                                     card.right - 10.0f, card.bottom - 5.0f),
                         row.enabled ? p.text : p.disabled);
    }
}

void drawHoverControlStyleRadio(fluent::FluentDialogSurface::Painter& painter,
                                const RowPaintContext& ctx, Row& row) {
    const auto& p = painter.colors();
    auto* titleFormat = painter.textFormat(fluent::font_sizes::settings::option, 500, true, true);
    if (!titleFormat)
        return;

    const float gridW = row.controlRect.right - row.controlRect.left;
    const float cardW = (gridW - kCoverEffectCardGap) * 0.5f;
    row.optionRects.clear();
    const size_t count = std::min<size_t>(row.options.size(), 2);
    for (size_t i = 0; i < count; ++i) {
        const float left = row.controlRect.left + static_cast<float>(i) *
                           (cardW + kCoverEffectCardGap);
        const D2D1_RECT_F card = D2D1::RectF(left, row.controlRect.top, left + cardW,
                                             row.controlRect.bottom);
        row.optionRects.push_back(card);

        const bool selected = static_cast<int>(i) == row.selected;
        const bool hovered = row.enabled && ctx.hoverId == row.id &&
                             ctx.hoverOption == static_cast<int>(i);
        const bool pressed = row.enabled && ctx.pressedId == row.id &&
                             ctx.pressedOption == static_cast<int>(i);
        drawRadioCardBackground(painter, ctx, row, card, selected, hovered, pressed);

        const D2D1_RECT_F artwork =
            D2D1::RectF(card.left + 10.0f, card.top + 10.0f, card.right - 30.0f,
                        card.bottom - 27.0f);
        ctx.artwork.drawHoverControlStyleArtwork(painter, artwork, i == 1, row.enabled);

        // 示意图先画，单选框最后画，确保它始终位于卡片内容之上。
        drawRadioIndicator(painter, row, card, selected, hovered, pressed, 16.0f, 14.0f);

        painter.drawText(row.options[i], titleFormat,
                         D2D1::RectF(card.left + 10.0f, card.bottom - 24.0f,
                                     card.right - 10.0f, card.bottom - 5.0f),
                         row.enabled ? p.text : p.disabled);
    }
}

} // namespace

D2D1_RECT_F sliderValueRect(const Row& row) {
    const float trackRight = row.controlRect.right - kSliderValueW;
    return D2D1::RectF(trackRight + kSliderValueGap, row.controlRect.top,
                       row.controlRect.right, row.controlRect.bottom);
}

D2D1_RECT_F sliderNumberRect(const Row& row) {
    const auto rect = sliderValueRect(row);
    return D2D1::RectF(rect.left + 4.0f, rect.top + 1.0f,
                       rect.right - 4.0f - kSliderSuffixW, rect.bottom - 1.0f);
}

void drawButton(fluent::FluentDialogSurface::Painter& painter, const RowPaintContext& ctx,
                const Row& row) {
    const auto& p = painter.colors();
    const bool hovered = ctx.hoverId == row.id;
    const bool pressed = ctx.pressedId == row.id;
    D2D1_COLOR_F fill = pressed ? p.controlPressed : hovered ? p.controlHover : p.controlFill;
    D2D1_COLOR_F textColor = row.enabled ? p.text : p.disabled;
    if (!row.enabled)
        fill = p.listHover;
    painter.fillRoundRect(fill, row.controlRect);
    painter.strokeRoundRect(p.controlStroke, row.controlRect);
    if (ctx.focusedId == row.id && ctx.focusVisible && row.enabled) {
        painter.strokeRoundRect(
            p.accent,
            D2D1::RectF(row.controlRect.left + 1.5f, row.controlRect.top + 1.5f,
                        row.controlRect.right - 1.5f, row.controlRect.bottom - 1.5f),
            1.5f, fluent::metrics::controlRadius - 1.0f);
    }
    if (row.id == kIdFloatingCardBackgroundColor) {
        auto* format = painter.textFormat(fluent::font_sizes::body, 400, false, true);
        constexpr float kPreviewSize = 18.0f;
        constexpr float kPreviewGap = 8.0f;
        const float textW = format ? painter.measureTextWidth(row.controlText, format) : 0.0f;
        const float groupW = kPreviewSize + kPreviewGap + textW;
        const float centerX = (row.controlRect.left + row.controlRect.right) * 0.5f;
        const float centerY = (row.controlRect.top + row.controlRect.bottom) * 0.5f;
        const float groupLeft = centerX - groupW * 0.5f;
        const D2D1_RECT_F previewRect =
            D2D1::RectF(groupLeft, centerY - kPreviewSize * 0.5f,
                        groupLeft + kPreviewSize, centerY + kPreviewSize * 0.5f);
        painter.fillRoundRect(fluent::toD2D(ctx.floatingCardBackgroundColor),
                              previewRect, 4.0f);
        painter.strokeRoundRect(row.enabled ? p.cardStroke : p.disabled, previewRect, 1.0f,
                                4.0f);
        if (format) {
            painter.drawText(
                row.controlText, format,
                D2D1::RectF(groupLeft + kPreviewSize + kPreviewGap, row.controlRect.top,
                            row.controlRect.right - 4.0f, row.controlRect.bottom),
                textColor);
        }
        return;
    }
    painter.drawText(row.controlText, painter.textFormat(fluent::font_sizes::button, 400, true, true),
                     D2D1::RectF(row.controlRect.left + 4.0f, row.controlRect.top,
                                 row.controlRect.right - 4.0f, row.controlRect.bottom),
                     textColor);
}

void drawToggle(fluent::FluentDialogSurface::Painter& painter, const Row& row,
                bool hovered, bool focused) {
    const auto& p = painter.colors();
    const bool enabled = row.enabled;
    const float trackH = std::min(20.0f, row.controlRect.bottom - row.controlRect.top);
    const float centerY = (row.controlRect.top + row.controlRect.bottom) * 0.5f;
    const D2D1_RECT_F track = D2D1::RectF(
        row.controlRect.left + 0.5f, centerY - trackH * 0.5f,
        row.controlRect.right - 0.5f, centerY + trackH * 0.5f);
    const float radius = trackH * 0.5f;
    const float knobR = trackH * 0.5f - 3.5f;
    const float knobX = row.checked ? track.right - trackH * 0.5f
                                    : track.left + trackH * 0.5f;
    if (!enabled) {
        painter.fillRoundRect(p.listHover, track, radius);
        painter.strokeRoundRect(p.controlStroke, track, 1.0f, radius);
    } else if (row.checked) {
        painter.fillRoundRect(hovered ? p.accentHover : p.accent, track, radius);
    } else {
        painter.fillRoundRect(hovered ? p.controlHover : p.controlFill, track, radius);
        painter.strokeRoundRect(p.textSecondary, track, 1.0f, radius);
    }
    if (auto* br = painter.brush(!enabled ? p.disabled
                                           : row.checked ? p.textOnAccent : p.textSecondary)) {
        painter.target()->FillEllipse(D2D1::Ellipse(D2D1::Point2F(knobX, centerY), knobR, knobR),
                                      br);
    }
    if (focused && enabled) {
        painter.strokeRoundRect(
            p.accent,
            D2D1::RectF(track.left + 1.5f, track.top + 1.5f,
                        track.right - 1.5f, track.bottom - 1.5f),
            1.5f, std::max(1.0f, radius - 1.5f));
    }
}

void drawSlider(fluent::FluentDialogSurface::Painter& painter, const RowPaintContext& ctx,
                const Row& row) {
    const auto& p = painter.colors();
    const float centerY = (row.controlRect.top + row.controlRect.bottom) * 0.5f;
    constexpr float trackH = 4.0f;
    const float trackLeft = row.controlRect.left;
    const float trackRight = row.controlRect.right - kSliderValueW;
    const float range = static_cast<float>(std::max(1, row.maxValue - row.minValue));
    const float t = std::clamp(
        (static_cast<float>(row.value - row.minValue) / range), 0.0f, 1.0f);
    const float knobX = trackLeft + (trackRight - trackLeft) * t;
    const D2D1_RECT_F track =
        D2D1::RectF(trackLeft, centerY - trackH * 0.5f, trackRight,
                    centerY + trackH * 0.5f);
    const D2D1_RECT_F filled =
        D2D1::RectF(track.left, track.top, knobX, track.bottom);
    painter.fillRoundRect(row.enabled ? p.controlFill : p.listHover, track, trackH * 0.5f);
    if (knobX > track.left)
        painter.fillRoundRect(row.enabled ? p.accent : p.disabled, filled, trackH * 0.5f);
    if (auto* br = painter.brush(row.enabled
                                     ? (ctx.hoverId == row.id ? p.accentHover : p.accent)
                                     : p.disabled)) {
        painter.target()->FillEllipse(D2D1::Ellipse(D2D1::Point2F(knobX, centerY), 7.0f, 7.0f),
                                      br);
    }
    const auto& editorState = ctx.sliderEditor.state();
    const bool editing = editorState.open && editorState.rowId == row.id;
    const auto valueRect = sliderValueRect(row);
    const auto numberRect = sliderNumberRect(row);
    painter.fillRoundRect(row.enabled ? p.controlFill : p.listHover, valueRect);
    painter.strokeRoundRect(editing ? p.accent : p.controlStroke, valueRect);
    auto* numberFormat = painter.textFormat(fluent::font_sizes::caption, 600, true, true);
    const std::wstring number =
        editing ? editorState.buffer.text : std::to_wstring(row.value);
    painter.target()->PushAxisAlignedClip(numberRect, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    if (editing && numberFormat) {
        const float textWidth = painter.measureTextWidth(number, numberFormat);
        const float textLeft = (numberRect.left + numberRect.right - textWidth) * 0.5f;
        for (size_t i = 0; i <= number.size(); ++i)
            ctx.sliderEditor.setCaretStop(
                i, std::clamp(textLeft +
                                  painter.measureTextWidth(number.substr(0, i), numberFormat),
                              numberRect.left + 1.0f, numberRect.right - 1.0f));
        const size_t first = editorState.buffer.selectionStart();
        const size_t last = editorState.buffer.selectionEnd();
        if (first != last)
            painter.fillRoundRect(
                p.listSelected,
                D2D1::RectF(editorState.caretStops[first], numberRect.top + 4.0f,
                            editorState.caretStops[last], numberRect.bottom - 4.0f),
                1.0f);
    }
    painter.drawText(number, numberFormat, numberRect, row.enabled ? p.text : p.disabled);
    if (editing && editorState.caretVisible && !editorState.buffer.hasSelection()) {
        if (auto* brush = painter.brush(p.accent))
            painter.target()->FillRectangle(
                D2D1::RectF(editorState.caretStops[editorState.buffer.caret],
                            numberRect.top + 5.0f,
                            editorState.caretStops[editorState.buffer.caret] + 1.0f,
                            numberRect.bottom - 5.0f),
                brush);
    }
    painter.target()->PopAxisAlignedClip();
    painter.drawText(row.valueSuffix, painter.textFormat(fluent::font_sizes::caption, 600, true, true),
                     D2D1::RectF(numberRect.right, numberRect.top,
                                 valueRect.right - 4.0f, numberRect.bottom),
                     row.enabled ? p.text : p.disabled);
    if (ctx.focusedId == row.id && ctx.focusVisible && row.enabled)
        painter.strokeRoundRect(p.accent,
                                D2D1::RectF(track.left - 3.0f, track.top - 5.0f,
                                            track.right + 3.0f, track.bottom + 5.0f),
                                1.0f, 5.0f);
}

void drawRadio(fluent::FluentDialogSurface::Painter& painter, const RowPaintContext& ctx,
               Row& row) {
    if (row.id == kIdSpectrumStyle) {
        drawSpectrumStyleRadio(painter, ctx, row);
        return;
    }
    if (row.id == kIdCoverEffect) {
        drawCoverEffectRadio(painter, ctx, row);
        return;
    }
    if (row.id == kIdHoverControlStyle) {
        drawHoverControlStyleRadio(painter, ctx, row);
        return;
    }
    if (row.id == kIdSongToastPosition) {
        drawSongToastPositionRadio(painter, ctx, row);
        return;
    }
    if (row.id == kIdIdleQuoteBackground) {
        drawIdleQuoteBackgroundRadio(painter, ctx, row);
        return;
    }

    const auto& p = painter.colors();
    auto* format = painter.textFormat(fluent::font_sizes::label, 400, true, true);
    if (!format)
        return;
    const float width = row.controlRect.right - row.controlRect.left;
    const float optionWidth =
        width / static_cast<float>(std::max<size_t>(1, row.options.size()));
    painter.fillRoundRect(p.listHover, row.controlRect);
    row.optionRects.clear();
    for (size_t i = 0; i < row.options.size(); ++i) {
        const bool optionEnabled = row.enabled && isOptionEnabled(row, i);
        const bool selected = static_cast<int>(i) == row.selected;
        const bool hovered = optionEnabled && ctx.hoverId == row.id &&
                             ctx.hoverOption == static_cast<int>(i);
        const bool pressed = optionEnabled && ctx.pressedId == row.id &&
                             ctx.pressedOption == static_cast<int>(i);
        const float left = row.controlRect.left + static_cast<float>(i) * optionWidth;
        const D2D1_RECT_F optionRect = D2D1::RectF(
            left, row.controlRect.top, left + optionWidth, row.controlRect.bottom);
        const D2D1_RECT_F face = D2D1::RectF(
            optionRect.left + 2.0f, optionRect.top + 2.0f,
            optionRect.right - 2.0f, optionRect.bottom - 2.0f);
        if (selected) {
            painter.fillRoundRect(
                optionEnabled ? (pressed ? p.accentPressed : hovered ? p.accentHover : p.accent)
                              : p.listSelected,
                face);
        } else if (hovered || pressed) {
            painter.fillRoundRect(pressed ? p.controlPressed : p.controlHover, face);
        }
        if (selected && optionEnabled && ctx.focusedId == row.id && ctx.focusVisible)
            painter.strokeRoundRect(
                p.textOnAccent,
                D2D1::RectF(face.left + 1.5f, face.top + 1.5f,
                            face.right - 1.5f, face.bottom - 1.5f),
                1.0f, fluent::metrics::controlRadius - 1.0f);
        painter.drawText(row.options[i], format,
                         D2D1::RectF(face.left + 4.0f, face.top,
                                     face.right - 4.0f, face.bottom),
                         optionEnabled ? (selected ? p.textOnAccent : p.text) : p.disabled);
        row.optionRects.push_back(optionRect);
    }
}

void drawModeGrid(fluent::FluentDialogSurface::Painter& painter, const RowPaintContext& ctx,
                  Row& row) {
    const auto& p = painter.colors();
    auto* titleFormat = painter.textFormat(fluent::font_sizes::label, 600, false, true);
    auto* hintFormat = painter.textFormat(fluent::font_sizes::smallText, 400);
    if (!titleFormat || !hintFormat)
        return;

    const float gridW = row.controlRect.right - row.controlRect.left;
    const float cardW = (gridW - kModeGridGap) * 0.5f;
    row.optionRects.clear();
    const size_t count = std::min<size_t>(row.options.size(), 4);
    for (size_t i = 0; i < count; ++i) {
        const int column = static_cast<int>(i % 2);
        const int line = static_cast<int>(i / 2);
        const float left = row.controlRect.left + column * (cardW + kModeGridGap);
        const float top = row.controlRect.top + line * (kModeGridCardH + kModeGridGap);
        const D2D1_RECT_F card =
            D2D1::RectF(left, top, left + cardW, top + kModeGridCardH);
        row.optionRects.push_back(card);

        const bool selected = static_cast<int>(i) == row.selected;
        const bool hovered = row.enabled && ctx.hoverId == row.id &&
                             ctx.hoverOption == static_cast<int>(i);
        const bool pressed = row.enabled && ctx.pressedId == row.id &&
                             ctx.pressedOption == static_cast<int>(i);
        D2D1_COLOR_F fill = p.controlFill;
        if (!row.enabled)
            fill = p.listHover;
        else if (pressed)
            fill = p.controlPressed;
        else if (selected)
            fill = p.listSelected;
        else if (hovered)
            fill = p.controlHover;
        painter.fillRoundRect(fill, card, fluent::metrics::controlRadius);

        if (selected) {
            painter.strokeRoundRect(hovered ? p.accentHover : p.accent, card,
                                    ctx.focusedId == row.id && ctx.focusVisible ? 2.0f : 1.5f,
                                    fluent::metrics::controlRadius);
            if (auto* br = painter.brush(row.enabled ? p.accent : p.disabled)) {
                painter.target()->FillEllipse(
                    D2D1::Ellipse(D2D1::Point2F(card.right - 14.0f, card.top + 14.0f),
                                  3.0f, 3.0f),
                    br);
            }
        } else {
            painter.strokeRoundRect(p.cardStroke, card, 1.0f,
                                    fluent::metrics::controlRadius);
        }

        const D2D1_COLOR_F titleColor = row.enabled ? p.text : p.disabled;
        painter.drawText(row.options[i], titleFormat,
                         D2D1::RectF(card.left + 12.0f, card.top + 8.0f,
                                     card.right - 12.0f, card.top + 28.0f),
                         titleColor);
        if (i < row.optionHints.size() && !row.optionHints[i].empty()) {
            painter.drawText(row.optionHints[i], hintFormat,
                             D2D1::RectF(card.left + 12.0f, card.top + 29.0f,
                                         card.right - 12.0f, card.bottom - 8.0f),
                             row.enabled ? p.textSecondary : p.disabled);
        }
    }
}

void drawAppList(fluent::FluentDialogSurface::Painter& painter, const RowPaintContext& ctx,
                 const Row& row) {
    const auto& p = painter.colors();
    const float innerX = row.cardRect.left + 16.0f;
    const float innerRight = row.cardRect.right - 16.0f;
    const float listTop = row.cardRect.top + kIdleAppsListTop;
    auto* nameFormat = painter.textFormat(fluent::font_sizes::label, 600, false, true);
    auto* pathFormat = painter.textFormat(fluent::font_sizes::smallText, 400, false, true);
    auto* actionFormat = painter.textFormat(fluent::font_sizes::smallText, 600, true, true);
    if (!nameFormat || !pathFormat || !actionFormat)
        return;

    auto drawAction = [&](const D2D1_RECT_F& rect, const std::wstring& text, bool enabled,
                          bool hovered, bool pressed) {
        const D2D1_COLOR_F fill = !enabled ? p.listHover
                                           : pressed ? p.controlPressed
                                                     : hovered ? p.controlHover
                                                               : p.controlFill;
        painter.fillRoundRect(fill, rect, 6.0f);
        painter.strokeRoundRect(enabled ? p.cardStroke : p.disabled, rect, 1.0f, 6.0f);
        painter.drawText(text, actionFormat, rect, enabled ? p.text : p.disabled);
    };

    if (row.id == kIdGenericSmtcApps) {
        const size_t count = ctx.genericSmtcRules.size();
        for (size_t i = 0; i < count && i < row.appItemRects.size() && i < row.appDeleteRects.size();
             ++i) {
            const auto& rule = ctx.genericSmtcRules[i];
            const D2D1_RECT_F item = row.appItemRects[i];
            const int option = ctx.hoverId == row.id ? ctx.hoverOption : -1;
            const int pressedOptionValue = ctx.pressedId == row.id ? ctx.pressedOption : -1;
            const bool hovered = row.enabled && option == static_cast<int>(i);
            const bool pressed = row.enabled && pressedOptionValue == static_cast<int>(i);
            painter.fillRoundRect(row.enabled ? (hovered ? p.controlHover : p.controlFill)
                                              : p.listHover,
                                  item, 7.0f);
            if (pressed)
                painter.fillRoundRect(p.controlPressed, item, 7.0f);

            const D2D1_RECT_F iconRect =
                D2D1::RectF(item.left + 8.0f, item.top + 8.0f, item.left + 36.0f,
                            item.top + 36.0f);
            bool drewIcon = false;
            if (rule.iconPixels && !rule.iconPixels->empty() && rule.iconWidth > 0 &&
                rule.iconHeight > 0 && painter.target()) {
                Microsoft::WRL::ComPtr<ID2D1Bitmap> bitmap;
                const auto props = D2D1::BitmapProperties(
                    D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,
                                      D2D1_ALPHA_MODE_PREMULTIPLIED),
                    static_cast<float>(ctx.dpi), static_cast<float>(ctx.dpi));
                if (SUCCEEDED(painter.target()->CreateBitmap(
                        D2D1::SizeU(rule.iconWidth, rule.iconHeight), rule.iconPixels->data(),
                        rule.iconWidth * 4, &props, &bitmap)) &&
                    bitmap) {
                    painter.target()->DrawBitmap(bitmap.Get(), iconRect,
                                                 row.enabled ? 1.0f : 0.45f,
                                                 D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
                    drewIcon = true;
                }
            }
            if (!drewIcon && painter.target()) {
                painter.fillRoundRect(p.listHover, iconRect, 6.0f);
                painter.strokeRoundRect(p.cardStroke, iconRect, 0.75f, 6.0f);
                Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> noteBrush;
                if (SUCCEEDED(painter.target()->CreateSolidColorBrush(
                        row.enabled ? p.textSecondary : p.disabled, &noteBrush)) &&
                    noteBrush) {
                    const D2D1_POINT_2F center{(iconRect.left + iconRect.right) * 0.5f,
                                               (iconRect.top + iconRect.bottom) * 0.5f};
                    media_control::drawMusicNote(painter.target(), center, 14.0f, noteBrush.Get());
                }
            }

            const float textRight = item.right - 82.0f;
            const D2D1_RECT_F nameRect =
                D2D1::RectF(item.left + 46.0f, item.top + 3.0f, textRight, item.top + 23.0f);
            const D2D1_RECT_F subRect =
                D2D1::RectF(item.left + 46.0f, item.top + 22.0f, textRight, item.bottom - 3.0f);
            const std::wstring title = !rule.displayName.empty() ? rule.displayName
                                       : !rule.processName.empty() ? rule.processName
                                       : !rule.sourceId.empty()    ? rule.sourceId
                                                                   : L"未知应用";
            const std::wstring subtitle = !rule.processName.empty() ? rule.processName
                                          : !rule.sourceId.empty()  ? rule.sourceId
                                                                    : rule.packageFamilyName;
            painter.drawTrimmedText(title, nameFormat, nameRect,
                                    row.enabled ? p.text : p.disabled);
            painter.drawTrimmedText(subtitle, pathFormat, subRect,
                                    row.enabled ? p.textSecondary : p.disabled);

            const D2D1_RECT_F deleteRect = row.appDeleteRects[i];
            drawAction(deleteRect, L"移除", row.enabled, hovered, pressed);
        }

        if (count == 0) {
            const wchar_t* emptyText =
                row.enabled
                    ? L"暂未添加应用，可从已检测到的 SMTC 应用或本地 EXE 添加"
                    : L"开启通用识别并选择“仅白名单”或“排除黑名单”后可管理应用";
            painter.drawText(emptyText, pathFormat,
                             D2D1::RectF(innerX + 8.0f, listTop + 10.0f, innerRight - 8.0f,
                                         listTop + 34.0f),
                             row.enabled ? p.textSecondary : p.disabled);
        }

        const bool canAdd = row.enabled && count < kMaxGenericSmtcRules;
        const bool addHovered = canAdd && ctx.hoverId == row.id && ctx.hoverOption == -1;
        const bool addPressed = canAdd && ctx.pressedId == row.id && ctx.pressedOption == -1;
        painter.fillRoundRect(
            canAdd ? (addPressed ? p.accentPressed : addHovered ? p.accentHover : p.accent)
                   : p.listHover,
            row.addAppRect, 7.0f);
        painter.strokeRoundRect(canAdd ? p.accent : p.disabled, row.addAppRect, 1.0f, 7.0f);
        painter.drawText(L"添加应用…", painter.textFormat(fluent::font_sizes::label, 600, true, true),
                         row.addAppRect, canAdd ? p.textOnAccent : p.disabled);
        return;
    }

    const size_t count = ctx.idleApps.size();

    const D2D1_RECT_F toggleLabelRect =
        D2D1::RectF(row.controlRect.left - kIdleAppNamesToggleGap -
                        kIdleAppNamesToggleLabelW,
                    row.controlRect.top, row.controlRect.left - kIdleAppNamesToggleGap,
                    row.controlRect.bottom);
    painter.drawText(L"显示名称", painter.textFormat(fluent::font_sizes::caption, 400, true, true),
                     toggleLabelRect, row.enabled ? p.textSecondary : p.disabled);
    drawToggle(painter, row,
               row.enabled && ctx.hoverId == row.id && ctx.hoverOption == kIdleAppNamesOption,
               ctx.focusedId == kIdIdleAppNames && ctx.focusVisible);

    for (size_t i = 0; i < count; ++i) {
        const auto& app = ctx.idleApps[i];
        const D2D1_RECT_F item = row.appItemRects[i];
        const int option = ctx.hoverId == row.id ? ctx.hoverOption : -1;
        const int pressedOptionValue = ctx.pressedId == row.id ? ctx.pressedOption : -1;
        const bool appHovered = option == static_cast<int>(i) ||
                                option == kIdleAppEditOptionBase + static_cast<int>(i) ||
                                option == kIdleAppDragOptionBase + static_cast<int>(i);
        const bool appPressed =
            pressedOptionValue == static_cast<int>(i) ||
            pressedOptionValue == kIdleAppEditOptionBase + static_cast<int>(i) ||
            pressedOptionValue == kIdleAppDragOptionBase + static_cast<int>(i);
        const bool hovered = row.enabled && ctx.hoverId == row.id && appHovered;
        const bool pressed = row.enabled && ctx.pressedId == row.id && appPressed;
        const bool dragging = ctx.idleAppDragging && ctx.idleAppDragIndex == static_cast<int>(i);
        painter.fillRoundRect(row.enabled ? (hovered ? p.controlHover : p.controlFill)
                                          : p.listHover,
                              item, 7.0f);
        if (pressed)
            painter.fillRoundRect(p.controlPressed, item, 7.0f);
        if (dragging)
            painter.strokeRoundRect(p.accent, item, 1.5f, 7.0f);

        const D2D1_RECT_F iconRect =
            D2D1::RectF(item.left + 8.0f, item.top + 8.0f, item.left + 36.0f,
                        item.top + 36.0f);
        if (app.iconPixels && !app.iconPixels->empty() && app.iconWidth > 0 &&
            app.iconHeight > 0 && painter.target()) {
            Microsoft::WRL::ComPtr<ID2D1Bitmap> bitmap;
            const auto props = D2D1::BitmapProperties(
                D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,
                                  D2D1_ALPHA_MODE_PREMULTIPLIED),
                static_cast<float>(ctx.dpi), static_cast<float>(ctx.dpi));
            if (SUCCEEDED(painter.target()->CreateBitmap(
                    D2D1::SizeU(app.iconWidth, app.iconHeight), app.iconPixels->data(),
                    app.iconWidth * 4, &props, &bitmap)) &&
                bitmap) {
                painter.target()->DrawBitmap(bitmap.Get(), iconRect,
                                             row.enabled ? 1.0f : 0.45f,
                                             D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
            }
        }

        const D2D1_RECT_F nameRect =
            D2D1::RectF(item.left + 46.0f, item.top + 3.0f,
                        item.right - kIdleAppInfoRightInset, item.top + 23.0f);
        const D2D1_RECT_F pathRect =
            D2D1::RectF(item.left + 46.0f, item.top + 22.0f,
                        item.right - kIdleAppInfoRightInset, item.bottom - 3.0f);
        painter.drawTrimmedText(app.name.empty() ? L"未命名应用" : app.name, nameFormat,
                                nameRect, row.enabled && app.pathValid ? p.text : p.disabled);
        painter.drawTrimmedText(app.pathValid ? app.path : L"路径失效，请重新选择",
                                pathFormat, pathRect,
                                row.enabled && app.pathValid ? p.textSecondary : p.disabled);

        const D2D1_RECT_F editRect = row.appEditRects[i];
        const D2D1_RECT_F deleteRect = row.appDeleteRects[i];
        drawAction(editRect, L"修改", row.enabled,
                   row.enabled && option == kIdleAppEditOptionBase + static_cast<int>(i),
                   row.enabled && pressedOptionValue == kIdleAppEditOptionBase +
                                                  static_cast<int>(i));
        drawAction(deleteRect, L"删除", row.enabled,
                   row.enabled && option == static_cast<int>(i),
                   row.enabled && pressedOptionValue == static_cast<int>(i));
    }

    if (ctx.idleAppDragging && ctx.idleAppDropIndex >= 0) {
        const float dropY = listTop + static_cast<float>(ctx.idleAppDropIndex) * kIdleAppItemH - 4.0f;
        painter.fillRoundRect(
            p.accent,
            D2D1::RectF(innerX + 4.0f, dropY - 1.0f, innerRight - 4.0f, dropY + 1.0f),
            1.0f);
    }

    if (count == 0)
        painter.drawText(L"未添加应用", pathFormat,
                         D2D1::RectF(innerX + 8.0f, listTop + 10.0f, innerRight - 8.0f,
                                     listTop + 34.0f),
                         row.enabled ? p.textSecondary : p.disabled);

    const bool canAdd = row.enabled && count < kMaxIdleApps;
    const bool addHovered = canAdd && ctx.hoverId == row.id && ctx.hoverOption == -1;
    const bool addPressed = canAdd && ctx.pressedId == row.id && ctx.pressedOption == -1;
    painter.fillRoundRect(
        canAdd ? (addPressed ? p.accentPressed : addHovered ? p.accentHover : p.accent)
               : p.listHover,
        row.addAppRect, 7.0f);
    painter.strokeRoundRect(canAdd ? p.accent : p.disabled, row.addAppRect, 1.0f, 7.0f);
    painter.drawText(L"添加应用", painter.textFormat(fluent::font_sizes::label, 600, true, true),
                     row.addAppRect, canAdd ? p.textOnAccent : p.disabled);
}

} // namespace settings_ui::row_painters
