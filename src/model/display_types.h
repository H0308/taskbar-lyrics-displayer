#pragma once
#include <windows.h>
#include <cstdint>

// 设置与显示共同使用的值类型，不依赖窗口或渲染器。
inline constexpr wchar_t kDefaultUiFontFamily[] = L"Noto Sans SC";

enum class UiFontSource { Independent, FollowGeneral, Default };
enum class SecondaryLyricAvailability { Available, Loading, Unavailable };
enum class LyricFontStyle {
    Normal,
    Bold,
    Italic,
    BoldItalic,
};
namespace fluent {
enum class ThemeMode {
    FollowSystem,
    FollowApp,
    Light,
    Dark,
};
}
enum class AlbumCoverEffect {
    Default,
    Vinyl,
};

enum class LyricAlignment {
    Left,
    Center,
    Right,
};

enum class HoverControlStyle {
    Inline,
    Popup,
};

// 内嵌播放控件的显示方式：鼠标悬浮时替换歌词，或常驻在歌曲信息与歌词之间。
enum class InlineControlDisplayMode {
    Hover,
    Always,
};

// 媒体卡片展开方式：悬浮停留片刻后展开，或点击歌词区域任意位置立即展开
enum class MediaPopupTrigger {
    Hover,
    Click,
};

enum class SpectrumStyle {
    Default,
    Bars,
    DreamyWave,
    Segmented,
    BottomBand,
};

// 任务栏歌词背景：无 / 封面模糊 / 纯色（跟随任务栏深浅色）；
// 画在最底层，可与播放进度背景、频谱背景波浪叠加
enum class TaskbarBackground {
    None,
    CoverBlur,
    Solid,
};

enum class TaskbarPlacementStatus {
    Unavailable,
    Safe,
    Compressed,
    Relocated,
    NoSpace,
    ForcedOverlap,
};

// 任务栏可用空白区内的停靠边：Start 对应横向左端/竖向上端，End 对应横向右端/竖向下端。
enum class TaskbarAnchorEdge {
    Start = 0,
    End = 1,
};

// 任务栏锚点规格：0 = 末区末端（通知区域左侧，默认）；1 = 首区起始端（任务栏最左侧）；
// 2 = 按归一化主轴比例与停靠边匹配任意中间空闲区域。
struct TaskbarAnchorSpec {
    int mode = 0;
    double ratio = 1.0;
    TaskbarAnchorEdge edge = TaskbarAnchorEdge::End;

    static constexpr TaskbarAnchorSpec outerEnd() { return {0, 1.0, TaskbarAnchorEdge::End}; }
    static constexpr TaskbarAnchorSpec outerStart() { return {1, 0.0, TaskbarAnchorEdge::Start}; }
    static constexpr TaskbarAnchorSpec relative(double anchorRatio, TaskbarAnchorEdge anchorEdge) {
        const double clamped =
            !(anchorRatio >= 0.0) ? 0.0 : (anchorRatio > 1.0 ? 1.0 : anchorRatio);
        return {2, clamped, anchorEdge};
    }

    bool operator==(const TaskbarAnchorSpec& other) const {
        if (mode != other.mode)
            return false;
        if (mode != 2)
            return true;
        if (edge != other.edge)
            return false;
        const double diff = ratio - other.ratio;
        return (diff < 0.0 ? -diff : diff) < 1e-4;
    }
};

// 提供给托盘/任务栏右键菜单的动态可用锚点描述。
struct TaskbarAnchorOption {
    TaskbarAnchorSpec spec;
    int spanIndex = 0;
    int spanCount = 1;
    bool spanHasBothEdges = false;
    bool vertical = false;
    bool active = false;

    bool operator==(const TaskbarAnchorOption&) const = default;
};

// 内嵌模式资源区所在端：Start 对应横向左侧/竖向顶部，End 对应横向右侧/竖向底部。
enum class ResourcePlacement {
    Start,
    End,
};

// 复用 Dock 资源区；所有监控项均可独立开关，按启用顺序紧凑排列。
struct DockResourceVisibility {
    bool embeddedEnabled = false;
    bool cpuUsage = true;
    bool memoryUsage = true;
    bool download = true;
    bool upload = true;
    bool gpuUsage = false;
    bool cpuFrequency = false;
    bool smallFont = false; // 性能监测用小一号字，块与块之间更紧凑
    bool hideLabels = false; // 隐藏指标标签，常态仅显示数值，翻页或悬停时短暂提示
    ResourcePlacement placement = ResourcePlacement::End; // 仅内嵌模式生效，Dock 固定在右侧
    constexpr bool hasAnyMetric() const noexcept {
        return cpuUsage || memoryUsage || download || upload || gpuUsage || cpuFrequency;
    }
    bool operator==(const DockResourceVisibility&) const = default;
};

// 任务栏内容动态背景；按作用范围覆盖实际显示的内容区域，不用于悬浮卡片。
enum class IdleQuoteBackground {
    None,
    FallingLeaves,
    TwinklingStars,
    BinaryRain,
    FloatingParticles,
};

// 作用范围：不启用 / 每日一言和资源监视 / 封面、歌曲信息、歌词和频谱 / 整体。
enum class IdleQuoteBackgroundScope {
    None,
    DailyQuote,
    Lyrics,
    All,
};

// 任务栏歌词的承载方式。嵌入模式只占用任务栏中的一段空闲区，沉浸模式
// 覆盖当前任务栏客户区，AppBar 作为独立顶栏/底栏保留桌面工作区。
enum class TaskbarViewMode {
    Embedded,
    Immersive,
    AppBar,
};

enum class AppBarEdge {
    Top,
    Bottom,
};

enum class RenderMode {
    Normal = 0,
    Low = 1,
    Stopped = 2,
    Minimal = 3,
};
enum class MediaPopupBackground {
    Solid,
    Frosted,
};

// 正式展示的语义场景，由展示帧、任务栏宿主与绘制组件共同使用。
enum class DisplayScene {
    NoPlayback,
    Idle,
    Searching,
    Lyrics,
    Message,
    Spectrum,
};

struct LyricAppearance {
    COLORREF played = RGB(49, 194, 124);
    COLORREF unplayed = RGB(49, 194, 124);
    int unplayedAlphaPct = 45;
    COLORREF glowColor = RGB(49, 194, 124);
    COLORREF outlineColor = RGB(0, 0, 0);
    bool glowOn = false;
    bool outlineOn = false;
};
