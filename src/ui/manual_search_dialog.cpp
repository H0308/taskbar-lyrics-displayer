#include "ui/manual_search_dialog.h"
#include "ui/font_sizes.h"

#include "ui/app_icon.h"
#include "ui/dialog_notify.h"
#include "ui/fluent_dialog_surface.h"
#include "ui/fluent_theme.h"
#include "logging/runtime_logger.h"

#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <numbers>
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

constexpr UINT kPreviewTimerId = 1;
constexpr UINT kPreviewLoadingTimerId = 2;
constexpr DWORD kPreviewResumeDelayMs = 2000;
constexpr int kPreviewWheelLinesPerNotch = 3;
constexpr float kPreviewLineScrollFrequency = 18.0f;
constexpr float kPreviewResumeScrollFrequency = 14.0f;
constexpr float kLyricTextWidthDip = 420.0f;
constexpr float kPreviewHeaderHeight = 62.0f;
constexpr float kLyricRowHeightDip = 50.0f;
constexpr float kPreviewSecondaryLineHeight = 18.0f;
constexpr float kPreviewSecondaryGap = 2.0f;
constexpr float kPreviewToggleSize = 28.0f;
constexpr float kPreviewToggleGap = 6.0f;
constexpr float kPreviewToggleMargin = 10.0f;
constexpr float kPreviewToggleTextGap = 10.0f;

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
        std::wstring text;
        std::wstring cue;
        size_t caret = 0;
        size_t anchor = 0;
        float scrollX = 0.0f;
    };

    struct CandidateItem {
        std::wstring text;
        int candidateIndex = -1; // -1 表示分组标题
        bool header = false;
    };

    enum class PreviewState { Empty, Loading, NoCandidates, Unavailable };

    HWND hwnd = nullptr;
    HWND notifyHwnd = nullptr; // 关闭时向托盘窗口投递 kMsgDialogClosed
    bool backdrop = false;

    fluent::FluentDialogSurface surface;
    fluent::CompositedScrollView candidateScrollView;
    fluent::CompositedScrollView previewScrollView;
    bool candidateContentDirty = true;
    float previewScrollPadding = 0.0f;

    TextField titleEdit;
    TextField artistEdit;
    bool searching = false;
    PreviewState previewState = PreviewState::Empty;
    bool previewAnimations = true;
    ULONGLONG previewLoadingSince = 0;
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

    std::vector<LyricLine> previewLines;
    std::vector<float> previewMainHeights;
    std::vector<float> previewRowHeights;
    std::vector<float> previewRowOffsets;
    float previewMeasuredWidth = -1.0f;
    float previewMeasuredSecondaryHeight = -1.0f;
    UINT previewMeasuredDpi = 0;
    SongInfo previewInfo;
    std::wstring previewCapabilityLabel;
    bool previewHasTranslation = false;
    bool previewHasRomanization = false;
    bool previewShowTranslation = false;
    bool previewShowRomanization = false;
    int64_t playbackPositionMs = 0;
    int64_t previewPositionMs = 0;
    int previewTopLine = 0;
    int previewCurrentLine = -1;
    bool previewManualScroll = false;
    bool previewSnapNextSync = true;
    bool previewAutoResumePending = false;
    bool previewContentDirty = true;
    DWORD previewLastScrollTick = 0;
    int previewWheelAccum = 0;

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

    static size_t selectionStart(const TextField& field) {
        return std::min(field.caret, field.anchor);
    }

    static size_t selectionEnd(const TextField& field) {
        return std::max(field.caret, field.anchor);
    }

    void moveCaret(TextField& field, size_t next, bool extend) {
        next = std::min(next, field.text.size());
        if (extend)
            field.caret = next;
        else
            field.caret = field.anchor = next;
        surface.invalidate();
    }

    void replaceSelection(TextField& field, const std::wstring& inserted) {
        const size_t start = selectionStart(field);
        const size_t end = selectionEnd(field);
        std::wstring accepted;
        for (wchar_t character : inserted) {
            if (character >= L' ' && character != 0x7f)
                accepted.push_back(character);
        }

        field.text.erase(start, end - start);
        field.text.insert(start, accepted);
        field.caret = start + accepted.size();
        field.anchor = field.caret;
    }

    void copySelection(const TextField& field) const {
        const size_t start = selectionStart(field);
        const size_t end = selectionEnd(field);
        if (start == end || !OpenClipboard(hwnd))
            return;

        EmptyClipboard();
        const std::wstring text = field.text.substr(start, end - start);
        const SIZE_T bytes = (text.size() + 1) * sizeof(wchar_t);
        HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
        if (memory) {
            void* data = GlobalLock(memory);
            if (data) {
                std::memcpy(data, text.c_str(), bytes);
                GlobalUnlock(memory);
                if (!SetClipboardData(CF_UNICODETEXT, memory))
                    GlobalFree(memory);
            } else {
                GlobalFree(memory);
            }
        }
        CloseClipboard();
    }

    std::wstring clipboardText() const {
        std::wstring result;
        if (!OpenClipboard(hwnd))
            return result;
        HANDLE handle = GetClipboardData(CF_UNICODETEXT);
        if (handle) {
            const auto* text = static_cast<const wchar_t*>(GlobalLock(handle));
            if (text)
                result = text;
            if (text)
                GlobalUnlock(handle);
        }
        CloseClipboard();
        return result;
    }

    void editChanged() {
        if (hwnd)
            surface.invalidate();
    }

    bool handleEditKey(WPARAM key) {
        TextField* field = editFor(focusedId);
        if (!field)
            return false;

        const bool ctrl = isCtrlDown();
        const bool shift = isShiftDown();
        if (ctrl && (key == 'A' || key == 'a')) {
            field->anchor = 0;
            field->caret = field->text.size();
            surface.invalidate();
            return true;
        }
        if (ctrl && (key == 'C' || key == 'c')) {
            copySelection(*field);
            return true;
        }
        if (ctrl && (key == 'X' || key == 'x')) {
            copySelection(*field);
            if (selectionStart(*field) != selectionEnd(*field)) {
                replaceSelection(*field, L"");
                editChanged();
            }
            return true;
        }
        if (ctrl && (key == 'V' || key == 'v')) {
            replaceSelection(*field, clipboardText());
            editChanged();
            return true;
        }

        switch (key) {
        case VK_LEFT:
            if (!shift && selectionStart(*field) != selectionEnd(*field))
                moveCaret(*field, selectionStart(*field), false);
            else if (field->caret > 0)
                moveCaret(*field, field->caret - 1, shift);
            else
                surface.invalidate();
            return true;
        case VK_RIGHT:
            if (!shift && selectionStart(*field) != selectionEnd(*field))
                moveCaret(*field, selectionEnd(*field), false);
            else if (field->caret < field->text.size())
                moveCaret(*field, field->caret + 1, shift);
            else
                surface.invalidate();
            return true;
        case VK_HOME:
            moveCaret(*field, 0, shift);
            return true;
        case VK_END:
            moveCaret(*field, field->text.size(), shift);
            return true;
        case VK_BACK:
            if (selectionStart(*field) != selectionEnd(*field)) {
                replaceSelection(*field, L"");
            } else if (field->caret > 0) {
                const size_t start = field->caret - 1;
                field->text.erase(start, 1);
                field->caret = field->anchor = start;
            }
            editChanged();
            return true;
        case VK_DELETE:
            if (selectionStart(*field) != selectionEnd(*field)) {
                replaceSelection(*field, L"");
            } else if (field->caret < field->text.size()) {
                field->text.erase(field->caret, 1);
            }
            editChanged();
            return true;
        default:
            return false;
        }
    }

    bool handleCharacter(wchar_t character) {
        TextField* field = editFor(focusedId);
        if (!field || character < L' ' || character == 0x7f)
            return false;
        replaceSelection(*field, std::wstring(1, character));
        editChanged();
        return true;
    }

    void updatePreviewLoadingTimer() {
        if (previewState == PreviewState::Loading && previewAnimations && !IsIconic(hwnd))
            SetTimer(hwnd, kPreviewLoadingTimerId, 33, nullptr);
        else
            KillTimer(hwnd, kPreviewLoadingTimerId);
    }

    void setPreviewState(PreviewState state) {
        if (state == previewState)
            return;
        previewState = state;
        if (state == PreviewState::Loading)
            previewLoadingSince = GetTickCount64();
        updatePreviewLoadingTimer();
    }

    void refreshPreviewAnimations() {
        BOOL enabled = TRUE;
        SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &enabled, 0);
        previewAnimations = enabled != FALSE;
        updatePreviewLoadingTimer();
    }

    void invalidatePreview() {
        const float scale = surface.dipScale();
        const RECT dirty{static_cast<LONG>(std::floor(previewRect.left * scale)),
                         static_cast<LONG>(std::floor(previewRect.top * scale)),
                         static_cast<LONG>(std::ceil(previewRect.right * scale)),
                         static_cast<LONG>(std::ceil(previewRect.bottom * scale))};
        surface.invalidate(&dirty);
    }

    void createControls() {
        titleEdit.cue = L"歌名";
        titleEdit.text = targetTitle;
        titleEdit.caret = titleEdit.anchor = titleEdit.text.size();
        artistEdit.cue = L"歌手";
        artistEdit.text = targetArtist;
        artistEdit.caret = artistEdit.anchor = artistEdit.text.size();

        setPreviewState(PreviewState::Empty);
        searching = false;
        focusedId = kIdTitleEdit;
        focusVisible = false;
        candidateScroll = 0.0f;
        candidateScrollView.setOffset(0.0f);
        candidateWheelAccum = 0;
        candidateScrollDragging = false;
        previewWheelAccum = 0;
        resetPreviewView();
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
            previewScrollView.layout({}, 0.0f, surface.dpi());
        }
        previewRomanizationRect = {};
        previewTranslationRect = {};
        const int previewToggleCount = (previewHasRomanization ? 1 : 0) +
                                       (previewHasTranslation ? 1 : 0);
        if (previewToggleCount > 0) {
            const float toggleRailHeight =
                previewToggleCount * kPreviewToggleSize +
                (previewToggleCount - 1) * kPreviewToggleGap;
            const float toggleBottom = previewRect.bottom - kPreviewToggleMargin;
            const float toggleTop =
                std::max(previewRect.top + previewContentTop(), toggleBottom - toggleRailHeight);
            const float toggleRight = previewRect.right - kPreviewToggleMargin;
            float nextTop = toggleTop;
            if (previewHasRomanization) {
                previewRomanizationRect =
                    D2D1::RectF(toggleRight - kPreviewToggleSize, nextTop, toggleRight,
                                 nextTop + kPreviewToggleSize);
                nextTop += kPreviewToggleSize + kPreviewToggleGap;
            }
            if (previewHasTranslation) {
                previewTranslationRect =
                    D2D1::RectF(toggleRight - kPreviewToggleSize, nextTop, toggleRight,
                                 nextTop + kPreviewToggleSize);
            }
        }

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
        syncPreviewToCurrentLine();
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

    void updatePreviewCapability() {
        previewHasTranslation = false;
        previewHasRomanization = false;
        for (const auto& line : previewLines) {
            previewHasTranslation = previewHasTranslation || !line.translation.empty();
            previewHasRomanization = previewHasRomanization || !line.romanization.empty();
        }
        previewShowTranslation = previewShowTranslation && previewHasTranslation;
        previewShowRomanization = previewShowRomanization && previewHasRomanization;
        if (previewHasTranslation && previewHasRomanization)
            previewCapabilityLabel = L"[支持翻译 + 罗马音]";
        else if (previewHasTranslation)
            previewCapabilityLabel = L"[支持翻译]";
        else if (previewHasRomanization)
            previewCapabilityLabel = L"[支持罗马音]";
        else
            previewCapabilityLabel.clear();
    }

    float previewContentTop() const {
        return kPreviewHeaderHeight + (previewCapabilityLabel.empty() ? 0.0f : 24.0f);
    }

    int previewSecondaryLineCount() const {
        return (previewShowRomanization ? 1 : 0) + (previewShowTranslation ? 1 : 0);
    }

    float previewSecondaryHeight() const {
        return previewSecondaryLineCount() *
               (kPreviewSecondaryLineHeight + kPreviewSecondaryGap);
    }

    float previewLyricRowHeight() const {
        return kLyricRowHeightDip + previewSecondaryHeight();
    }

    float measurePreviewMainHeight(fluent::FluentDialogSurface::Painter& painter,
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

    int visiblePreviewRows(float height) const {
        const float availableHeight = std::max(0.0f, height - previewContentTop());
        return std::max(1, static_cast<int>(std::floor(availableHeight / previewLyricRowHeight())));
    }

    int visiblePreviewRows() const {
        return visiblePreviewRows(std::max(0.0f, previewRect.bottom - previewRect.top));
    }

    void syncPreviewToCurrentLine() {
        const int total = static_cast<int>(previewLines.size());
        if (total == 0) {
            previewTopLine = 0;
            return;
        }
        const int visible = visiblePreviewRows();
        const int maxTop = std::max(0, total - 1);
        if (previewCurrentLine < 0)
            previewTopLine = 0;
        else
            previewTopLine = std::clamp(previewCurrentLine - visible / 2, 0, maxTop);
    }

    void resetPreviewView() {
        previewScrollView.setOffset(0.0f);
        previewPositionMs = playbackPositionMs;
        previewCurrentLine = LyricProvider::findLine(previewLines, previewPositionMs);
        previewTopLine = 0;
        previewManualScroll = false;
        previewSnapNextSync = true;
        previewAutoResumePending = false;
        previewContentDirty = true;
        previewLastScrollTick = 0;
        previewWheelAccum = 0;
        syncPreviewToCurrentLine();
    }

    void setPreviewLines(const std::vector<LyricLine>& lines) {
        previewLines = lines;
        previewMeasuredWidth = -1.0f;
        previewMainHeights.clear();
        previewRowHeights.clear();
        previewRowOffsets.clear();
        updatePreviewCapability();
        resetPreviewView();
        layout();
        surface.invalidate();
    }

    void setPreviewPosition(int64_t positionMs) {
        const int nextLine = LyricProvider::findLine(previewLines, positionMs);
        const bool lineChanged = nextLine != previewCurrentLine;
        const bool karaokeActive =
            nextLine >= 0 && nextLine < static_cast<int>(previewLines.size()) &&
            !previewLines[nextLine].chars.empty() && positionMs != previewPositionMs;
        previewPositionMs = positionMs;
        previewCurrentLine = nextLine;
        if (!previewManualScroll)
            syncPreviewToCurrentLine();
        if (!lineChanged && !karaokeActive)
            return;
        previewContentDirty = true;
        if (hwnd)
            surface.invalidate();
    }

    void scrollPreviewBy(int lines) {
        if (previewLines.empty())
            return;
        const int maxTop = std::max(0, static_cast<int>(previewLines.size()) - 1);
        previewTopLine = std::clamp(previewTopLine + lines, 0, maxTop);
        previewManualScroll = true;
        previewAutoResumePending = false;
        previewSnapNextSync = false;
        previewLastScrollTick = GetTickCount();
        previewScrollView.scrollBy(lines * previewLyricRowHeight());
    }

    void scrollPreviewByWheel(int delta) {
        if (previewLines.empty())
            return;
        previewManualScroll = true;
        previewAutoResumePending = false;
        previewSnapNextSync = false;
        previewLastScrollTick = GetTickCount();
        previewScrollView.scrollBy(-static_cast<float>(delta) / WHEEL_DELTA *
                                   kPreviewWheelLinesPerNotch * previewLyricRowHeight());
    }

    void onPreviewTimer() {
        if (previewManualScroll &&
            GetTickCount() - previewLastScrollTick >= kPreviewResumeDelayMs) {
            previewManualScroll = false;
            previewAutoResumePending = true;
            previewContentDirty = true;
            syncPreviewToCurrentLine();
            surface.invalidate();
        }
    }

    std::vector<D2D1_RECT_F> karaokeHighlightClips(IDWriteTextLayout* layout,
                                                   const LyricLine& line) const {
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
            if (textOffset >= textLength || character.startMs > previewPositionMs)
                break;

            const UINT32 rangeLength = std::min(tokenLength, textLength - textOffset);
            const int64_t durationMs =
                std::max<int64_t>(character.endMs - character.startMs, 1);
            const float fraction = static_cast<float>(std::clamp(
                static_cast<double>(previewPositionMs - character.startMs) /
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

    void drawCurrentLyric(fluent::FluentDialogSurface::Painter& painter, const LyricLine& line,
                          const D2D1_RECT_F& rect) {
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

        const float pixelScale = surface.dipScale();
        const D2D1_POINT_2F origin = D2D1::Point2F(
            std::round(rect.left * pixelScale) / pixelScale,
            std::round(rect.top * pixelScale) / pixelScale);
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
            painter.target()->PushAxisAlignedClip(
                clip, D2D1_ANTIALIAS_MODE_ALIASED);
            painter.drawTextLayout(layout, origin, color);
            painter.target()->PopAxisAlignedClip();
        };
        for (const auto& clip : unplayedClips)
            drawClippedText(clip, p.textSecondary);
        for (const auto& clip : playedClips)
            drawClippedText(clip, fluent::toD2D(RGB(49, 194, 124)));
    }

    void drawPreviewLyricLine(fluent::FluentDialogSurface::Painter& painter,
                              const LyricLine& line, const D2D1_RECT_F& rowRect,
                              float mainHeight, bool current) {
        const auto& p = fluent::palette();
        const int secondaryCount = previewSecondaryLineCount();
        const float contentHeight =
            mainHeight + secondaryCount * (kPreviewSecondaryLineHeight + kPreviewSecondaryGap);
        const float top = rowRect.top +
                          std::max(0.0f, (rowRect.bottom - rowRect.top - contentHeight) * 0.5f);
        const D2D1_RECT_F mainRect =
            D2D1::RectF(rowRect.left, top, rowRect.right, top + mainHeight);
        if (current) {
            drawCurrentLyric(painter, line, mainRect);
        } else {
            painter.drawText(line.text, painter.textFormat(fluent::font_sizes::body, 400, true), mainRect, p.text);
        }

        float secondaryTop = mainRect.bottom + kPreviewSecondaryGap;
        auto drawSecondary = [&](const std::wstring& text) {
            if (!text.empty()) {
                painter.drawTrimmedText(
                    text, painter.textFormat(fluent::font_sizes::caption, 400, true, true),
                    D2D1::RectF(rowRect.left, secondaryTop, rowRect.right,
                                secondaryTop + kPreviewSecondaryLineHeight),
                    p.textSecondary);
            }
            secondaryTop += kPreviewSecondaryLineHeight + kPreviewSecondaryGap;
        };
        if (previewShowRomanization)
            drawSecondary(line.romanization);
        if (previewShowTranslation)
            drawSecondary(line.translation);
    }

    void drawPreviewToggleButton(fluent::FluentDialogSurface::Painter& painter,
                                 const D2D1_RECT_F& rect, const wchar_t* label, bool checked,
                                 int id) {
        const auto& p = fluent::palette();
        const bool hovered = hoverId == id;
        const bool pressed = pressedId == id;
        const bool showFill = hovered || pressed;
        const D2D1_COLOR_F border = checked || hovered || pressed ? p.accent : p.controlStroke;
        const D2D1_COLOR_F textColor = checked ? p.accent : (hovered ? p.text : p.textSecondary);
        if (showFill) {
            painter.fillRoundRect(pressed ? p.controlPressed : p.controlHover, rect,
                                  fluent::metrics::controlRadius);
        }
        painter.strokeRoundRect(border, rect, 1.0f, fluent::metrics::controlRadius);
        if (focusedId == id && focusVisible) {
            painter.strokeRoundRect(
                p.accent,
                D2D1::RectF(rect.left + 1.5f, rect.top + 1.5f, rect.right - 1.5f,
                            rect.bottom - 1.5f),
                1.5f, std::max(1.0f, fluent::metrics::controlRadius - 1.0f));
        }
        painter.drawText(label, painter.textFormat(fluent::font_sizes::label, 600, true, true), rect, textColor);
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
            painter.drawTrimmedText(field.text, format, textRect, p.text);
            return;
        }

        const float width = std::max(1.0f, textRect.right - textRect.left - 1.0f);
        auto* layout = painter.textLayout(field.text, format, width, textRect.bottom - textRect.top);
        if (!layout)
            return;
        DWRITE_TEXT_METRICS metrics{};
        layout->GetMetrics(&metrics);
        float caretX = 0.0f;
        if (!field.text.empty()) {
            const bool atEnd = field.caret >= field.text.size();
            const auto index = static_cast<UINT32>(atEnd ? field.text.size() - 1 : field.caret);
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
        const auto start = static_cast<UINT32>(selectionStart(field));
        const auto length = static_cast<UINT32>(selectionEnd(field) - selectionStart(field));
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
            return previewHasRomanization;
        if (id == kIdPreviewTranslation)
            return previewHasTranslation;
        if (id == kIdApplyButton || id == kIdAdvanceButton || id == kIdDelayButton)
            return !previewLines.empty();
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

    void drawPreviewPlaceholder(fluent::FluentDialogSurface::Painter& painter) const {
        const auto& p = fluent::palette();
        const float centerX = (previewRect.left + previewRect.right) * 0.5f;
        const float centerY = (previewRect.top + kPreviewHeaderHeight + previewRect.bottom) * 0.5f;
        const bool loading = previewState == PreviewState::Loading;
        const wchar_t* text = loading ? L"正在加载预览…"
            : previewState == PreviewState::Unavailable ? L"该候选没有可用歌词"
                                                        : L"选择左侧歌曲预览歌词";

        if (loading) {
            // 五根静止的节拍柱错开亮度，避免持续改变图形位置或尺寸。
            constexpr float heights[] = {10.0f, 18.0f, 26.0f, 18.0f, 10.0f};
            constexpr ULONGLONG beatMs = 500;
            const auto elapsed = GetTickCount64() - previewLoadingSince;
            for (size_t i = 0; i < _countof(heights); ++i) {
                const float x = centerX + (static_cast<float>(i) - 2.0f) * 9.0f;
                const float halfHeight = heights[i] * 0.5f;
                const float phase = static_cast<float>((elapsed + beatMs - i * 60) % beatMs) /
                                    static_cast<float>(beatMs);
                const float pulse = 0.5f - 0.5f * std::cos(phase * 2.0f * std::numbers::pi_v<float>);
                auto color = p.accent;
                color.a *= previewAnimations ? 0.30f + 0.65f * pulse : 0.75f;
                painter.fillRoundRect(color, D2D1::RectF(x - 2.0f, centerY - 14.0f - halfHeight,
                                                       x + 2.0f, centerY - 14.0f + halfHeight),
                                      2.0f);
            }
        }
        const float textY = centerY + (loading ? 18.0f : 0.0f);
        painter.drawText(text, painter.textFormat(fluent::font_sizes::emphasis, 400, true),
                         D2D1::RectF(previewRect.left + 12.0f, textY - 14.0f,
                                     previewRect.right - 12.0f, textY + 14.0f),
                         p.textSecondary);
    }

    void drawPreview(fluent::FluentDialogSurface::Painter& painter) {
        const auto& p = fluent::palette();
        const float width = previewRect.right - previewRect.left;
        if (focusedId == kIdPreview && focusVisible) {
            painter.strokeRoundRect(
                p.accent,
                D2D1::RectF(previewRect.left + 1.5f, previewRect.top + 1.5f,
                            previewRect.right - 1.5f, previewRect.bottom - 1.5f),
                1.5f, fluent::metrics::cardRadius - 1.0f);
        }

        if (selectedIdx >= 0 && selectedIdx < static_cast<int>(candidates.size())) {
            const auto& candidate = candidates[selectedIdx];
            const float left = previewRect.left + 18.0f;
            const float right = previewRect.right - 18.0f;
            const float top = previewRect.top;
            painter.drawTrimmedText(
                candidate.name, painter.textFormat(fluent::font_sizes::emphasis, 600, false, true),
                D2D1::RectF(left, top + 7.0f, right, top + 31.0f), p.text);
            painter.drawTrimmedText(
                candidate.singer, painter.textFormat(fluent::font_sizes::caption, 400, false, true),
                D2D1::RectF(left, top + 31.0f, right, top + 51.0f), p.textSecondary);
            if (auto* brush = painter.brush(p.cardStroke)) {
                const float y = top + kPreviewHeaderHeight - 6.0f;
                painter.target()->FillRectangle(D2D1::RectF(left, y, right, y + 1.0f), brush);
            }
        }

        if (previewLines.empty()) {
            previewScrollView.layout({}, 0.0f, surface.dpi());
            drawPreviewPlaceholder(painter);
            return;
        }

        const float contentTop = previewContentTop();
        if (!previewCapabilityLabel.empty())
            painter.drawText(previewCapabilityLabel, painter.textFormat(fluent::font_sizes::smallText, 400, true),
                             D2D1::RectF(previewRect.left + 12.0f, previewRect.top + kPreviewHeaderHeight,
                                         previewRect.right - 12.0f,
                                         previewRect.top + kPreviewHeaderHeight + 24.0f),
                             p.textSecondary);

        const bool hasToggleRail = previewHasRomanization || previewHasTranslation;
        const float lyricLeftLimit = previewRect.left + 12.0f;
        const float lyricRightLimit = previewRect.right - 12.0f -
                                      (hasToggleRail ? kPreviewToggleSize + kPreviewToggleTextGap
                                                     : 0.0f);
        const float lyricAreaWidth = std::max(0.0f, lyricRightLimit - lyricLeftLimit);
        const float lyricWidth = std::min(kLyricTextWidthDip, lyricAreaWidth);
        const float lyricLeft = lyricLeftLimit + (lyricAreaWidth - lyricWidth) * 0.5f;
        const float lyricRight = lyricLeft + lyricWidth;

        const int total = static_cast<int>(previewLines.size());
        const float bodyTop = previewRect.top + contentTop;
        const float bodyBottom = std::max(bodyTop, previewRect.bottom - 2.0f);
        const float availableHeight = std::max(0.0f, bodyBottom - bodyTop);

        const float secondaryHeight = previewSecondaryHeight();
        if (previewMeasuredWidth != lyricWidth ||
            previewMeasuredSecondaryHeight != secondaryHeight ||
            previewMeasuredDpi != surface.dpi()) {
            previewMainHeights.resize(total);
            previewRowHeights.resize(total);
            previewRowOffsets.assign(total + 1, 0.0f);
            for (int index = 0; index < total; ++index) {
                previewMainHeights[index] =
                    measurePreviewMainHeight(painter, previewLines[index].text, lyricWidth);
                previewRowHeights[index] = previewMainHeights[index] + secondaryHeight;
                previewRowOffsets[index + 1] =
                    previewRowOffsets[index] + previewRowHeights[index];
            }
            previewMeasuredWidth = lyricWidth;
            previewMeasuredSecondaryHeight = secondaryHeight;
            previewMeasuredDpi = surface.dpi();
            previewContentDirty = true;
        }
        const auto& mainHeights = previewMainHeights;
        const auto& rowHeights = previewRowHeights;
        const auto& rowOffsets = previewRowOffsets;

        float totalScrollContentHeight = availableHeight;
        if (rowOffsets[total] <= availableHeight) {
            previewScrollPadding =
                std::max(0.0f, (availableHeight - rowOffsets[total]) * 0.5f);
        } else {
            float firstPageHeight = 0.0f;
            for (int index = 0; index < total; ++index) {
                if (index > 0 && firstPageHeight + rowHeights[index] > availableHeight)
                    break;
                firstPageHeight += rowHeights[index];
            }
            previewScrollPadding = std::max(6.0f, (availableHeight - firstPageHeight) * 0.5f);
            totalScrollContentHeight = rowOffsets[total] + previewScrollPadding * 2.0f;
        }
        const float maxScrollOffset =
            std::max(0.0f, totalScrollContentHeight - availableHeight);

        previewScrollView.layout(D2D1::RectF(lyricLeft, bodyTop, lyricRight, bodyBottom),
                                 totalScrollContentHeight, surface.dpi());
        if (!previewManualScroll) {
            float desiredOffset = 0.0f;
            if (previewCurrentLine >= 0 && previewCurrentLine < total) {
                const float lineCenterY =
                    previewScrollPadding + rowOffsets[previewCurrentLine] +
                    rowHeights[previewCurrentLine] * 0.5f;
                desiredOffset = std::clamp(lineCenterY - availableHeight * 0.5f,
                                           0.0f, maxScrollOffset);
            }
            if (previewSnapNextSync) {
                previewSnapNextSync = false;
                previewAutoResumePending = false;
                previewScrollView.setOffset(desiredOffset);
            } else if (previewAutoResumePending) {
                previewAutoResumePending = false;
                previewScrollView.scrollTo(desiredOffset, true,
                                           kPreviewResumeScrollFrequency, false);
            } else if (std::fabs(previewScrollView.targetOffset() - desiredOffset) > 0.5f) {
                previewScrollView.scrollTo(desiredOffset, true,
                                           kPreviewLineScrollFrequency, false);
            }
        }

        const bool forcePreviewRedraw = previewContentDirty;
        previewContentDirty = false;
        previewScrollView.paint([&](fluent::FluentDialogSurface::Painter& painter,
                                    const D2D1_RECT_F& paintBounds, float position) {
            painter.fillRoundRect(p.cardFill, paintBounds, 0.0f);
            for (int index = 0; index < total; ++index) {
                const float rowTop = bodyTop + previewScrollPadding + rowOffsets[index] - position;
                const D2D1_RECT_F rect = D2D1::RectF(
                    lyricLeft, rowTop, lyricRight, rowTop + rowHeights[index]);
                if (rect.bottom < paintBounds.top || rect.top > paintBounds.bottom)
                    continue;
                drawPreviewLyricLine(painter, previewLines[index], rect,
                                     mainHeights[index], index == previewCurrentLine);
            }
        }, false, forcePreviewRedraw);

        if (previewHasRomanization)
            drawPreviewToggleButton(painter, previewRomanizationRect, L"音",
                                    previewShowRomanization, kIdPreviewRomanization);
        if (previewHasTranslation)
            drawPreviewToggleButton(painter, previewTranslationRect, L"译", previewShowTranslation,
                                    kIdPreviewTranslation);
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
            drawPreview(painter);
            drawButton(painter, advanceRect, L"提前 0.5 秒", false, kIdAdvanceButton);
            drawButton(painter, delayRect, L"延后 0.5 秒", false, kIdDelayButton);
            drawButton(painter, applyRect, L"使用此歌词", true, kIdApplyButton);
        } else {
            const wchar_t* hint = searching ? L"正在搜索歌词候选…"
                : previewState == PreviewState::NoCandidates ? L"未找到歌词，试试缩短歌名或更换歌手关键词。"
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
        if (previewHasRomanization && contains(previewRomanizationRect, x, y))
            return kIdPreviewRomanization;
        if (previewHasTranslation && contains(previewTranslationRect, x, y))
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
            previewShowRomanization = !previewShowRomanization;
            runtime_log::writef(L"[action][manual-search] preview-romanization=%s",
                                previewShowRomanization ? L"on" : L"off");
            previewManualScroll = false;
            previewSnapNextSync = true;
            previewAutoResumePending = false;
            previewContentDirty = true;
            syncPreviewToCurrentLine();
            surface.invalidate();
            break;
        case kIdPreviewTranslation:
            previewShowTranslation = !previewShowTranslation;
            runtime_log::writef(L"[action][manual-search] preview-translation=%s",
                                previewShowTranslation ? L"on" : L"off");
            previewManualScroll = false;
            previewSnapNextSync = true;
            previewAutoResumePending = false;
            previewContentDirty = true;
            syncPreviewToCurrentLine();
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
        setPreviewState(PreviewState::Empty);
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

        const std::wstring title = titleEdit.text;
        const std::wstring artist = artistEdit.text;
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
            setPreviewState(PreviewState::NoCandidates);
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
        setPreviewState(PreviewState::Loading);
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
            setPreviewState(PreviewState::Unavailable);
            surface.invalidate();
            return;
        }
        setPreviewState(PreviewState::Empty);
        previewInfo = info;
        previewInfo.timingOffsetKnown = true;
        setPreviewLines(lines);
        surface.invalidate();
    }

    void applySelection() {
        if (selectedIdx < 0 || selectedIdx >= static_cast<int>(candidates.size()) ||
            previewLines.empty())
            return;
        SongInfo info = previewInfo;
        if (!targetNeteaseSongId.empty())
            info.neteaseSongId = targetNeteaseSongId;
        if (onApply)
            onApply(LyricResult{true, previewLines, std::move(info), false, true});
        destroy();
    }

    void shiftPreviewTimes(int64_t deltaMs) {
        if (previewLines.empty())
            return;
        previewInfo.timingOffsetMs += deltaMs;
        for (auto& line : previewLines) {
            line.ms += deltaMs;
            for (auto& character : line.chars) {
                character.startMs += deltaMs;
                character.endMs += deltaMs;
            }
        }
        resetPreviewView();
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
                !previewScrollView.create(hwnd, 0x6402, [this](float, bool redraw) {
                    if (redraw)
                        surface.invalidate();
                }))
                return -1;
            createControls();
            refreshPreviewAnimations();
            layout();
            SetTimer(hwnd, kPreviewTimerId, 100, nullptr);
            return 0;
        case WM_SIZE:
            updatePreviewLoadingTimer();
            candidateScroll = candidateScrollView.offset();
            candidateScrollView.stop();
            previewScrollView.stop();
            if (!previewManualScroll)
                previewSnapNextSync = true;
            previewContentDirty = true;
            layout();
            surface.invalidate();
            return 0;
        case WM_DPICHANGED: {
            fluent::applyDialogDpiChange(hwnd, wp, reinterpret_cast<RECT*>(lp), kDialogStyle,
                                         kDialogExStyle, kMinClientWidthDip,
                                         kMinClientHeightDip);
            if (!previewManualScroll)
                previewSnapNextSync = true;
            previewContentDirty = true;
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
            refreshPreviewAnimations();
            previewMeasuredWidth = -1.0f;
            candidateScroll = candidateScrollView.offset();
            candidateScrollView.stop();
            previewScrollView.stop();
            if (!previewManualScroll)
                previewSnapNextSync = true;
            previewContentDirty = true;
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
            if (wp == kPreviewLoadingTimerId) {
                if (previewState == PreviewState::Loading && previewAnimations)
                    invalidatePreview();
                return 0;
            }
            if (candidateScrollView.tick(wp) || previewScrollView.tick(wp))
                return 0;
            if (wp == kPreviewTimerId)
                onPreviewTimer();
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
            if (id == kIdPreview && previewManualScroll) {
                previewScrollView.stop();
                previewLastScrollTick = GetTickCount();
            }
            focusedId = id;
            pressedId = 0;

            if (id == kIdTitleEdit || id == kIdArtistEdit) {
                if (auto* field = editFor(id))
                    field->caret = field->anchor = field->text.size();
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
                scrollPreviewByWheel(GET_WHEEL_DELTA_WPARAM(wp));
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
            previewScrollView.reset();
            discardPendingResults(hwnd);
            KillTimer(hwnd, kPreviewTimerId);
            KillTimer(hwnd, kPreviewLoadingTimerId);
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
    impl_->setPreviewPosition(positionMs);
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
    impl_->previewInfo = {};
    impl_->setPreviewLines({});
    impl_->createControls();
    impl_->surface.invalidate();
}
