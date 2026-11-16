#pragma once

#include "lyric/lyric_provider.h"
#include "ui/fluent_dialog_surface.h"

#include <windows.h>
#include <d2d1.h>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

/// 手动搜索弹窗右侧歌词预览面板的展示状态。
enum class ManualSearchPreviewState { Empty, Loading, NoCandidates, Unavailable };

/// 绘制单帧歌词预览面板时由宿主借出的只读视图上下文。
struct ManualSearchPreviewDrawContext {
    D2D1_RECT_F rect{};
    D2D1_RECT_F romanizationRect{};
    D2D1_RECT_F translationRect{};
    const SearchCandidate* selectedCandidate = nullptr;
    float dipScale = 1.0f;
    UINT dpi = 96;
    int focusedId = 0;
    int hoverId = 0;
    int pressedId = 0;
    bool focusVisible = false;
    int previewId = 0;
    int romanizationId = 0;
    int translationId = 0;
};

/// 负责手动搜索弹窗右侧歌词预览的测量、滚动同步、卡拉 OK 进度着色与状态绘制。
class ManualSearchPreview {
public:
    static constexpr UINT kTimerId = 1;
    static constexpr UINT kLoadingTimerId = 2;

    bool create(HWND hwnd, std::function<void()> onInvalidate);
    void hideScroll(UINT dpi) { scrollView_.layout({}, 0.0f, dpi); }
    void resetScroll();

    ManualSearchPreviewState state() const { return state_; }
    bool animationsEnabled() const { return animations_; }
    bool manualScroll() const { return manualScroll_; }
    bool hasTranslation() const { return hasTranslation_; }
    bool hasRomanization() const { return hasRomanization_; }
    bool showTranslation() const { return showTranslation_; }
    bool showRomanization() const { return showRomanization_; }
    const std::vector<LyricLine>& lines() const { return lines_; }
    const SongInfo& songInfo() const { return info_; }
    void setSongInfo(const SongInfo& info) { info_ = info; }
    void clearSongInfo() { info_ = {}; }

    void setState(ManualSearchPreviewState state, HWND hwnd);
    void updateLoadingTimer(HWND hwnd);
    void refreshAnimations(HWND hwnd);

    void resetView(int64_t playbackPositionMs);
    void setLines(const std::vector<LyricLine>& lines, int64_t playbackPositionMs);
    bool setPosition(int64_t positionMs);
    void shiftTimes(int64_t deltaMs, int64_t playbackPositionMs);

    void toggleRomanization();
    void toggleTranslation();

    void onPointerDown();
    void scrollByWheel(int delta);
    bool onResumeTimer();
    bool tickScroll(UINT_PTR timerId) { return scrollView_.tick(timerId); }
    void onContainerResizeOrDpiChange();
    void onThemeChange();

    void layoutToggleButtons(const D2D1_RECT_F& previewRect, D2D1_RECT_F& romanizationRect,
                             D2D1_RECT_F& translationRect) const;
    void draw(fluent::FluentDialogSurface::Painter& painter,
              const ManualSearchPreviewDrawContext& ctx);

private:
    float contentTop() const;
    int secondaryMask() const;
    int secondaryLineCount(const LyricLine& line) const;
    float secondaryHeight(int secondaryCount) const;
    float defaultLyricRowHeight() const;
    float measureMainHeight(fluent::FluentDialogSurface::Painter& painter,
                            const std::wstring& text, float width) const;
    void updateCapability();

    std::vector<D2D1_RECT_F> karaokeHighlightClips(IDWriteTextLayout* layout,
                                                   const LyricLine& line) const;
    void drawPlaceholder(fluent::FluentDialogSurface::Painter& painter,
                         const D2D1_RECT_F& rect) const;
    void drawCurrentLyric(fluent::FluentDialogSurface::Painter& painter,
                          const LyricLine& line, const D2D1_RECT_F& rect,
                          float dipScale) const;
    void drawLyricLine(fluent::FluentDialogSurface::Painter& painter, const LyricLine& line,
                       const D2D1_RECT_F& rowRect, float mainHeight, bool current,
                       float dipScale) const;
    void drawToggleButton(fluent::FluentDialogSurface::Painter& painter,
                          const D2D1_RECT_F& rect, const wchar_t* label, bool checked, int id,
                          const ManualSearchPreviewDrawContext& ctx) const;

    fluent::CompositedScrollView scrollView_;
    ManualSearchPreviewState state_ = ManualSearchPreviewState::Empty;
    bool animations_ = true;
    ULONGLONG loadingSince_ = 0;

    std::vector<LyricLine> lines_;
    std::vector<float> mainHeights_;
    std::vector<float> rowHeights_;
    std::vector<float> rowOffsets_;
    float measuredWidth_ = -1.0f;
    int measuredSecondaryMask_ = -1;
    UINT measuredDpi_ = 0;
    float scrollPadding_ = 0.0f;

    SongInfo info_{};
    std::wstring capabilityLabel_;
    bool hasTranslation_ = false;
    bool hasRomanization_ = false;
    bool showTranslation_ = false;
    bool showRomanization_ = false;

    int64_t positionMs_ = 0;
    int currentLine_ = -1;
    bool manualScroll_ = false;
    bool snapNextSync_ = true;
    bool autoResumePending_ = false;
    bool contentDirty_ = true;
    DWORD lastScrollTick_ = 0;
};
