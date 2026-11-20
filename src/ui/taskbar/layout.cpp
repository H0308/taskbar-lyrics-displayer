#include "ui/taskbar/layout.h"
#include <algorithm>

namespace taskbar_detail {
namespace {
struct PinnedTierSpec {
    PinnedInlineTier tier;
    float radius = 0.0f;
    float spacing = 0.0f;
    float repeatGap = 0.0f;
    float volumeGap = 0.0f;
    float span = 0.0f;
    float slotWidth = 0.0f;
    bool includeRepeat = false;
    bool includeVolume = false;
};

PinnedTierSpec buildPinnedTierSpec(PinnedInlineTier tier, float heightDip, bool canRepeat,
                                   float lyricFontSize, bool canSwitchSource) {
    PinnedTierSpec spec{};
    spec.tier = tier;
    if (heightDip <= 0.0f || tier == PinnedInlineTier::FallbackHover)
        return spec;

    const float maxFullR = std::min(14.0f, heightDip * 0.32f);
    const float minFullR = std::min(maxFullR, std::max(7.5f, heightDip * 0.23f));
    const float targetFullR =
        lyricFontSize > 0.0f ? lyricFontSize * 0.74f : heightDip * 0.29f;
    const float fullR = std::clamp(targetFullR, minFullR, maxFullR);
    const auto finish = [canSwitchSource](PinnedTierSpec& tierSpec, float sidePadding) {
        if (canSwitchSource)
            tierSpec.span += tierSpec.spacing + tierSpec.radius * 0.14f;
        tierSpec.slotWidth = tierSpec.span + tierSpec.radius * 2.0f + sidePadding * 2.0f;
    };

    if (tier == PinnedInlineTier::Full) {
        spec.radius = fullR;
        spec.spacing = spec.radius * 2.20f;
        constexpr float sidePad = 6.0f;
        spec.includeVolume = true;
        spec.includeRepeat = canRepeat;
        if (canRepeat) {
            spec.repeatGap = spec.spacing + spec.radius * 0.12f;
            spec.volumeGap = spec.spacing + spec.radius * 0.22f;
            spec.span = 2.0f * spec.spacing + spec.repeatGap + spec.volumeGap;
        } else {
            spec.volumeGap = spec.spacing + spec.radius * 0.26f;
            spec.span = 2.0f * spec.spacing + spec.volumeGap;
        }
        finish(spec, sidePad);
        return spec;
    }

    const float compactMaxR = std::min(11.2f, maxFullR);
    const float compactMinR = std::min(compactMaxR, 6.8f);
    spec.radius = std::clamp(fullR * 0.82f, compactMinR, compactMaxR);
    constexpr float sidePad = 5.0f;
    if (tier == PinnedInlineTier::CompactFull) {
        spec.spacing = spec.radius * 2.05f;
        spec.includeVolume = true;
        spec.includeRepeat = canRepeat;
        if (canRepeat) {
            spec.repeatGap = spec.spacing + spec.radius * 0.10f;
            spec.volumeGap = spec.spacing + spec.radius * 0.20f;
            spec.span = 2.0f * spec.spacing + spec.repeatGap + spec.volumeGap;
        } else {
            spec.volumeGap = spec.spacing + spec.radius * 0.24f;
            spec.span = 2.0f * spec.spacing + spec.volumeGap;
        }
        finish(spec, sidePad);
        return spec;
    }

    spec.spacing = spec.radius * 2.15f;
    spec.span = 2.0f * spec.spacing;
    finish(spec, sidePad);
    return spec;
}
} // namespace
DynamicBackgroundRegions dynamicBackgroundRegions(
    float width, float height, IdleQuoteBackgroundScope scope,
    std::span<const D2D1_RECT_F> playback, std::span<const D2D1_RECT_F> idle) {
    DynamicBackgroundRegions result;
    if (width <= 0.0f || height <= 0.0f || scope == IdleQuoteBackgroundScope::None)
        return result;
    if (scope == IdleQuoteBackgroundScope::All) {
        result.rects[result.count++] = D2D1::RectF(0.0f, 0.0f, width, height);
        return result;
    }
    const auto selected = scope == IdleQuoteBackgroundScope::Lyrics ? playback : idle;
    for (const auto& rect : selected) {
        const auto clipped = D2D1::RectF(std::clamp(rect.left, 0.0f, width),
                                        std::clamp(rect.top, 0.0f, height),
                                        std::clamp(rect.right, 0.0f, width),
                                        std::clamp(rect.bottom, 0.0f, height));
        if (clipped.right > clipped.left && clipped.bottom > clipped.top &&
            result.count < result.rects.size())
            result.rects[result.count++] = clipped;
    }
    // 重叠的背景波浪和歌词区合并为一次绘制；竖向任务栏沿 Y 轴合并。
    for (size_t i = 0; i < result.count; ++i) {
        for (size_t j = i + 1; j < result.count;) {
            auto& a = result.rects[i];
            const auto& b = result.rects[j];
            const bool horizontal = a.top == b.top && a.bottom == b.bottom &&
                                    a.left <= b.right && b.left <= a.right;
            const bool vertical = a.left == b.left && a.right == b.right &&
                                  a.top <= b.bottom && b.top <= a.bottom;
            if (horizontal || vertical) {
                a = D2D1::RectF(std::min(a.left, b.left), std::min(a.top, b.top),
                                std::max(a.right, b.right), std::max(a.bottom, b.bottom));
                result.rects[j] = result.rects[--result.count];
                // 合并后可能再次碰到已检查的区间。
                j = i + 1;
            } else {
                ++j;
            }
        }
    }
    return result;
}

LayoutMetrics horizontalLayout(const HorizontalLayoutInput& input) {

    LayoutMetrics m;
    m.w = input.width;
    m.h = input.height;
    const bool immersiveControls = input.immersiveControls;
    m.immersiveControlsW = input.controlsWidth;
    if (input.scene == DisplayScene::Idle) {
        m.leftW = 0.0f;
        m.rightW = std::max(1.0f, m.w - m.immersiveControlsW - input.spectrumWidth);
        return m;
    }
    float effW = m.w - input.spectrumWidth;
    const float immersiveStart = input.songInfoVisible ? kSongInfoLyricGap : kTextPadding;
    const float contentW =
        std::max(1.0f, effW - m.immersiveControlsW - (immersiveControls ? immersiveStart : 0.0f));
    if (input.expanded && input.songInfoVisible) {
        // 沉浸模式的宽度来自整个任务栏，歌曲信息只保留稳定的左侧栏，
        // 把主要空间留给歌词和右侧频谱，避免 1920px 任务栏出现过宽信息区。
        const float available = contentW;
        const float minimum = input.albumCoverVisible
                                  ? kCoverPadding + input.coverSize + kTextPadding + 80.0f
                                  : 140.0f;
        const float preferred = std::clamp(available * 0.24f, 180.0f, 280.0f);
        m.leftW = std::min(available, std::max(minimum, preferred));
    } else if (input.expanded) {
        // 没有歌曲信息时，只有实际显示的封面需要占用左侧安全区。
        // 这样切换封面不会改变歌词的中心锚点，也不会给控件留下无意义的空洞。
        m.leftW = input.albumCoverVisible ? input.coverSlotWidth : 0.0f;
    } else {
        m.leftW = input.songInfoVisible ? contentW * kLeftRatio : input.coverSlotWidth;
    }
    m.rightW = std::max(1.0f, m.w - m.leftW - m.immersiveControlsW);
    return m;
}

VerticalLayout verticalLayout(float width, float height, DisplayScene scene, bool albumCoverVisible,
                              bool showControls, bool canSwitchSource) {

    VerticalLayout layout;
    layout.w = width;
    layout.h = height;
    if (layout.w <= 0.0f || layout.h <= 0.0f)
        return layout;

    const float pad = std::min(kVerticalRailPadding, layout.w * 0.18f);
    const float contentW = std::max(1.0f, layout.w - pad * 2.0f);
    const bool playbackScene = scene != DisplayScene::Idle && scene != DisplayScene::NoPlayback;
    if (albumCoverVisible && playbackScene) {
        layout.coverSize = contentW;
        layout.coverX = (layout.w - layout.coverSize) * 0.5f;
        layout.coverY = pad;
    }

    layout.lyricY =
        layout.coverSize > 0.0f ? layout.coverY + layout.coverSize + kVerticalCoverGap : pad;
    layout.lyricBottom = std::max(layout.lyricY + 1.0f, layout.h - pad);
    layout.showControls = showControls && playbackScene;
    if (layout.showControls) {
        layout.controlRadius = std::clamp(layout.w * 0.23f, 6.0f, 9.0f);
        const float diameter = layout.controlRadius * 2.0f;
        // 与横向内嵌控件复用同一套视觉节奏：按钮中心间距约为 2.8r。
        const float transportPitch = layout.controlRadius * 2.8f;
        const float transportGap = std::max(kVerticalControlsGap, transportPitch - diameter);
        // 音量是独立的辅助操作，与上一首/播放/下一首之间留出更明显的层级间距。
        const float volumeGap = transportGap + layout.controlRadius * 0.9f;
        const float sourcePitch = diameter + transportGap;
        const float blockH = diameter * 4.0f + transportGap * 2.0f + volumeGap +
                             (canSwitchSource ? sourcePitch : 0.0f);
        const float controlsTop = layout.lyricY;
        const float controlsBottom = layout.h - pad;
        const float freeSpace = controlsBottom - controlsTop - blockH;
        const float groupTop = controlsTop + std::max(0.0f, freeSpace) * 0.5f;
        const float firstY = groupTop + layout.controlRadius;
        if (freeSpace >= 0.0f && groupTop - kVerticalLyricGap > layout.lyricY) {
            for (int i = 0; i < 3; ++i)
                layout.controlCenters[i] =
                    D2D1::Point2F(layout.w * 0.5f, firstY + i * transportPitch);
            layout.controlCenters[3] = D2D1::Point2F(
                layout.w * 0.5f, firstY + transportPitch * 2.0f + diameter + volumeGap);
            if (canSwitchSource)
                layout.controlCenters[4] =
                    D2D1::Point2F(layout.w * 0.5f, layout.controlCenters[3].y + sourcePitch);
            layout.lyricBottom = groupTop - kVerticalLyricGap;
        } else {
            layout.showControls = false;
        }
    }
    return layout;
}

float expandedSpectrumWidth(float hostWidth) {
    return hostWidth <= 0.0f ? 0.0f : std::clamp(hostWidth * 0.22f, 220.0f, 440.0f);
}

float expandedClockWidth(float hostWidth) {
    return hostWidth <= 0.0f ? 0.0f : std::clamp(hostWidth * 0.045f, 80.0f, 96.0f);
}

ExpandedSideLayout expandedSideLayout(float w, bool dock, float resourceWidth) {
    ExpandedSideLayout result;
    result.zoneWidth = expandedSpectrumWidth(w);
    result.resourceWidth = dock ? resourceWidth : 0.0f;
    result.resourceX = w - result.resourceWidth - (dock ? 3.0f : kTextPadding);
    result.zoneX =
        dock ? result.resourceX - 10.0f - result.zoneWidth : w - result.zoneWidth - kTextPadding;
    result.clockWidth = dock ? 0.0f : std::min(result.zoneWidth, expandedClockWidth(w));
    result.clockX = w - result.clockWidth - kTextPadding;
    result.spectrumRight = dock ? result.resourceX - 10.0f : result.clockX - 10.0f;
    result.spectrumWidth = std::max(0.0f, result.spectrumRight - result.zoneX);
    return result;
}

float pinnedInlineControlsIdealWidth(float heightDip, bool canRepeat, float lyricFontSize,
                                     bool canSwitchSource) {
    if (heightDip <= 0.0f)
        return 0.0f;
    return buildPinnedTierSpec(PinnedInlineTier::Full, heightDip, canRepeat, lyricFontSize,
                               canSwitchSource)
        .slotWidth;
}

PinnedInlineControlsLayout planPinnedInlineControls(const PinnedInlineControlsInput& input) {
    PinnedInlineControlsLayout result;
    if (input.width <= 0.0f || input.height <= 0.0f || input.scene == DisplayScene::Idle ||
        input.scene == DisplayScene::NoPlayback)
        return result;

    const float effW = input.width - input.spectrumWidth;
    const float start = input.songInfoVisible ? kSongInfoLyricGap : kTextPadding;
    const float minKeepFull = input.songInfoVisible ? 152.0f : input.coverSlotWidth + 91.2f;
    const float minKeepCompactFull =
        input.songInfoVisible ? 136.0f : input.coverSlotWidth + 78.0f;
    const float minKeepCore3 = input.songInfoVisible ? 120.0f : input.coverSlotWidth + 68.0f;

    struct TierCandidate {
        PinnedInlineTier tier;
        float minKeep;
    };
    const TierCandidate candidates[] = {
        {PinnedInlineTier::Full, minKeepFull},
        {PinnedInlineTier::CompactFull, minKeepCompactFull},
        {PinnedInlineTier::CompactCore3, minKeepCore3},
    };

    PinnedTierSpec chosen{};
    for (const auto& candidate : candidates) {
        const PinnedTierSpec spec = buildPinnedTierSpec(candidate.tier, input.height, input.canRepeat,
                                                        input.lyricFontSize, input.canSwitchSource);
        // 预留 1 DIP 浮点与像素取整容差，避免最小安全宽度下误降级。
        if (effW - start - spec.slotWidth + 1.0f >= candidate.minKeep) {
            chosen = spec;
            break;
        }
    }
    if (chosen.tier == PinnedInlineTier::FallbackHover || chosen.slotWidth <= 0.0f)
        return result;

    const LayoutMetrics metrics = horizontalLayout(
        {input.width, input.height, input.scene, false, true, input.songInfoVisible,
         input.albumCoverVisible, chosen.slotWidth, input.spectrumWidth, 0.0f,
         input.coverSlotWidth});
    result.tier = chosen.tier;
    result.slotWidth = chosen.slotWidth;
    result.radius = chosen.radius;
    result.centerY = input.height * 0.5f;
    result.boundsLeft = metrics.leftW;
    result.boundsRight = result.boundsLeft + chosen.slotWidth;

    const float firstX = result.boundsLeft + (chosen.slotWidth - chosen.span) * 0.5f;
    for (int i = 0; i < 3; ++i) {
        result.visible[i] = true;
        result.centerX[i] = firstX + static_cast<float>(i) * chosen.spacing;
    }
    if (chosen.includeVolume) {
        if (chosen.includeRepeat) {
            result.visible[3] = true;
            result.centerX[3] = result.centerX[2] + chosen.repeatGap;
            result.visible[4] = true;
            result.centerX[4] = result.centerX[3] + chosen.volumeGap;
        } else {
            result.visible[4] = true;
            result.centerX[4] = result.centerX[2] + chosen.volumeGap;
        }
    }
    if (input.canSwitchSource) {
        result.visible[kInlineSourceControl] = true;
        const int previous = chosen.includeVolume ? 4 : 2;
        result.centerX[kInlineSourceControl] =
            result.centerX[previous] + chosen.spacing + chosen.radius * 0.14f;
    }
    return result;
}

void layoutInlineHoverControls(float left, float width, float height, bool canRepeat,
                               bool canSwitchSource, float (&centers)[kInlineControlCount],
                               float& cy, float& radius) {
    constexpr float pitch = 2.8f;
    constexpr float margin = 6.0f;
    const float repeatExtra = canRepeat ? 0.3f : 0.0f;
    const float volumeExtra = canRepeat ? 0.4f : 0.9f;
    const float sourceExtra = canSwitchSource ? 0.3f : 0.0f;
    const int gaps = 3 + static_cast<int>(canRepeat) + static_cast<int>(canSwitchSource);
    const float span = pitch * gaps + repeatExtra + volumeExtra + sourceExtra;
    cy = height * 0.5f;
    radius = std::min(height * 0.26f, std::max(0.0f, width - margin * 2.0f) / (span + 2.0f));

    const float start = left + (width - span * radius) * 0.5f;
    for (int i = 0; i < 3; ++i)
        centers[i] = start + i * pitch * radius;
    if (canRepeat)
        centers[3] = centers[2] + (pitch + repeatExtra) * radius;
    centers[4] = centers[canRepeat ? 3 : 2] + (pitch + volumeExtra) * radius;
    if (canSwitchSource)
        centers[kInlineSourceControl] = centers[4] + (pitch + sourceExtra) * radius;
}
} // namespace taskbar_detail
