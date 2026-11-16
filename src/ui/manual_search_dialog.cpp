#include "ui/manual_search_dialog.h"
#include "ui/font_sizes.h"

#include "ui/app_icon.h"
#include "ui/dialog_notify.h"
#include "ui/fluent_dialog_surface.h"
#include "ui/fluent_theme.h"
#include "ui/manual_search_preview.h"
#include "ui/text_edit_buffer.h"
#include "logging/runtime_logger.h"

#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace {

constexpr int kIdTitleEdit = 101;
constexpr int kIdArtistEdit = 102;
constexpr int kIdSearchButton = 103;
constexpr int kIdCandidateList = 104;
constexpr int kIdApplyButton = 105;
constexpr int kIdCancelButton = 106;
constexpr int kIdPreview = 107;
constexpr int kIdPreviewRomanization = 108;
constexpr int kIdPreviewTranslation = 109;
constexpr int kIdAdvanceButton = 110;
constexpr int kIdDelayButton = 111;

constexpr DWORD kDialogStyle = WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_THICKFRAME;
constexpr DWORD kDialogExStyle = WS_EX_DLGMODALFRAME | WS_EX_WINDOWEDGE;

constexpr float kMinClientWidthDip = 520.0f;
constexpr float kMinClientHeightDip = 504.0f;
constexpr float kMinClientAspectRatio = kMinClientWidthDip / kMinClientHeightDip;

constexpr float kCandidateHeaderHeight = 28.0f;
constexpr float kCandidateRowHeight = 48.0f;
constexpr float kScrollBarWidth = 3.0f;
constexpr float kScrollBarHitWidth = 12.0f;
constexpr float kScrollBarInset = 8.0f;

constexpr fluent::TextEditOptions kSearchFieldEditOptions{
    .digitsOnly = false,
    .printableOnly = true,
    .maxLength = 0,
    .allowUndo = false,
};

constexpr UINT kMsgCandidatesReady = WM_APP + 10;
constexpr UINT kMsgPreviewLyricReady = WM_APP + 11;

void releaseResultPayload(const MSG& msg) {
    if (msg.message == kMsgCandidatesReady) {
        delete reinterpret_cast<std::vector<SearchCandidate>*>(msg.lParam);
    } else if (msg.message == kMsgPreviewLyricReady) {
        delete reinterpret_cast<std::tuple<int, bool, std::vector<LyricLine>, SongInfo>*>(
            msg.lParam);
    }
}

void discardPendingResults(HWND hwnd) {
    if (!hwnd)
        return;
    MSG msg{};
    while (PeekMessageW(&msg, hwnd, kMsgCandidatesReady, kMsgPreviewLyricReady, PM_REMOVE))
        releaseResultPayload(msg);
}

struct ScrollBarGeometry {
    float trackTop = 0.0f;
    float thumbHeight = 0.0f;
    float usable = 0.0f;
};

ScrollBarGeometry scrollBarGeometry(float viewHeight, float contentHeight) {
    const float trackHeight = std::max(0.0f, viewHeight - kScrollBarInset * 2.0f);
    const float thumbHeight = std::min(
        trackHeight, std::max(20.0f, trackHeight * viewHeight / std::max(1.0f, contentHeight)));
    return {kScrollBarInset, thumbHeight, std::max(0.0f, trackHeight - thumbHeight)};
}

float scrollBarThumbY(const ScrollBarGeometry& bar, float scrollY, float maxScroll) {
    if (maxScroll <= 0.0f || bar.usable <= 0.0f)
        return bar.trackTop;
    return bar.trackTop + scrollY / maxScroll * bar.usable;
}

} // namespace

struct ManualSearchDialog::Impl {
    struct TextField {
        fluent::TextEditBuffer buffer;
        std::wstring cue;
        float scrollX = 0.0f;
    };

    struct CandidateItem {
        std::wstring text;
        int candidateIndex = -1; // -1 表示分组标题
        bool header = false;
    };

    HWND hwnd = nullptr;
    HWND notifyHwnd = nullptr; // 关闭时向托盘窗口投递 kMsgDialogClosed
    bool backdrop = false;

    fluent::FluentDialogSurface surface;
    fluent::CompositedScrollView candidateScrollView;
    bool candidateContentDirty = true;

    TextField titleEdit;
    TextField artistEdit;
    bool searching = false;
    UINT_PTR searchGeneration = 0;
    UINT_PTR previewGeneration = 0;

    LyricProvider* provider = nullptr;
    std::wstring targetTitle;
    std::wstring targetArtist;
    int64_t targetDurationMs = 0;
    std::wstring targetNeteaseSongId;
    SmtcPlayerType targetPlayer = SmtcPlayerType::Unknown;
    std::wstring targetAlbum;

    bool targetIsSoda() const { return targetPlayer == SmtcPlayerType::Soda; }
    bool targetIsNetease() const { return targetPlayer == SmtcPlayerType::NetEase; }

    std::vector<SearchCandidate> candidates;
    std::vector<CandidateItem> candidateItems;
    int selectedItemRow = -1;
    int selectedIdx = -1;
    int hoverItemRow = -1;

    ManualSearchPreview preview;
    int64_t playbackPositionMs = 0;

    float candidateScroll = 0.0f;
    int candidateWheelAccum = 0;
    bool candidateScrollDragging = false;
    float candidateScrollGrabDy = 0.0f;

    D2D1_RECT_F titleEditRect{};
    D2D1_RECT_F artistEditRect{};
    D2D1_RECT_F searchRect{};
    D2D1_RECT_F workspaceRect{};
    D2D1_RECT_F candidateListRect{};
    D2D1_RECT_F previewRect{};
    D2D1_RECT_F previewRomanizationRect{};
    D2D1_RECT_F previewTranslationRect{};
    D2D1_RECT_F advanceRect{};
    D2D1_RECT_F delayRect{};
    D2D1_RECT_F applyRect{};
    D2D1_RECT_F cancelRect{};

    int hoverId = 0;
    int pressedId = 0;
    int focusedId = kIdTitleEdit;
    bool focusVisible = false;

    ManualSearchDialog::ApplyCallback onApply;

    ~Impl() = default;

    static LRESULT CALLBACK wndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
        Impl* self = nullptr;
        if (msg == WM_NCCREATE) {
            self = static_cast<Impl*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
            self->hwnd = h;
            SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        } else {
            self = reinterpret_cast<Impl*>(GetWindowLongPtrW(h, GWLP_USERDATA));
        }
        if (self)
            return self->handle(msg, wp, lp);
        return DefWindowProcW(h, msg, wp, lp);
    }

    static bool contains(const D2D1_RECT_F& rect, float x, float y) {
        return x >= rect.left && x < rect.right && y >= rect.top && y < rect.bottom;
    }

    static bool isCtrlDown() {
        return (GetKeyState(VK_CONTROL) & 0x8000) != 0;
    }

    static bool isShiftDown() {
        return (GetKeyState(VK_SHIFT) & 0x8000) != 0;
    }

    TextField* editFor(int id) {
        if (id == kIdTitleEdit)
            return &titleEdit;
        if (id == kIdArtistEdit)
            return &artistEdit;
        return nullptr;
    }

    const TextField* editFor(int id) const {
        if (id == kIdTitleEdit)
            return &titleEdit;
        if (id == kIdArtistEdit)
            return &artistEdit;
        return nullptr;
    }

    void editChanged() {
        if (hwnd)
            surface.invalidate();
    }

    bool handleEditKey(WPARAM key) {
        TextField* field = editFor(focusedId);
        if (!field)
            return false;

        if (!field->buffer.handleKey(hwnd, key, isCtrlDown(), isShiftDown(),
                                     kSearchFieldEditOptions))
            return false;
        editChanged();
        return true;
    }

    bool handleCharacter(wchar_t character) {
        TextField* field = editFor(focusedId);
        if (!field || character < L' ' || character == 0x7f)
            return false;
        field->buffer.replaceSelection(std::wstring(1, character), kSearchFieldEditOptions);
        editChanged();
        return true;
    }

    void invalidatePreview() {
        const float scale = surface.dipScale();
        const RECT dirty{static_cast<LONG>(std::floor(previewRect.left * scale)),
                         static_cast<LONG>(std::floor(previewRect.top * scale)),
                         static_cast<LONG>(std::ceil(previewRect.right * scale)),
                         static_cast<LONG>(std::ceil(previewRect.bottom * scale))};
        surface.invalidate(&dirty);
    }

    void setPreviewLines(const std::vector<LyricLine>& lines) {
        preview.setLines(lines, playbackPositionMs);
        layout();
        surface.invalidate();
    }

    void createControls() {
        titleEdit.cue = L"歌名";
        titleEdit.buffer.reset(targetTitle, false);
        artistEdit.cue = L"歌手";
        artistEdit.buffer.reset(targetArtist, false);

        preview.setState(ManualSearchPreviewState::Empty, hwnd);
        searching = false;
        focusedId = kIdTitleEdit;
        focusVisible = false;
        candidateScroll = 0.0f;
        candidateScrollView.setOffset(0.0f);
        candidateWheelAccum = 0;
        candidateScrollDragging = false;
        preview.resetView(playbackPositionMs);
    }

    void layout() {
        RECT rc{};
        GetClientRect(hwnd, &rc);
        const float scale = surface.dipScale();
        const float clientWidth = std::max(0.0f, static_cast<float>(rc.right - rc.left) / scale);
        const auto content = fluent::centeredDialogLayout(clientWidth);
        const float w = content.width;
        const float h = std::max(0.0f, static_cast<float>(rc.bottom - rc.top) / scale);
        const float pad = fluent::metrics::pagePadding;
        const float gap = fluent::metrics::controlGap;
        const float controlH = fluent::metrics::controlHeight;

        const float inputY = pad + 22.0f;
        const float searchW = 104.0f;
        const float fieldsW = std::max(80.0f, w - 2.0f * pad - 2.0f * gap - searchW);
        const float titleW = fieldsW * 0.68f;
        titleEditRect = D2D1::RectF(pad, inputY, pad + titleW, inputY + controlH);
        artistEditRect = D2D1::RectF(pad + titleW + gap, inputY,
                                     pad + fieldsW + gap, inputY + controlH);
        searchRect = D2D1::RectF(w - pad - searchW, inputY, w - pad, inputY + controlH);

        const float contentTop = inputY + controlH + fluent::metrics::sectionGap;
        const float buttonH = controlH;
        const float buttonY = std::max(contentTop, h - pad - buttonH);
        const float contentBottom = std::max(contentTop, buttonY - fluent::metrics::sectionGap);
        const float workspaceW = std::max(0.0f, w - 2.0f * pad);
        workspaceRect = D2D1::RectF(pad, contentTop, pad + workspaceW, contentBottom);
        candidateListRect = workspaceRect;
        previewRect = {};
        advanceRect = {};
        delayRect = {};
        if (!candidates.empty()) {
            const float listW = std::min(workspaceW, std::clamp(workspaceW * 0.36f, 168.0f, 360.0f));
            candidateListRect.right = pad + listW;
            const float previewLeft = candidateListRect.right + 1.0f;
            const float toolsY = std::max(contentTop, contentBottom - gap - controlH);
            previewRect = D2D1::RectF(previewLeft, contentTop, workspaceRect.right,
                                      std::max(contentTop, toolsY - gap));
            const float timingW = 100.0f;
            const float toolsLeft = previewLeft + 18.0f;
            advanceRect = D2D1::RectF(toolsLeft, toolsY, toolsLeft + timingW, toolsY + controlH);
            delayRect = D2D1::RectF(advanceRect.right + gap, toolsY,
                                    advanceRect.right + gap + timingW, toolsY + controlH);
        } else {
            candidateScrollView.layout({}, 0.0f, surface.dpi());
            preview.hideScroll(surface.dpi());
        }
        preview.layoutToggleButtons(previewRect, previewRomanizationRect, previewTranslationRect);

        const float cancelW = 88.0f;
        const float applyW = 120.0f;
        cancelRect = D2D1::RectF(w - pad - cancelW, buttonY, w - pad, buttonY + buttonH);
        applyRect = D2D1::RectF(w - pad - cancelW - gap - applyW, buttonY,
                                w - pad - cancelW - gap, buttonY + buttonH);
        fluent::offsetDialogRects(content.offsetX, {
            &titleEditRect, &artistEditRect, &searchRect, &workspaceRect,
            &candidateListRect, &previewRect, &previewRomanizationRect, &previewTranslationRect,
            &advanceRect, &delayRect, &applyRect, &cancelRect});
        candidateScroll = std::clamp(candidateScroll, 0.0f, candidateMaxScroll());
    }

    float candidateItemHeight(int row) const {
        if (row < 0 || row >= static_cast<int>(candidateItems.size()))
            return 0.0f;
        return candidateItems[row].header ? kCandidateHeaderHeight : kCandidateRowHeight;
    }

    float candidateContentHeight() const {
        float height = 0.0f;
        for (int i = 0; i < static_cast<int>(candidateItems.size()); ++i)
            height += candidateItemHeight(i);
        return height;
    }

    float candidateMaxScroll() const {
        return std::max(0.0f, candidateContentHeight() -
                                  (candidateListRect.bottom - candidateListRect.top));
    }

    int candidateRowAt(float y) const {
        if (y < candidateListRect.top || y > candidateListRect.bottom)
            return -1;
        float localY = y - candidateListRect.top + candidateScroll;
        if (localY < 0.0f)
            return -1;
        for (int row = 0; row < static_cast<int>(candidateItems.size()); ++row) {
            const float rowH = candidateItemHeight(row);
            if (localY < rowH)
                return row;
            localY -= rowH;
        }
        return -1;
    }

    bool candidateHasScroll() const {
        return candidateContentHeight() > candidateListRect.bottom - candidateListRect.top;
    }

    bool isCandidateScrollBarHit(float x) const {
        return candidateHasScroll() && x >= candidateListRect.right - kScrollBarHitWidth;
    }

    void scrollCandidatesBy(int delta) {
        candidateScrollView.scrollBy(-static_cast<float>(delta) / WHEEL_DELTA *
                                     3.0f * kCandidateRowHeight);
    }

    void ensureCandidateVisible(int row) {
        if (row < 0 || row >= static_cast<int>(candidateItems.size()))
            return;
        float top = 0.0f;
        for (int i = 0; i < row; ++i)
            top += candidateItemHeight(i);
        const float bottom = top + candidateItemHeight(row);
        const float viewHeight = candidateListRect.bottom - candidateListRect.top;
        if (top < candidateScroll)
            candidateScroll = top;
        else if (bottom > candidateScroll + viewHeight)
            candidateScroll = bottom - viewHeight;
        candidateScroll = std::clamp(candidateScroll, 0.0f, candidateMaxScroll());
    }

    int nextCandidateRow(int from, int direction) const {
        int row = from;
        while (true) {
            row += direction;
            if (row < 0 || row >= static_cast<int>(candidateItems.size()))
                return -1;
            if (!candidateItems[row].header && candidateItems[row].candidateIndex >= 0)
                return row;
        }
    }

    bool beginCandidateScrollDrag(float x, float y) {
        if (!isCandidateScrollBarHit(x))
            return false;
        const float viewHeight = candidateListRect.bottom - candidateListRect.top;
        const auto bar = scrollBarGeometry(viewHeight, candidateContentHeight());
        const float thumbY = candidateListRect.top +
                             scrollBarThumbY(bar, candidateScroll, candidateMaxScroll());
        if (y >= thumbY && y <= thumbY + bar.thumbHeight) {
            candidateScrollDragging = true;
            candidateScrollGrabDy = y - thumbY;
            SetCapture(hwnd);
        } else {
            candidateScroll = std::clamp(
                candidateScroll + (y < thumbY ? -viewHeight : viewHeight), 0.0f,
                candidateMaxScroll());
            surface.invalidate();
            SetCapture(hwnd);
        }
        return true;
    }

    void updateCandidateScrollDrag(float y) {
        if (!candidateScrollDragging)
            return;
        const float viewHeight = candidateListRect.bottom - candidateListRect.top;
        const auto bar = scrollBarGeometry(viewHeight, candidateContentHeight());
        if (bar.usable > 0.0f) {
            const float localY = y - candidateListRect.top;
            candidateScroll = std::clamp(
                (localY - bar.trackTop - candidateScrollGrabDy) / bar.usable *
                    candidateMaxScroll(),
                0.0f, candidateMaxScroll());
        }
        surface.invalidate();
    }

    void drawTextField(fluent::FluentDialogSurface::Painter& painter,
                       const D2D1_RECT_F& rect, TextField& field, int id) {
        const auto& p = fluent::palette();
        const bool focused = focusedId == id && GetFocus() == hwnd;
        painter.drawText(field.cue, painter.textFormat(fluent::font_sizes::caption, 600, false, true),
                         D2D1::RectF(rect.left, rect.top - 23.0f, rect.right, rect.top - 4.0f),
                         p.accent);
        painter.fillRoundRect(p.cardFill, rect);
        painter.strokeRoundRect(focused ? p.accent : p.controlStroke, rect);

        auto* format = painter.textFormat(fluent::font_sizes::body, 400, false, true);
        if (!format)
            return;
        const D2D1_RECT_F textRect =
            D2D1::RectF(rect.left + 12.0f, rect.top, rect.right - 12.0f, rect.bottom);
        if (!focused) {
            field.scrollX = 0.0f;
            painter.drawTrimmedText(field.buffer.text, format, textRect, p.text);
            return;
        }

        const float width = std::max(1.0f, textRect.right - textRect.left - 1.0f);
        auto* layout = painter.textLayout(field.buffer.text, format, width,
                                          textRect.bottom - textRect.top);
        if (!layout)
            return;
        DWRITE_TEXT_METRICS metrics{};
        layout->GetMetrics(&metrics);
        float caretX = 0.0f;
        if (!field.buffer.text.empty()) {
            const bool atEnd = field.buffer.caret >= field.buffer.text.size();
            const auto index =
                static_cast<UINT32>(atEnd ? field.buffer.text.size() - 1 : field.buffer.caret);
            float caretY = 0.0f;
            DWRITE_HIT_TEST_METRICS caretMetrics{};
            layout->HitTestTextPosition(index, atEnd, &caretX, &caretY, &caretMetrics);
        }
        const float maxScroll = std::max(0.0f, metrics.widthIncludingTrailingWhitespace - width);
        field.scrollX = std::clamp(field.scrollX, 0.0f, maxScroll);
        if (caretX < field.scrollX)
            field.scrollX = caretX;
        else if (caretX > field.scrollX + width)
            field.scrollX = caretX - width;

        const float originX = textRect.left - field.scrollX;
        auto* target = painter.target();
        target->PushAxisAlignedClip(textRect, D2D1_ANTIALIAS_MODE_ALIASED);
        const auto start = static_cast<UINT32>(field.buffer.selectionStart());
        const auto length =
            static_cast<UINT32>(field.buffer.selectionEnd() - field.buffer.selectionStart());
        if (length > 0) {
            UINT32 count = 0;
            layout->HitTestTextRange(start, length, originX, textRect.top, nullptr, 0, &count);
            if (count > 0) {
                std::vector<DWRITE_HIT_TEST_METRICS> spans(count);
                if (auto* brush = painter.brush(p.listSelected);
                    brush && SUCCEEDED(layout->HitTestTextRange(start, length, originX, textRect.top,
                                                              spans.data(), count, &count))) {
                    for (UINT32 i = 0; i < count; ++i) {
                        const auto& span = spans[i];
                        target->FillRectangle(D2D1::RectF(span.left, span.top, span.left + span.width,
                                                        span.top + span.height), brush);
                    }
                }
            }
        }
        painter.drawTextLayout(layout, D2D1::Point2F(originX, textRect.top), p.text);
        if (auto* brush = painter.brush(p.accent)) {
            const float x = originX + caretX;
            target->FillRectangle(D2D1::RectF(x, textRect.top + 7.0f, x + 1.0f,
                                             textRect.bottom - 7.0f), brush);
        }
        target->PopAxisAlignedClip();
    }

    bool isEnabled(int id) const {
        if (id == kIdSearchButton)
            return !searching;
        if (id == kIdCandidateList || id == kIdPreview)
            return !candidates.empty();
        if (id == kIdPreviewRomanization)
            return preview.hasRomanization();
        if (id == kIdPreviewTranslation)
            return preview.hasTranslation();
        if (id == kIdApplyButton || id == kIdAdvanceButton || id == kIdDelayButton)
            return !preview.lines().empty();
        return id != 0;
    }

    void drawButton(fluent::FluentDialogSurface::Painter& painter, const D2D1_RECT_F& rect,
                    const wchar_t* text, bool accent, int id) {
        const auto& p = fluent::palette();
        const bool enabled = isEnabled(id);
        const bool hovered = enabled && hoverId == id;
        const bool pressed = enabled && pressedId == id;
        D2D1_COLOR_F fill{};
        D2D1_COLOR_F textColor{};
        if (!enabled) {
            fill = p.listHover;
            textColor = p.disabled;
        } else if (accent) {
            fill = pressed ? p.accentPressed : hovered ? p.accentHover : p.accent;
            textColor = p.textOnAccent;
        } else {
            fill = pressed ? p.controlPressed : hovered ? p.controlHover : p.controlFill;
            textColor = p.text;
        }
        painter.fillRoundRect(fill, rect);
        if (!accent || !enabled)
            painter.strokeRoundRect(p.controlStroke, rect);
        if (focusedId == id && focusVisible && enabled) {
            painter.strokeRoundRect(
                accent ? p.textOnAccent : p.accent,
                D2D1::RectF(rect.left + 1.5f, rect.top + 1.5f, rect.right - 1.5f,
                            rect.bottom - 1.5f),
                1.5f, fluent::metrics::controlRadius - 1.0f);
        }
        painter.drawText(text, painter.textFormat(fluent::font_sizes::button, 400, true, true),
                         D2D1::RectF(rect.left + 4.0f, rect.top, rect.right - 4.0f, rect.bottom),
                         textColor);
    }

    void drawCandidateList(fluent::FluentDialogSurface::Painter& painter) {
        const auto& p = fluent::palette();
        const float height = candidateListRect.bottom - candidateListRect.top;
        candidateScroll = std::clamp(candidateScroll, 0.0f, candidateMaxScroll());

        if (focusedId == kIdCandidateList && focusVisible)
            painter.strokeRoundRect(p.accent, candidateListRect, 1.0f, fluent::metrics::cardRadius);

        // 合成滚动层是矩形，留出圆角和边框空间，避免覆盖卡片边缘。
        candidateScrollView.layout(
            D2D1::RectF(candidateListRect.left + fluent::metrics::cardRadius,
                        candidateListRect.top + 4.0f,
                        candidateListRect.right - kScrollBarHitWidth, candidateListRect.bottom - 4.0f),
            std::max(0.0f, candidateContentHeight() - 8.0f), surface.dpi());
        if (!candidateScrollView.animating() &&
            std::fabs(candidateScrollView.offset() - candidateScroll) > 0.01f)
            candidateScrollView.setOffset(candidateScroll);
        candidateScroll = candidateScrollView.offset();
        candidateScrollView.paint([&](fluent::FluentDialogSurface::Painter& painter,
                                      const D2D1_RECT_F& paintBounds, float position) {
        painter.fillRoundRect(p.cardFill, paintBounds, 0.0f);
        float y = candidateListRect.top - position;
        for (int row = 0; row < static_cast<int>(candidateItems.size()); ++row) {
            const float rowH = candidateItemHeight(row);
            const D2D1_RECT_F rowRect = D2D1::RectF(candidateListRect.left, y,
                                                   candidateListRect.right, y + rowH);
            if (rowRect.bottom >= paintBounds.top && rowRect.top <= paintBounds.bottom) {
                const auto& item = candidateItems[row];
                if (!item.header) {
                    const bool selected = row == selectedItemRow;
                    const bool hovered = row == hoverItemRow;
                    if (selected)
                        painter.fillRoundRect(
                            p.listSelected,
                            D2D1::RectF(paintBounds.left + 2.0f, y + 2.0f,
                                        paintBounds.right - 2.0f, y + rowH - 2.0f));
                    else if (hovered)
                        painter.fillRoundRect(
                            p.listHover,
                            D2D1::RectF(paintBounds.left + 2.0f, y + 2.0f,
                                        paintBounds.right - 2.0f, y + rowH - 2.0f));
                    if (selected)
                        painter.fillRoundRect(
                            p.accent,
                            D2D1::RectF(paintBounds.left + 4.0f,
                                        y + rowH * 0.5f - 8.0f,
                                        paintBounds.left + 7.0f,
                                        y + rowH * 0.5f + 8.0f),
                            1.5f);
                    const auto& candidate = candidates[item.candidateIndex];
                    const float left = paintBounds.left + 16.0f;
                    const float right = paintBounds.right - 12.0f;
                    float titleRight = right;
                    auto* detailFormat = painter.textFormat(fluent::font_sizes::caption, 400,
                                                            false, true);
                    if (candidate.durationMs > 0) {
                        const auto seconds = candidate.durationMs / 1000;
                        const std::wstring duration = std::to_wstring(seconds / 60) + L":" +
                            (seconds % 60 < 10 ? L"0" : L"") + std::to_wstring(seconds % 60);
                        const float durationWidth = painter.measureTextWidth(duration, detailFormat);
                        titleRight = std::max(left, right - durationWidth - 10.0f);
                        painter.drawText(duration, detailFormat,
                                         D2D1::RectF(right - durationWidth, y + 5.0f,
                                                     right, y + 25.0f),
                                         p.textSecondary);
                    }
                    painter.drawTrimmedText(
                        candidate.name, painter.textFormat(fluent::font_sizes::label, 550, false, true),
                        D2D1::RectF(left, y + 4.0f, titleRight, y + 26.0f), p.text);
                    std::wstring detail = candidate.singer;
                    if (!candidate.album.empty()) {
                        if (!detail.empty())
                            detail += L"  /  ";
                        detail += candidate.album;
                    }
                    painter.drawTrimmedText(detail, detailFormat,
                                             D2D1::RectF(left, y + 26.0f, right, y + rowH - 4.0f),
                                             p.textSecondary);
                } else {
                    painter.drawTrimmedText(
                        item.text, painter.textFormat(fluent::font_sizes::caption, 600, false, true),
                        D2D1::RectF(paintBounds.left + 16.0f, y,
                                    paintBounds.right - 8.0f, y + rowH),
                        p.accent);
                }
            }
            y += rowH;
        }

        }, false, candidateContentDirty);
        candidateContentDirty = false;
        if (candidateHasScroll() && height > 0.0f) {
            const auto bar = scrollBarGeometry(height, candidateContentHeight());
            const float thumbY = candidateListRect.top +
                                 scrollBarThumbY(bar, candidateScroll, candidateMaxScroll());
            painter.fillRoundRect(
                p.textSecondary,
                D2D1::RectF(candidateListRect.right - kScrollBarInset - kScrollBarWidth, thumbY,
                            candidateListRect.right - kScrollBarInset, thumbY + bar.thumbHeight),
                kScrollBarWidth * 0.5f);
        }
    }

    void paint(fluent::FluentDialogSurface::Painter& painter, float, float) {
        const auto& p = fluent::palette();
        drawTextField(painter, titleEditRect, titleEdit, kIdTitleEdit);
        drawTextField(painter, artistEditRect, artistEdit, kIdArtistEdit);
        drawButton(painter, searchRect, L"搜索", true, kIdSearchButton);

        if (!candidates.empty()) {
            painter.fillRoundRect(p.cardFill, workspaceRect, fluent::metrics::cardRadius);
            painter.strokeRoundRect(p.cardStroke, workspaceRect, 1.0f, fluent::metrics::cardRadius);
            if (auto* brush = painter.brush(p.cardStroke)) {
                const float x = candidateListRect.right;
                painter.target()->FillRectangle(
                    D2D1::RectF(x, workspaceRect.top + 1.0f, x + 1.0f, workspaceRect.bottom - 1.0f),
                    brush);
            }
            drawCandidateList(painter);
            const SearchCandidate* selectedCandidate =
                (selectedIdx >= 0 && selectedIdx < static_cast<int>(candidates.size()))
                    ? &candidates[selectedIdx]
                    : nullptr;
            preview.draw(painter, {
                .rect = previewRect,
                .romanizationRect = previewRomanizationRect,
                .translationRect = previewTranslationRect,
                .selectedCandidate = selectedCandidate,
                .dipScale = surface.dipScale(),
                .dpi = surface.dpi(),
                .focusedId = focusedId,
                .hoverId = hoverId,
                .pressedId = pressedId,
                .focusVisible = focusVisible,
                .previewId = kIdPreview,
                .romanizationId = kIdPreviewRomanization,
                .translationId = kIdPreviewTranslation,
            });
            drawButton(painter, advanceRect, L"提前 0.5 秒", false, kIdAdvanceButton);
            drawButton(painter, delayRect, L"延后 0.5 秒", false, kIdDelayButton);
            drawButton(painter, applyRect, L"使用此歌词", true, kIdApplyButton);
        } else {
            const wchar_t* hint = searching ? L"正在搜索歌词候选…"
                : preview.state() == ManualSearchPreviewState::NoCandidates
                    ? L"未找到歌词，试试缩短歌名或更换歌手关键词。"
                    : L"输入歌名和歌手后搜索。";
            painter.drawText(hint,
                             painter.textFormat(fluent::font_sizes::label, 400, false, true),
                             D2D1::RectF(workspaceRect.left, workspaceRect.top,
                                         workspaceRect.right, workspaceRect.top + 26.0f),
                             p.textSecondary);
        }
        drawButton(painter, cancelRect, L"取消", false, kIdCancelButton);
    }

    int hitTest(float x, float y) const {
        if (contains(titleEditRect, x, y))
            return kIdTitleEdit;
        if (contains(artistEditRect, x, y))
            return kIdArtistEdit;
        if (contains(searchRect, x, y))
            return kIdSearchButton;
        if (isEnabled(kIdCandidateList) && contains(candidateListRect, x, y))
            return kIdCandidateList;
        if (preview.hasRomanization() && contains(previewRomanizationRect, x, y))
            return kIdPreviewRomanization;
        if (preview.hasTranslation() && contains(previewTranslationRect, x, y))
            return kIdPreviewTranslation;
        if (contains(previewRect, x, y))
            return kIdPreview;
        if (contains(advanceRect, x, y))
            return kIdAdvanceButton;
        if (contains(delayRect, x, y))
            return kIdDelayButton;
        if (!candidates.empty() && contains(applyRect, x, y))
            return kIdApplyButton;
        if (contains(cancelRect, x, y))
            return kIdCancelButton;
        return 0;
    }

    std::vector<int> focusOrder() const {
        std::vector<int> order{kIdTitleEdit, kIdArtistEdit, kIdSearchButton,
                               kIdCandidateList, kIdPreviewRomanization,
                               kIdPreviewTranslation, kIdAdvanceButton, kIdDelayButton,
                               kIdApplyButton, kIdCancelButton};
        order.erase(std::remove_if(order.begin(), order.end(),
                                   [this](int id) { return !isEnabled(id); }),
                    order.end());
        return order;
    }

    void focusStep(int direction) {
        const auto order = focusOrder();
        if (order.empty())
            return;
        auto it = std::find(order.begin(), order.end(), focusedId);
        int index = it == order.end() ? (direction > 0 ? -1 : 0)
                                      : static_cast<int>(it - order.begin());
        index = (index + direction + static_cast<int>(order.size())) %
                static_cast<int>(order.size());
        focusedId = order[index];
        focusVisible = true;
        surface.invalidate();
    }

    void selectCandidateRow(int row) {
        if (row < 0 || row >= static_cast<int>(candidateItems.size()) ||
            candidateItems[row].header || candidateItems[row].candidateIndex < 0 ||
            candidateItems[row].candidateIndex >= static_cast<int>(candidates.size()))
            return;
        selectedItemRow = row;
        selectedIdx = candidateItems[row].candidateIndex;
        candidateContentDirty = true;
        const auto& candidate = candidates[selectedIdx];
        runtime_log::writef(L"[action][manual-search] candidate-selected index=%d name=\"%ls\" "
                            L"artist=\"%ls\" source=%d",
                            selectedIdx, candidate.name.c_str(), candidate.singer.c_str(),
                            static_cast<int>(candidate.source));
        ensureCandidateVisible(row);
        onSelectionChanged();
    }

    void onCommand(int id) {
        switch (id) {
        case kIdSearchButton:
            doSearch();
            break;
        case kIdAdvanceButton:
            runtime_log::writef(L"[action][manual-search] preview-offset delta=-500ms");
            shiftPreviewTimes(-500);
            break;
        case kIdDelayButton:
            runtime_log::writef(L"[action][manual-search] preview-offset delta=+500ms");
            shiftPreviewTimes(500);
            break;
        case kIdPreviewRomanization:
            preview.toggleRomanization();
            runtime_log::writef(L"[action][manual-search] preview-romanization=%s",
                                preview.showRomanization() ? L"on" : L"off");
            surface.invalidate();
            break;
        case kIdPreviewTranslation:
            preview.toggleTranslation();
            runtime_log::writef(L"[action][manual-search] preview-translation=%s",
                                preview.showTranslation() ? L"on" : L"off");
            surface.invalidate();
            break;
        case kIdApplyButton:
            runtime_log::writef(L"[action][manual-search] apply-click");
            applySelection();
            break;
        case kIdCancelButton:
            runtime_log::writef(L"[action][manual-search] cancel-click");
            destroy();
            break;
        default:
            break;
        }
    }

    void doSearch() {
        if (!provider)
            return;
        const UINT_PTR generation = ++searchGeneration;
        ++previewGeneration;
        searching = true;
        preview.setState(ManualSearchPreviewState::Empty, hwnd);
        candidateItems.clear();
        candidates.clear();
        selectedItemRow = -1;
        selectedIdx = -1;
        hoverItemRow = -1;
        candidateScroll = 0.0f;
        candidateScrollView.setOffset(0.0f);
        candidateWheelAccum = 0;
        setPreviewLines({});
        surface.invalidate();

        const std::wstring title = titleEdit.buffer.text;
        const std::wstring artist = artistEdit.buffer.text;
        HWND hwndCopy = hwnd;
        provider->searchCandidatesAsync(title, artist,
            [hwndCopy, generation](const std::vector<SearchCandidate>& result) {
                if (!IsWindow(hwndCopy))
                    return;
                auto* payload = new std::vector<SearchCandidate>(result);
                if (!PostMessageW(hwndCopy, kMsgCandidatesReady, generation,
                                  reinterpret_cast<LPARAM>(payload)))
                    delete payload;
            }, targetPlayer);
    }

    void handleCandidatesReady(const std::vector<SearchCandidate>& result) {
        candidates = result;
        candidateContentDirty = true;
        candidateItems.clear();
        selectedItemRow = -1;
        selectedIdx = -1;
        hoverItemRow = -1;
        candidateScroll = 0.0f;
        candidateScrollView.setOffset(0.0f);
        candidateWheelAccum = 0;
        searching = false;
        setPreviewLines({});

        auto addCandidate = [this](int index) {
            candidateItems.push_back({{}, index, false});
        };
        auto addGroup = [&](const wchar_t* header, LyricSource source, bool wordSync = false) {
            bool hasHeader = false;
            for (int i = 0; i < static_cast<int>(candidates.size()); ++i) {
                const auto& candidate = candidates[i];
                if (candidate.source != source ||
                    (source == LyricSource::Lrclib && candidate.lrclibWordSync != wordSync))
                    continue;
                if (!hasHeader) {
                    candidateItems.push_back({header, -1, true});
                    hasHeader = true;
                }
                addCandidate(i);
            }
        };
        if (targetIsSoda())
            addGroup(L"汽水音乐歌词（逐字／LRC）", LyricSource::Soda);
        if (targetIsNetease())
            addGroup(L"网易云歌词（YRC/LRC）", LyricSource::Yrc);
        addGroup(L"酷狗逐字歌词（KRC）", LyricSource::Krc);
        addGroup(L"QQ 音乐逐字歌词（QRC）", LyricSource::Qrc);
        addGroup(L"QQ 音乐逐行歌词（LRC）", LyricSource::Lrc);
        addGroup(L"LRCLIB 逐字歌词", LyricSource::Lrclib, true);
        addGroup(L"LRCLIB 逐行歌词（LRC）", LyricSource::Lrclib);
        if (!targetIsNetease())
            addGroup(L"网易云歌词（YRC/LRC）", LyricSource::Yrc);
        if (!targetIsSoda())
            addGroup(L"汽水音乐歌词（逐字／LRC）", LyricSource::Soda);

        if (candidates.empty()) {
            preview.setState(ManualSearchPreviewState::NoCandidates, hwnd);
        } else {
            for (int row = 0; row < static_cast<int>(candidateItems.size()); ++row) {
                if (!candidateItems[row].header) {
                    selectCandidateRow(row);
                    break;
                }
            }
        }
        surface.invalidate();
    }

    void onSelectionChanged() {
        if (selectedIdx < 0 || selectedIdx >= static_cast<int>(candidates.size()) || !provider)
            return;
        preview.setState(ManualSearchPreviewState::Loading, hwnd);
        setPreviewLines({});
        surface.invalidate();

        HWND hwndCopy = hwnd;
        const int index = selectedIdx;
        const UINT_PTR generation = ++previewGeneration;
        provider->fetchLyricAsync(candidates[index],
            [hwndCopy, index, generation](bool ok, const std::vector<LyricLine>& lines,
                              const SongInfo& info) {
                if (!IsWindow(hwndCopy))
                    return;
                auto* payload = new std::tuple<int, bool, std::vector<LyricLine>, SongInfo>(
                    index, ok, lines, info);
                if (!PostMessageW(hwndCopy, kMsgPreviewLyricReady, generation,
                                  reinterpret_cast<LPARAM>(payload)))
                    delete payload;
            }, targetDurationMs);
    }

    void handlePreviewLyricReady(int index, bool ok, const std::vector<LyricLine>& lines,
                                 const SongInfo& info) {
        if (index != selectedIdx)
            return;
        if (!ok || lines.empty()) {
            preview.setState(ManualSearchPreviewState::Unavailable, hwnd);
            surface.invalidate();
            return;
        }
        preview.setState(ManualSearchPreviewState::Empty, hwnd);
        SongInfo updatedInfo = info;
        updatedInfo.timingOffsetKnown = true;
        preview.setSongInfo(updatedInfo);
        setPreviewLines(lines);
        surface.invalidate();
    }

    void applySelection() {
        if (selectedIdx < 0 || selectedIdx >= static_cast<int>(candidates.size()) ||
            preview.lines().empty())
            return;
        SongInfo info = preview.songInfo();
        if (!targetNeteaseSongId.empty())
            info.neteaseSongId = targetNeteaseSongId;
        if (onApply)
            onApply(LyricResult{true, preview.lines(), std::move(info), false, true});
        destroy();
    }

    void shiftPreviewTimes(int64_t deltaMs) {
        if (preview.lines().empty())
            return;
        preview.shiftTimes(deltaMs, playbackPositionMs);
        surface.invalidate();
    }

    void destroy() {
        if (hwnd) {
            HWND target = hwnd;
            DestroyWindow(target);
            discardPendingResults(target);
            hwnd = nullptr;
        }
    }

    LRESULT handle(UINT msg, WPARAM wp, LPARAM lp) {
        switch (msg) {
        case WM_CREATE:
            backdrop = fluent::styleDialogWindow(hwnd);
            surface.initialize(hwnd, backdrop);
            if (!candidateScrollView.create(hwnd, 0x6401, [this](float position, bool redraw) {
                    candidateScroll = position;
                    const float scale = surface.dipScale();
                    const RECT dirty{
                        static_cast<LONG>(std::floor((redraw ? candidateListRect.left :
                                                      candidateListRect.right - kScrollBarHitWidth) * scale)),
                        static_cast<LONG>(std::floor(candidateListRect.top * scale)),
                        static_cast<LONG>(std::ceil(candidateListRect.right * scale)),
                        static_cast<LONG>(std::ceil(candidateListRect.bottom * scale))};
                    surface.invalidate(&dirty);
                }) ||
                !preview.create(hwnd, [this]() { surface.invalidate(); }))
                return -1;
            createControls();
            preview.refreshAnimations(hwnd);
            layout();
            SetTimer(hwnd, ManualSearchPreview::kTimerId, 100, nullptr);
            return 0;
        case WM_SIZE:
            preview.updateLoadingTimer(hwnd);
            candidateScroll = candidateScrollView.offset();
            candidateScrollView.stop();
            preview.onContainerResizeOrDpiChange();
            layout();
            surface.invalidate();
            return 0;
        case WM_DPICHANGED: {
            fluent::applyDialogDpiChange(hwnd, wp, reinterpret_cast<RECT*>(lp), kDialogStyle,
                                         kDialogExStyle, kMinClientWidthDip,
                                         kMinClientHeightDip);
            preview.onContainerResizeOrDpiChange();
            layout();
            surface.invalidate();
            return 0;
        }
        case WM_GETMINMAXINFO:
            fluent::setDialogMinimumTrackSize(hwnd, reinterpret_cast<MINMAXINFO*>(lp),
                                               kDialogStyle, kDialogExStyle,
                                               kMinClientWidthDip, kMinClientHeightDip);
            return 0;
        case WM_SIZING:
            fluent::enforceDialogMinimumAspectRatio(hwnd, wp, reinterpret_cast<RECT*>(lp),
                                                     kMinClientAspectRatio);
            return TRUE;
        case WM_SETTINGCHANGE:
        case WM_THEMECHANGED:
            candidateContentDirty = true;
            preview.refreshAnimations(hwnd);
            preview.onThemeChange();
            candidateScroll = candidateScrollView.offset();
            candidateScrollView.stop();
            backdrop = fluent::restyleDialogWindow(hwnd, backdrop);
            surface.setBackdrop(backdrop);
            surface.invalidate();
            return 0;
        case WM_PAINT: {
            PAINTSTRUCT ps{};
            HDC hdc = BeginPaint(hwnd, &ps);
            surface.paint(hdc, backdrop,
                          [this](fluent::FluentDialogSurface::Painter& painter, float w, float h) {
                              paint(painter, w, h);
                          });
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_ERASEBKGND:
            surface.eraseBackground(reinterpret_cast<HDC>(wp), backdrop);
            return 1;
        case WM_TIMER:
            if (wp == ManualSearchPreview::kLoadingTimerId) {
                if (preview.state() == ManualSearchPreviewState::Loading &&
                    preview.animationsEnabled())
                    invalidatePreview();
                return 0;
            }
            if (candidateScrollView.tick(wp) || preview.tickScroll(wp))
                return 0;
            if (wp == ManualSearchPreview::kTimerId && preview.onResumeTimer())
                surface.invalidate();
            return 0;
        case kMsgCandidatesReady: {
            auto* payload = reinterpret_cast<std::vector<SearchCandidate>*>(lp);
            if (payload) {
                if (wp == searchGeneration)
                    handleCandidatesReady(*payload);
                delete payload;
            }
            return 0;
        }
        case kMsgPreviewLyricReady: {
            auto* payload =
                reinterpret_cast<std::tuple<int, bool, std::vector<LyricLine>, SongInfo>*>(lp);
            if (payload) {
                if (wp == previewGeneration)
                    handlePreviewLyricReady(std::get<0>(*payload), std::get<1>(*payload),
                                            std::get<2>(*payload), std::get<3>(*payload));
                delete payload;
            }
            return 0;
        }
        case WM_MOUSEMOVE: {
            if (candidateScrollDragging) {
                const float scale = surface.dipScale();
                updateCandidateScrollDrag(GET_Y_LPARAM(lp) / scale);
                return 0;
            }
            TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd, 0};
            TrackMouseEvent(&tme);
            const float scale = surface.dipScale();
            const float x = GET_X_LPARAM(lp) / scale;
            const float y = GET_Y_LPARAM(lp) / scale;
            const int id = hitTest(x, y);
            const int row = id == kIdCandidateList ? candidateRowAt(y) : -1;
            if (id != hoverId || row != hoverItemRow) {
                if (row != hoverItemRow)
                    candidateContentDirty = true;
                hoverId = id;
                hoverItemRow = row;
                surface.invalidate();
            }
            return 0;
        }
        case WM_MOUSELEAVE:
            if (!candidateScrollDragging) {
                if (hoverItemRow != -1)
                    candidateContentDirty = true;
                hoverId = 0;
                hoverItemRow = -1;
                surface.invalidate();
            }
            return 0;
        case WM_LBUTTONDOWN: {
            candidateScroll = candidateScrollView.offset();
            candidateScrollView.stop();
            SetFocus(hwnd);
            focusVisible = false;
            const float scale = surface.dipScale();
            const float x = GET_X_LPARAM(lp) / scale;
            const float y = GET_Y_LPARAM(lp) / scale;
            const int id = hitTest(x, y);
            if (id == kIdPreview)
                preview.onPointerDown();
            focusedId = id;
            pressedId = 0;

            if (id == kIdTitleEdit || id == kIdArtistEdit) {
                if (auto* field = editFor(id))
                    field->buffer.moveCaret(field->buffer.text.size(), false);
            } else if (id == kIdCandidateList) {
                if (beginCandidateScrollDrag(x, y)) {
                    focusedId = kIdCandidateList;
                } else {
                    const int row = candidateRowAt(y);
                    if (row >= 0 && row < static_cast<int>(candidateItems.size()) &&
                        !candidateItems[row].header)
                        selectCandidateRow(row);
                }
            } else if (isEnabled(id) && id != kIdPreview) {
                pressedId = id;
                SetCapture(hwnd);
            }
            surface.invalidate();
            return 0;
        }
        case WM_LBUTTONUP: {
            if (candidateScrollDragging) {
                candidateScrollDragging = false;
                if (GetCapture() == hwnd)
                    ReleaseCapture();
                surface.invalidate();
                return 0;
            }
            const float scale = surface.dipScale();
            const int hit = hitTest(GET_X_LPARAM(lp) / scale, GET_Y_LPARAM(lp) / scale);
            const int pressed = pressedId;
            pressedId = 0;
            if (GetCapture() == hwnd)
                ReleaseCapture();
            if (pressed != 0 && pressed == hit && isEnabled(pressed))
                onCommand(pressed);
            surface.invalidate();
            return 0;
        }
        case WM_CAPTURECHANGED:
            candidateScrollDragging = false;
            pressedId = 0;
            surface.invalidate();
            return 0;
        case WM_MOUSEWHEEL: {
            POINT point{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            ScreenToClient(hwnd, &point);
            const float scale = surface.dipScale();
            const float x = point.x / scale;
            const float y = point.y / scale;
            if (contains(candidateListRect, x, y))
                scrollCandidatesBy(GET_WHEEL_DELTA_WPARAM(wp));
            else if (contains(previewRect, x, y))
                preview.scrollByWheel(GET_WHEEL_DELTA_WPARAM(wp));
            return 0;
        }
        case WM_GETDLGCODE:
            return DLGC_WANTALLKEYS | DLGC_WANTTAB | DLGC_WANTCHARS;
        case WM_KEYDOWN:
            candidateScroll = candidateScrollView.offset();
            candidateScrollView.stop();
            if (wp == VK_TAB) {
                focusStep(isShiftDown() ? -1 : 1);
                return 0;
            }
            if (wp == VK_ESCAPE) {
                destroy();
                return 0;
            }
            if (handleEditKey(wp))
                return 0;
            if (focusedId == kIdCandidateList &&
                (wp == VK_UP || wp == VK_DOWN || wp == VK_PRIOR || wp == VK_NEXT ||
                 wp == VK_HOME || wp == VK_END)) {
                int target = -1;
                if (wp == VK_UP || wp == VK_DOWN) {
                    const int direction = wp == VK_DOWN ? 1 : -1;
                    target = nextCandidateRow(
                        selectedItemRow < 0
                            ? (direction > 0 ? -1 : static_cast<int>(candidateItems.size()))
                            : selectedItemRow,
                        direction);
                } else if (wp == VK_HOME || wp == VK_END) {
                    target = wp == VK_HOME
                                 ? nextCandidateRow(-1, 1)
                                 : nextCandidateRow(static_cast<int>(candidateItems.size()), -1);
                } else {
                    const int direction = wp == VK_NEXT ? 1 : -1;
                    int cursor = selectedItemRow < 0
                                     ? (direction > 0 ? -1
                                                      : static_cast<int>(candidateItems.size()))
                                     : selectedItemRow;
                    const int page = std::max(
                        1, static_cast<int>((candidateListRect.bottom - candidateListRect.top) /
                                            kCandidateRowHeight) -
                               1);
                    for (int i = 0; i < page; ++i) {
                        const int next = nextCandidateRow(cursor, direction);
                        if (next < 0)
                            break;
                        cursor = next;
                    }
                    target = cursor == selectedItemRow ? -1 : cursor;
                }
                if (target >= 0)
                    selectCandidateRow(target);
                return 0;
            }
            if (wp == VK_RETURN || wp == VK_SPACE) {
                if (focusedId == kIdTitleEdit || focusedId == kIdArtistEdit) {
                    if (wp == VK_RETURN && isEnabled(kIdSearchButton))
                        doSearch();
                } else if (isEnabled(focusedId) && focusedId != kIdCandidateList &&
                           focusedId != kIdPreview) {
                    onCommand(focusedId);
                }
                return 0;
            }
            break;
        case WM_CHAR:
            if (handleCharacter(static_cast<wchar_t>(wp)))
                return 0;
            break;
        case WM_IME_CHAR:
            if (handleCharacter(static_cast<wchar_t>(wp)))
                return 0;
            break;
        case WM_SETFOCUS:
            focusVisible = true;
            surface.invalidate();
            return 0;
        case WM_KILLFOCUS:
            focusVisible = false;
            surface.invalidate();
            return 0;
        case WM_CLOSE:
            destroy();
            return 0;
        case WM_DESTROY:
            candidateScrollView.reset();
            preview.resetScroll();
            discardPendingResults(hwnd);
            KillTimer(hwnd, ManualSearchPreview::kTimerId);
            KillTimer(hwnd, ManualSearchPreview::kLoadingTimerId);
            surface.discard();
            hwnd = nullptr;
            if (notifyHwnd)
                PostMessageW(notifyHwnd, kMsgDialogClosed,
                             static_cast<WPARAM>(DialogKind::ManualSearch), 0);
            return 0;
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
};

ManualSearchDialog::ManualSearchDialog() : impl_(std::make_unique<Impl>()) {}

ManualSearchDialog::~ManualSearchDialog() {
    destroy();
}

bool ManualSearchDialog::create(HINSTANCE inst, HWND parent, LyricProvider* provider,
                                const std::wstring& targetTitle,
                                const std::wstring& targetArtist,
                                int64_t targetDurationMs, SmtcPlayerType targetPlayer,
                                const std::wstring& targetNeteaseSongId,
                                const std::wstring& targetAlbum) {
    impl_->notifyHwnd = parent; // 仅用于关闭通知
    impl_->provider = provider;
    impl_->targetTitle = targetTitle;
    impl_->targetArtist = targetArtist;
    impl_->targetDurationMs = targetDurationMs;
    impl_->targetNeteaseSongId = targetNeteaseSongId;
    impl_->targetPlayer = targetPlayer;
    impl_->targetAlbum = targetAlbum;

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_DBLCLKS | CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = Impl::wndProc;
    wc.hInstance = inst;
    wc.lpszClassName = L"QQMusicLyricManualSearch";
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = app_icon::windowIcon();
    RegisterClassExW(&wc);

    RECT work{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    const UINT dpi = GetDpiForSystem();
    const float scale = fluent::dipScale(dpi);
    RECT rc{0, 0, static_cast<LONG>(std::lround(1024.0f * scale)),
            static_cast<LONG>(std::lround(720.0f * scale))};
    AdjustWindowRectExForDpi(&rc, kDialogStyle, FALSE, kDialogExStyle, dpi);
    const int width = rc.right - rc.left;
    const int height = rc.bottom - rc.top;
    const int x = work.left + ((work.right - work.left) - width) / 2;
    const int y = work.top + ((work.bottom - work.top) - height) / 2;

    impl_->hwnd = CreateWindowExW(kDialogExStyle, L"QQMusicLyricManualSearch", L"手动搜索歌词",
                                  kDialogStyle, x, y, width, height, nullptr, nullptr, inst,
                                  impl_.get());
    if (impl_->hwnd) {
        fluent::ensureDialogMinimumSize(impl_->hwnd, kDialogStyle, kDialogExStyle,
                                         kMinClientWidthDip, kMinClientHeightDip);
        app_icon::applyWindowIcon(impl_->hwnd);
    }
    return impl_->hwnd != nullptr;
}

void ManualSearchDialog::show() {
    if (impl_->hwnd) {
        ShowWindow(impl_->hwnd, IsIconic(impl_->hwnd) ? SW_RESTORE : SW_SHOW);
        SetForegroundWindow(impl_->hwnd);
        SetFocus(impl_->hwnd);
    }
}

void ManualSearchDialog::destroy() {
    impl_->destroy();
}

bool ManualSearchDialog::isOpen() const {
    return impl_->hwnd != nullptr && IsWindow(impl_->hwnd);
}

HWND ManualSearchDialog::hwnd() const {
    return impl_->hwnd;
}

void ManualSearchDialog::onCandidatesReady(const std::vector<SearchCandidate>& cands) {
    if (!impl_->hwnd)
        return;
    auto* payload = new std::vector<SearchCandidate>(cands);
    if (!PostMessageW(impl_->hwnd, kMsgCandidatesReady, impl_->searchGeneration,
                      reinterpret_cast<LPARAM>(payload)))
        delete payload;
}

void ManualSearchDialog::onPreviewLyricReady(int idx, bool ok,
                                             const std::vector<LyricLine>& lines,
                                             const SongInfo& info) {
    if (!impl_->hwnd)
        return;
    auto* payload =
        new std::tuple<int, bool, std::vector<LyricLine>, SongInfo>(idx, ok, lines, info);
    if (!PostMessageW(impl_->hwnd, kMsgPreviewLyricReady, impl_->previewGeneration,
                      reinterpret_cast<LPARAM>(payload)))
        delete payload;
}

void ManualSearchDialog::setApplyCallback(ApplyCallback cb) {
    impl_->onApply = std::move(cb);
}

void ManualSearchDialog::setPlaybackPosition(int64_t positionMs) {
    impl_->playbackPositionMs = positionMs;
    if (impl_->preview.setPosition(positionMs) && impl_->hwnd)
        impl_->surface.invalidate();
}

void ManualSearchDialog::setTargetSong(const std::wstring& title, const std::wstring& artist,
                                      int64_t durationMs, SmtcPlayerType player,
                                      const std::wstring& neteaseSongId,
                                      const std::wstring& album) {
    const bool changed = title != impl_->targetTitle || artist != impl_->targetArtist ||
                         neteaseSongId != impl_->targetNeteaseSongId ||
                         player != impl_->targetPlayer ||
                         (player == SmtcPlayerType::Spotify && album != impl_->targetAlbum);
    impl_->targetDurationMs = durationMs;
    if (!changed)
        return;
    impl_->targetTitle = title;
    impl_->targetArtist = artist;
    impl_->targetNeteaseSongId = neteaseSongId;
    impl_->targetPlayer = player;
    impl_->targetAlbum = album;
    ++impl_->searchGeneration;
    ++impl_->previewGeneration;
    impl_->candidateItems.clear();
    impl_->candidates.clear();
    impl_->selectedItemRow = -1;
    impl_->selectedIdx = -1;
    impl_->hoverItemRow = -1;
    impl_->preview.clearSongInfo();
    impl_->setPreviewLines({});
    impl_->createControls();
    impl_->surface.invalidate();
}
