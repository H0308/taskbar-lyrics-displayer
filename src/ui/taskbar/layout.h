#pragma once
#include "ui/lyric_window.h"
#include <d2d1.h>
#include <array>
#include <span>

namespace taskbar_detail {
inline constexpr float kCoverPadding = 4.0f;
inline constexpr float kTextPadding = 8.0f;
inline constexpr float kSongInfoLyricGap = 8.0f;
inline constexpr float kLeftRatio = 0.38f;
inline constexpr float kVerticalBaseLengthDip = 320.0f;
inline constexpr float kVerticalSongInfoLengthDip = kVerticalBaseLengthDip * kLeftRatio;
inline constexpr float kSongInfoLineGap = 2.0f;
inline float songInfoStart(bool coverVisible, float coverSize) {
    return coverVisible ? 2.0f * kCoverPadding + coverSize : kTextPadding;
}
inline constexpr float kVerticalControlsGap = 4.0f;
inline constexpr float kVerticalLyricGap = 6.0f;
inline constexpr int kInlineSourceControl = static_cast<int>(MediaControl::NextSource);
inline constexpr int kInlineProgressControl = kInlineSourceControl + 1;
inline constexpr int kInlineControlCount = kInlineProgressControl + 1;
struct LayoutMetrics {
    float w = 0.0f;
    float h = 0.0f;
    float leftW = 0.0f;
    float rightW = 0.0f;
    float immersiveControlsW = 0.0f;
};
enum class PinnedInlineTier {
    FallbackHover,
    CompactCore3,
    CompactFull,
    Full,
};

struct PinnedInlineControlsInput {
    float width = 0.0f;
    float height = 0.0f;
    DisplayScene scene = DisplayScene::NoPlayback;
    bool songInfoVisible = true;
    bool albumCoverVisible = true;
    bool canRepeat = false;
    float spectrumWidth = 0.0f;
    float coverSlotWidth = 0.0f;
    float lyricFontSize = 0.0f;
    bool canSwitchSource = false;
    float leadingOffset = 0.0f;
    bool canSeek = false;
};

struct PinnedInlineControlsLayout {
    PinnedInlineTier tier = PinnedInlineTier::FallbackHover;
    float slotWidth = 0.0f;
    float radius = 0.0f;
    float centerY = 0.0f;
    float boundsLeft = 0.0f;
    float boundsRight = 0.0f;
    bool visible[kInlineControlCount]{};
    float centerX[kInlineControlCount]{};

    bool active() const { return tier != PinnedInlineTier::FallbackHover && slotWidth > 0.0f; }
};

struct VerticalLayout {
    float w = 0.0f;
    float h = 0.0f;
    float coverX = 0.0f;
    float coverY = 0.0f;
    float coverSize = 0.0f;
    D2D1_RECT_F songInfo{};
    float lyricY = 0.0f;
    float lyricBottom = 0.0f;
    D2D1_RECT_F resources{};
    bool showControls = false;
    bool showProgressControl = false;
    float controlRadius = 0.0f;
    D2D1_POINT_2F controlCenters[6]{};
};

struct HorizontalLayoutInput {
    float width;
    float height;
    DisplayScene scene;
    bool expanded;
    bool immersiveControls;
    bool songInfoVisible;
    bool albumCoverVisible;
    float controlsWidth;
    float spectrumWidth;
    float coverSize;
    float coverSlotWidth;
    float leadingOffset = 0.0f;
};

// 沉浸模式与 Dock 右侧区域的纯几何结果。
struct ExpandedSideLayout {
    float zoneWidth{}, zoneX{};
    float resourceWidth{}, resourceX{};
    float clockWidth{}, clockX{};
    float spectrumRight{}, spectrumWidth{};
};

struct DynamicBackgroundRegions {
    std::array<D2D1_RECT_F, 6> rects{};
    size_t count = 0;
};

// 各区域沿横向或竖向排布；合并相邻区间，保持同一背景时间轴且不重复叠色。
DynamicBackgroundRegions dynamicBackgroundRegions(
    float width, float height, IdleQuoteBackgroundScope scope,
    std::span<const D2D1_RECT_F> playback, std::span<const D2D1_RECT_F> idle);

float expandedSpectrumWidth(float hostWidth);
float expandedClockWidth(float hostWidth);
ExpandedSideLayout expandedSideLayout(float hostWidth, bool dock, float resourceWidth);
float pinnedInlineControlsIdealWidth(float heightDip, bool canRepeat, float lyricFontSize = 0.0f,
                                     bool canSwitchSource = false, bool canSeek = false);
PinnedInlineControlsLayout planPinnedInlineControls(const PinnedInlineControlsInput& input);
void layoutInlineHoverControls(float left, float width, float height, bool canRepeat,
                               bool canSwitchSource, float (&centers)[kInlineControlCount],
                               float& cy, float& radius, bool canSeek = false);

// 纯几何计算。渲染与命中检测读取同一份布局，窗口句柄和设备资源不进入计算。
LayoutMetrics horizontalLayout(const HorizontalLayoutInput& input);
VerticalLayout verticalLayout(float width, float height, DisplayScene scene, bool albumCoverVisible,
                              bool showControls, bool canSwitchSource = false,
                              float resourceHeight = 0.0f, bool songInfoVisible = true,
                              ResourcePlacement resourcePlacement = ResourcePlacement::End,
                              bool resourceOnly = false, bool canSeek = false);
} // namespace taskbar_detail
