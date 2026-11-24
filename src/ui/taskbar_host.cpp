#include "taskbar_host.h"
#include "ui/font_sizes.h"
#include "lyric/lyric_provider.h"
#include "lyric/simulated_karaoke.h"
#include "media/player_source.h"
#include "ui/taskbar/vinyl_animation.h"
#include "ui/taskbar/vinyl_renderer.h"
#include "ui/taskbar/idle_background_renderer.h"
#include "ui/taskbar/lyric_transition.h"
#include "ui/taskbar/scene_transition.h"
#include "ui/taskbar/lyric_typesetter.h"
#include "ui/taskbar/karaoke_tracker.h"
#include "ui/taskbar/text_scroll.h"
#include "ui/taskbar/lyric_text_renderer.h"
#include "ui/taskbar/interaction.h"
#include "ui/taskbar/lyric_effects_renderer.h"
#include "ui/taskbar/appbar_registration.h"
#include "ui/taskbar/placement.h"
#include "ui/taskbar/placement_probe.h"
using taskbar_detail::WindowPlacement;
#include "ui/taskbar/spectrum_renderer.h"
#include "ui/taskbar/animation_clock.h"
#include "ui/taskbar/frame_scheduler.h"
using taskbar_detail::kImmersiveSpectrumBarCount;
using taskbar_detail::kImmersiveSpectrumWideBarCount;
using taskbar_detail::kImmersiveSpectrumWideZoneThreshold;
using taskbar_detail::kSpectrumBarRadius;
using taskbar_detail::kSpectrumBarW;
using taskbar_detail::kSpectrumBottomPadding;
using taskbar_detail::kSpectrumGap;
using taskbar_detail::monotonicNowMs;
#include "ui/taskbar/render_state.h"
#include "ui/taskbar/platform.h"
#include "ui/taskbar/window_attachment.h"
#include "ui/taskbar/layout.h"

using taskbar_detail::findTrafficMonitorWnd;
using taskbar_detail::InvalidationSnapshot;
using taskbar_detail::isTaskbarCenterAlign;
using taskbar_detail::isValidTaskbarEdge;
using taskbar_detail::isVerticalTaskbarEdge;
using taskbar_detail::kCoverPadding;
using taskbar_detail::kLeftRatio;
using taskbar_detail::kSongInfoLyricGap;
using taskbar_detail::kTextPadding;
using taskbar_detail::kVerticalControlsGap;
using taskbar_detail::kVerticalLyricGap;
using taskbar_detail::LayoutMetrics;
using taskbar_detail::queryKugouTaskbarWindows;
using taskbar_detail::queryTaskbarButtonsUia;
using taskbar_detail::queryTaskbarEdge;
using taskbar_detail::RenderInvalidation;
using taskbar_detail::RenderInvalidationMask;
using taskbar_detail::RenderState;
using taskbar_detail::taskbarCrossPixels;
using taskbar_detail::toMask;
using taskbar_detail::VerticalLayout;
#include "logging/runtime_logger.h"
#include "ui/app_icon.h"
#include "fluent_theme.h"
#include "dock_pet.h"
#include "graphics/composition_renderer.h"
#include "lyric_star.h"
#include "media_control_icons.h"
#include "media_popup.h"
#include "ui/taskbar/resource_panel.h"
#include "ui/taskbar/cover_image.h"
#include "dock_backdrop.h"
#include "platform_icon.h"
#include "progress_popup.h"
#include "settings_icons.h"
#include "volume_popup.h"

#include <d2d1.h>
#include <d2d1effects.h>
#include <dwrite.h>
#include <objbase.h>
#include <shellapi.h>
#include <uiautomation.h>
#include <windowsx.h>

#include <algorithm>
#include <utility>
#include <wrl/client.h>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <cwchar>
#include <functional>
#include <iterator>
#include <memory>
#include <optional>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

namespace {

constexpr UINT kFrameMessage = WM_APP + 41;
constexpr UINT_PTR kTaskbarAttachTimerId = 3;
constexpr UINT kTimerPausedMs = 33; // 暂停时 ~30fps：超长文本继续滚动，任务栏合成开销减半
constexpr UINT kTaskbarAttachRetryMs = 250;
constexpr UINT_PTR kPlacementTimerId = 4;
// 沉浸模式让出任务栏：悬停确认（单次）与显隐过渡/离开检测（轮询）。
constexpr UINT_PTR kImmersiveYieldTimerId = 5;
constexpr UINT_PTR kImmersivePresenceTimerId = 6;
// 点击循环按钮后，模式名短暂显示一会儿（单次定时器）。
constexpr UINT_PTR kRepeatHintTimerId = 7;
constexpr UINT kRepeatHintMs = 1600;
constexpr UINT kImmersiveYieldDelayMs = 100;   // 悬停非控件区多久后让出任务栏
constexpr UINT kImmersiveHideMs = TaskbarPresence::kHideMs; // 淡出时长
constexpr UINT kImmersiveShowMs = TaskbarPresence::kShowMs; // 淡入时长
constexpr UINT kImmersivePresencePollMs = 20;
constexpr UINT kAppBarCallbackMessage = WM_APP + 40;
constexpr UINT kPlacementTimerMs = 500; // 隐藏时只拾取避让结果，不运行完整渲染帧
constexpr UINT kProbeIntervalMs = 3000;
constexpr UINT kNoSpaceProbeIntervalMs = 1000;
constexpr int kSlowTickInterval = 15; // 慢速分支（任务栏位置跟踪等）每 15 帧一次，约 250ms
constexpr wchar_t kWndClassName[] = L"QQMusicLyricTaskbar";

constexpr float kMinWidthDip = 160.0f;
constexpr float kMaxWidthDip = 280.0f;
constexpr float kCompressedMinWidthRatio = 0.5f;

constexpr wchar_t kDragPreviewText[] = L"松手固定到这里";
constexpr float kLyricDragThresholdDip = 6.0f;
// 滚动文本左缘（滚出侧）的渐隐宽度
constexpr float kLyricEdgeFadeDip = 18.0f;
// 歌曲信息与歌词之间的分隔间距
constexpr float kSongInfoDividerWidth = 1.0f;
constexpr float kSongInfoDividerInset = 5.0f;
constexpr float kCornerRadius = 8.0f;
constexpr float kVerticalMinLengthDip = 220.0f;

constexpr float kLyricTransitionMs = 280.0f;         // 相邻歌词上下切换时长
constexpr float kSceneTransitionMs = taskbar_detail::SceneTransition::kDurationMs;
constexpr float kSongTransitionMs = 220.0f;          // 切歌时新内容滑入时长
constexpr float kSongTransitionTravelDip = 24.0f;    // 切歌时新内容的水平入场距离
constexpr float kLyricPreviewGap = 3.0f;             // 普通双行模式的核心行与下一行间距
constexpr float kLyricPreviewOpacity = 0.90f;        // 下一行预览透明度
constexpr int kImmersiveControlPrevious = 0;
constexpr int kImmersiveControlPlayPause = 1;
constexpr int kImmersiveControlNext = 2;
constexpr int kImmersiveControlRepeat = 3;
constexpr int kImmersiveControlVolume = 4;
using taskbar_detail::kInlineControlCount;
using taskbar_detail::kInlineProgressControl;
using taskbar_detail::kInlineSourceControl;
constexpr int kImmersiveControlExit = 6;
// Dock 的快捷应用入口使用独立图标和命中项，沉浸模式不显示。
constexpr int kDockControlQuickApps = 7;
constexpr int kImmersiveControlMenu = 8;
constexpr int kImmersiveControlProgress = 9;
constexpr int kExpandedControlCount = 10;
constexpr float kAppBarHeightDip = 48.0f;
constexpr float kImmersiveControlRadiusFactor = 0.24f;
constexpr float kImmersiveControlMinRadius = 8.0f;
constexpr float kImmersiveControlMaxRadius = 12.0f;
constexpr float kImmersiveControlPitchFactor = 3.0f;
constexpr float kLyricPreviewScale =
    fluent::font_sizes::taskbar::previewScale / fluent::font_sizes::taskbar::mainScale;
constexpr float kCompactTaskbarHeightDip = 36.0f;


constexpr float kImmersiveSpectrumClockGap = 10.0f;
constexpr float kDockResourceZoneGap = 10.0f;
constexpr float kDockResourceRightPadding = 3.0f;
constexpr float kDockPetSeatW = 42.0f;
constexpr float kPetLaneMinWidthDip = 44.0f;

// 读取注册表 DWORD，失败返回默认值

bool sameLyricLine(const LyricLine& a, const LyricLine& b) {
    if (a.ms != b.ms || a.text != b.text || a.translation != b.translation ||
        a.romanization != b.romanization || a.chars.size() != b.chars.size())
        return false;
    for (size_t i = 0; i < a.chars.size(); ++i) {
        const LyricChar& ac = a.chars[i];
        const LyricChar& bc = b.chars[i];
        if (ac.startMs != bc.startMs || ac.endMs != bc.endMs || ac.text != bc.text)
            return false;
    }
    return true;
}

bool sameLyrics(const std::vector<LyricLine>& a, const std::vector<LyricLine>& b) {
    if (a.size() != b.size())
        return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (!sameLyricLine(a[i], b[i]))
            return false;
    }
    return true;
}

// 这些位表示“需要重新处理什么”，不再分别由多个布尔量表达。它们不是
// 生命周期阶段：阶段使用下面 RenderState 中的枚举表示，失效位只描述待处理工作。
} // namespace

struct TaskbarHost::Impl : std::enable_shared_from_this<TaskbarHost::Impl> {

    bool destroying_ = false;

    mutable SpectrumRenderer spectrum_;
    SpectrumContext spectrumContext(ID2D1DeviceContext* target = nullptr) const {
        SpectrumContext context;
        context.target = target;
        context.visible = spectrumVisible_;
        context.animations = clientAnimations_;
        context.scene = scene_;
        context.nowMs = frameNowMs_;
        context.style = spectrumStyle_;
        context.customColor = spectrumCustomColor_;
        context.color = spectrumColor_;
        context.followAlbum = spectrumFollowAlbum_;
        context.albumColor = spectrumAlbumColor_;
        context.hasAlbumColor = spectrumAlbumColorAvailable_;
        context.gradient = spectrumGradient_;
        context.effects = spectrumEffects_;
        context.background = spectrumBackground_;
        context.opacity = spectrumOpacityPct_;
        context.playedColor = lyricColor_;
        context.compactHeight = compactTaskbarHeight_;
        context.playing = media.playing;
        context.minimal = isMinimalMode();
        context.stopped = isStoppedMode();
        context.horizontalImmersive = horizontalImmersiveMode();
        return context;
    }
    mutable LyricEffectsRenderer lyricEffectsRenderer_;
    LyricEffectsContext lyricEffectsContext(ID2D1DeviceContext* target = nullptr) const {
        LyricEffectsContext context;
        context.target = target;
        context.lines = &lines;
        context.effects = lyricEffects_;
        context.animations = clientAnimations_;
        context.scene = scene_;
        context.currentLine = lyricEffectLayoutLine_;
        context.outgoingLine = outgoingLyricEffectLine_;
        context.currentLayout = typesetter_.resources().lyricLayout_.Get();
        context.outgoingLayout = typesetter_.resources().outgoingLyricLayout_.Get();
        context.width = lyricWidth_;
        context.outgoingWidth = outgoingLyricWidth_;
        context.positionMs = positionMs_;
        context.outgoingDoubleLine = transition_.outgoingDoubleLine();
        context.compactHeight = compactTaskbarHeight_;
        context.doubleLineEnabled = doubleLineLyricsEnabled_;
        context.color = lyricColor_;
        context.glow = lyricGlow_;
        context.outline = lyricOutline_;
        context.fontSize = fontSize_;
        context.outlineBrush = brushLyricOutline_.Get();
        context.playing = media.playing;
        context.minimal = isMinimalMode();
        context.stopped = isStoppedMode();
        context.vertical = isVerticalTaskbar();
        context.currentDoubleLine = useDoubleLineLyrics();
        return context;
    }
    LyricTypesetter typesetter_;
    LyricTransition transition_;
    VinylAnimation vinyl_;
    RenderState renderState_;
    std::optional<TaskbarAppearance> appearance_;
    unsigned updateDepth_ = 0;
    bool committingUpdates_ = false;
    bool rendering_ = false;
    bool positionPending_ = false;
    bool brushesPending_ = false;
    bool formatsPending_ = false;
    bool paintPending_ = false;

    // 完整状态写入期间只收集副作用，最外层提交统一执行资源、定位和绘制。
    class UpdateScope {
    public:
        explicit UpdateScope(Impl& owner) : owner_(owner) { ++owner_.updateDepth_; }
        ~UpdateScope() {
            if (--owner_.updateDepth_ == 0 && !owner_.committingUpdates_)
                owner_.commitPendingUpdates();
        }
        UpdateScope(const UpdateScope&) = delete;
        UpdateScope& operator=(const UpdateScope&) = delete;
    private:
        Impl& owner_;
    };

    void commitPendingUpdates() {
        committingUpdates_ = true;
        // 定位产生的同步窗口消息可能追加设置更新；先处理完资源与位置，再画一帧。
        while (formatsPending_ || brushesPending_ || positionPending_) {
            if (std::exchange(formatsPending_, false))
                recreateFormats();
            if (std::exchange(brushesPending_, false))
                createLyricBrushes();
            if (std::exchange(positionPending_, false))
                adjustPosition();
        }
        committingUpdates_ = false;
        if (std::exchange(paintPending_, false))
            flushRenderRequest();
    }

    void applyAppearance(const TaskbarAppearance& value) {
        const auto lifetime = shared_from_this();
        if (appearance_ && *appearance_ == value)
            return;
        hoverTip_.hide();
        sourceWheelRemainder_ = 0;
        const UpdateScope update(*this);
        const bool disableSongTransition =
            !value.songTransitionEnabled && (!appearance_ || appearance_->songTransitionEnabled);
        applyTextAppearance(value);
        applyPopupAppearance(value);
        applyPlaybackVisualAppearance(value);
        applyBackgroundAppearance(value);
        applyCoverAndFontAppearance(value);
        appearance_ = value;
        if (disableSongTransition)
            cancelSongTransition();
        refreshFrameTimer();
    }

    void applyTextAppearance(const TaskbarAppearance& value) {
        if (!appearance_ || appearance_->secondaryLyricMode != value.secondaryLyricMode)
            setSecondaryLyricMode(value.secondaryLyricMode.translation,
                                  value.secondaryLyricMode.romanization);
        if (!appearance_ || appearance_->doubleLineLyrics != value.doubleLineLyrics)
            setDoubleLineLyrics(value.doubleLineLyrics);
        if (!appearance_ ||
            appearance_->simulateWordByWordLyrics != value.simulateWordByWordLyrics)
            setSimulateWordByWordLyrics(value.simulateWordByWordLyrics);
        if (!appearance_ || appearance_->lyricEffects != value.lyricEffects)
            setLyricEffects(value.lyricEffects);
        if (!appearance_ || appearance_->lyricAlignment != value.lyricAlignment)
            setLyricAlignment(value.lyricAlignment);
        if (!appearance_ || appearance_->idleQuoteAlignment != value.idleQuoteAlignment)
            setIdleQuoteAlignment(value.idleQuoteAlignment);
        if (!appearance_ || appearance_->songInfoVisible != value.songInfoVisible)
            setSongInfoVisible(value.songInfoVisible);
    }

    void applyPopupAppearance(const TaskbarAppearance& value) {
        if (!appearance_ || appearance_->songTransitionEnabled != value.songTransitionEnabled)
            mediaPopup.setSongTransitionEnabled(value.songTransitionEnabled);
        if (!appearance_ || appearance_->controlsOnHover != value.controlsOnHover)
            setControlsOnHover(value.controlsOnHover);
        if (!appearance_ || appearance_->hoverControlStyle != value.hoverControlStyle)
            setHoverControlStyle(value.hoverControlStyle);
        if (!appearance_ || appearance_->inlineControlDisplayMode != value.inlineControlDisplayMode)
            setInlineControlDisplayMode(value.inlineControlDisplayMode);
        if (!appearance_ || appearance_->floatingCardTrigger != value.floatingCardTrigger)
            setFloatingCardTrigger(value.floatingCardTrigger);
        if (!appearance_ || appearance_->floatingCardBackground != value.floatingCardBackground)
            setFloatingCardBackground(value.floatingCardBackground);
        if (!appearance_ ||
            appearance_->floatingCardBackgroundColor != value.floatingCardBackgroundColor)
            setFloatingCardBackgroundColor(value.floatingCardBackgroundColor.color,
                                           value.floatingCardBackgroundColor.customized);
        if (!appearance_ || appearance_->idleQuoteBackground != value.idleQuoteBackground)
            setIdleQuoteBackground(value.idleQuoteBackground);
        if (!appearance_ || appearance_->idleQuoteBackgroundScope != value.idleQuoteBackgroundScope)
            setIdleQuoteBackgroundScope(value.idleQuoteBackgroundScope);
        if (!appearance_ || appearance_->floatingCardFollowAlbum != value.floatingCardFollowAlbum)
            setFloatingCardFollowAlbum(value.floatingCardFollowAlbum);
        if (!appearance_ ||
            appearance_->floatingCardAutoTextContrast != value.floatingCardAutoTextContrast)
            setFloatingCardAutoTextContrast(value.floatingCardAutoTextContrast);
    }

    void applyPlaybackVisualAppearance(const TaskbarAppearance& value) {
        if (!appearance_ || appearance_->albumCoverEffect != value.albumCoverEffect)
            setAlbumCoverEffect(value.albumCoverEffect);
        if (!appearance_ || appearance_->spectrumStyle != value.spectrumStyle)
            setSpectrumStyle(value.spectrumStyle);
        if (!appearance_ || appearance_->spectrumColor != value.spectrumColor)
            setSpectrumColor(value.spectrumColor.color, value.spectrumColor.customized,
                             value.spectrumColor.followAlbum);
        if (!appearance_ || appearance_->spectrumAlbumColor != value.spectrumAlbumColor)
            setSpectrumAlbumColor(value.spectrumAlbumColor.color,
                                  value.spectrumAlbumColor.available);
        if (!appearance_ || appearance_->spectrumGradient != value.spectrumGradient)
            setSpectrumGradient(value.spectrumGradient);
        if (!appearance_ || appearance_->spectrumEffects != value.spectrumEffects)
            setSpectrumEffects(value.spectrumEffects);
        if (!appearance_ || appearance_->spectrumBackground != value.spectrumBackground)
            setSpectrumBackground(value.spectrumBackground);
        if (!appearance_ || appearance_->spectrumOpacity != value.spectrumOpacity)
            setSpectrumOpacity(value.spectrumOpacity);
        if (!appearance_ || appearance_->progressBackground != value.progressBackground)
            setProgressBackground(value.progressBackground);
        if (!appearance_ ||
            appearance_->progressBackgroundOpacity != value.progressBackgroundOpacity)
            setProgressBackgroundOpacity(value.progressBackgroundOpacity);
    }

    void applyBackgroundAppearance(const TaskbarAppearance& value) {
        if (!appearance_ || appearance_->background != value.background)
            setBackground(value.background);
        if (!appearance_ || appearance_->coverBackgroundOpacity != value.coverBackgroundOpacity)
            setCoverBackgroundOpacity(value.coverBackgroundOpacity);
        if (!appearance_ ||
            appearance_->immersiveBackgroundAdjustment != value.immersiveBackgroundAdjustment)
            setImmersiveBackgroundAdjustment(value.immersiveBackgroundAdjustment);
        if (!appearance_ || appearance_->dockBackgroundAdjustment != value.dockBackgroundAdjustment)
            setDockBackgroundAdjustment(value.dockBackgroundAdjustment);
        if (!appearance_ || appearance_->immersiveBackgroundBlur != value.immersiveBackgroundBlur)
            setImmersiveBackgroundBlur(value.immersiveBackgroundBlur);
        if (!appearance_ || appearance_->dockBackgroundBlur != value.dockBackgroundBlur)
            setDockBackgroundBlur(value.dockBackgroundBlur);
        if (!appearance_ || appearance_->dockResourceVisibility != value.dockResourceVisibility)
            setDockResourceVisibility(value.dockResourceVisibility);
    }

    void applyCoverAndFontAppearance(const TaskbarAppearance& value) {
        if (!appearance_ || appearance_->lyricPlayedGradient != value.lyricPlayedGradient)
            requestFrameAndFlush();
        if (!appearance_ || appearance_->albumCoverVisible != value.albumCoverVisible)
            setAlbumCoverVisible(value.albumCoverVisible);
        if (!appearance_ || appearance_->platformIconVisible != value.platformIconVisible)
            setPlatformIconVisible(value.platformIconVisible);
        if (!appearance_ || appearance_->contextMenuEnabled != value.contextMenuEnabled)
            contextMenuEnabled_ = value.contextMenuEnabled;
        if (!appearance_ || appearance_->fontColors != value.fontColors)
            setFontColors(value.fontColors.played, value.fontColors.unplayed,
                          value.fontColors.unplayedAlphaPct);
        if (!appearance_ || appearance_->fontGlow != value.fontGlow)
            setFontGlow(value.fontGlow);
        if (!appearance_ || appearance_->fontOutline != value.fontOutline)
            setFontOutline(value.fontOutline);
        if (!appearance_ || appearance_->fontGlowColors != value.fontGlowColors)
            setFontGlowColors(value.fontGlowColors.glow, value.fontGlowColors.outline);
    }

    HINSTANCE inst = nullptr;
    HWND hwnd = nullptr;
    bool timerRunning_ = false;
    double timerMs_ = 0;
    taskbar_detail::FrameScheduler frameScheduler_;
    bool placementTimerRunning_ = false;
    UINT displayRefreshHz_ = 60;
    std::wstring frameDisplayName_;
    bool allowOverlap_ = false;
    bool probeReady_ = false;
    TaskbarPlacementStatus placementStatus_ = TaskbarPlacementStatus::Unavailable;
    std::function<void(TaskbarPlacementStatus)> onPlacementStatusChanged_;

    // 任务栏句柄与子部件
    HWND taskbar_ = nullptr;
    HMONITOR targetMonitor_ = nullptr;
    std::wstring targetMonitorDeviceName_;
    TaskbarWindowAttachment attachment_;
    ULONGLONG floatingZOrderCooldownUntilMs_ = 0;
    AppBarRegistration appBar_;
    AppBarEdge appBarEdge_ = AppBarEdge::Top;
    HWND notify_ = nullptr;
    bool notifyIsSecondaryClock_ = false;
    HWND start_ = nullptr;
    RECT rcTaskbar_{};        // 任务栏屏幕坐标（缓存）
    RECT rcNotify_{};         // 通知区屏幕坐标（缓存）
    RECT rcStart_{};          // 开始按钮屏幕坐标（缓存，找不到时为空）
    RECT rcTrafficMonitor_{}; // TrafficMonitor 屏幕坐标（缓存，未运行时为 empty）
    // 任务栏 XAML 按钮（开始/搜索/任务视图/小组件/应用图标）包围矩形缓存（屏幕坐标）。
    // 以上探测数据由工作线程产出、UI 线程拾取（由 PlacementProbe 交付）
    std::vector<RECT> uiaButtons_;
    // 酷狗进程中与任务栏相交的可见窗口（屏幕坐标）。
    std::vector<RECT> kugouTaskbarWindows_;
    UINT dpi_ = 96;
    bool centerAlign_ = true;
    UINT taskbarEdge_ = ABE_BOTTOM;
    bool lightTheme_ = false;
    TaskbarAnchorSpec anchor_ = TaskbarAnchorSpec::outerEnd();

    // 工作线程只持有独立状态，不持有 Impl；窗口销毁只发停止信号，不能 join。
    // 最后一个持有者负责回收尚未拾取的结果，允许跨进程调用晚于窗口销毁返回。
    PlacementProbe placementProbe_;

    // 歌词状态：sourceLines_ 保持原始歌词不变，lines 为渲染使用的派生视图。
    std::vector<LyricLine> sourceLines_;
    std::vector<LyricLine> lines;
    std::wstring statusText = L"等待播放…";
    bool statusTextIsTrackInfo = false;
    std::function<void()> onStatusTextCycleCompleted_;
    int currentLine = -1;
    int64_t positionMs_ = 0; // 播放进度（每帧更新），驱动逐字高亮
    PlaybackPatch playbackAnchor_;
    std::function<std::array<float, kPresentationSpectrumBands>()> spectrumReader_;
    ULONGLONG playbackAnchorClock_ = 0;
    ULONGLONG lastFrameTick_ = 0;
    std::function<void()> onClose_;
    DisplayScene scene_ = DisplayScene::NoPlayback;
    std::wstring trackKey_;
    uint64_t requestGeneration_ = 0;
    uint64_t frameRevision_ = 0;

    // 任务栏默认字体独立于用户选择的界面字体。
    float fontSize_ = fluent::font_sizes::taskbar::base;
    float compactTaskbarHeight_ = 0.0f;
    std::wstring fontFamily_ = kDefaultUiFontFamily;
    LyricFontStyle fontStyle_ = LyricFontStyle::Normal;

    // 媒体信息
    OverlayMediaInfo media;
    IdlePresentation idle;
    CoverImage cover_;
    Microsoft::WRL::ComPtr<ID2D1Bitmap> platformIconBmp;

    // 交互
    std::function<void()> tick;
    std::function<void(MediaControl, uint64_t, int64_t)> onControl;
    std::function<void(const std::wstring&)> onSourceOpen;
    TaskbarTooltip hoverTip_;
    std::wstring pressedCoverSource_;
    std::function<void()> onImmersiveExit;
    std::function<void(POINT)> onContextMenu;
    std::function<void(POINT)> onImmersiveMenu;
    std::function<void(POINT)> onDockQuickApps;
    std::function<void(TaskbarAnchorSpec)> onAnchorChanged_;
    std::function<void(std::vector<TaskbarAnchorOption>)> onAnchorOptionsChanged_;
    std::vector<TaskbarAnchorOption> publishedAnchorOptions_;
    bool mouseOver_ = false;
    bool resourceHovered_ = false;
    bool verticalResourceHovered_ = false;
    bool trackingLeave_ = false;
    int immersiveControlHover_ = -1;
    int inlineControlHover_ = -1;
    bool inlineSourcePressed_ = false;
    int sourceWheelRemainder_ = 0;
    int immersiveControlPressed_ = -1;
    uint64_t pressedControlSessionGeneration_ = 0;
    bool controlsOnHover_ = true;
    bool contextMenuEnabled_ = true;
    HoverControlStyle hoverControlStyle_ = HoverControlStyle::Inline;
    InlineControlDisplayMode inlineControlDisplayMode_ = InlineControlDisplayMode::Hover;
    MediaPopupTrigger floatingCardTrigger_ = MediaPopupTrigger::Hover;
    MediaPopup mediaPopup;
    bool mediaPopupEnabled_ = false;

    // 沉浸模式默认覆盖任务栏；鼠标悬停在播放控件以外的区域时淡出并隐藏窗口，
    // 把任务栏交还给用户，鼠标离开任务栏后再淡入恢复。
    using ImmersivePresence = TaskbarPresence::Phase;
    TaskbarPresence presence_;
    TaskbarDragGesture drag_;
    std::vector<taskbar_detail::TaskbarAnchorSlot> dragAnchors_;
    bool dragVertical_ = false;
    bool immersiveYieldArmed_ = false;
    bool immersivePresenceTimerRunning_ = false;

    // 应用音量（内嵌控件音量图标 + 悬停滑块浮窗）
    AppVolumeState appVolume_;
    VolumePopup volumePopup_;
    std::function<void(AppVolumeRequest)> onAppVolume;
    bool volumeHover_ = false;
    // 播放进度（内嵌控件进度图标 + 悬停单行滑块浮窗）
    ProgressPopup progressPopup_;
    bool progressHover_ = false;

    bool inlineProgressAvailable() const {
        return scene_ != DisplayScene::Idle && scene_ != DisplayScene::NoPlayback &&
               media.canSeek && media.durationMs > 0;
    }

    void syncProgressPopup() {
        progressPopup_.setProgress(media.sessionGeneration, positionMs_, media.durationMs,
                                   inlineProgressAvailable());
    }

    void hideProgressPopup() {
        progressHover_ = false;
        progressPopup_.onAnchorLeave();
        progressPopup_.hide();
    }

    KaraokeTracker karaoke_;
    TextScroll scroll_;
    LyricTextRenderer lyricTextRenderer_;

    // 滚动字幕（歌名、歌手、歌词独立滚动）
    float titleWidth_ = 0.0f;
    float titleHeight_ = 0.0f;
    float artistWidth_ = 0.0f;
    float artistHeight_ = 0.0f;
    float lyricWidth_ = 0.0f;
    float lyricHeight_ = 0.0f;
    float secondaryWidth_ = 0.0f;
    float secondaryHeight_ = 0.0f;
    std::wstring lastTitle_;
    std::wstring lastArtist_;
    std::wstring lastLyric_;
    std::wstring lastSecondary_;
    std::wstring clockTimeText_;
    std::wstring clockDateText_;
    uint64_t clockMinuteKey_ = 0;
    ResourcePanel resourcePanel_;
    int slowTick_ = 0; // 慢速分支计数器

    // 渲染

    CompositionRenderer renderer;
    uint64_t deviceGeneration_ = 0;
    // 沉浸模式切歌时，歌曲内容会先绘制到独立的 DirectComposition 表面。
    // 绘制辅助函数统一从这里取目标，普通帧保持使用交换链上下文。
    ID2D1DeviceContext* drawTargetOverride_ = nullptr;
    int lyricEffectLayoutLine_ = -1;
    int outgoingLyricEffectLine_ = -1;
    uint32_t lyricEffects_ = 0;
    bool lyricFxWasAnimating_ = false;

    // 侧边任务栏使用独立的竖排布局；中文/符号逐字排列，连续英文片段整组旋转。
    // 横向布局仍保留给上下任务栏和逐字时间轴计算，避免两种排版互相改变测量结果。
    // 行切换动画保留上一帧的布局，避免新行直接替换导致文字瞬移。
    bool lyricLayoutDoubleLine_ = false;
    float outgoingLyricWidth_ = 0.0f;
    float outgoingLyricHeight_ = 0.0f;
    float verticalLyricWidth_ = 0.0f;
    float verticalLyricHeight_ = 0.0f;
    bool verticalLyricRotated_ = false;
    VerticalLyricColumns verticalLyricColumns_;
    float outgoingVerticalLyricWidth_ = 0.0f;
    float outgoingVerticalLyricHeight_ = 0.0f;
    bool outgoingVerticalLyricRotated_ = false;
    VerticalLyricColumns outgoingVerticalLyricColumns_;
    float outgoingLyricBlockHeight_ = 0.0f;
    float outgoingLyricScrollOffset_ = 0.0f;
    float outgoingSecondaryWidth_ = 0.0f;
    float outgoingSecondaryHeight_ = 0.0f;
    float outgoingSecondaryScrollOffset_ = 0.0f;
    float outgoingNextLyricWidth_ = 0.0f;
    float outgoingNextLyricHeight_ = 0.0f;
    float nextLyricWidth_ = 0.0f;
    float nextLyricHeight_ = 0.0f;

    // 行过渡目标：currentLine 是逻辑当前行；transition_.target() 是本次动画要进入的行，
    // transition_.pending() 是动画期间收到的最新目标（latest-frame-wins，只覆盖不累积），
    // 动画结束收尾时统一消费，不让旧的结束逻辑覆盖新行。

    ULONGLONG frameNowMs_ = 0;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brushBg_;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brushHover_;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brushText_;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brushDim_;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brushBtn_;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brushBtnDisabled_;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brushLyric_;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brushLyricDim_;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brushLyricGlow_;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brushLyricOutline_;
    std::wstring repeatHintText_;     // 提示词文本（等回读结果确定后再填）
    int repeatHintTarget_ = -1;       // 本次点击期望的模式；>=0 表示正在等结果
    ULONGLONG repeatHintUntil_ = 0;   // 提示词消失时刻（0 表示未激活）
    taskbar_detail::TaskbarVinylRenderer vinylRenderer_;
    COLORREF lyricColor_ = RGB(49, 194, 124);         // 已播放颜色，默认 QQ 绿
    COLORREF lyricUnplayedColor_ = RGB(49, 194, 124); // 逐字未播放颜色
    int lyricUnplayedAlphaPct_ = 45;                  // 逐字未播放不透明度（%）
    COLORREF lyricGlowColor_ = RGB(49, 194, 124);     // 光晕颜色
    COLORREF lyricOutlineColor_ = RGB(0, 0, 0);       // 描边颜色
    bool lyricGlow_ = false;                          // 光晕开关
    bool lyricOutline_ = false;                       // 描边开关
    bool translationEnabled_ = true;
    bool romanizationEnabled_ = false;
    bool doubleLineLyricsEnabled_ = false;
    bool simulateWordByWordLyrics_ = false;
    LyricAlignment lyricAlignment_ = LyricAlignment::Left;
    LyricAlignment idleQuoteAlignment_ = LyricAlignment::Left;
    IdleQuoteBackground idleQuoteBackground_ = IdleQuoteBackground::None;
    IdleQuoteBackgroundScope idleQuoteBackgroundScope_ = IdleQuoteBackgroundScope::DailyQuote;
    bool songInfoVisible_ = true;
    bool albumCoverVisible_ = true;
    bool platformIconVisible_ = false;
    AlbumCoverEffect albumCoverEffect_ = AlbumCoverEffect::Default;
    bool clientAnimations_ = true;
    // 渲染模式：0 正常；1 低渲染（播放中也固定 ~30fps）；2 完全停止（窗口隐藏、
    // 帧定时器停止、GPU 设备释放，数据状态保留在内存）；3 极简（保留歌词刷新率，
    // 仅关闭附加视觉、媒体卡片和切歌弹窗）
    // 频谱：基色可跟随已播放色、专辑主题色或自定义色（createLyricBrushes），
    // bands 由 UI 线程每帧写入
    SpectrumStyle spectrumStyle_ = SpectrumStyle::Default;
    COLORREF spectrumColor_ = RGB(49, 194, 124);
    bool spectrumCustomColor_ = false;
    bool spectrumFollowAlbum_ = false;
    COLORREF spectrumAlbumColor_ = RGB(49, 194, 124);
    bool spectrumAlbumColorAvailable_ = false;
    bool spectrumGradient_ = false;
    uint32_t spectrumEffects_ = 0;
    bool spectrumBackground_ = false;
    int spectrumOpacityPct_ = 40;
    // 播放进度背景：与背景波浪互斥，颜色取专辑主色（无封面时回退系统强调色）
    bool progressBackground_ = false;
    int progressBackgroundOpacityPct_ = 25;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brushProgressBg_;
    // 任务栏歌词背景：封面模糊（GaussianBlur → Scale 效果链 + 主题遮罩）或纯色；
    // 画在最底层，可与进度背景、背景波浪叠加
    TaskbarBackground background_ = TaskbarBackground::None;
    int coverBackgroundOpacityPct_ = 60;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brushBackground_;
    // 沉浸模式使用完整任务栏客户区；AppBar 使用独立的 Shell 工作区协议。
    TaskbarViewMode viewMode_ = TaskbarViewMode::Embedded;
    int immersiveBackgroundBlurPct_ = 88;
    int dockBackgroundBlurPct_ = 88;
    int immersiveBackgroundAdjustment_ = 0;
    int dockBackgroundAdjustment_ = 0;
    bool backgroundBlurSupported_ = true;
    DockBackdrop dockBackdrop_;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brushIdleWarm_;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brushIdleCool_;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brushIdleAccent_;
    DockPet dockPet_;
    // 伴听宠物已见到的曲目（title|artist），用于检测"播放中切歌"并触发雀跃。
    std::wstring dockPetTrack_;
    Microsoft::WRL::ComPtr<ID2D1StrokeStyle> dragPreviewStroke_;
    bool spectrumVisible_ = false;
    Microsoft::WRL::ComPtr<ID2D1RoundedRectangleGeometry> coverClip_;
    Microsoft::WRL::ComPtr<ID2D1Layer> coverLayer_;
    // 滚动歌词左缘渐隐：固定两停止点，渐变轴每帧按滚动偏移更新
    // 右缘渐隐带：完全位于歌词区右缘之外的留白（到频谱/窗口边缘的间隙），
    // 不遮挡可读区域内的文字
    // 歌曲信息与歌词之间的悬浮分隔线，使用上下渐隐的主题色画刷
    Microsoft::WRL::ComPtr<ID2D1LinearGradientBrush> songInfoDividerBrush_;
    media_control::Geometry controlGeometry;
    taskbar_detail::SceneTransition sceneTransition_;
    struct SongContentTransition {
        ULONGLONG startMs = 0;
        bool compositorLayer = false;
        bool pendingConsumed = false;
        bool finishRequested = false;
    };
    std::optional<SongContentTransition> songContentTransition_;
    bool songRootAnimationInstalled_ = false;
    int lastPxW_ = 0;
    int lastPxH_ = 0;
    int lastLogicalPxW_ = 0;
    int lastLogicalPxH_ = 0;
    // 滚动与逐字平滑分别报告是否仍在变化，静止且无脏状态时跳过整帧重绘。
    // 光晕/描边离屏缓存：glow+outline+本体三层只合成一次，滚动/淡变时做 DrawImage 平移，
    // 避免每帧对主歌词重复 16+ 次 DrawTextLayout。双行转场同时绘制旧/新布局，保留两个
    // 缓存槽，避免两套布局在同一帧互相驱逐。布局、画刷或效果组合变化时由效果缓存的失效代次控制。

    bool isWindowVisible() const {
        return renderState_.windowPhase() == RenderState::WindowPhase::Visible;
    }

    bool isSessionVisible() const { return renderState_.sessionVisible(); }

    bool isRenderMode(RenderMode mode) const { return renderState_.mode() == mode; }

    bool isStoppedMode() const { return isRenderMode(RenderMode::Stopped); }

    bool isLyricTransitionPending() const {
        return transition_.phase() == LyricTransition::Phase::Pending;
    }

    bool isLyricTransitionActive() const {
        return transition_.phase() == LyricTransition::Phase::Running;
    }

    bool isLyricTransitionInProgress() const {
        return transition_.phase() != LyricTransition::Phase::Idle;
    }

    void setLyricTransitionPending() { transition_.setPhase(LyricTransition::Phase::Pending); }

    void setLyricTransitionActive() { transition_.setPhase(LyricTransition::Phase::Running); }

    void clearLyricTransitionPhase() { transition_.setPhase(LyricTransition::Phase::Idle); }

    bool isLyricDCompActive() const {
        return renderState_.dcompTransitionPhase() == RenderState::DCompTransitionPhase::Active;
    }

    bool isLyricDCompEndRequested() const {
        return renderState_.dcompTransitionPhase() ==
               RenderState::DCompTransitionPhase::EndRequested;
    }

    void requestLyricDCompEnd() {
        if (isLyricDCompActive())
            renderState_.setDCompTransitionPhase(RenderState::DCompTransitionPhase::EndRequested);
    }

    void clearLyricDCompState() {
        renderState_.setDCompTransitionPhase(RenderState::DCompTransitionPhase::Inactive);
    }

    bool isSongTransitionPending() const { return renderState_.songTransitionPending(); }

    void setSongTransitionPending(bool pending) { renderState_.setSongTransitionPending(pending); }

    bool usesSongContentTransition() const {
        return isExpandedView() || (!isVerticalTaskbar() && embeddedResourceVisible());
    }

    bool immersiveSongContentLayerActive() const {
        return songContentTransition_ && songContentTransition_->compositorLayer;
    }

    bool isSceneResizeActive() const { return sceneTransition_.resizing(); }

    void requestInvalidation(RenderInvalidation value) { renderState_.requestInvalidation(value); }

    void requestInvalidation(RenderInvalidationMask mask) {
        renderState_.requestInvalidation(mask);
    }

    bool isInvalidated(RenderInvalidation value) const { return renderState_.isInvalidated(value); }

    void requestFrame() { requestInvalidation(RenderInvalidation::Paint); }

    void flushRenderRequest() {
        if (updateDepth_ != 0 || committingUpdates_) {
            paintPending_ = true;
            return;
        }
        if (rendering_) {
            requestFrame();
            return;
        }
        if (isWindowVisible() && hwnd)
            render();
    }

    void requestFrameAndFlush() {
        requestFrame();
        flushRenderRequest();
    }

    bool isExpandedView() const {
        return viewMode_ == TaskbarViewMode::Immersive || viewMode_ == TaskbarViewMode::AppBar;
    }

    bool isAppBarView() const { return viewMode_ == TaskbarViewMode::AppBar; }

    bool resourceMonitoringEnabled() const {
        return resourcePanel_.visibility().hasAnyMetric() &&
               (isAppBarView() || (viewMode_ == TaskbarViewMode::Embedded &&
                                   resourcePanel_.visibility().embeddedEnabled));
    }

    bool embeddedResourceVisible() const {
        return viewMode_ == TaskbarViewMode::Embedded &&
               resourcePanel_.visibility().embeddedEnabled &&
               resourcePanel_.visibility().hasAnyMetric() && !isVerticalTaskbar();
    }

    bool embeddedResourceAtStart() const {
        return embeddedResourceVisible() &&
               resourcePanel_.visibility().placement == ResourcePlacement::Start;
    }

    bool embeddedResourceOnly() const {
        return embeddedResourceVisible() && scene_ == DisplayScene::Idle && statusText.empty();
    }

    bool verticalResourceVisible() const {
        return viewMode_ == TaskbarViewMode::Embedded &&
               resourcePanel_.visibility().embeddedEnabled &&
               resourcePanel_.visibility().hasAnyMetric() && isVerticalTaskbar();
    }

    bool verticalResourceOnly() const {
        return verticalResourceVisible() && scene_ == DisplayScene::Idle && statusText.empty();
    }

    float resourceZoneWidth() const { return resourcePanel_.width(); }

    float embeddedResourceExtra() const {
        if (embeddedResourceOnly())
            return resourceZoneWidth() + kDockResourceRightPadding;
        return embeddedResourceVisible() ? resourceZoneWidth() + kDockResourceZoneGap : 0.0f;
    }

    float embeddedResourceLeftOffset() const {
        return embeddedResourceAtStart() ? embeddedResourceExtra() : 0.0f;
    }

    float embeddedResourceRightExtra() const {
        return embeddedResourceVisible() && !embeddedResourceAtStart() ? embeddedResourceExtra()
                                                                       : 0.0f;
    }

    bool expandedControlVisible(int index) const {
        if (index < 0 || index >= kExpandedControlCount)
            return false;
        if (index == kImmersiveControlRepeat)
            return media.canRepeat;
        if (index == kImmersiveControlProgress)
            return inlineProgressAvailable();
        if (index == kInlineSourceControl)
            return media.canSwitchSource;
        if (index == kDockControlQuickApps)
            return isAppBarView();
        return true;
    }

    int expandedControlCount() const {
        int count = 0;
        for (int i = 0; i < kExpandedControlCount; ++i) {
            if (expandedControlVisible(i))
                ++count;
        }
        return count;
    }

    bool shouldShowWindow() const {
        if (!hwnd || isStoppedMode() || !isSessionVisible() || renderState_.visibilitySuppressed())
            return false;
        if (presence_.phase() == ImmersivePresence::Hidden)
            return false;
        if (!allowOverlap_ && (placementStatus_ == TaskbarPlacementStatus::NoSpace ||
                               placementStatus_ == TaskbarPlacementStatus::Unavailable))
            return false;
        return true;
    }

    bool reconcileWindowVisibility(bool prewarmBeforeShow = false) {
        const bool wantVisible = shouldShowWindow();
        const bool nativeVisible = hwnd && IsWindowVisible(hwnd);
        const bool appBarRegistrationMismatch =
            isAppBarView() && (wantVisible != appBar_.registered());
        if (wantVisible == isWindowVisible() && wantVisible == nativeVisible &&
            !appBarRegistrationMismatch)
            return false;

        renderState_.setWindowPhase(wantVisible ? RenderState::WindowPhase::Visible
                                                : RenderState::WindowPhase::Hidden);
        if (wantVisible) {
            if (isAppBarView() && !registerAppBar()) {
                renderState_.setWindowPhase(RenderState::WindowPhase::Hidden);
                ShowWindow(hwnd, SW_HIDE);
                setPlacementStatus(TaskbarPlacementStatus::Unavailable);
                return false;
            }
            // 完全停止模式会释放 DComp 设备链。先在原生窗口仍隐藏时完成首帧，
            // 让设备/交换链/字体和位图资源的重建成本不落到用户可见的第一帧。
            if (prewarmBeforeShow)
                requestFrameAndFlush();
            ShowWindow(hwnd, SW_SHOWNA);
            ensureImmersiveZOrder();
            ensureFloatingFallbackZOrder(true);
            startFrameTimer();
        } else {
            stopFrameTimer();
            clearImmersiveSongContentTransition();
            if (hwnd)
                ShowWindow(hwnd, SW_HIDE);
            if (isAppBarView())
                unregisterAppBar();
        }
        return true;
    }

    float scale() const { return static_cast<float>(dpi_) / 96.0f; }

    ID2D1DeviceContext* drawTarget() {
        return drawTargetOverride_ ? drawTargetOverride_ : renderer.renderTarget();
    }
    float dip(int px) const { return static_cast<float>(px) / scale(); }

    bool isVerticalTaskbar() const {
        return !isAppBarView() && isVerticalTaskbarEdge(taskbarEdge_);
    }

    void clientPixelSize(int& width, int& height) const {
        width = 0;
        height = 0;
        if (!hwnd)
            return;
        RECT rc{};
        if (GetClientRect(hwnd, &rc)) {
            width = rc.right - rc.left;
            height = rc.bottom - rc.top;
        }
    }

    void logicalClientPixelSize(int& width, int& height) const { clientPixelSize(width, height); }

    void clientPointToLogicalDip(float clientX, float clientY, float& xDip, float& yDip) const {
        xDip = clientX / scale();
        yDip = clientY / scale();
    }

    POINT logicalDipToClientPoint(float xDip, float yDip) const {
        return POINT{static_cast<LONG>(std::lround(xDip * scale())),
                     static_cast<LONG>(std::lround(yDip * scale()))};
    }

    bool updateMediaInfo(const OverlayMediaInfo& info, bool resetDisc) {
        bool thumbChanged = info.thumbnail != media.thumbnail;
        bool textChanged = info.title != media.title || info.artist != media.artist;
        const bool canRepeatChanged = info.canRepeat != media.canRepeat;
        const bool canSwitchSourceChanged = info.canSwitchSource != media.canSwitchSource;
        bool controlsChanged = info.canPrev != media.canPrev ||
                               info.canPlayPause != media.canPlayPause ||
                               info.canNext != media.canNext ||
                               info.canSeek != media.canSeek ||
                               canRepeatChanged || canSwitchSourceChanged ||
                               info.repeatMode != media.repeatMode;
        bool playingChanged = info.playing != media.playing;
        bool platformChanged = info.sourceAppUserModelId != media.sourceAppUserModelId;
        const bool sessionChanged = info.sessionGeneration != media.sessionGeneration;
        bool dominantColorChanged =
            info.hasDominantColor != media.hasDominantColor ||
            (info.hasDominantColor && info.dominantColor != media.dominantColor);
        const bool previousSeekAvailable = media.canSeek && media.durationMs > 0;
        const bool nextSeekAvailable = info.canSeek && info.durationMs > 0;
        const bool canSeekChanged = previousSeekAvailable != nextSeekAvailable;
        bool durationChanged = info.durationMs != media.durationMs;
        media = info;
        if ((canRepeatChanged || canSwitchSourceChanged || canSeekChanged) &&
            (isExpandedView() || pinnedInlineControlsConfigured())) {
            requestInvalidation(RenderInvalidation::Layout);
            if (drag_.dragging())
                adjustPosition();
            immersiveControlHover_ = -1;
            immersiveControlPressed_ = -1;
            inlineControlHover_ = -1;
            volumeHover_ = false;
            volumePopup_.hide();
            hideProgressPopup();
        }
        if (!nextSeekAvailable) {
            hideProgressPopup();
            if (inlineControlHover_ == kInlineProgressControl)
                inlineControlHover_ = -1;
        }
        if (canSwitchSourceChanged) {
            hoverTip_.hide();
            sourceWheelRemainder_ = 0;
            inlineSourcePressed_ = false;
            if (inlineControlHover_ == kInlineSourceControl)
                inlineControlHover_ = -1;
        }
        if (sessionChanged || !media.canRepeat) {
            if (hwnd && (repeatHintTarget_ >= 0 || repeatHintUntil_ != 0))
                KillTimer(hwnd, kRepeatHintTimerId);
            repeatHintTarget_ = -1;
            repeatHintUntil_ = 0;
        }
        if (!media.canRepeat && inlineControlHover_ == 3)
            inlineControlHover_ = -1;
        if (thumbChanged)
            requestInvalidation(RenderInvalidation::Cover);
        if (platformChanged)
            requestInvalidation(RenderInvalidation::PlatformIcon);
        // 同曲的封面可能随媒体属性刷新重新加载，转盘角度只随曲目切换重置。
        if (resetDisc)
            vinyl_.resetDisc();
        if (textChanged)
            requestInvalidation(RenderInvalidation::SongInfo);
        if (resetDisc || playingChanged)
            vinyl_.resetClock(monotonicNowMs());
        if (playingChanged)
            updateVinylRotation();
        return thumbChanged || textChanged || controlsChanged || playingChanged ||
               platformChanged || dominantColorChanged || durationChanged;
    }

    // ---------- 窗口创建与定位 ----------

    bool findTaskbar() {
        const auto displays = enumerateTaskbarDisplays();
        auto display = std::find_if(displays.begin(), displays.end(), [this](const auto& item) {
            return !targetMonitorDeviceName_.empty() && item.deviceName == targetMonitorDeviceName_;
        });
        if (display == displays.end())
            display = std::find_if(displays.begin(), displays.end(),
                                   [](const auto& item) { return item.primary; });
        if (display == displays.end() && !displays.empty())
            display = displays.begin();
        if (display == displays.end())
            return false;

        targetMonitor_ = display->monitor;
        if (targetMonitorDeviceName_.empty())
            targetMonitorDeviceName_ = display->deviceName;
        taskbar_ = display->taskbarWindow;
        placementProbe_.setTaskbar(taskbar_); // 同步给探测工作线程
        if (!taskbar_ && !isAppBarView())
            return false;
        notify_ = taskbar_ ? FindWindowExW(taskbar_, nullptr, L"TrayNotifyWnd", nullptr) : nullptr;
        notifyIsSecondaryClock_ = false;
        if (!notify_ && taskbar_) {
            notify_ = FindWindowExW(taskbar_, nullptr, L"ClockButton", nullptr);
            notifyIsSecondaryClock_ = notify_ != nullptr;
        }
        start_ = taskbar_ ? FindWindowExW(taskbar_, nullptr, L"Start", nullptr) : nullptr;
        dpi_ = taskbar_ ? GetDpiForWindow(taskbar_) : GetDpiForSystem();
        if (dpi_ == 0)
            dpi_ = 96;
        centerAlign_ = isTaskbarCenterAlign();
        lightTheme_ = !fluent::isDarkMode(fluent::ThemeTarget::Taskbar);
        updateRects();
        if (taskbar_)
            taskbarEdge_ = queryTaskbarEdge(taskbar_, rcTaskbar_);
        else
            taskbarEdge_ = ABE_BOTTOM;
        return true;
    }

    bool notifyWindowActive() const {
        return notify_ != nullptr && (!notifyIsSecondaryClock_ || IsWindowVisible(notify_));
    }

    void updateRects() {
        if (taskbar_)
            GetWindowRect(taskbar_, &rcTaskbar_);
        if (notifyWindowActive())
            GetWindowRect(notify_, &rcNotify_);
        else
            rcNotify_ = {};
        if (start_)
            GetWindowRect(start_, &rcStart_);
        else
            rcStart_ = {};
    }

    void scheduleTaskbarAttachRetry() {
        if (hwnd && !isStoppedMode() && !attachment_.retriesExhausted())
            SetTimer(hwnd, kTaskbarAttachTimerId, kTaskbarAttachRetryMs, nullptr);
    }

    void cancelTaskbarAttachRetry() {
        if (hwnd)
            KillTimer(hwnd, kTaskbarAttachTimerId);
    }

    bool attachToTaskbar(HWND window) { return attachment_.attach(window, taskbar_); }

    bool detachFromTaskbar() {
        cancelTaskbarAttachRetry();
        return attachment_.detach(hwnd);
    }

    HMONITOR appBarMonitor() const {
        if (targetMonitor_)
            return targetMonitor_;
        if (taskbar_ && IsWindow(taskbar_))
            return MonitorFromWindow(taskbar_, MONITOR_DEFAULTTOPRIMARY);
        if (hwnd)
            return MonitorFromWindow(hwnd, MONITOR_DEFAULTTOPRIMARY);
        return MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
    }

    void updateAppBarZOrder() { appBar_.updateZOrder(hwnd); }

    bool updateAppBarPosition() {
        if (destroying_ || !isAppBarView() || !hwnd || !appBar_.registered())
            return false;
        MONITORINFO monitor{};
        monitor.cbSize = sizeof(monitor);
        if (!GetMonitorInfoW(appBarMonitor(), &monitor))
            return false;

        UINT nextDpi = GetDpiForWindow(hwnd);
        if (nextDpi == 0 && taskbar_)
            nextDpi = GetDpiForWindow(taskbar_);
        if (nextDpi == 0)
            nextDpi = 96;
        if (dpi_ != nextDpi) {
            dpi_ = nextDpi;
            renderer.setDpi(dpi_);
            requestInvalidation(toMask(RenderInvalidation::Layout) |
                                toMask(RenderInvalidation::Text) |
                                toMask(RenderInvalidation::Cover));
        }

        const LONG height =
            std::max<LONG>(1, static_cast<LONG>(std::lround(kAppBarHeightDip * dpi_ / 96.0f)));
        const HWND window = hwnd;
        if (!appBar_.positionWindow(window, appBarMonitor(), appBarEdge_, height))
            return false;
        // Shell 调用可能处理同步消息；返回时模式或窗口生命周期可能已经改变。
        if (destroying_ || hwnd != window || !isAppBarView() || !appBar_.registered())
            return false;
        mediaPopup.setAnchor(hwnd);
        setPlacementStatus(TaskbarPlacementStatus::Safe);
        return true;
    }

    bool registerAppBar() {
        if (!isAppBarView() || !hwnd)
            return false;
        if (appBar_.registered())
            return updateAppBarPosition();
        if (!detachFromTaskbar())
            return false;
        if (!appBar_.registerWindow(hwnd, kAppBarCallbackMessage)) {
            runtime_log::writef(L"[appbar] ABM_NEW failed");
            return false;
        }
        if (!updateAppBarPosition()) {
            unregisterAppBar();
            return false;
        }
        runtime_log::writef(L"[appbar] registered edge=%s",
                            appBarEdge_ == AppBarEdge::Top ? L"top" : L"bottom");
        return true;
    }

    void unregisterAppBar() { appBar_.unregisterWindow(hwnd); }

    void retryTaskbarAttach() {
        if (!hwnd || isAppBarView())
            return;
        if (!findTaskbar())
            return;

        const bool immersiveBackdropPrepared =
            viewMode_ == TaskbarViewMode::Immersive && !dockBackdrop_.available();
        if (immersiveBackdropPrepared)
            initializeBackdropForCurrentViewMode();
        if (!attachToTaskbar(hwnd)) {
            if (attachment_.retriesExhausted())
                cancelTaskbarAttachRetry();
            // 附着失败时仍保持顶层窗口，但必须使用屏幕坐标，避免落到屏幕顶部。
            adjustPosition();
            return;
        }

        cancelTaskbarAttachRetry();
        if (!immersiveBackdropPrepared)
            initializeBackdropForCurrentViewMode();
        adjustPosition();
        if (isWindowVisible())
            requestFrameAndFlush();
    }

    bool createWindow(HINSTANCE inst, const std::wstring& monitorDeviceName,
                      TaskbarViewMode initialViewMode) {
        const auto lifetime = shared_from_this();
        this->inst = inst;
        targetMonitorDeviceName_ = monitorDeviceName;
        viewMode_ = initialViewMode;
        BOOL animations = TRUE;
        SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &animations, 0);
        clientAnimations_ = animations != FALSE;

        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = Impl::wndProc;
        wc.hInstance = inst;
        wc.lpszClassName = kWndClassName;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        const HICON icon = app_icon::taskbarIcon();
        wc.hIcon = icon;
        wc.hIconSm = icon;
        RegisterClassExW(&wc);

        if (!findTaskbar())
            return false;

        // 内容完全由 DirectComposition visual 提供；禁用窗口自身的不透明重定向位图，
        // 避免登录或解锁后重建的底面透过半透明遮罩显示为灰底。
        DWORD ex = WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_NOREDIRECTIONBITMAP;
        HWND h = CreateWindowExW(ex, kWndClassName, L"QQMusicLyricTaskbar", WS_POPUP, 0, 0, 1, 1,
                                 nullptr, nullptr, inst, this);
        if (!h)
            return false;

        hwnd = h;
        app_icon::applyTaskbarIcon(hwnd);
        const bool immersiveBackdropPrepared = viewMode_ == TaskbarViewMode::Immersive;
        if (immersiveBackdropPrepared)
            initializeBackdropForCurrentViewMode();
        if (isAppBarView()) {
            attachment_.reset();
            renderState_.setVisibilitySuppressed(false);
            setPlacementStatus(TaskbarPlacementStatus::Safe);
        } else if (!attachToTaskbar(h)) {
            scheduleTaskbarAttachRetry();
        }
        if (!immersiveBackdropPrepared)
            initializeBackdropForCurrentViewMode();
        if (!mediaPopup.create(inst, hwnd))
            runtime_log::writef(L"[taskbar] media popup creation failed");
        if (!volumePopup_.create(inst))
            runtime_log::writef(L"[taskbar] volume popup creation failed");
        if (!progressPopup_.create(inst))
            runtime_log::writef(L"[taskbar] progress popup creation failed");
        progressPopup_.setCallback([this](uint64_t generation, int64_t posMs) {
            if (onControl)
                onControl(MediaControl::Seek, generation, posMs);
        });
        updateResourceColumnWidths();
        adjustPosition();
        startProbe(); // 避让探测（阻塞型跨进程调用）全程在工作线程执行
        if (resourceMonitoringEnabled() && !isStoppedMode())
            startResourceMonitoring();
        if (renderState_.visibilitySuppressed() && !isStoppedMode())
            startPlacementTimer();
        return true;
    }

    // 任务栏主轴上的占用区间（屏幕坐标，已合并）：横向任务栏取 [left, right)，
    // 纵向任务栏取 [top, bottom)。来源为 UIA 按钮 + 通知区 + TrafficMonitor + 酷狗窗口。
    // 开始按钮的 HWND 矩形与 UIA StartButton 重复，合并后无害，留着可在 UIA
    // 不可用时兜底。
    std::vector<std::pair<int, int>> occupiedIntervals() const {
        return taskbar_detail::occupiedIntervals(
            {isVerticalTaskbar(), notifyWindowActive(), start_ != nullptr, rcNotify_, rcStart_,
             rcTrafficMonitor_, uiaButtons_, kugouTaskbarWindows_});
    }

    void startPlacementTimer() {
        if (placementTimerRunning_ || !hwnd || isStoppedMode())
            return;
        if (SetTimer(hwnd, kPlacementTimerId, kPlacementTimerMs, nullptr))
            placementTimerRunning_ = true;
    }

    void stopPlacementTimer() {
        if (placementTimerRunning_ && hwnd)
            KillTimer(hwnd, kPlacementTimerId);
        placementTimerRunning_ = false;
    }

    void resumeAfterPlacementAvailable() {
        if ((!probeReady_ && !allowOverlap_) ||
            placementStatus_ == TaskbarPlacementStatus::NoSpace ||
            placementStatus_ == TaskbarPlacementStatus::Unavailable)
            return;

        renderState_.setVisibilitySuppressed(false);
        placementProbe_.setFast(false);
        stopPlacementTimer();
        if (isSessionVisible() && !isStoppedMode()) {
            // 隐藏期间不运行完整帧定时器；恢复前补一次播放进度/当前行，
            // 避免窗口重新出现时先显示旧歌词，等下一帧才追上。
            if (tick)
                tick();
            const bool visibilityChanged = reconcileWindowVisibility(true);
            if (isWindowVisible() && !visibilityChanged)
                requestFrameAndFlush();
        } else if (!isWindowVisible()) {
            stopFrameTimer();
        }
    }

    TaskbarPlacementStatus setPlacementStatus(TaskbarPlacementStatus status) {
        if (placementStatus_ == status)
            return status;
        const TaskbarPlacementStatus previous = placementStatus_;
        placementStatus_ = status;
        if (status == TaskbarPlacementStatus::NoSpace ||
            status == TaskbarPlacementStatus::Unavailable) {
            placementProbe_.setFast(status == TaskbarPlacementStatus::NoSpace);
            startPlacementTimer();
            reconcileWindowVisibility();
            stopFrameTimer();
        } else if (previous == TaskbarPlacementStatus::NoSpace ||
                   previous == TaskbarPlacementStatus::Unavailable) {
            resumeAfterPlacementAvailable();
        }
        if (onPlacementStatusChanged_)
            onPlacementStatusChanged_(status);
        return status;
    }

    void releaseVisibilitySuppression() {
        if (!renderState_.visibilitySuppressed() || !probeReady_)
            return;

        if (placementStatus_ == TaskbarPlacementStatus::NoSpace) {
            // 首次真实探测已经完成：解除“等待首次结果”的抑制，但仍保持隐藏，
            // 后续由低频避让计时器等待空间恢复。
            renderState_.setVisibilitySuppressed(false);
            placementProbe_.setFast(true);
            startPlacementTimer();
            return;
        }
        if (placementStatus_ == TaskbarPlacementStatus::Unavailable)
            return;
        resumeAfterPlacementAvailable();
    }

    void setVisibilitySuppressed(bool on) {
        if (on) {
            renderState_.setVisibilitySuppressed(true);
            reconcileWindowVisibility();
            if (hwnd && !isStoppedMode())
                startPlacementTimer();
            return;
        }

        if (!probeReady_)
            return;
        releaseVisibilitySuppression();
    }

    void show() {
        if (isAppBarView()) {
            renderState_.setVisibilitySuppressed(false);
            setPlacementStatus(TaskbarPlacementStatus::Safe);
            renderState_.setSessionVisible(true);
            reconcileWindowVisibility();
            requestFrameAndFlush();
            return;
        }
        if (renderState_.visibilitySuppressed() && !allowOverlap_)
            return;
        if ((placementStatus_ == TaskbarPlacementStatus::NoSpace ||
             placementStatus_ == TaskbarPlacementStatus::Unavailable) &&
            !allowOverlap_)
            return;
        if (allowOverlap_) {
            renderState_.setVisibilitySuppressed(false);
            placementProbe_.setFast(false);
            stopPlacementTimer();
        }
        renderState_.setSessionVisible(true);
        reconcileWindowVisibility();
        requestFrameAndFlush();
    }

    void hide() {
        renderState_.setSessionVisible(false);
        reconcileWindowVisibility();
        volumeHover_ = false;
        immersiveControlHover_ = -1;
        inlineControlHover_ = -1;
        volumePopup_.hide();
        hideProgressPopup();
        mediaPopup.hideImmediate();
    }

    static TaskbarAnchorSpec normalizeAnchor(TaskbarAnchorSpec anchor) {
        const int mode = std::clamp(anchor.mode, 0, 2);
        if (mode == 0)
            return TaskbarAnchorSpec::outerEnd();
        if (mode == 1)
            return TaskbarAnchorSpec::outerStart();
        const double finiteRatio = std::isfinite(anchor.ratio) ? anchor.ratio : 0.0;
        const double rounded =
            std::round(std::clamp(finiteRatio, 0.0, 1.0) * 10000.0) / 10000.0;
        return TaskbarAnchorSpec::relative(
            rounded, anchor.edge == TaskbarAnchorEdge::Start ? TaskbarAnchorEdge::Start
                                                             : TaskbarAnchorEdge::End);
    }

    static std::vector<TaskbarAnchorOption> anchorOptionsFromSlots(
        const std::vector<taskbar_detail::TaskbarAnchorSlot>& slots, int activeIndex,
        bool vertical) {
        std::vector<TaskbarAnchorOption> options;
        options.reserve(slots.size());
        for (size_t i = 0; i < slots.size(); ++i) {
            TaskbarAnchorOption option;
            option.spec = slots[i].spec;
            option.spanIndex = slots[i].spanIndex;
            option.spanCount = slots[i].spanCount;
            option.spanHasBothEdges = slots[i].spanHasBothEdges;
            option.vertical = vertical;
            option.active = static_cast<int>(i) == activeIndex;
            options.push_back(option);
        }
        return options;
    }

    bool buildEmbeddedPlacementInput(taskbar_detail::PlacementInput& input,
                                     const TaskbarAnchorSpec& anchor) {
        if (!hwnd || !taskbar_)
            return false;

        updateRects();

        const bool vertical = isVerticalTaskbar();
        const int taskbarCross = taskbarCrossPixels(rcTaskbar_, taskbarEdge_);
        const int crossMargin = std::max(2, (int)std::lround(2.0f * scale()));
        int crossPx = taskbarCross - crossMargin * 2;
        if (crossPx < 16)
            crossPx = taskbarCross;
        if (crossPx <= 0)
            return false;
        syncCompactTaskbarLayout(vertical ? 0 : crossPx);

        int gap = std::max(4, (int)std::lround(4.0f * scale()));
        float minWidthDip = vertical ? kVerticalMinLengthDip : kMinWidthDip;
        float maxWidthDip = vertical ? taskbar_detail::kVerticalBaseLengthDip : kMaxWidthDip;
        float compressedMinWidthDip = minWidthDip * kCompressedMinWidthRatio;
        if (embeddedResourceOnly()) {
            minWidthDip = 0.0f;
            compressedMinWidthDip = 0.0f;
            maxWidthDip = 0.0f;
        } else if (vertical) {
            if (verticalResourceOnly()) {
                if (!typesetter_.resources().fmtResource_)
                    updateResourceColumnWidths();
                const float resourceHeight = resourcePanel_.verticalHeight(dip(crossPx));
                if (resourceHeight > 0.0f) {
                    const float resourceOnlyHeight = resourceHeight + 2.0f * kTextPadding;
                    minWidthDip = resourceOnlyHeight;
                    compressedMinWidthDip = resourceOnlyHeight;
                    maxWidthDip = resourceOnlyHeight;
                }
            } else if (songInfoVisible_ && isTaskbarMediaScene(scene_)) {
                maxWidthDip += taskbar_detail::kVerticalSongInfoLengthDip;
            }
        } else if (!songInfoVisible_ && scene_ != DisplayScene::Idle) {
            // 保留原歌词区宽度，只扣除歌曲信息区；左侧压缩为可见的封面区域。
            const float compactLeftDip = albumCoverVisible_ ? coverSlotWidth(dip(crossPx)) : 0.0f;
            const float flexibleMinDip = minWidthDip * (1.0f - kLeftRatio);
            minWidthDip = flexibleMinDip + compactLeftDip;
            compressedMinWidthDip = flexibleMinDip * kCompressedMinWidthRatio + compactLeftDip;
            maxWidthDip = maxWidthDip * (1.0f - kLeftRatio) + compactLeftDip;
        }
        int minW = (int)std::lround(minWidthDip * scale());
        int compressedMinW = (int)std::lround(compressedMinWidthDip * scale());
        int maxW = (int)std::lround(maxWidthDip * scale());
        // 独立频谱和内嵌资源区整体加宽；背景波浪复用内容区。
        const float spectrumExtra = spectrumExtraForScene(scene_);
        if (!vertical && spectrumExtra > 0.0f) {
            int extra = (int)std::lround(spectrumExtra * scale());
            minW += extra;
            compressedMinW += extra;
            maxW += extra;
        }
        if (!vertical && pinnedInlineControlsForScene(scene_)) {
            const float pinnedExtra = taskbar_detail::pinnedInlineControlsIdealWidth(
                dip(crossPx), media.canRepeat, pinnedInlineLyricFontSize(), media.canSwitchSource,
                inlineProgressAvailable());
            if (pinnedExtra > 0.0f) {
                const int extra = (int)std::lround(pinnedExtra * scale());
                minW += extra;
                maxW += extra;
            }
        }

        input = taskbar_detail::PlacementInput{};
        input.taskbar = rcTaskbar_;
        input.edge = taskbarEdge_;
        input.vertical = vertical;
        input.allowOverlap = allowOverlap_;
        input.positionMode = anchor.mode;
        input.anchorRatio = anchor.ratio;
        input.anchorEdge = anchor.edge;
        input.gap = gap;
        input.minMajor = minW;
        input.compressedMinMajor = compressedMinW;
        input.maxMajor = maxW;
        input.crossPixels = crossPx;
        input.crossMargin = crossMargin;
        input.minDistinctAnchorOffset = std::max(24, (int)std::lround(32.0f * scale()));
        input.occupied = occupiedIntervals();
        return true;
    }

    void syncAvailableAnchorOptions(const taskbar_detail::PlacementInput& input,
                                    const WindowPlacement& placement,
                                    TaskbarPlacementStatus status) {
        std::vector<TaskbarAnchorOption> options;
        if (status != TaskbarPlacementStatus::NoSpace &&
            status != TaskbarPlacementStatus::Unavailable) {
            const auto slots = taskbar_detail::collectAvailableAnchors(input, 0);
            const int activeIndex =
                taskbar_detail::findActiveAnchorIndex(slots, anchor_, placement, input.vertical);
            options = anchorOptionsFromSlots(slots, activeIndex, input.vertical);
        }
        if (options == publishedAnchorOptions_)
            return;
        publishedAnchorOptions_ = std::move(options);
        if (onAnchorOptionsChanged_)
            onAnchorOptionsChanged_(publishedAnchorOptions_);
    }

    TaskbarPlacementStatus calculateWindowPlacement(WindowPlacement& placement,
                                                    bool updateStatus = true) {
        auto finishStatus = [&](TaskbarPlacementStatus status) {
            return updateStatus ? setPlacementStatus(status) : status;
        };
        if (!hwnd || !taskbar_)
            return finishStatus(TaskbarPlacementStatus::Unavailable);

        updateRects();

        const bool vertical = isVerticalTaskbar();
        if (viewMode_ == TaskbarViewMode::Immersive) {
            RECT client{};
            POINT origin{0, 0};
            if (!GetClientRect(taskbar_, &client) || client.right <= client.left ||
                client.bottom <= client.top || !ClientToScreen(taskbar_, &origin))
                return finishStatus(TaskbarPlacementStatus::Unavailable);

            placement.x = origin.x;
            placement.y = origin.y;
            placement.width = client.right - client.left;
            placement.height = client.bottom - client.top;
            placement.availableMajor = vertical ? placement.height : placement.width;
            syncCompactTaskbarLayout(placement.height);
            return finishStatus(TaskbarPlacementStatus::Safe);
        }

        taskbar_detail::PlacementInput input;
        if (!buildEmbeddedPlacementInput(input, anchor_)) {
            if (updateStatus && (probeReady_ || allowOverlap_))
                syncAvailableAnchorOptions(input, placement, TaskbarPlacementStatus::Unavailable);
            return finishStatus(TaskbarPlacementStatus::Unavailable);
        }
        const TaskbarPlacementStatus status = taskbar_detail::calculatePlacement(input, placement);
        if (updateStatus && (probeReady_ || allowOverlap_))
            syncAvailableAnchorOptions(input, placement, status);
        return finishStatus(status);
    }

    void applyWindowPlacement(const WindowPlacement& placement, bool bringToFront = true,
                              bool noRedraw = false) {
        if (!hwnd || placement.width <= 0 || placement.height <= 0)
            return;

        WindowPlacement current;
        const bool hasCurrent = currentWindowPlacement(current);
        const bool moved = !hasCurrent || current.x != placement.x || current.y != placement.y;
        const bool resized = !hasCurrent || current.width != placement.width ||
                             current.height != placement.height;
        const bool embedded = attachment_.embedded();
        if (!moved && !resized) {
            if (!bringToFront)
                return;
            if (embedded) {
                if (GetTopWindow(GetParent(hwnd)) == hwnd)
                    return;
            } else {
                ensureFloatingFallbackZOrder(true);
                return;
            }
        }

        POINT pt{placement.x, placement.y};
        if (embedded)
            ScreenToClient(taskbar_, &pt);

        // 嵌入模式把窗口提到任务栏子窗口最前面，避免被其他任务栏子窗口盖住；
        // 未嵌入悬浮回退模式不在几何更新里抢全屏 HWND_TOP，改由相对任务栏 Z 序统一维护。
        // 尺寸转场期间保持 Z 序且禁止系统自动重绘，画面由后续 render() 统一提交。
        UINT flags = SWP_NOACTIVATE;
        if (!moved)
            flags |= SWP_NOMOVE;
        if (!resized)
            flags |= SWP_NOSIZE;
        if (!bringToFront || !embedded)
            flags |= SWP_NOZORDER;
        if (noRedraw)
            flags |= SWP_NOREDRAW;
        SetWindowPos(hwnd, (bringToFront && embedded) ? HWND_TOP : nullptr, pt.x, pt.y,
                     placement.width, placement.height, flags);
        if (bringToFront && !embedded)
            ensureFloatingFallbackZOrder(true);
        mediaPopup.setAnchor(hwnd);
    }

    // TrafficMonitor 是任务栏的外部子窗口，刷新自身内容时可能重新进入任务栏
    // 子窗口 Z 序的顶部。嵌入模式需要和它互相避让，沉浸模式则必须始终覆盖它；
    // 这里只在发现宿主不在顶部时修正，并明确禁止激活，避免伪装成“跑到前台”。
    void ensureImmersiveZOrder() {
        if (viewMode_ != TaskbarViewMode::Immersive || !hwnd || !taskbar_ ||
            !attachment_.embedded() || GetParent(hwnd) != taskbar_)
            return;
        if (GetTopWindow(taskbar_) == hwnd)
            return;
        SetWindowPos(hwnd, HWND_TOP, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
    }

    // SetParent 无法嵌入任务栏时的顶层悬浮回退 Z 序维护：
    // 任务栏处于置顶层时，把窗口稳固在任务栏正上方且不抢占开始菜单、弹窗及其他置顶窗；
    // 全屏应用导致任务栏退出置顶层时，同步撤销置顶并退回到任务栏正上方，避免遮挡全屏画面。
    void ensureFloatingFallbackZOrder(bool bypassCooldown = false) {
        if (isAppBarView() || attachment_.embedded() || !isWindowVisible() || !hwnd || !taskbar_ ||
            !IsWindow(taskbar_))
            return;

        const bool taskbarTopmost =
            (GetWindowLongPtrW(taskbar_, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0;
        const bool selfTopmost =
            (GetWindowLongPtrW(hwnd, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0;
        const HWND aboveTaskbar = GetWindow(taskbar_, GW_HWNDPREV);
        if (aboveTaskbar == hwnd && selfTopmost == taskbarTopmost) {
            floatingZOrderCooldownUntilMs_ = 0;
            return;
        }

        const ULONGLONG nowMs = monotonicNowMs();
        if (!bypassCooldown && floatingZOrderCooldownUntilMs_ != 0 &&
            nowMs < floatingZOrderCooldownUntilMs_)
            return;

        constexpr UINT kFlags = SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER;

        if (taskbarTopmost) {
            const bool validAboveTopmost =
                aboveTaskbar && aboveTaskbar != hwnd &&
                (GetWindowLongPtrW(aboveTaskbar, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0;
            bool adjusted = false;
            if (validAboveTopmost &&
                SetWindowPos(hwnd, aboveTaskbar, 0, 0, 0, 0, kFlags) &&
                (GetWindowLongPtrW(hwnd, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0) {
                adjusted = true;
            }
            if (!adjusted) {
                // 任务栏位于置顶队列最顶端（aboveTaskbar == nullptr）或跨层插入未自动提升时，
                // 必须使用 HWND_TOPMOST 而非 HWND_TOP，确保非置顶窗口越过置顶边界。
                SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, kFlags);
                if (validAboveTopmost)
                    SetWindowPos(hwnd, aboveTaskbar, 0, 0, 0, 0, kFlags);
            }
        } else {
            if (selfTopmost)
                SetWindowPos(hwnd, HWND_NOTOPMOST, 0, 0, 0, 0, kFlags);
            if (aboveTaskbar && aboveTaskbar != hwnd &&
                (GetWindowLongPtrW(aboveTaskbar, GWL_EXSTYLE) & WS_EX_TOPMOST) == 0) {
                SetWindowPos(hwnd, aboveTaskbar, 0, 0, 0, 0, kFlags);
            } else if (!selfTopmost) {
                SetWindowPos(hwnd, HWND_TOP, 0, 0, 0, 0, kFlags);
            }
        }

        const bool settledTopmost =
            (GetWindowLongPtrW(hwnd, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0;
        const bool settled =
            GetWindow(taskbar_, GW_HWNDPREV) == hwnd && settledTopmost == taskbarTopmost;
        floatingZOrderCooldownUntilMs_ = settled ? 0 : nowMs + kTaskbarAttachRetryMs;
    }

    void adjustPosition() {
        if (updateDepth_ != 0) {
            positionPending_ = true;
            return;
        }
        if (drag_.dragging()) {
            if (refreshDragAnchors()) {
                if (updateDragPreviewPlacement())
                    requestFrameAndFlush();
            } else {
                finishLyricDrag(false);
            }
            return;
        }
        if (isAppBarView()) {
            cancelSceneWindowResize();
            if (appBar_.registered())
                updateAppBarPosition();
            return;
        }
        WindowPlacement placement;
        const TaskbarPlacementStatus status = calculateWindowPlacement(placement);
        // 探测到的按钮变化未影响最终位置时，继续场景尺寸动画，避免中途跳到终点。
        if (status != TaskbarPlacementStatus::Unavailable &&
            status != TaskbarPlacementStatus::NoSpace &&
            sceneTransition_.targets(placement))
            return;
        cancelSceneWindowResize();
        if (status != TaskbarPlacementStatus::Unavailable &&
            status != TaskbarPlacementStatus::NoSpace)
            applyWindowPlacement(placement);
    }

    TaskbarPlacementStatus refreshPlacement() {
        adjustPosition();
        return placementStatus_;
    }

    bool currentWindowPlacement(WindowPlacement& placement) const {
        if (!hwnd || !IsWindow(hwnd))
            return false;
        RECT rect{};
        if (!GetWindowRect(hwnd, &rect) || rect.right <= rect.left || rect.bottom <= rect.top)
            return false;

        placement.x = rect.left;
        placement.y = rect.top;
        placement.width = rect.right - rect.left;
        placement.height = rect.bottom - rect.top;
        return true;
    }

    bool refreshDragAnchors() {
        taskbar_detail::PlacementInput input;
        if (!buildEmbeddedPlacementInput(input, anchor_) || input.vertical != dragVertical_)
            return false;
        const int previous = drag_.candidate();
        const auto preferred = previous >= 0 && previous < static_cast<int>(dragAnchors_.size())
                                   ? dragAnchors_[static_cast<size_t>(previous)].spec
                                   : anchor_;
        auto anchors = taskbar_detail::collectAvailableAnchors(input, drag_.previewMajor());
        if (anchors.empty())
            return false;
        WindowPlacement current;
        currentWindowPlacement(current);
        drag_.setCandidate(
            taskbar_detail::findActiveAnchorIndex(anchors, preferred, current, dragVertical_));
        dragAnchors_ = std::move(anchors);
        return true;
    }

    int dragAnchorIndexAt(POINT cursor) const {
        const int effectiveMajor = (dragVertical_ ? cursor.y : cursor.x) - drag_.grabOffset();
        const int hysteresisPx = std::max(6, (int)std::lround(10.0f * scale()));
        return taskbar_detail::selectDragAnchorIndex(dragAnchors_, effectiveMajor, dragVertical_,
                                                     drag_.candidate(), hysteresisPx);
    }

    bool updateDragPreviewPlacement() {
        const int currentIdx = drag_.candidate();
        const int nextIdx = dragAnchorIndexAt(drag_.cursor());
        if (nextIdx < 0)
            return false;

        drag_.setCandidate(nextIdx);
        const WindowPlacement placement =
            dragAnchors_[static_cast<size_t>(nextIdx)].previewPlacement;
        WindowPlacement current;
        const bool sameBox = currentWindowPlacement(current) && current.x == placement.x &&
                             current.y == placement.y && current.width == placement.width &&
                             current.height == placement.height;
        if (!sameBox)
            applyWindowPlacement(placement, true, true);
        return !sameBox || nextIdx != currentIdx;
    }

    void beginLyricDrag() {
        if (isExpandedView() || drag_.dragging())
            return;
        WindowPlacement current;
        const bool hasCurrent = currentWindowPlacement(current);
        const bool vertical = isVerticalTaskbar();
        // 预览表达的是用户此刻看到的整个任务栏歌词宿主，而非单独歌词文本：
        // 当前 HWND 主轴尺寸已包含封面、歌曲信息、歌词区和独立频谱。
        const int preview = hasCurrent ? (vertical ? current.height : current.width) : 0;
        taskbar_detail::PlacementInput input;
        if (!buildEmbeddedPlacementInput(input, anchor_)) {
            drag_.end();
            if (GetCapture() == hwnd)
                ReleaseCapture();
            return;
        }
        dragAnchors_ = taskbar_detail::collectAvailableAnchors(input, preview);
        if (dragAnchors_.empty()) {
            drag_.end();
            if (GetCapture() == hwnd)
                ReleaseCapture();
            return;
        }
        int initialIdx =
            taskbar_detail::findActiveAnchorIndex(dragAnchors_, anchor_, current, vertical);
        if (initialIdx < 0)
            initialIdx = 0;

        int grabOffset = 0;
        if (hasCurrent) {
            const POINT cursor = drag_.cursor();
            const int cursorStartMajor = vertical ? cursor.y : cursor.x;
            const int currentStart = vertical ? current.y : current.x;
            const int currentLen = vertical ? current.height : current.width;
            const int currentCenter = currentStart + currentLen / 2;
            const int limit = std::max(1, preview / 2);
            grabOffset = std::clamp(cursorStartMajor - currentCenter, -limit, limit);
        }

        dragVertical_ = vertical;
        drag_.begin(preview, initialIdx, grabOffset);
        SetCursor(LoadCursorW(nullptr, IDC_SIZEALL));
        cancelSceneWindowResize();
        setSongTransitionPending(false);
        resetRootVisual();
        clearImmersiveSongContentTransition();
        renderer.clearTransitionLayers();
        clearLyricDCompState();
        mouseOver_ = false;
        trackingLeave_ = false;
        volumeHover_ = false;
        immersiveControlHover_ = -1;
        inlineControlHover_ = -1;
        volumePopup_.hide();
        hideProgressPopup();
        mediaPopup.onAnchorLeave();
        mediaPopup.hideImmediate();
        updateDragPreviewPlacement();
        requestFrameAndFlush();
    }

    void finishLyricDrag(bool commit) {
        const bool wasDragging = drag_.dragging();
        int committedIdx = -1;
        if (commit && wasDragging) {
            detectChanges();
            if (probeReady_)
                pickProbeResult();
            if (refreshDragAnchors()) {
                POINT cursor = drag_.cursor();
                GetCursorPos(&cursor);
                committedIdx = dragAnchorIndexAt(cursor);
            }
        }
        const bool hasCommitted = committedIdx >= 0;
        const TaskbarAnchorSpec committedSpec =
            hasCommitted ? dragAnchors_[static_cast<size_t>(committedIdx)].spec : anchor_;
        dragAnchors_.clear();
        drag_.end();
        if (GetCapture() == hwnd)
            ReleaseCapture();
        SetCursor(LoadCursorW(nullptr, IDC_ARROW));

        if (!wasDragging)
            return;
        trackingLeave_ = false;
        mouseOver_ = false;
        immersiveControlHover_ = -1;
        if (hasCommitted)
            anchor_ = normalizeAnchor(committedSpec);
        requestInvalidation(RenderInvalidation::Layout);
        adjustPosition();
        requestFrameAndFlush();
        if (hasCommitted && onAnchorChanged_)
            onAnchorChanged_(anchor_);
        drag_.clearPreview();
    }

    bool detectChanges() {
        if (!taskbar_ || !IsWindow(taskbar_))
            return false;

        RECT rcTaskbar{}, rcNotify{};
        GetWindowRect(taskbar_, &rcTaskbar);
        if (notifyWindowActive())
            GetWindowRect(notify_, &rcNotify);
        UINT dpi = GetDpiForWindow(taskbar_);
        bool center = isTaskbarCenterAlign();
        bool light = !fluent::isDarkMode(fluent::ThemeTarget::Taskbar);
        const UINT edge = queryTaskbarEdge(taskbar_, rcTaskbar);
        const bool edgeChanged = edge != taskbarEdge_;
        bool themeChanged = light != lightTheme_;

        RECT rcStart{};
        if (start_)
            GetWindowRect(start_, &rcStart);

        // TrafficMonitor / 酷狗 / UIA 按钮矩形由探测工作线程提供（pickProbeResult），
        // 这里只做非阻塞检查，UI 线程不允许出现阻塞型跨进程调用
        bool changed = dpi != dpi_ || center != centerAlign_ || edgeChanged || themeChanged ||
                       !EqualRect(&rcTaskbar, &rcTaskbar_) || !EqualRect(&rcNotify, &rcNotify_) ||
                       !EqualRect(&rcStart, &rcStart_);
        if (changed) {
            // 封面位图按显示尺寸解码（decodeCover），DPI/任务栏厚度或方向变化时按新尺寸重解码
            const bool crossMetricsChanged =
                dpi != dpi_ || edge != taskbarEdge_ ||
                taskbarCrossPixels(rcTaskbar, edge) != taskbarCrossPixels(rcTaskbar_, taskbarEdge_);
            if (crossMetricsChanged)
                requestInvalidation(RenderInvalidation::Cover);
            dpi_ = dpi;
            centerAlign_ = center;
            taskbarEdge_ = edge;
            if (edgeChanged)
                resetRootVisual();
            if (edgeChanged) {
                // 左右侧的竖排文字方向相反；任务栏方向变化时必须重建逐字布局，
                // 同时丢弃沿用自另一方向的滚动偏移。
                requestInvalidation(RenderInvalidation::Text);
                requestInvalidation(RenderInvalidation::SongInfo);
                scroll_.resetTitle();
                scroll_.resetArtist();
                scroll_.resetLyric();
            }
            rcTaskbar_ = rcTaskbar;
            rcNotify_ = rcNotify;
            rcStart_ = rcStart;
            renderer.setDpi(dpi_);
            if (crossMetricsChanged)
                recreateResourceFormat();
            requestInvalidation(RenderInvalidation::Layout);
            if (themeChanged) {
                lightTheme_ = light;
                updateBackdropSolidColor();
                discardDeviceResources();
            }
        }
        return changed;
    }

    // ---------- 资源 ----------

    void createDeviceResources() {
        if (brushBg_.Get()) {
            renderState_.setDeviceResourcePhase(RenderState::DeviceResourcePhase::Ready);
            return;
        }
        renderer.initialize();
        auto* rt = renderer.renderTarget();
        if (!rt)
            return;
        renderer.setDpi(dpi_);

        // 根据任务栏深浅主题选择配色，贴近 Windows 11 原生媒体控件
        // 背景使用极低的 alpha，视觉上透明但保证分层窗口命中测试覆盖整个区域
        if (lightTheme_) {
            rt->CreateSolidColorBrush(D2D1::ColorF(0.96f, 0.96f, 0.96f, 0.01f),
                                      brushBg_.ReleaseAndGetAddressOf());
            rt->CreateSolidColorBrush(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.09f),
                                      brushHover_.ReleaseAndGetAddressOf());
            rt->CreateSolidColorBrush(D2D1::ColorF(0.08f, 0.08f, 0.08f, 0.95f),
                                      brushText_.ReleaseAndGetAddressOf());
            rt->CreateSolidColorBrush(D2D1::ColorF(0.30f, 0.30f, 0.30f, 0.75f),
                                      brushDim_.ReleaseAndGetAddressOf());
            rt->CreateSolidColorBrush(D2D1::ColorF(0.10f, 0.10f, 0.10f, 0.90f),
                                      brushBtn_.ReleaseAndGetAddressOf());
            rt->CreateSolidColorBrush(D2D1::ColorF(0.10f, 0.10f, 0.10f, 0.30f),
                                      brushBtnDisabled_.ReleaseAndGetAddressOf());
        } else {
            rt->CreateSolidColorBrush(D2D1::ColorF(0.12f, 0.12f, 0.12f, 0.01f),
                                      brushBg_.ReleaseAndGetAddressOf());
            rt->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.09f),
                                      brushHover_.ReleaseAndGetAddressOf());
            rt->CreateSolidColorBrush(D2D1::ColorF(1.00f, 1.00f, 1.00f, 0.95f),
                                      brushText_.ReleaseAndGetAddressOf());
            rt->CreateSolidColorBrush(D2D1::ColorF(1.00f, 1.00f, 1.00f, 0.65f),
                                      brushDim_.ReleaseAndGetAddressOf());
            rt->CreateSolidColorBrush(D2D1::ColorF(1.00f, 1.00f, 1.00f, 0.90f),
                                      brushBtn_.ReleaseAndGetAddressOf());
            rt->CreateSolidColorBrush(D2D1::ColorF(1.00f, 1.00f, 1.00f, 0.35f),
                                      brushBtnDisabled_.ReleaseAndGetAddressOf());
        }
        // 歌词画刷与主题无关，单独创建（用户可换色，换色时只重建这四个）
        createLyricBrushes();
        // 进度背景颜色每帧经 SetColor 写入，这里只建空画刷
        rt->CreateSolidColorBrush(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f),
                                  brushProgressBg_.ReleaseAndGetAddressOf());
        // 纯色背景/封面模糊遮罩同样每帧 SetColor
        rt->CreateSolidColorBrush(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f),
                                  brushBackground_.ReleaseAndGetAddressOf());
        if (lightTheme_) {
            rt->CreateSolidColorBrush(D2D1::ColorF(0.78f, 0.32f, 0.15f, 1.0f),
                                      brushIdleWarm_.ReleaseAndGetAddressOf());
            rt->CreateSolidColorBrush(D2D1::ColorF(0.10f, 0.40f, 0.64f, 1.0f),
                                      brushIdleCool_.ReleaseAndGetAddressOf());
            rt->CreateSolidColorBrush(D2D1::ColorF(0.08f, 0.47f, 0.34f, 1.0f),
                                      brushIdleAccent_.ReleaseAndGetAddressOf());
        } else {
            rt->CreateSolidColorBrush(D2D1::ColorF(1.00f, 0.56f, 0.24f, 1.0f),
                                      brushIdleWarm_.ReleaseAndGetAddressOf());
            rt->CreateSolidColorBrush(D2D1::ColorF(0.56f, 0.80f, 1.00f, 1.0f),
                                      brushIdleCool_.ReleaseAndGetAddressOf());
            rt->CreateSolidColorBrush(D2D1::ColorF(0.38f, 0.95f, 0.70f, 1.0f),
                                      brushIdleAccent_.ReleaseAndGetAddressOf());
        }
        rt->CreateLayer(coverLayer_.ReleaseAndGetAddressOf());
        lyricTextRenderer_.createDeviceResources(rt);
        recreateFormats();
        typesetter_.setFactory(renderer.dwrite());
        typesetter_.createDragPreviewFormat();
        if (auto* factory = renderer.d2d()) {
            D2D1_STROKE_STYLE_PROPERTIES props{};
            props.startCap = D2D1_CAP_STYLE_FLAT;
            props.endCap = D2D1_CAP_STYLE_FLAT;
            props.dashCap = D2D1_CAP_STYLE_FLAT;
            props.lineJoin = D2D1_LINE_JOIN_ROUND;
            props.miterLimit = 10.0f;
            props.dashStyle = D2D1_DASH_STYLE_DASH;
            props.dashOffset = 0.0f;
            factory->CreateStrokeStyle(props, nullptr, 0,
                                       dragPreviewStroke_.ReleaseAndGetAddressOf());
        }
        renderState_.setDeviceResourcePhase(brushBg_.Get()
                                                ? RenderState::DeviceResourcePhase::Ready
                                                : RenderState::DeviceResourcePhase::Uninitialized);
    }

    // 歌词与黑胶画刷：随用户颜色重建，与主题画刷解耦
    void createLyricBrushes() {
        if (updateDepth_ != 0) {
            brushesPending_ = true;
            return;
        }
        auto* rt = renderer.renderTarget();
        if (!rt)
            return;
        lyricTextRenderer_.invalidate(); // 颜色变化使离屏缓存失效
        if (brushLyric_.Get()) {
            brushLyric_.Reset();
        }
        if (brushLyricDim_.Get()) {
            brushLyricDim_.Reset();
        }
        if (brushLyricGlow_.Get()) {
            brushLyricGlow_.Reset();
        }
        if (brushLyricOutline_.Get()) {
            brushLyricOutline_.Reset();
        }
        auto rgb = [](COLORREF c, float a) {
            return D2D1::ColorF(GetRValue(c) / 255.0f, GetGValue(c) / 255.0f, GetBValue(c) / 255.0f,
                                a);
        };
        rt->CreateSolidColorBrush(rgb(lyricColor_, 1.00f), brushLyric_.ReleaseAndGetAddressOf());
        lyricEffectsRenderer_.setContext(lyricEffectsContext(rt));
        lyricEffectsRenderer_.createBrushes();
        rt->CreateSolidColorBrush(rgb(lyricUnplayedColor_, lyricUnplayedAlphaPct_ / 100.0f),
                                  brushLyricDim_.ReleaseAndGetAddressOf());
        rt->CreateSolidColorBrush(rgb(lyricGlowColor_, 0.28f),
                                  brushLyricGlow_.ReleaseAndGetAddressOf());
        rt->CreateSolidColorBrush(rgb(lyricOutlineColor_, 0.50f),
                                  brushLyricOutline_.ReleaseAndGetAddressOf());
        // 频谱颜色模式独立于歌词颜色设置：默认跟随已播放色，也可跟随专辑主题色或使用自定义色。
        // 尚未提取到当前专辑主色时，专辑模式沿用歌词模式的当前有效已播放色。
        spectrum_.setContext(spectrumContext(rt));
        spectrum_.createBrushes();
        if (albumCoverEffect_ == AlbumCoverEffect::Vinyl)
            vinylRenderer_.createBrushes(rt);
        else
            vinylRenderer_.releaseResources();
    }

    void setFontGlowColors(COLORREF glow, COLORREF outline) {
        if (lyricGlowColor_ == glow && lyricOutlineColor_ == outline)
            return;
        lyricGlowColor_ = glow;
        lyricOutlineColor_ = outline;
        createLyricBrushes();
        requestFrameAndFlush();
    }

    void setFontColors(COLORREF played, COLORREF unplayed, int unplayedAlphaPct) {
        if (lyricColor_ == played && lyricUnplayedColor_ == unplayed &&
            lyricUnplayedAlphaPct_ == unplayedAlphaPct)
            return;
        lyricColor_ = played;
        lyricUnplayedColor_ = unplayed;
        lyricUnplayedAlphaPct_ = unplayedAlphaPct;
        createLyricBrushes();
        requestFrameAndFlush();
    }

    void setFontGlow(bool on) {
        if (lyricGlow_ == on)
            return;
        lyricGlow_ = on;
        if (compactTaskbarHeight_ > 0.0f)
            recreateFormats();
        else if (isVerticalTaskbar() && (translationEnabled_ || romanizationEnabled_))
            requestInvalidation(RenderInvalidation::Text);
        lyricTextRenderer_.invalidate(); // 效果组合变化，不能复用旧的离屏缓存
        requestFrameAndFlush();
    }

    void setFontOutline(bool on) {
        if (lyricOutline_ == on)
            return;
        lyricOutline_ = on;
        if (compactTaskbarHeight_ > 0.0f)
            recreateFormats();
        else if (isVerticalTaskbar() && (translationEnabled_ || romanizationEnabled_))
            requestInvalidation(RenderInvalidation::Text);
        lyricTextRenderer_.invalidate(); // 效果组合变化，不能复用旧的离屏缓存
        requestFrameAndFlush();
    }

    void setSecondaryLyricMode(bool translation, bool romanization) {
        // 调用方保证互斥，这里再收紧一次，避免异常状态绘制三行。
        if (translation && romanization)
            romanization = false;
        if (translationEnabled_ == translation && romanizationEnabled_ == romanization)
            return;
        translationEnabled_ = translation;
        romanizationEnabled_ = romanization;
        resetLyricTransition();
        verticalLyricColumns_ = {};
        requestInvalidation(RenderInvalidation::Text);
        requestFrameAndFlush();
    }

    void setDoubleLineLyrics(bool on) {
        if (doubleLineLyricsEnabled_ == on)
            return;
        doubleLineLyricsEnabled_ = on;
        resetLyricTransition();
        requestInvalidation(RenderInvalidation::Text);
        requestFrameAndFlush();
    }

    void rebuildDisplayLyrics() {
        if (simulateWordByWordLyrics_ && !sourceLines_.empty())
            lines = lyric_timing::buildSimulatedKaraokeLines(sourceLines_, media.durationMs);
        else
            lines = sourceLines_;
    }

    void setSimulateWordByWordLyrics(bool on) {
        if (simulateWordByWordLyrics_ == on)
            return;
        simulateWordByWordLyrics_ = on;
        rebuildDisplayLyrics();
        clearLyricEffectGeometry();
        syncLyricEffectClock(positionMs_, media.playing, true);
        karaoke_.clearGeometry();
        karaoke_.reset();
        if (isLyricDCompActive()) {
            renderer.clearTransitionLayers();
            clearLyricDCompState();
        }
        clearImmersiveSongContentTransition();
        requestInvalidation(RenderInvalidation::Text);
        requestFrameAndFlush();
    }

    void setLyricAlignment(LyricAlignment alignment) {
        if (lyricAlignment_ == alignment)
            return;
        lyricAlignment_ = alignment;
        scroll_.resetLyric();
        scroll_.resetSecondary();
        requestFrameAndFlush();
    }

    void setIdleQuoteAlignment(LyricAlignment alignment) {
        if (idleQuoteAlignment_ == alignment)
            return;
        idleQuoteAlignment_ = alignment;
        scroll_.resetLyric();
        scroll_.resetSecondary();
        requestFrameAndFlush();
    }

    void setIdleQuoteBackground(IdleQuoteBackground background) {
        if (idleQuoteBackground_ == background)
            return;
        idleQuoteBackground_ = background;
        refreshFrameTimer();
        requestFrameAndFlush();
    }

    void setIdleQuoteBackgroundScope(IdleQuoteBackgroundScope scope) {
        if (idleQuoteBackgroundScope_ == scope)
            return;
        idleQuoteBackgroundScope_ = scope;
        refreshFrameTimer();
        requestFrameAndFlush();
    }

    bool isMinimalMode() const { return isRenderMode(RenderMode::Minimal); }

    bool mediaPopupAvailable(bool sessionVisible) const {
        return sessionVisible && !isStoppedMode() && !isMinimalMode();
    }

    static bool isTaskbarMediaScene(DisplayScene scene) {
        return taskbar_detail::SceneTransition::isMediaScene(scene);
    }

    static bool isTaskbarSceneTransition(DisplayScene from, DisplayScene to) {
        return taskbar_detail::SceneTransition::switchesPage(from, to);
    }

    bool mediaPopupEnabledForScene() const {
        if (isExpandedView() || isMinimalMode() || isStoppedMode())
            return false;
        if (scene_ == DisplayScene::Idle)
            return isSessionVisible() && idle.hasContent();
        if (hoverControlStyle_ == HoverControlStyle::Popup)
            return controlsOnHover_;
        return isSessionVisible() && idle.hasContent();
    }

    void syncMediaPopupEnabled() {
        const bool enabled = mediaPopupEnabledForScene();
        const bool wasEnabled = mediaPopupEnabled_;
        mediaPopupEnabled_ = enabled;
        mediaPopup.setEnabled(enabled);
        if (enabled && !wasEnabled) {
            // 弹窗样式可能在媒体会话已经存在时才开启；补送当前完整快照，
            // 让弹窗的可用状态和展示类别不依赖下一次 SMTC 事件。
            const bool available = mediaPopupAvailable(isSessionVisible());
            mediaPopup.beginPresentationUpdate();
            mediaPopup.setIdleContent(idle, available);
            mediaPopup.setPresentationMode(scene_, available,
                                           hoverControlStyle_ == HoverControlStyle::Inline);
            mediaPopup.setMedia(media, available);
            mediaPopup.endPresentationUpdate();
        } else if (enabled) {
            const bool available = mediaPopupAvailable(isSessionVisible());
            mediaPopup.beginPresentationUpdate();
            mediaPopup.setPresentationMode(scene_, available,
                                           hoverControlStyle_ == HoverControlStyle::Inline);
            mediaPopup.endPresentationUpdate();
        }
        mediaPopup.setTriggerOnHover(floatingCardTrigger_ == MediaPopupTrigger::Hover);
        if (enabled && mouseOver_) {
            POINT cursor{};
            bool inPinned = false;
            if (hwnd && GetCursorPos(&cursor) && ScreenToClient(hwnd, &cursor)) {
                inPinned = pointInPinnedControlsZone(static_cast<float>(cursor.x),
                                                       static_cast<float>(cursor.y));
            }
            if (inPinned)
                mediaPopup.onAnchorLeave();
            else
                mediaPopup.onAnchorEnter();
        }
    }

    void releaseCoverBackgroundResources() { cover_.releaseBackground(); }

    void setControlsOnHover(bool on) {
        if (controlsOnHover_ == on)
            return;
        controlsOnHover_ = on;
        inlineControlHover_ = -1;
        volumeHover_ = false;
        volumePopup_.hide();
        hideProgressPopup();
        syncMediaPopupEnabled();
        requestInvalidation(toMask(RenderInvalidation::Layout) | toMask(RenderInvalidation::Paint));
        adjustPosition();
        requestFrameAndFlush();
    }

    void setHoverControlStyle(HoverControlStyle style) {
        if (hoverControlStyle_ == style)
            return;
        hoverControlStyle_ = style;
        inlineControlHover_ = -1;
        volumeHover_ = false;
        volumePopup_.hide();
        hideProgressPopup();
        syncMediaPopupEnabled();
        requestInvalidation(toMask(RenderInvalidation::Layout) | toMask(RenderInvalidation::Paint));
        adjustPosition();
        requestFrameAndFlush();
    }

    void setInlineControlDisplayMode(InlineControlDisplayMode mode) {
        if (inlineControlDisplayMode_ == mode)
            return;
        inlineControlDisplayMode_ = mode;
        inlineControlHover_ = -1;
        volumeHover_ = false;
        volumePopup_.hide();
        hideProgressPopup();
        syncMediaPopupEnabled();
        requestInvalidation(toMask(RenderInvalidation::Layout) | toMask(RenderInvalidation::Paint));
        adjustPosition();
        requestFrameAndFlush();
    }

    void setFloatingCardTrigger(MediaPopupTrigger trigger) {
        if (floatingCardTrigger_ == trigger)
            return;
        floatingCardTrigger_ = trigger;
        syncMediaPopupEnabled();
    }

    void setFloatingCardBackground(MediaPopupBackground mode) {
        mediaPopup.setBackgroundMode(mode);
    }

    void setFloatingCardBackgroundColor(COLORREF color, bool customized) {
        mediaPopup.setBackgroundColor(color, customized);
    }

    void setFloatingCardFollowAlbum(bool on) { mediaPopup.setFollowAlbumBackground(on); }

    void setFloatingCardAutoTextContrast(bool on) { mediaPopup.setAutoTextContrast(on); }

    bool backgroundWaveEnabled() const {
        spectrum_.setContext(spectrumContext());
        return spectrum_.backgroundWaveEnabled();
    }

    bool integratedSpectrum() const {
        spectrum_.setContext(spectrumContext());
        return spectrum_.integratedSpectrum();
    }

    void resetSpectrumVisuals() {
        spectrum_.setContext(spectrumContext(drawTarget()));
        spectrum_.resetSpectrumVisuals();
        if (spectrum_.consumeRedraw())
            requestFrame();
    }

    void resetSpectrumEffectHistory() {
        spectrum_.setContext(spectrumContext(drawTarget()));
        spectrum_.resetSpectrumEffectHistory();
        if (spectrum_.consumeRedraw())
            requestFrame();
    }

    void releaseSpectrumEffectResources() {
        spectrum_.setContext(spectrumContext(drawTarget()));
        spectrum_.releaseSpectrumEffectResources();
        if (spectrum_.consumeRedraw())
            requestFrame();
    }

    void setSpectrumEffects(uint32_t effects) {
        effects &= kSpectrumEffectsMask;
        if (spectrumEffects_ == effects)
            return;
        spectrumEffects_ = effects;
        releaseSpectrumEffectResources();
        requestFrameAndFlush();
    }

    bool spectrumEffectsAnimating() const {
        spectrum_.setContext(spectrumContext());
        return spectrum_.spectrumEffectsAnimating();
    }

    void updateSpectrumEffects(ULONGLONG now) {
        spectrum_.setContext(spectrumContext(drawTarget()));
        spectrum_.updateSpectrumEffects(now);
        if (spectrum_.consumeRedraw())
            requestFrame();
    }

    bool spectrumVisualsAnimating() const {
        spectrum_.setContext(spectrumContext());
        return spectrum_.spectrumVisualsAnimating();
    }

    void updateSpectrumVisuals(ULONGLONG now) {
        spectrum_.setContext(spectrumContext(drawTarget()));
        spectrum_.updateSpectrumVisuals(now);
        if (spectrum_.consumeRedraw())
            requestFrame();
    }

    bool backgroundWaveAnimating() const {
        spectrum_.setContext(spectrumContext());
        return spectrum_.backgroundWaveAnimating();
    }

    void setSpectrumStyle(SpectrumStyle style) {
        if (spectrumStyle_ == style)
            return;
        spectrumStyle_ = style;
        resetSpectrumVisuals();
        if (spectrumVisible_)
            adjustPosition();
        requestFrameAndFlush();
    }

    void setSpectrumColor(COLORREF color, bool customized, bool followAlbum) {
        if (spectrumColor_ == color && spectrumCustomColor_ == customized &&
            spectrumFollowAlbum_ == followAlbum)
            return;
        spectrumColor_ = color;
        spectrumCustomColor_ = customized;
        spectrumFollowAlbum_ = followAlbum && !customized;
        resetSpectrumEffectHistory();
        createLyricBrushes();
        requestFrameAndFlush();
    }

    void setSpectrumAlbumColor(COLORREF color, bool available) {
        if (spectrumAlbumColor_ == color && spectrumAlbumColorAvailable_ == available)
            return;
        spectrumAlbumColor_ = color;
        spectrumAlbumColorAvailable_ = available;
        if (spectrumFollowAlbum_ && !spectrumCustomColor_) {
            resetSpectrumEffectHistory();
            createLyricBrushes();
        }
        requestFrameAndFlush();
    }

    void setSpectrumGradient(bool on) {
        if (spectrumGradient_ == on)
            return;
        spectrumGradient_ = on;
        resetSpectrumEffectHistory();
        requestFrameAndFlush();
    }

    void setSpectrumBackground(bool on) {
        if (spectrumBackground_ == on)
            return;
        const bool wasBackground = backgroundWaveEnabled();
        spectrumBackground_ = on;
        resetSpectrumEffectHistory();
        if (spectrumVisible_ && wasBackground != backgroundWaveEnabled())
            adjustPosition();
        requestFrameAndFlush();
    }

    void setSpectrumOpacity(int percent) {
        const int next = std::clamp(percent, 0, 100);
        if (spectrumOpacityPct_ == next)
            return;
        spectrumOpacityPct_ = next;
        resetSpectrumEffectHistory();
        if (backgroundWaveEnabled())
            requestFrameAndFlush();
    }

    // 进度背景实际生效条件：用户开启 && 背景波浪未占用背景 && 当前歌曲有时长
    bool progressBackgroundActive() const {
        return progressBackground_ && !backgroundWaveEnabled() && media.durationMs > 0;
    }

    void setProgressBackground(bool on) {
        if (progressBackground_ == on)
            return;
        progressBackground_ = on;
        requestFrameAndFlush();
    }

    void setProgressBackgroundOpacity(int percent) {
        const int next = std::clamp(percent, 0, 100);
        if (progressBackgroundOpacityPct_ == next)
            return;
        progressBackgroundOpacityPct_ = next;
        if (progressBackgroundActive())
            requestFrameAndFlush();
    }

    void setBackground(TaskbarBackground mode) {
        if (background_ == mode)
            return;
        background_ = mode;
        if (mode != TaskbarBackground::CoverBlur)
            releaseCoverBackgroundResources();
        requestFrameAndFlush();
    }

    void setCoverBackgroundOpacity(int percent) {
        const int next = std::clamp(percent, 0, 100);
        if (coverBackgroundOpacityPct_ == next)
            return;
        coverBackgroundOpacityPct_ = next;
        if (background_ == TaskbarBackground::CoverBlur)
            requestFrameAndFlush();
    }

    void setViewMode(TaskbarViewMode mode) {
        const auto lifetime = shared_from_this();
        if (viewMode_ == mode)
            return;

        const TaskbarViewMode previousMode = viewMode_;
        if (hwnd && IsWindowVisible(hwnd))
            ShowWindow(hwnd, SW_HIDE);
        resetImmersivePresence();
        const bool hadImmersivePress = immersiveControlPressed_ >= 0;
        if (drag_.pressed() || drag_.dragging())
            finishLyricDrag(false);
        immersiveControlPressed_ = -1;
        if (hadImmersivePress && GetCapture() == hwnd)
            ReleaseCapture();
        resetRootVisual();
        clearImmersiveSongContentTransition();
        if (previousMode == TaskbarViewMode::AppBar)
            unregisterAppBar();
        if (destroying_)
            return;
        stopResourceMonitoring();
        viewMode_ = mode;
        bool immersiveBackdropPrepared = false;
        if (mode == TaskbarViewMode::Immersive && !dockBackdrop_.available()) {
            if (!attachment_.embedded() || detachFromTaskbar())
                initializeBackdropForCurrentViewMode();
            else
                backgroundBlurSupported_ = false;
            immersiveBackdropPrepared = true;
        }
        dockPet_.setMode(DockPetMode::Hidden, monotonicNowMs());
        resourcePanel_.resetPage();
        volumeHover_ = false;
        immersiveControlHover_ = -1;
        inlineControlHover_ = -1;
        volumePopup_.hide();
        hideProgressPopup();
        mediaPopup.onAnchorLeave();
        mediaPopup.hideImmediate();
        syncMediaPopupEnabled();
        cancelSceneWindowResize();
        if (mode == TaskbarViewMode::AppBar) {
            detachFromTaskbar();
            renderState_.setVisibilitySuppressed(false);
            stopPlacementTimer();
            setPlacementStatus(TaskbarPlacementStatus::Safe);
        } else {
            findTaskbar();
            if (!attachment_.embedded())
                attachment_.reset();
            if (!attachToTaskbar(hwnd))
                scheduleTaskbarAttachRetry();
        }
        if (!isStoppedMode() && resourceMonitoringEnabled())
            startResourceMonitoring();
        if (!immersiveBackdropPrepared)
            initializeBackdropForCurrentViewMode();
        // 沉浸模式仍需要完成首次真实探测，才能解除创建阶段的显示抑制；
        // 探测完成且窗口已可见后才停止避让定时器。否则重启时会一直隐藏。
        if (mode == TaskbarViewMode::Immersive && probeReady_ &&
            !renderState_.visibilitySuppressed())
            stopPlacementTimer();
        else if (mode != TaskbarViewMode::AppBar && !isStoppedMode())
            startPlacementTimer();
        recreateResourceFormat();
        requestInvalidation(toMask(RenderInvalidation::Layout) | toMask(RenderInvalidation::Paint) |
                            toMask(RenderInvalidation::Text));
        adjustPosition();
        reconcileWindowVisibility();
        requestFrameAndFlush();
    }

    void setAppBarEdge(AppBarEdge edge) {
        if (appBarEdge_ == edge)
            return;
        appBarEdge_ = edge;
        if (isAppBarView() && appBar_.registered())
            updateAppBarPosition();
        requestInvalidation(RenderInvalidation::Layout);
        requestFrameAndFlush();
    }

    void setImmersiveBackgroundBlur(int percent) {
        const int nextBlur = std::clamp(percent, 0, 100);
        if (immersiveBackgroundBlurPct_ == nextBlur)
            return;
        immersiveBackgroundBlurPct_ = nextBlur;
        if (viewMode_ == TaskbarViewMode::Immersive) {
            dockBackdrop_.setBlurPercent(immersiveBackgroundBlurPct_);
            if (!dockBackdrop_.available())
                backgroundBlurSupported_ = false;
            requestFrame();
        }
    }

    void setDockBackgroundBlur(int percent) {
        const int nextBlur = std::clamp(percent, 0, 100);
        if (dockBackgroundBlurPct_ == nextBlur)
            return;
        dockBackgroundBlurPct_ = nextBlur;
        if (viewMode_ == TaskbarViewMode::AppBar) {
            dockBackdrop_.setBlurPercent(dockBackgroundBlurPct_);
            if (!dockBackdrop_.available())
                backgroundBlurSupported_ = false;
            requestFrame();
        }
    }

    void setImmersiveBackgroundAdjustment(int mode) {
        const int nextMode = std::clamp(mode, 0, 1);
        if (immersiveBackgroundAdjustment_ == nextMode)
            return;
        immersiveBackgroundAdjustment_ = nextMode;
        if (viewMode_ == TaskbarViewMode::Immersive) {
            updateBackdropSolidColor();
            dockBackdrop_.setAdjustmentMode(immersiveBackgroundAdjustment_);
            if (!dockBackdrop_.available())
                backgroundBlurSupported_ = false;
            requestFrameAndFlush();
        }
    }

    void setDockBackgroundAdjustment(int mode) {
        const int nextMode = std::clamp(mode, 0, 1);
        if (dockBackgroundAdjustment_ == nextMode)
            return;
        dockBackgroundAdjustment_ = nextMode;
        if (viewMode_ == TaskbarViewMode::AppBar) {
            updateBackdropSolidColor();
            dockBackdrop_.setAdjustmentMode(dockBackgroundAdjustment_);
            if (!dockBackdrop_.available())
                backgroundBlurSupported_ = false;
            requestFrameAndFlush();
        }
    }

    bool backgroundBlurAvailable() const { return backgroundBlurSupported_; }

    void updateBackdropSolidColor() {
        if (viewMode_ == TaskbarViewMode::Embedded)
            return;
        const COLORREF color =
            fluent::isWindowsAppDarkMode() ? RGB(32, 32, 32) : RGB(243, 243, 243);
        dockBackdrop_.setSolidColor(color);
        if (!dockBackdrop_.available())
            backgroundBlurSupported_ = false;
    }

    void initializeBackdropForCurrentViewMode() {
        if (viewMode_ == TaskbarViewMode::Embedded) {
            dockBackdrop_.setVisible(false);
            backgroundBlurSupported_ = true;
            return;
        }

        if (dockBackdrop_.available()) {
            backgroundBlurSupported_ = true;
            runtime_log::writef(L"[dock-backdrop] init result=reused view=%s hwnd=%p",
                                viewMode_ == TaskbarViewMode::Immersive ? L"immersive" : L"dock",
                                hwnd);
        } else {
            backgroundBlurSupported_ = dockBackdrop_.initialize(hwnd);
        }
        updateBackdropSolidColor();
        dockBackdrop_.setAdjustmentMode(viewMode_ == TaskbarViewMode::Immersive
                                            ? immersiveBackgroundAdjustment_
                                            : dockBackgroundAdjustment_);
        dockBackdrop_.setBlurPercent(viewMode_ == TaskbarViewMode::Immersive
                                         ? immersiveBackgroundBlurPct_
                                         : dockBackgroundBlurPct_);
        dockBackdrop_.setVisible(true);
        if (!dockBackdrop_.available())
            backgroundBlurSupported_ = false;
    }

    void setDockResourceVisibility(const DockResourceVisibility& visibility) {
        if (resourcePanel_.visibility() == visibility)
            return;
        const bool wasMonitoring = resourceMonitoringEnabled();
        const bool fontChanged = resourcePanel_.visibility().smallFont != visibility.smallFont;
        const bool hideLabelsChanged =
            resourcePanel_.visibility().hideLabels != visibility.hideLabels;
        const bool placementChanged = resourcePanel_.visibility().placement != visibility.placement;
        resourcePanel_.setVisibility(visibility);
        const bool nowMonitoring = resourceMonitoringEnabled();
        if (visibility.hideLabels && nowMonitoring)
            resourcePanel_.triggerTransientLabels(monotonicNowMs());
        if (placementChanged) {
            resetLyricTransition();
            clearImmersiveSongContentTransition();
        }
        if (fontChanged || hideLabelsChanged || isVerticalTaskbar())
            recreateResourceFormat(); // 字号、标签显示或竖向窄栏拟合依赖已启用指标，必须清缓存重建
        else
            updateResourceColumnWidths();
        if (wasMonitoring != nowMonitoring && hwnd) {
            if (nowMonitoring && !isStoppedMode())
                startResourceMonitoring();
            else
                stopResourceMonitoring();
        }
        resourcePanel_.resetPage();
        requestInvalidation(toMask(RenderInvalidation::Layout) | toMask(RenderInvalidation::Paint) |
                            toMask(RenderInvalidation::Text));
        if (isAppBarView() || viewMode_ == TaskbarViewMode::Embedded)
            adjustPosition();
        requestFrameAndFlush();
    }

    void setSpectrumVisible(bool on) {
        if (spectrumVisible_ == on) {
            if (!on)
                spectrum_.clearBands();
            return;
        }
        spectrumVisible_ = on;
        resetSpectrumVisuals();
        if (!on)
            spectrum_.clearBands();
        // 先改窗口宽度再渲染：若在 render 内经 Layout 失效改大小，
        // render 结尾的 present 会用旧尺寸位图把窗口尺寸拽回去（与 setAnchor 同序）
        adjustPosition();
        requestFrameAndFlush();
    }

    void setSongInfoVisible(bool on) {
        if (songInfoVisible_ == on)
            return;
        songInfoVisible_ = on;
        scroll_.resetTitle();
        scroll_.resetArtist();
        requestInvalidation(RenderInvalidation::SongInfo);
        adjustPosition();
        requestFrameAndFlush();
    }

    void setAlbumCoverVisible(bool on) {
        if (albumCoverVisible_ == on)
            return;
        albumCoverVisible_ = on;
        if (!on) {
            requestInvalidation(RenderInvalidation::PlatformIcon);
            if (platformIconBmp.Get()) {
                platformIconBmp.Reset();
            }
        } else if (platformIconVisible_) {
            requestInvalidation(RenderInvalidation::PlatformIcon);
        }
        requestInvalidation(RenderInvalidation::Text);
        adjustPosition();
        requestFrameAndFlush();
    }

    void setPlatformIconVisible(bool on) {
        if (platformIconVisible_ == on)
            return;
        platformIconVisible_ = on;
        requestInvalidation(RenderInvalidation::PlatformIcon);
        requestFrameAndFlush();
    }

    void setAlbumCoverEffect(AlbumCoverEffect effect) {
        if (albumCoverEffect_ == effect)
            return;
        albumCoverEffect_ = effect;
        vinyl_.resetDisc();
        vinyl_.resetClock(monotonicNowMs());
        requestInvalidation(RenderInvalidation::Geometry);
        if (effect == AlbumCoverEffect::Vinyl)
            vinylRenderer_.createBrushes(renderer.renderTarget());
        else
            vinylRenderer_.releaseResources();
        updateVinylRotation();
        requestFrameAndFlush();
    }

    void setSpectrumBands(const std::array<float, TaskbarHost::kSpectrumBands>& bands) {
        spectrum_.setBands(bands);
    }

    void acceptPlaybackPatch(const PlaybackPatch& patch) {
        if (patch.frameRevision != frameRevision_ || patch.requestGeneration != requestGeneration_)
            return;
        playbackAnchor_ = patch;
        playbackAnchorClock_ = monotonicNowMs();
        applyPlaybackPatch(patch);
    }

    void advancePlaybackClock() {
        if (!playbackAnchor_.playing || !media.playing || !isSessionVisible())
            return;
        PlaybackPatch patch = playbackAnchor_;
        const auto now = monotonicNowMs();
        const auto elapsed = static_cast<int64_t>(now >= playbackAnchorClock_
                                                     ? now - playbackAnchorClock_ : 0);
        patch.actualPositionMs += elapsed;
        patch.lineSelectionPositionMs += elapsed;
        if (media.durationMs > 0)
            patch.actualPositionMs = std::min(patch.actualPositionMs, media.durationMs);
        patch.currentLine = LyricProvider::findLine(lines, patch.lineSelectionPositionMs);
        applyPlaybackPatch(patch);
    }

    void syncKaraokePlayback(int64_t position, bool playing) {
        if (playing != media.playing) {
            if (playing)
                karaoke_.invalidateProgress();
            else
                karaoke_.holdProgress();
        } else if (!playing && position != positionMs_) {
            // 已暂停时收到的位置变化属于暂停中的跳转。
            karaoke_.invalidateProgress();
        }
    }

    void applyPlaybackPatch(const PlaybackPatch& patch) {
        // 高频补丁必须属于当前已经应用的完整帧；旧曲目或旧帧的位置不能回写。
        if (patch.frameRevision != frameRevision_ || patch.requestGeneration != requestGeneration_)
            return;

        syncLyricEffectClock(patch.actualPositionMs, patch.playing);
        syncKaraokePlayback(patch.actualPositionMs, patch.playing);
        positionMs_ = patch.actualPositionMs;
        syncProgressPopup();
        mediaPopup.beginPresentationUpdate();
        mediaPopup.setProgress(patch.actualPositionMs);
        const bool playingChanged = media.playing != patch.playing;
        if (playingChanged) {
            media.playing = patch.playing;
            vinyl_.resetClock(monotonicNowMs());
            updateVinylRotation();
            mediaPopup.setMedia(media, mediaPopupAvailable(isSessionVisible()));
            mediaPopup.setPresentationMode(scene_, mediaPopupAvailable(isSessionVisible()),
                                           hoverControlStyle_ == HoverControlStyle::Inline);
        }
        mediaPopup.endPresentationUpdate();
        if (playingChanged) {
            // 悬浮控制按钮的播放/暂停图标随状态变化，直接提交一帧；弹窗自身
            // 的完整展示帧已在上面的批量更新中提交。
            // 这里仅保留宿主歌词的刷新，不再重复触发弹窗绘制。
            requestFrameAndFlush();
        }

        if (patch.currentLine == currentLine)
            return;

        if (transition_.kind() == LyricTransitionKind::Scene && isLyricTransitionInProgress()) {
            // 场景翻页期间不重排目标层，先记下最新歌词行，待翻页完成后
            // 统一提交，避免目标层重建把上下翻页打断成瞬移。
            currentLine = patch.currentLine;
            sceneTransition_.setNeedsRelayout(true);
            transition_.setRevision(frameRevision_);
            return;
        }

        onLyricLineTargetChanged(patch.currentLine, patch.actualPositionMs, frameRevision_, true);
    }

    void applySpectrumPatch(const SpectrumPatch& patch) {
        if (patch.frameRevision != frameRevision_ || patch.requestGeneration != requestGeneration_)
            return;
        spectrum_.setBands(patch.bands);
    }

    void applyPresentationFrame(const PresentationFrame& frame) {
        // 完整帧只允许按版本向前提交；高频播放补丁不改变这个版本边界。
        if (frame.frameRevision < frameRevision_ ||
            (frame.frameRevision == frameRevision_ && frame.frameRevision != 0 &&
             frame.trackKey != trackKey_))
            return;

        if (frame.scene != scene_ || frame.media.sourceAppUserModelId != media.sourceAppUserModelId)
            hoverTip_.hide();
        const UpdateScope update(*this);
        const bool trackChanged = frame.trackKey != trackKey_;
        playbackAnchor_ = {frame.frameRevision, frame.requestGeneration, frame.actualPositionMs,
                           frame.lineSelectionPositionMs, frame.currentLine, frame.media.playing};
        playbackAnchorClock_ = monotonicNowMs();
        syncLyricEffectClock(frame.actualPositionMs, frame.media.playing, trackChanged);
        if (trackChanged)
            resetSpectrumVisuals();
        const int64_t previousDurationMs = media.durationMs;
        const bool lyricsChanged = !sameLyrics(sourceLines_, frame.lyrics);
        const bool lineChanged = frame.currentLine != currentLine;
        const bool statusChanged = frame.statusText != statusText ||
                                   frame.statusTextIsTrackInfo != statusTextIsTrackInfo;
        const bool statusOneShotChanged = frame.statusTextOneShot != scroll_.oneShot();
        const DisplayScene previousScene = scene_;
        const bool sceneChanged = frame.scene != scene_;
        const SmtcPlayerType incomingPlayer =
            player_source::resolvePlayerType(frame.media.sourceAppUserModelId);
        const bool inlineControlsShown =
            !pinnedInlineControlsLayoutForScene(scene_, lastLogicalPxW_, lastLogicalPxH_).active() &&
            mouseOver_ && controlsOnHover_ && hoverControlStyle_ == HoverControlStyle::Inline &&
            !isExpandedView();
        const bool loadedLyricsFromPrelude = !trackChanged && lyricsChanged && sourceLines_.empty() &&
            !inlineControlsShown &&
            player_source::showsTrackInfoBeforeLyrics(incomingPlayer) &&
            isTaskbarMediaScene(scene_) && frame.scene == DisplayScene::Lyrics &&
            frame.currentLine >= 0 && frame.media.playing && frame.animateTransition &&
            !statusText.empty() && lastLyric_ == statusText;
        if (sceneChanged)
            resetSpectrumEffectHistory();
        const bool sceneWidthChanged = sceneWidthPolicyDiffers(scene_, frame.scene);
        const bool idleChanged =
            frame.idle.sentence != idle.sentence || frame.idle.source != idle.source ||
            frame.idle.loading != idle.loading || frame.idle.showText != idle.showText ||
            frame.idle.showQuote != idle.showQuote || frame.idle.copyEnabled != idle.copyEnabled ||
            frame.idle.quickStartEnabled != idle.quickStartEnabled ||
            frame.idle.todayTasksEnabled != idle.todayTasksEnabled ||
            frame.idle.apps.size() != idle.apps.size();
        const bool wasVisible = isWindowVisible();
        const bool shouldAnimateScene = isWindowVisible() && frame.visible && clientAnimations_ &&
                                        !isMinimalMode() && !isStoppedMode() &&
                                        (isTaskbarSceneTransition(scene_, frame.scene) ||
                                         loadedLyricsFromPrelude) &&
                                        typesetter_.resources().lyricLayout_.Get();
        const bool continueSceneTransition =
            sceneChanged && transition_.kind() == LyricTransitionKind::Scene &&
            isLyricTransitionInProgress() && isTaskbarMediaScene(scene_) &&
            isTaskbarMediaScene(frame.scene);
        const bool mediaIdentityChanged =
            frame.media.title != media.title || frame.media.artist != media.artist ||
            frame.media.sourceAppUserModelId != media.sourceAppUserModelId ||
            frame.media.thumbnail != media.thumbnail;
        // 播放控制器已区分时长补齐和 Soda 的同曲时长修正；即使同批补齐了
        // 封面或专辑字段，也不能把这次元数据更新再次当作切歌播放整卡动画。
        const bool durationOnlyMediaUpdate = trackChanged && frame.durationOnlyUpdate;
        const bool confirmedDurationChange = media.durationMs > 0 && frame.media.durationMs > 0 &&
                                             media.durationMs != frame.media.durationMs;
        const bool songChanged = trackChanged && !trackKey_.empty() && !frame.trackKey.empty() &&
                                 !durationOnlyMediaUpdate &&
                                 (mediaIdentityChanged || confirmedDurationChange);
        if (trackChanged || lyricsChanged)
            karaoke_.invalidateProgress();
        else
            syncKaraokePlayback(frame.actualPositionMs, frame.media.playing);
        const bool mediaChanged = updateMediaInfo(frame.media, trackChanged && !durationOnlyMediaUpdate);
        const bool popupAvailable = mediaPopupAvailable(frame.visible);
        mediaPopup.beginPresentationUpdate();
        mediaPopup.setIdleContent(frame.idle, popupAvailable);
        mediaPopup.setMedia(frame.media, popupAvailable, songChanged);
        // 先同步完整媒体数据，再建立页面转场层，避免 Idle → Media 时目标层
        // 先绘制旧歌曲、随后才收到本帧最新标题/歌手而在转场结束时跳变。
        mediaPopup.setPresentationMode(frame.scene, popupAvailable,
                                       hoverControlStyle_ == HoverControlStyle::Inline);
        mediaPopup.setProgress(frame.actualPositionMs);
        mediaPopup.endPresentationUpdate();

        frameRevision_ = frame.frameRevision;
        requestGeneration_ = frame.requestGeneration;
        trackKey_ = frame.trackKey;
        if (sceneChanged || (loadedLyricsFromPrelude && shouldAnimateScene)) {
            // 类别切换拥有最高优先级：先结束可能正在进行的歌词行转场，
            // 再把旧场景布局交给新的内容块上下翻页。
            if (continueSceneTransition) {
                // Searching/Spectrum/Message 都属于播放侧内容。歌词加载完成时
                // 继续同一页翻转，不能把中间场景当成一次新的类别切换。
                sceneTransition_.setNeedsRelayout(true);
                transition_.setRevision(frame.frameRevision);
            } else {
                resetLyricTransition();
                if (shouldAnimateScene) {
                    sceneTransition_.beginContent(scene_, lastLogicalPxW_, lastLogicalPxH_);
                    transition_.setOutgoingDoubleLine(lyricLayoutDoubleLine_);
                    transition_.setKind(LyricTransitionKind::Scene);
                    transition_.setDirection(frame.scene == DisplayScene::Lyrics ? 1 : -1);
                    setLyricTransitionPending();
                    transition_.setRevision(frame.frameRevision);
                }
            }
        }
        const bool resourceOnlyChanged =
            (embeddedResourceVisible() || verticalResourceVisible()) &&
            ((previousScene == DisplayScene::Idle && statusText.empty()) !=
             (frame.scene == DisplayScene::Idle && frame.statusText.empty()));
        scene_ = frame.scene;
        // 窗口目标宽度依赖空闲文案是否为空，必须用新帧的状态计算转场终点。
        statusText = frame.statusText;
        statusTextIsTrackInfo = frame.statusTextIsTrackInfo;
        if (sceneWidthChanged) {
            if (shouldAnimateScene && !embeddedResourceOnly() && !verticalResourceOnly()) {
                // 与内容转场使用同一段平滑时间曲线，窗口宽度/锚点连续变化，避免
                // 在转场开始或结束时一次性收窄/撑开并触发位图重绑闪烁。
                if (beginSceneWindowResize()) {
                    // 窗口尺寸动画接管本次布局失效；render() 在动画期间不再执行
                    // 一次性的 adjustPosition，成功提交后由失效快照统一确认。
                } else {
                    requestInvalidation(RenderInvalidation::Layout);
                }
            } else {
                cancelSceneWindowResize();
                requestInvalidation(RenderInvalidation::Layout);
            }
        }
        idle = frame.idle;
        positionMs_ = frame.actualPositionMs;
        if (statusOneShotChanged || (statusChanged && frame.statusTextOneShot)) {
            scroll_.setOneShot(frame.statusTextOneShot, monotonicNowMs());
            scroll_.resetLyric();
            scroll_.resetClock();
        }
        if (resourceOnlyChanged)
            requestInvalidation(RenderInvalidation::Layout);
        if (drag_.dragging() && (sceneWidthChanged || resourceOnlyChanged))
            adjustPosition();

        // 只对已有曲目之间的切换做入场动画；首次显示、同曲刷新和会话关闭保持即时提交。
        setSongTransitionPending(songChanged && frame.visible && !isMinimalMode() &&
                                 !isStoppedMode() && clientAnimations_ &&
                                 (!appearance_ || appearance_->songTransitionEnabled));

        const bool sceneTransitionInProgress =
            transition_.kind() == LyricTransitionKind::Scene && isLyricTransitionInProgress();
        if (trackChanged || lyricsChanged) {
            sourceLines_ = frame.lyrics;
            rebuildDisplayLyrics();
            currentLine = frame.currentLine;
            if (!sceneTransitionInProgress) {
                resetLyricTransition();
                typesetter_.clearNextContent();
                nextLyricWidth_ = 0.0f;
                nextLyricHeight_ = 0.0f;
                requestInvalidation(RenderInvalidation::Text);
            } else if (!sceneChanged || continueSceneTransition) {
                sceneTransition_.setNeedsRelayout(true);
                transition_.setRevision(frame.frameRevision);
            }
        } else {
            if (simulateWordByWordLyrics_ && previousDurationMs != media.durationMs &&
                !sourceLines_.empty() && sourceLines_.back().chars.empty()) {
                lines.back().chars = lyric_timing::synthesizeSimulatedKaraokeChars(
                    sourceLines_, sourceLines_.size() - 1, media.durationMs);
                const int displayLine = displayLyricLine();
                if (!lineChanged && displayLine == static_cast<int>(lines.size()) - 1) {
                    clearLyricEffectGeometry();
                    if (!isMinimalMode() && typesetter_.resources().lyricLayout_.Get())
                        karaoke_.build(lines, displayLine,
                                       typesetter_.resources().lyricLayout_.Get());
                    requestFrame();
                }
            }
            if (lineChanged) {
                if (sceneTransitionInProgress) {
                    currentLine = frame.currentLine;
                    sceneTransition_.setNeedsRelayout(true);
                    transition_.setRevision(frame.frameRevision);
                } else {
                    onLyricLineTargetChanged(frame.currentLine, frame.actualPositionMs,
                                             frame.frameRevision, frame.animateTransition);
                }
            } else if (isLyricTransitionInProgress()) {
                // 同一目标行的低频媒体/场景更新不应打断动画，但动画版本要跟随最新完整帧。
                transition_.setRevision(frame.frameRevision);
            }
        }
        if (statusChanged || statusOneShotChanged || sceneChanged) {
            if (sceneTransitionInProgress && (!sceneChanged || continueSceneTransition)) {
                sceneTransition_.setNeedsRelayout(true);
                transition_.setRevision(frame.frameRevision);
            } else {
                requestInvalidation(RenderInvalidation::Text);
            }
        }

        renderState_.setSessionVisible(frame.visible);
        // 无媒体且没有任何待机内容时会停止定位定时器；重新显示时也要恢复
        // 首次探测和暂不可用状态的处理，否则显示抑制可能一直无法解除。
        if (isSessionVisible() &&
            (renderState_.visibilitySuppressed() ||
             placementStatus_ == TaskbarPlacementStatus::NoSpace ||
             placementStatus_ == TaskbarPlacementStatus::Unavailable))
            startPlacementTimer();
        else if (!isSessionVisible())
            stopPlacementTimer();
        reconcileWindowVisibility();
        syncMediaPopupEnabled();
        syncProgressPopup();

        if (isWindowVisible() &&
            (wasVisible != isWindowVisible() || trackChanged || mediaChanged || lyricsChanged ||
             lineChanged || statusChanged || statusOneShotChanged || sceneChanged || idleChanged))
            requestFrameAndFlush();
    }

    // 频谱簇总宽（含柱间间隙）
    float spectrumClusterW() const {
        spectrum_.setContext(spectrumContext());
        return spectrum_.spectrumClusterW();
    }

    float spectrumVisualClusterW(int barCount) const {
        spectrum_.setContext(spectrumContext());
        return spectrum_.spectrumVisualClusterW(barCount);
    }

    bool horizontalImmersiveMode() const { return isExpandedView() && !isVerticalTaskbar(); }

    // 沉浸模式把频谱提升为右侧固定视觉区，长任务栏自动给它更多空间。
    float immersiveSpectrumZoneW(float hostW) const {
        if (!horizontalImmersiveMode() || hostW <= 0.0f)
            return 0.0f;
        return taskbar_detail::expandedSpectrumWidth(hostW);
    }

    float immersiveClockZoneW(float hostW) const {
        if (!horizontalImmersiveMode() || hostW <= 0.0f)
            return 0.0f;
        return taskbar_detail::expandedClockWidth(hostW);
    }

    bool dockResourceExtrasVisible() const { return dockResourcePageCount() > 1; }

    size_t dockResourcePageCount() const { return resourcePanel_.pageCount(); }

    float dockResourceZoneW(float hostW) const {
        if (!isAppBarView() || !resourcePanel_.visibility().hasAnyMetric() || hostW <= 0.0f)
            return 0.0f;
        return std::min(resourceZoneWidth(), std::max(1.0f, hostW - kDockResourceRightPadding));
    }

    D2D1_RECT_F horizontalResourceRect(float hostW, float hostH) const {
        if (hostW <= 0.0f || hostH <= 0.0f)
            return {};
        const float resourceW =
            embeddedResourceVisible()
                ? std::min(resourceZoneWidth(), std::max(1.0f, hostW - kDockResourceRightPadding))
                : dockResourceZoneW(hostW);
        const float resourceX = embeddedResourceAtStart()
                                    ? kDockResourceRightPadding
                                    : hostW - resourceW - kDockResourceRightPadding;
        return D2D1::RectF(resourceX, 0.0f, resourceX + resourceW, hostH);
    }

    bool pointInResourceArea(float clientX, float clientY) const {
        if (!resourceMonitoringEnabled())
            return false;

        float x = 0.0f;
        float y = 0.0f;
        clientPointToLogicalDip(clientX, clientY, x, y);
        if (isVerticalTaskbar()) {
            const auto area = verticalLayout(false).resources;
            return x >= area.left && x < area.right && y >= area.top && y < area.bottom;
        }
        int pxW = 0;
        int pxH = 0;
        logicalClientPixelSize(pxW, pxH);
        const auto area = horizontalResourceRect(dip(pxW), dip(pxH));
        return x >= area.left && x <= area.right && y >= area.top && y <= area.bottom;
    }

    bool hitDockResourceArea(float clientX, float clientY) const {
        return dockResourceExtrasVisible() && pointInResourceArea(clientX, clientY);
    }

    void advanceDockResourcePage(int steps) {
        if (!resourcePanel_.advancePage(steps))
            return;
        requestInvalidation(toMask(RenderInvalidation::Paint) | toMask(RenderInvalidation::Text));
        requestFrameAndFlush();
    }

    int immersiveSpectrumBarCount(float visualW) const {
        spectrum_.setContext(spectrumContext());
        return spectrum_.immersiveSpectrumBarCount(visualW);
    }

    float spectrumContentWForScene(DisplayScene scene, float hostW) const {
        if (scene == DisplayScene::Idle || !spectrumVisible_ || backgroundWaveEnabled() ||
            integratedSpectrum())
            return 0.0f;
        return horizontalImmersiveMode() ? immersiveSpectrumZoneW(hostW) : spectrumClusterW();
    }

    // 嵌入模式按开关预留独立频谱和资源区；沉浸模式始终保留右侧
    // “频谱 + 时钟/资源状态”固定区，避免开关频谱时歌词安全区发生变化。
    float spectrumExtraForScene(DisplayScene scene, float hostW = -1.0f) const {
        if (hostW <= 0.0f) {
            int pxW = 0;
            int pxH = 0;
            logicalClientPixelSize(pxW, pxH);
            hostW = dip(pxW);
        }
        // 沉浸模式的最右侧始终保留给频谱和时钟；关闭频谱只隐藏其可视内容，
        // 不让歌词安全区或居中效果跟着变化。
        if (horizontalImmersiveMode()) {
            const float dockResW = dockResourceZoneW(hostW);
            const float rightPadding =
                (isAppBarView() && dockResW > 0.0f) ? kDockResourceRightPadding : kTextPadding;
            float extra = immersiveSpectrumZoneW(hostW) + rightPadding;
            if (isAppBarView() && dockResW > 0.0f)
                extra += dockResW + kDockResourceZoneGap;
            return extra;
        }
        if (scene == DisplayScene::Idle || !spectrumVisible_ || backgroundWaveEnabled() ||
            integratedSpectrum())
            return embeddedResourceExtra();
        return spectrumContentWForScene(scene, hostW) + kTextPadding + embeddedResourceExtra();
    }

    float rightSideExtraForScene(DisplayScene scene, float hostW = -1.0f) const {
        return spectrumExtraForScene(scene, hostW) - embeddedResourceLeftOffset();
    }

    bool sceneUsesCompactWidth(DisplayScene scene) const {
        return !songInfoVisible_ && scene != DisplayScene::Idle;
    }

    bool sceneWidthPolicyDiffers(DisplayScene from, DisplayScene to) const {
        if (isVerticalTaskbar())
            return songInfoVisible_ && isTaskbarMediaScene(from) != isTaskbarMediaScene(to);
        if (isExpandedView())
            return false;
        return sceneUsesCompactWidth(from) != sceneUsesCompactWidth(to) ||
               pinnedInlineControlsForScene(from) != pinnedInlineControlsForScene(to) ||
               spectrumExtraForScene(from) != spectrumExtraForScene(to);
    }

    void cancelSceneWindowResize() { sceneTransition_.cancelResize(); }

    bool beginSceneWindowResize() {
        if (isExpandedView() || drag_.dragging())
            return false;
        WindowPlacement from;
        WindowPlacement to;
        if (!currentWindowPlacement(from))
            return false;
        const TaskbarPlacementStatus placementStatus = calculateWindowPlacement(to);
        if (placementStatus == TaskbarPlacementStatus::Unavailable ||
            placementStatus == TaskbarPlacementStatus::NoSpace)
            return false;
        return sceneTransition_.beginResize(from, to, monotonicNowMs());
    }

    bool updateSceneWindowResize(ULONGLONG now) {
        if (!isSceneResizeActive())
            return false;
        if (const auto placement = sceneTransition_.advanceResize(now))
            applyWindowPlacement(*placement, false, true);
        return true;
    }

    // 新增样式：普通柱状图，柱底贴近歌词窗口下边缘，电平只向上增长。

    void drawBottomSpectrum(float x, float h, float width) {
        spectrum_.setContext(spectrumContext(drawTarget()));
        spectrum_.drawBottomSpectrum(x, h, width);
        if (spectrum_.consumeRedraw())
            requestFrame();
    }

    // 填充波浪：以 baseY 为底边，width 可覆盖独立频谱区或歌曲内容背景区。

    void drawBackgroundWaveSpectrum(float x, float h, float width) {
        spectrum_.setContext(spectrumContext(drawTarget()));
        spectrum_.drawBackgroundWaveSpectrum(x, h, width);
        if (spectrum_.consumeRedraw())
            requestFrame();
    }

    // 默认保留原有的中线对称频谱效果；柱状图样式从窗口下边缘向上增长。
    // 沉浸模式中，柱状频谱在右侧宽频谱区内居中，梦幻波浪则铺满整个区域。
    void drawSpectrum(float x, float h, float width) {
        spectrum_.setContext(spectrumContext(drawTarget()));
        spectrum_.drawSpectrum(x, h, width);
        if (spectrum_.consumeRedraw())
            requestFrame();
    }

    bool refreshImmersiveClockText() {
        if (!horizontalImmersiveMode())
            return false;

        SYSTEMTIME now{};
        GetLocalTime(&now);
        const uint64_t dateKey = (static_cast<uint64_t>(now.wYear) * 10000ULL) +
                                 (static_cast<uint64_t>(now.wMonth) * 100ULL) + now.wDay;
        const uint64_t minuteKey =
            (dateKey * 10000ULL) + (static_cast<uint64_t>(now.wHour) * 100ULL) + now.wMinute;
        if (clockMinuteKey_ == minuteKey && !clockTimeText_.empty() && !clockDateText_.empty())
            return false;

        wchar_t timeText[64]{};
        wchar_t dateText[64]{};
        if (!GetTimeFormatEx(LOCALE_NAME_USER_DEFAULT, TIME_NOSECONDS, &now, nullptr, timeText,
                             static_cast<int>(_countof(timeText))))
            swprintf_s(timeText, L"%02u:%02u", now.wHour, now.wMinute);
        if (!GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, DATE_SHORTDATE, &now, nullptr, dateText,
                             static_cast<int>(_countof(dateText)), nullptr))
            swprintf_s(dateText, L"%04u/%02u/%02u", now.wYear, now.wMonth, now.wDay);

        const bool changed = clockTimeText_ != timeText || clockDateText_ != dateText;
        clockTimeText_ = timeText;
        clockDateText_ = dateText;
        clockMinuteKey_ = minuteKey;
        return changed;
    }

    void startResourceMonitoring() { resourcePanel_.start(); }

    void stopResourceMonitoring() { resourcePanel_.stop(); }

    bool refreshResourceSnapshot() {
        return resourceMonitoringEnabled() && resourcePanel_.refresh();
    }

    void drawImmersiveClock(float x, float h, float width) {
        auto* rt = renderer.renderTarget();
        if (!rt || width <= 0.0f || !typesetter_.resources().fmtClockTime_.Get() ||
            !typesetter_.resources().fmtClockDate_.Get() || !brushText_.Get() || !brushDim_.Get())
            return;
        typesetter_.resources().fmtClockTime_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);

        const float blockH = std::min(35.0f, std::max(1.0f, h - 2.0f));
        const float top = std::max(0.0f, (h - blockH) * 0.5f);
        const float timeH = blockH * 0.56f;
        rt->DrawTextW(clockTimeText_.c_str(), static_cast<UINT32>(clockTimeText_.size()),
                      typesetter_.resources().fmtClockTime_.Get(),
                      D2D1::RectF(x, top, x + width, top + timeH), brushText_.Get(),
                      D2D1_DRAW_TEXT_OPTIONS_CLIP, DWRITE_MEASURING_MODE_NATURAL);
        rt->DrawTextW(clockDateText_.c_str(), static_cast<UINT32>(clockDateText_.size()),
                      typesetter_.resources().fmtClockDate_.Get(),
                      D2D1::RectF(x, top + timeH, x + width, top + blockH), brushDim_.Get(),
                      D2D1_DRAW_TEXT_OPTIONS_CLIP, DWRITE_MEASURING_MODE_NATURAL);
    }

    void drawDockResourceStatus(float x, float h, float width) {
        if (resourceMonitoringEnabled() && ensureResourceFormat())
            resourcePanel_.draw(renderer.renderTarget(), typesetter_.resources().fmtResource_.Get(),
                                brushText_.Get(), x, h, width);
    }

    DockPetMode dockPetMode() const {
        if (!horizontalImmersiveMode() || !isSessionVisible() || isMinimalMode() || isStoppedMode())
            return DockPetMode::Hidden;
        if (scene_ == DisplayScene::Idle)
            return DockPetMode::Roaming;
        return media.playing ? DockPetMode::Listening : DockPetMode::Paused;
    }

    void drawDockPet(float w, float h, float leftW) {
        const ULONGLONG now = frameNowMs_ != 0 ? frameNowMs_ : monotonicNowMs();
        const DockPetMode mode = dockPetMode();
        if (mode == DockPetMode::Hidden) {
            dockPet_.setMode(mode, now);
            return;
        }

        // Dock 右侧为“频谱 + 资源区”，沉浸模式为“频谱 + 时钟”，统一按右侧固定区起点收束。
        const float spectrumX = w - spectrumExtraForScene(scene_, w);
        const float start =
            songInfoVisible_ && scene_ != DisplayScene::Idle ? kSongInfoLyricGap : kTextPadding;
        const float laneLeft = leftW + start + immersiveControlsWidth(h);
        const float laneRight = mode == DockPetMode::Roaming
                                    ? spectrumX - kTextPadding
                                    : std::min(spectrumX - kTextPadding, laneLeft + kDockPetSeatW);
        const float minLaneWidth = mode == DockPetMode::Roaming ? kPetLaneMinWidthDip : 28.0f;
        if (laneRight <= laneLeft + minLaneWidth) {
            dockPet_.setMode(DockPetMode::Hidden, now);
            return;
        }

        dockPet_.setLane(laneLeft, laneRight, h);
        dockPet_.setMode(mode, now);
        dockPet_.setLightTheme(lightTheme_);
        // 播放中切歌（曲目变化但始终处于 Listening）时补一次雀跃；
        // 暂停/恢复与开始播放的雀跃由 DockPet::setMode 内部处理。
        if (mode == DockPetMode::Listening) {
            const std::wstring track = media.title + L"|" + media.artist;
            if (track != dockPetTrack_) {
                if (!dockPetTrack_.empty())
                    dockPet_.hop(now);
                dockPetTrack_ = track;
            }
        }
        dockPet_.draw(drawTarget());
    }

    void drawImmersiveRightSide(float w, float h, bool showSpectrum) {
        if (!horizontalImmersiveMode())
            return;

        const bool dock = isAppBarView();
        const auto side = taskbar_detail::expandedSideLayout(w, dock, dockResourceZoneW(w));
        const float zoneW = side.zoneWidth;
        const float zoneX = side.zoneX;
        const float resourceW = side.resourceWidth;
        const float resourceX = side.resourceX;
        const float clockW = side.clockWidth;
        const float clockX = side.clockX;
        const float spectrumRight = side.spectrumRight;
        const float spectrumW = side.spectrumWidth;
        if (showSpectrum && spectrumW > 0.0f) {
            if (spectrumStyle_ == SpectrumStyle::DreamyWave) {
                drawSpectrum(zoneX, h, spectrumW);
            } else {
                // 先按原始频谱区确定柱数；Dock 贴右对齐，普通沉浸模式仍按原逻辑居中。
                const int availableCount =
                    std::max(1, static_cast<int>(std::floor((spectrumW + kSpectrumGap) /
                                                            (kSpectrumBarW + kSpectrumGap))));
                const int visualCount = std::min(immersiveSpectrumBarCount(zoneW), availableCount);
                const float clusterW = spectrumVisualClusterW(visualCount);
                const float naturalX = zoneX + std::max(0.0f, (zoneW - clusterW) * 0.5f);
                // Dock 模式将频谱簇贴到右边界，保证它与资源区之间只保留
                // kDockResourceZoneGap；普通沉浸模式继续保持原有居中位置。
                const float clusterX =
                    dock ? std::max(zoneX, spectrumRight - clusterW)
                         : std::max(zoneX, std::min(naturalX, spectrumRight - clusterW));
                spectrum_.setContext(spectrumContext(drawTarget()));
                spectrum_.drawCluster(clusterX, h, clusterW, visualCount);
                if (spectrum_.consumeRedraw())
                    requestFrame();
            }
        }
        if (dock)
            drawDockResourceStatus(resourceX, h, resourceW);
        else
            drawImmersiveClock(clockX, h, clockW);
    }

    float backgroundWaveRight(float w) const {
        if (!horizontalImmersiveMode())
            return w - kTextPadding - embeddedResourceRightExtra();
        if (isAppBarView()) {
            const float dockResW = dockResourceZoneW(w);
            return dockResW > 0.0f
                       ? w - dockResW - kDockResourceRightPadding - kDockResourceZoneGap
                       : w - kTextPadding;
        }
        return w - immersiveClockZoneW(w) - kImmersiveSpectrumClockGap - kTextPadding;
    }

    taskbar_detail::DynamicBackgroundRegions taskbarDynamicBackgroundRegions(float w,
                                                                           float h) const {
        if (scene_ == DisplayScene::NoPlayback ||
            idleQuoteBackground_ == IdleQuoteBackground::None ||
            idleQuoteBackgroundScope_ == IdleQuoteBackgroundScope::None || isMinimalMode())
            return {};
        if (idleQuoteBackgroundScope_ == IdleQuoteBackgroundScope::All)
            return taskbar_detail::dynamicBackgroundRegions(w, h, idleQuoteBackgroundScope_, {}, {});

        std::array<D2D1_RECT_F, 5> playback{};
        std::array<D2D1_RECT_F, 2> idleRegions{};
        const bool idleScene = scene_ == DisplayScene::Idle;
        const bool showQuote = idleScene && idle.showText && idle.showQuote && !statusText.empty();
        if (isVerticalTaskbar()) {
            const auto layout = verticalLayout();
            if (!idleScene && layout.coverSize > 0.0f)
                playback[0] = D2D1::RectF(0.0f, std::max(0.0f, layout.coverY - kCoverPadding), w,
                                          layout.coverY + layout.coverSize);
            playback[1] = layout.songInfo;
            const auto text = D2D1::RectF(0.0f, layout.lyricY, w, layout.lyricBottom);
            if (scene_ == DisplayScene::Lyrics && !layout.showControls)
                playback[2] = text;
            if (showQuote)
                idleRegions[0] = text;
            if (layout.resources.bottom > layout.resources.top)
                idleRegions[1] = layout.resources;
        } else {
            const int pxW = static_cast<int>(std::lround(w * scale()));
            const int pxH = static_cast<int>(std::lround(h * scale()));
            const auto layout = layoutMetricsForScene(scene_, pxW, pxH);
            const auto lyric = lyricAreaForScene(scene_, pxW, pxH);
            const auto text = D2D1::RectF(lyric.x, 0.0f, lyric.x + lyric.w, h);
            const auto pinned = pinnedInlineControlsLayoutForScene(scene_, pxW, pxH);
            const bool showControls = !pinned.active() && mouseOver_ && controlsOnHover_ &&
                                      hoverControlStyle_ == HoverControlStyle::Inline &&
                                      !idleScene && !isExpandedView() &&
                                      !(transition_.kind() == LyricTransitionKind::Scene &&
                                        isLyricTransitionActive() &&
                                        isTaskbarSceneTransition(sceneTransition_.outgoingScene(),
                                                                 scene_));
            if (!idleScene && albumCoverVisible_)
                playback[0] = D2D1::RectF(embeddedResourceLeftOffset(), 0.0f, infoStartX(), h);
            if (!idleScene && songInfoVisible_)
                playback[1] = D2D1::RectF(infoStartX(), 0.0f, layout.leftW, h);
            if (!idleScene && pinned.active())
                playback[4] = D2D1::RectF(layout.leftW, 0.0f, lyric.x, h);
            if (scene_ == DisplayScene::Lyrics && !showControls)
                playback[2] = text;
            if (!idleScene && spectrumVisible_) {
                if (backgroundWaveEnabled()) {
                    playback[3] = D2D1::RectF(infoStartX(), 0.0f, backgroundWaveRight(w), h);
                } else if (!showControls && integratedSpectrum()) {
                    playback[3] = text;
                } else if (!showControls && horizontalImmersiveMode()) {
                    const auto side = taskbar_detail::expandedSideLayout(
                        w, isAppBarView(), dockResourceZoneW(w));
                    playback[3] = D2D1::RectF(side.zoneX, 0.0f, side.spectrumRight, h);
                } else if (!showControls) {
                    const float spectrumW = spectrumContentWForScene(scene_, w);
                    const float right = w - embeddedResourceRightExtra() - kTextPadding;
                    playback[3] = D2D1::RectF(right - spectrumW, 0.0f, right, h);
                }
            }
            if (showQuote)
                idleRegions[0] = text;
            // 资源区独立于播放场景，播放期间也遵守“待机内容”范围选择。
            if (resourceMonitoringEnabled())
                idleRegions[1] = horizontalResourceRect(w, h);
        }
        return taskbar_detail::dynamicBackgroundRegions(w, h, idleQuoteBackgroundScope_, playback,
                                                        idleRegions);
    }

    bool taskbarDynamicBackgroundVisible() const {
        int pxW = 0;
        int pxH = 0;
        if (isVerticalTaskbar())
            clientPixelSize(pxW, pxH);
        else
            logicalClientPixelSize(pxW, pxH);
        return taskbarDynamicBackgroundRegions(dip(pxW), dip(pxH)).count != 0;
    }

    bool taskbarDynamicBackgroundAnimating() const {
        return taskbarDynamicBackgroundVisible() && clientAnimations_;
    }

    void drawFallbackCover(ID2D1DeviceContext* rt, const D2D1_RECT_F& rect, float radius) const {
        if (!rt || !brushDim_.Get())
            return;
        const float side = std::min(rect.right - rect.left, rect.bottom - rect.top);
        if (side <= 0.0f)
            return;

        const D2D1_COLOR_F saved = brushDim_->GetColor();
        brushDim_->SetColor(lightTheme_ ? D2D1::ColorF(0.08f, 0.10f, 0.15f, 0.10f)
                                        : D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.11f));
        rt->FillRoundedRectangle(D2D1::RoundedRect(rect, radius, radius), brushDim_.Get());

        brushDim_->SetColor(lightTheme_ ? D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.10f)
                                        : D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.15f));
        rt->DrawRoundedRectangle(
            D2D1::RoundedRect(D2D1::RectF(rect.left + 0.4f, rect.top + 0.4f, rect.right - 0.4f,
                                          rect.bottom - 0.4f),
                              std::max(0.5f, radius - 0.3f), std::max(0.5f, radius - 0.3f)),
            brushDim_.Get(), 0.75f);

        brushDim_->SetColor(lightTheme_ ? D2D1::ColorF(0.18f, 0.21f, 0.26f, 0.78f)
                                        : D2D1::ColorF(0.92f, 0.94f, 0.97f, 0.80f));
        const D2D1_POINT_2F center{(rect.left + rect.right) * 0.5f,
                                   (rect.top + rect.bottom) * 0.5f};
        media_control::drawMusicNote(rt, controlGeometry, center, side * 0.52f, brushDim_.Get());
        brushDim_->SetColor(saved);
    }

    void drawVinylCover(float coverX, float coverY, float s) {
        vinylRenderer_.draw({
            .rt = drawTarget(),
            .coverBitmap = cover_.bitmap(),
            .coverLayer = coverLayer_.Get(),
            .fallbackDimBrush = brushDim_.Get(),
            .coverX = coverX,
            .coverY = coverY,
            .coverSize = s,
            .discAngle = vinyl_.angle(),
            .armAngle = vinyl_.armAngle(),
            .lightTheme = lightTheme_,
            .hasDominantColor = media.hasDominantColor,
            .dominantColor = media.dominantColor,
        });
    }

    void drawPlatformIcon(float coverX, float coverY, float s) {
        auto* rt = drawTarget();
        if (!rt || !albumCoverVisible_ || !platformIconVisible_ || !platformIconBmp.Get() ||
            s <= 0.0f)
            return;

        const float iconSize = std::max(1.0f, std::min(s * 0.50f, 13.0f));
        const float inset = std::min(1.5f, s * 0.06f);
        D2D1_RECT_F iconRect =
            D2D1::RectF(coverX + s - iconSize - inset, coverY + s - iconSize - inset,
                        coverX + s - inset, coverY + s - inset);
        rt->DrawBitmap(platformIconBmp.Get(), iconRect, 1.0f,
                       D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
    }

    void setAnchor(TaskbarAnchorSpec anchor) {
        anchor = normalizeAnchor(anchor);
        if (anchor_ == anchor)
            return;
        anchor_ = anchor;
        adjustPosition();
        requestFrameAndFlush();
    }

    // Explorer 重启后由托盘主窗口调用（TaskbarCreated 广播只发给顶层窗口）。
    // 作为 Shell_TrayWnd 的子窗口，歌词窗在 explorer 退出时会随任务栏一起被销毁，
    // 此时只剩野句柄，必须整体重建；若幸存（崩溃非正常退出）则重新附着即可。
    // 歌词/媒体/字体等状态都保存在 Impl 成员里，重建窗口后原样恢复
    void onTaskbarCreated() {
        runtime_log::writef(L"[taskbar] TaskbarCreated: hwnd=%p alive=%d visible=%d timer=%d", hwnd,
                            (hwnd && IsWindow(hwnd)) ? 1 : 0, isWindowVisible() ? 1 : 0,
                            timerRunning_ ? 1 : 0);
        if (hwnd && IsWindow(hwnd)) {
            if (findTaskbar()) {
                const bool immersiveBackdropPrepared =
                    viewMode_ == TaskbarViewMode::Immersive && !dockBackdrop_.available();
                if (immersiveBackdropPrepared) {
                    if (!attachment_.embedded() || detachFromTaskbar())
                        initializeBackdropForCurrentViewMode();
                    else
                        backgroundBlurSupported_ = false;
                }
                if (isAppBarView()) {
                    // Explorer 重启后 Shell 的 AppBar 注册表已丢失，本地标记不再可信。
                    appBar_.forget();
                    cancelTaskbarAttachRetry();
                    if (isSessionVisible() && !isStoppedMode())
                        registerAppBar();
                } else {
                    attachment_.reset();
                    if (!attachToTaskbar(hwnd))
                        scheduleTaskbarAttachRetry();
                    else
                        cancelTaskbarAttachRetry();
                }
                if (!immersiveBackdropPrepared)
                    initializeBackdropForCurrentViewMode();
                adjustPosition();
                reconcileWindowVisibility();
                requestFrameAndFlush();
            } else {
                attachment_.reset();
                if (!isAppBarView())
                    scheduleTaskbarAttachRetry();
            }
            return;
        }
        // 旧窗口未收到 WM_DESTROY 时也要停止旧探测，避免迟到结果进入新窗口。
        stopProbe();
        dockBackdrop_.reset();
        runtime_log::removeTaskbarFrameRate(reinterpret_cast<uintptr_t>(hwnd));
        hwnd = nullptr;
        hoverTip_.reset();
        pressedCoverSource_.clear();
        resetImmersivePresence();
        attachment_.reset();
        floatingZOrderCooldownUntilMs_ = 0;
        // Explorer 强制退出时旧窗口可能没有走 WM_DESTROY，Shell 侧与本地的
        // AppBar 注册状态都必须按已失效处理，避免重建窗口跳过 ABM_NEW。
        appBar_.forget();
        // 旧窗口被系统侧销毁时不一定投递 WM_DESTROY（Explorer 被强杀），
        // timerRunning_ 可能残留为 true，但定时器已随旧窗口消失；重建时同样
        // 等待新任务栏的首个探测结果，避免按旧矩形先显示一帧。
        frameScheduler_.stop();
        timerRunning_ = false;
        timerMs_ = 0;
        renderState_.setVisibilitySuppressed(true);
        renderState_.setWindowPhase(RenderState::WindowPhase::Hidden);
        probeReady_ = false;
        placementProbe_.clearResult();
        dragAnchors_.clear();
        drag_.end();
        drag_.clearPreview();
        publishedAnchorOptions_.clear();
        if (createWindow(inst, targetMonitorDeviceName_, viewMode_)) {
            if (isAppBarView()) {
                renderState_.setVisibilitySuppressed(false);
                setPlacementStatus(TaskbarPlacementStatus::Safe);
            }
            reconcileWindowVisibility();
            if (isWindowVisible())
                requestFrameAndFlush();
        }
    }

    void recreateClockFormats() {
        typesetter_.setFactory(renderer.dwrite());
        typesetter_.recreateClockFormats(
            fontFamily_, fontStyle_,
            fitCompactFontSize(fluent::font_sizes::taskbar::clock,
                               (compactTaskbarHeight_ - 2.0f) * 0.56f,
                               DWRITE_FONT_WEIGHT_SEMI_BOLD),
            fitCompactFontSize(fluent::font_sizes::taskbar::date,
                               (compactTaskbarHeight_ - 2.0f) * 0.44f));
    }

    float verticalResourceWidthDip() const {
        if (!isVerticalTaskbar())
            return 0.0f;
        const int taskbarCross = taskbarCrossPixels(rcTaskbar_, taskbarEdge_);
        if (taskbarCross > 0) {
            const int crossMargin = std::max(2, (int)std::lround(2.0f * scale()));
            int crossPx = taskbarCross - crossMargin * 2;
            if (crossPx < 16)
                crossPx = taskbarCross;
            if (crossPx > 0)
                return dip(crossPx);
        }
        int width = 0;
        int height = 0;
        clientPixelSize(width, height);
        return width > 1 ? dip(width) : 0.0f;
    }

    bool ensureResourceFormat() {
        if (!renderer.dwrite())
            renderer.initialize();
        typesetter_.setFactory(renderer.dwrite());
        if (typesetter_.resources().fmtResource_)
            return true;
        const float requested = resourcePanel_.visibility().smallFont
                                    ? 12.0f
                                    : fluent::font_sizes::taskbar::resource;
        const float fitted = fitCompactFontSize(requested, compactTaskbarHeight_ - 2.0f);
        if (!typesetter_.ensureResourceFormat(fontFamily_, fontStyle_, fitted))
            return false;
        const float verticalAvailable =
            ResourcePanel::verticalAvailableWidth(verticalResourceWidthDip());
        if (verticalAvailable > 0.0f) {
            const float needed = resourcePanel_.measureVerticalStackedWidth(
                renderer.dwrite(), typesetter_.resources().fmtResource_.Get());
            if (needed > verticalAvailable) {
                const float shrunk = std::clamp(
                    fitted * (verticalAvailable / needed),
                    fluent::font_sizes::taskbar::minimum, fitted);
                if (shrunk + 0.05f < fitted) {
                    typesetter_.resetResourceFormat();
                    if (!typesetter_.ensureResourceFormat(fontFamily_, fontStyle_, shrunk))
                        return false;
                }
            }
        }
        return true;
    }

    bool updateResourceColumnWidths() {
        return ensureResourceFormat() &&
               resourcePanel_.updateColumnWidths(renderer.dwrite(),
                                                 typesetter_.resources().fmtResource_.Get());
    }

    void recreateResourceFormat() {
        typesetter_.resetResourceFormat();
        if (updateResourceColumnWidths())
            requestInvalidation(RenderInvalidation::Layout);
    }

    float fitCompactFontSize(float requested, float budget,
                             DWRITE_FONT_WEIGHT weight = DWRITE_FONT_WEIGHT_NORMAL) {
        if (compactTaskbarHeight_ <= 0.0f)
            return requested;
        auto* dwrite = renderer.dwrite();
        if (!dwrite)
            return requested;
        IDWriteTextFormat* format = nullptr;
        if (FAILED(dwrite->CreateTextFormat(fontFamily_.c_str(), nullptr,
                                            isBoldFontStyle(fontStyle_) ? DWRITE_FONT_WEIGHT_BOLD
                                                                        : weight,
                                            dwriteStyleOf(fontStyle_), DWRITE_FONT_STRETCH_NORMAL,
                                            requested, L"zh-cn", &format)))
            return requested;
        fluent::applyUiFontFallback(format);
        IDWriteTextLayout* sample = nullptr;
        float fitted = requested;
        if (SUCCEEDED(dwrite->CreateTextLayout(L"永Ag", 3, format, 4096.0f, 4096.0f, &sample))) {
            DWRITE_TEXT_METRICS metrics{};
            if (SUCCEEDED(sample->GetMetrics(&metrics)) && metrics.height > 0.0f)
                fitted *= std::min(1.0f, std::max(1.0f, budget) / metrics.height);
            sample->Release();
        }
        format->Release();
        return fitted;
    }

    void syncCompactTaskbarLayout(int heightPixels) {
        const float height = dip(heightPixels);
        const float taskbarHeight = dip(rcTaskbar_.bottom - rcTaskbar_.top);
        const bool vertical = isVerticalTaskbar();
        const float compactHeight =
            !vertical && !isAppBarView() && taskbarHeight > 0.0f &&
                    taskbarHeight < kCompactTaskbarHeightDip && height > 0.0f
                ? height
                : 0.0f;
        resourcePanel_.setLayout(compactHeight > 0.0f, vertical);
        if (compactHeight == compactTaskbarHeight_)
            return;
        compactTaskbarHeight_ = compactHeight;
        resetLyricTransition();
        clearImmersiveSongContentTransition();
        releaseSpectrumEffectResources();
        recreateFormats();
        requestInvalidation(
            toMask(RenderInvalidation::Text) | toMask(RenderInvalidation::SongInfo) |
            toMask(RenderInvalidation::Layout) | toMask(RenderInvalidation::Geometry) |
            toMask(RenderInvalidation::Cover));
    }

    void recreateFormats() {
        if (updateDepth_ != 0) {
            formatsPending_ = true;
            return;
        }
        recreateResourceFormat();
        IDWriteFactory* dwrite = renderer.dwrite();
        if (!dwrite)
            return;
        recreateClockFormats();
        const float lyricPad = lyricGlow_ ? 6.0f : lyricOutline_ ? 4.0f : 2.0f;
        LyricFontSizes sizes;
        sizes.title = fitCompactFontSize(fluent::font_sizes::taskbar::base * 1.05f,
                                         (compactTaskbarHeight_ - 6.0f) * 1.05f / 1.97f,
                                         DWRITE_FONT_WEIGHT_SEMI_BOLD);
        sizes.artist = fitCompactFontSize(fluent::font_sizes::taskbar::base * 0.92f,
                                          (compactTaskbarHeight_ - 6.0f) * 0.92f / 1.97f);
        sizes.lyric =
            fitCompactFontSize(fontSize_ * fluent::font_sizes::taskbar::mainScale,
                               compactTaskbarHeight_ - lyricPad);
        sizes.next = fontSize_ * fluent::font_sizes::taskbar::previewScale;
        sizes.secondary = fontSize_ * 0.78f;
        typesetter_.setFactory(dwrite);
        typesetter_.recreateLyricFormats(fontFamily_, fontStyle_, sizes);
        requestInvalidation(RenderInvalidation::Text);
        requestInvalidation(RenderInvalidation::SongInfo);
    }

    void changeFont(float delta) { setFont(fontFamily_, fontSize_ + delta, fontStyle_); }

    LyricFontSizeContext fontSizeContext() const {
        LyricFontSizeContext context;
        context.minSize = fluent::font_sizes::taskbar::minimum;
        context.maxSize = fluent::font_sizes::taskbar::maximum;
        RECT client{};
        if (!hwnd || !GetClientRect(hwnd, &client))
            return context;
        const bool vertical = isVerticalTaskbar();
        const float crossAxis =
            static_cast<float>(vertical ? client.right - client.left : client.bottom - client.top) /
            scale();
        const float effectPad = lyricGlow_ ? 4.8f : lyricOutline_ ? 2.4f : 0.0f;
        context.availableDip = std::max(0.0f, crossAxis - 4.0f - effectPad);
        context.mainScale = fluent::font_sizes::taskbar::mainScale;
        if (compactTaskbarHeight_ > 0.0f && typesetter_.resources().fmtLyric_.Get() &&
            fontSize_ > 0.0f)
            context.mainScale = typesetter_.resources().fmtLyric_->GetFontSize() / fontSize_;
        if (!vertical && compactTaskbarHeight_ <= 0.0f && doubleLineLyricsEnabled_) {
            context.extraScale = fluent::font_sizes::taskbar::previewScale;
            context.gapDip = kLyricPreviewGap;
        } else if (!vertical && compactTaskbarHeight_ <= 0.0f &&
                   (translationEnabled_ || romanizationEnabled_)) {
            context.extraScale = 0.78f;
            context.gapDip = 1.0f;
        }
        context.star = lyricStarEnabled(lyricEffectLayoutLine_);
        return context;
    }

    void setFont(const std::wstring& family, float size, LyricFontStyle style) {
        fontFamily_ = family;
        fontSize_ = std::clamp(size, fluent::font_sizes::taskbar::minimum,
                             fluent::font_sizes::taskbar::maximum);
        fontStyle_ = style;
        recreateFormats();
        requestInvalidation(RenderInvalidation::Layout);
        if (hwnd &&
            (isAppBarView() || embeddedResourceVisible() || pinnedInlineControlsConfigured()))
            adjustPosition();
        requestFrameAndFlush();
    }

    void releaseDeviceResources() {
        lyricEffectsRenderer_.discardDeviceResources();
        spectrum_.discardDeviceResources();
        cancelSceneWindowResize();
        typesetter_.discardDeviceResources();
        brushBg_.Reset();
        brushHover_.Reset();
        brushText_.Reset();
        brushDim_.Reset();
        brushBtn_.Reset();
        brushBtnDisabled_.Reset();
        brushLyric_.Reset();
        brushLyricDim_.Reset();
        brushLyricGlow_.Reset();
        brushLyricOutline_.Reset();
        vinylRenderer_.releaseResources();
        brushProgressBg_.Reset();
        brushBackground_.Reset();
        brushIdleWarm_.Reset();
        brushIdleCool_.Reset();
        brushIdleAccent_.Reset();
        dockPet_.discardDeviceResources();
        dragPreviewStroke_.Reset();
        cover_.discardDeviceResources();
        lyricTextRenderer_.discardDeviceResources();
        karaoke_.clearGeometry();
        coverClip_.Reset();
        coverLayer_.Reset();
        songInfoDividerBrush_.Reset();
        media_control::release(controlGeometry);
        if (platformIconBmp.Get()) {
            platformIconBmp.Reset();
        }
        drawTargetOverride_ = nullptr;
        renderState_.setDeviceResourcePhase(RenderState::DeviceResourcePhase::Uninitialized);
        songContentTransition_.reset();
        songRootAnimationInstalled_ = false;
        clearLyricDCompState();
        requestInvalidation(
            toMask(RenderInvalidation::Text) | toMask(RenderInvalidation::SongInfo) |
            toMask(RenderInvalidation::Geometry) | toMask(RenderInvalidation::Layout) |
            toMask(RenderInvalidation::Cover) | toMask(RenderInvalidation::PlatformIcon));
    }

    void discardDeviceResources() {
        releaseDeviceResources();
        renderer.discard();
        deviceGeneration_ = 0;
    }

    bool bindRenderer(int width, int height) {
        if (!renderer.bind(hwnd, width, height))
            return false;
        if (deviceGeneration_ != renderer.deviceGeneration()) {
            // 其他窗口也可能使共享设备失效，不能只依赖本窗口的 Present 失败路径。
            if (deviceGeneration_ != 0)
                releaseDeviceResources();
            deviceGeneration_ = renderer.deviceGeneration();
            if (viewMode_ == TaskbarViewMode::Immersive) {
                const ULONGLONG now = GetTickCount64();
                const float opacity = immersivePresenceOpacity(now);
                renderer.setRootOpacity(opacity);
                if (presence_.startMs() != 0 && (presence_.phase() == ImmersivePresence::Hiding ||
                                                 presence_.phase() == ImmersivePresence::Showing)) {
                    const bool hiding = presence_.phase() == ImmersivePresence::Hiding;
                    const UINT duration = hiding ? kImmersiveHideMs : kImmersiveShowMs;
                    const ULONGLONG elapsed = now - presence_.startMs();
                    if (elapsed < duration)
                        renderer.animateRoot(0.0f, 0.0f, 0.0f, 0.0f, opacity, hiding ? 0.0f : 1.0f,
                                             static_cast<float>(duration - elapsed) / 1000.0f);
                }
            }
        }
        renderer.setDpi(dpi_);
        createDeviceResources();
        return renderState_.deviceResourcePhase() == RenderState::DeviceResourcePhase::Ready;
    }

    void refreshTheme() {
        app_icon::applyTaskbarIcon(hwnd);
        updateBackdropSolidColor();
        const bool light = !fluent::isDarkMode(fluent::ThemeTarget::Taskbar);
        if (light != lightTheme_) {
            lightTheme_ = light;
            discardDeviceResources();
        }
        // 媒体卡片属于普通悬浮窗，使用 Window 主题，而不是任务栏主题。
        mediaPopup.refreshTheme();
        if (isWindowVisible())
            requestFrameAndFlush();
    }

    void releaseAll() {
        discardDeviceResources();
        renderer.releaseAll();
    }

    // ---------- 避让探测工作线程 ----------

    // 正常每 3 秒探测一次：TrafficMonitor、酷狗窗口矩形 + UIA 任务栏按钮矩形；
    // 空间不足隐藏时缩短到 1 秒，尽快发现可恢复空间。UIA 属性查询由 explorer
    // 的 UI 线程执行，不能跟随歌词帧率高频调用。
    // 每次循环都重新发布结果（即使没变化），变化比较在 UI 线程拾取时做

    void startProbe() { placementProbe_.start(); }

    void stopProbe() { placementProbe_.stop(); }

    // UI 线程慢速分支：拾取探测结果，与缓存比较有变化才更新。返回是否有变化
    bool pickProbeResult() {
        auto p = placementProbe_.take();
        if (!p)
            return false;
        const bool firstProbe = !probeReady_;
        probeReady_ = true;
        bool changed = !EqualRect(&p->rcTm, &rcTrafficMonitor_);
        if (!changed) {
            if (p->buttons.size() != uiaButtons_.size()) {
                changed = true;
            } else {
                for (size_t i = 0; i < p->buttons.size(); ++i) {
                    if (!EqualRect(&p->buttons[i], &uiaButtons_[i])) {
                        changed = true;
                        break;
                    }
                }
            }
        }
        if (!changed) {
            if (p->kugouWindows.size() != kugouTaskbarWindows_.size()) {
                changed = true;
            } else {
                for (size_t i = 0; i < p->kugouWindows.size(); ++i) {
                    if (!EqualRect(&p->kugouWindows[i], &kugouTaskbarWindows_[i])) {
                        changed = true;
                        break;
                    }
                }
            }
        }
        if (changed) {
            rcTrafficMonitor_ = p->rcTm;
            uiaButtons_ = std::move(p->buttons);
            kugouTaskbarWindows_ = std::move(p->kugouWindows);
        }
        return changed || firstProbe;
    }

    // ---------- 封面解码 ----------

    void decodeCover() {
        cover_.decode(renderer.renderTarget(), media.thumbnail.get(), dpi_,
                      std::max(1u, static_cast<UINT>(std::ceil(coverSize() * scale()))));
    }

    // 封面模糊链：GaussianBlur → Scale 铺满全窗。效果与设备同生命周期，
    // 封面更换或设备重建后经 coverBlurInput_ 惰性重绑；窗口尺寸每帧可能变化，
    // Scale 参数便宜，直接按帧写入
    ID2D1Effect* ensureCoverBlurChain(float w, float h) {
        return cover_.blurred(renderer.renderTarget(), w, h);
    }

    void decodePlatformIcon() {
        if (platformIconBmp.Get()) {
            platformIconBmp.Reset();
        }
        if (!albumCoverVisible_ || !platformIconVisible_ || media.sourceAppUserModelId.empty())
            return;

        std::vector<BYTE> pixels;
        UINT width = 0;
        UINT height = 0;
        if (!platform_icon::readSourceIconPixels(media.sourceAppUserModelId, pixels, width, height))
            return;

        auto* rt = renderer.renderTarget();
        if (!rt)
            return;
        D2D1_BITMAP_PROPERTIES1 props = D2D1::BitmapProperties1(
            D2D1_BITMAP_OPTIONS_NONE,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
            static_cast<float>(dpi_), static_cast<float>(dpi_));
        ID2D1Bitmap1* decoded = nullptr;
        rt->CreateBitmap(D2D1::SizeU(width, height), pixels.data(), width * 4, &props, &decoded);
        platformIconBmp.Attach(decoded);
    }

    int displayLyricLine() const {
        if (scene_ == DisplayScene::NoPlayback || scene_ == DisplayScene::Idle ||
            scene_ == DisplayScene::Searching || scene_ == DisplayScene::Message)
            return -1;
        if (currentLine >= 0 && (size_t)currentLine < lines.size())
            return currentLine;
        // 支持前奏文案的来源在首句未开始时使用歌曲信息。
        const SmtcPlayerType sourcePlayer =
            player_source::resolvePlayerType(media.sourceAppUserModelId);
        if (player_source::showsTrackInfoBeforeLyrics(sourcePlayer))
            return -1;
        return lines.empty() ? -1 : 0;
    }

    std::wstring_view selectedSecondary(const LyricLine& line) const {
        if (compactTaskbarHeight_ > 0.0f)
            return {};
        if (translationEnabled_)
            return line.translation;
        if (romanizationEnabled_)
            return line.romanization;
        return {};
    }

    bool useDoubleLineLyricsForLine(int lineIndex) const {
        return compactTaskbarHeight_ <= 0.0f && doubleLineLyricsEnabled_ && lineIndex >= 0 &&
               static_cast<size_t>(lineIndex) < lines.size() &&
               selectedSecondary(lines[static_cast<size_t>(lineIndex)]).empty();
    }

    bool useDoubleLineLyrics() const {
        if (isLyricTransitionActive() && typesetter_.resources().lyricLayout_.Get())
            return lyricLayoutDoubleLine_;
        return useDoubleLineLyricsForLine(displayLyricLine());
    }

    // ---------- 行过渡状态 ----------

    void releaseOutgoingLyricLayouts() {
        outgoingLyricEffectLine_ = -1;
        typesetter_.releaseOutgoing();
        outgoingLyricWidth_ = 0.0f;
        outgoingLyricHeight_ = 0.0f;
        outgoingVerticalLyricWidth_ = 0.0f;
        outgoingVerticalLyricHeight_ = 0.0f;
        outgoingVerticalLyricRotated_ = false;
        outgoingVerticalLyricColumns_ = {};
        outgoingSecondaryWidth_ = 0.0f;
        outgoingSecondaryHeight_ = 0.0f;
        outgoingLyricBlockHeight_ = 0.0f;
        outgoingLyricScrollOffset_ = 0.0f;
        outgoingSecondaryScrollOffset_ = 0.0f;
        outgoingNextLyricWidth_ = 0.0f;
        outgoingNextLyricHeight_ = 0.0f;
    }

    // 丢弃当前行过渡：清空待启动/进行中状态并释放旧布局，下一次排版直接提交最终行。
    void resetLyricTransition() {
        if (isSceneResizeActive()) {
            cancelSceneWindowResize();
            requestInvalidation(RenderInvalidation::Layout);
        }
        clearLyricTransitionPhase();
        transition_.setKind(LyricTransitionKind::None);
        sceneTransition_.resetContent();
        transition_.setOutgoingDoubleLine(false);
        transition_.setStartMs(0);
        transition_.setRevision(0);
        transition_.clearTarget();
        transition_.clearPending();
        requestLyricDCompEnd();
        releaseOutgoingLyricLayouts();
        karaoke_.cancelEntry();
    }

    // 行目标变化统一入口：动画进行中只记录最新目标（不重启当前动画），
    // 待启动的过渡直接改打最新目标，空闲时发起新过渡；不允许动画时立即提交最终状态。
    void onLyricLineTargetChanged(int newLine, int64_t actualPositionMs, uint64_t revision,
                                  bool allowAnimate) {
        const int previous = currentLine;
        const SmtcPlayerType sourcePlayer =
            previous == -1 && newLine == 0
                ? player_source::resolvePlayerType(media.sourceAppUserModelId)
                : SmtcPlayerType::Unknown;
        const bool fromPrelude = player_source::showsTrackInfoBeforeLyrics(sourcePlayer) &&
                                 scene_ == DisplayScene::Lyrics && media.playing &&
                                 !statusText.empty() && lastLyric_ == statusText &&
                                 typesetter_.resources().lyricLayout_.Get();
        currentLine = newLine;
        const auto decision = transition_.request(previous, newLine, actualPositionMs, revision,
                                                  allowAnimate && clientAnimations_, fromPrelude);
        if (decision == LyricTransition::Decision::Reset)
            resetLyricTransition();
        if (decision != LyricTransition::Decision::Queued)
            requestInvalidation(RenderInvalidation::Text);
    }

    // 行过渡收尾：先冻结动画再交换状态。释放旧布局、消费动画期间记录的最新目标；
    // 自然转场中的新行保留逐字追赶过程，避免收尾时又跳到真实位置。
    void finalizeLyricTransition(ULONGLONG now) {
        const bool sceneTransition = transition_.kind() == LyricTransitionKind::Scene;
        const bool sceneNeedsRelayout = sceneTransition_.needsRelayout();
        requestLyricDCompEnd();
        releaseOutgoingLyricLayouts();
        clearLyricTransitionPhase();
        transition_.setStartMs(0);

        if (sceneTransition) {
            // 类别转场不消费歌词行的 pendingTarget；场景稳定后用最新完整帧
            // 再做一次排版，避免把转场期间收到的歌词行状态写回旧布局。
            transition_.setKind(LyricTransitionKind::None);
            sceneTransition_.resetContent();
            transition_.setOutgoingDoubleLine(false);
            transition_.clearTarget();
            transition_.clearPending();
            transition_.setRevision(0);
            if (sceneNeedsRelayout)
                requestInvalidation(RenderInvalidation::Text);
            return;
        }

        if (transition_.advancePending(frameRevision_)) {
            // 动画期间收到了更新的目标行：当前布局仍是旧目标，以它为旧行立即
            // 发起向最新目标的过渡，不经过稳定的中间帧。
            requestInvalidation(RenderInvalidation::Text);
            // 布局尚未对应新行：解绑逐字平滑状态，排版完成后直接对齐真实目标。
            karaoke_.retarget(now);
            return;
        }
        transition_.clearTarget();
        transition_.setRevision(0);

        karaoke_.finishTransition(currentLine, typesetter_.resources().lyricLayout_.Get(),
                                  positionMs_, karaokeLine() != nullptr, now);
        clearLyricTransitionPhase();
        transition_.setKind(LyricTransitionKind::None);
        sceneTransition_.resetContent();
        transition_.setOutgoingDoubleLine(false);
    }

    // ---------- 排版 ----------

    // 把一行歌词拆成逐字换行的布局。中文和符号按 Unicode 码点处理，
    // 常见括号转换为竖排字形；避免 UTF-16 代理项被拆开后出现半个字符。
    // 纯拉丁文本走整句旋转布局。


    void buildTextLayouts(float leftW, float rightW) {
        // 文本布局以超宽无换行创建，度量与区域宽度无关，leftW/rightW 仅保留签名兼容
        (void)leftW;
        (void)rightW;
        IDWriteFactory* dwrite = renderer.dwrite();
        if (!dwrite || !typesetter_.resources().fmtTitle_.Get() ||
            !typesetter_.resources().fmtArtist_.Get() || !typesetter_.resources().fmtLyric_.Get())
            return;

        // 歌曲信息（标题/歌手）独立重建：换行只走下面的歌词分支，
        // 歌曲信息变化也不触碰歌词布局与行过渡状态
        if (isInvalidated(RenderInvalidation::SongInfo)) {
            typesetter_.setFactory(dwrite);
            const float columnWidth = isVerticalTaskbar()
                ? std::max(1.0f, (dip(lastPxW_) - taskbar_detail::kSongInfoLineGap) * 0.5f)
                : 0.0f;
            const auto metrics =
                typesetter_.buildSongInfo(media.title, media.artist, songInfoVisible_, columnWidth);
            titleWidth_ = metrics.titleWidth;
            titleHeight_ = metrics.titleHeight;
            artistWidth_ = metrics.artistWidth;
            artistHeight_ = metrics.artistHeight;
            const bool titleChanged = media.title != lastTitle_;
            const bool artistChanged = media.artist != lastArtist_;
            if (titleChanged) {
                scroll_.resetTitle();
                lastTitle_ = media.title;
            }
            if (artistChanged) {
                scroll_.resetArtist();
                lastArtist_ = media.artist;
            }
            if (titleChanged || artistChanged)
                scroll_.resetClock();
        }
        if (!isInvalidated(RenderInvalidation::Text))
            return;
        lyricTextRenderer_.invalidate(); // 布局指针重建，离屏缓存全部失效
        clearLyricEffectGeometry();
        karaoke_.clearGeometry();

        const bool doubleLineLyrics = useDoubleLineLyricsForLine(displayLyricLine());
        // 准备阶段：先把当前布局移交为旧行，目标行布局构建完成后才记录动画起点，
        // 避免“新布局已替换但动画初始位置还没准备好”导致的文字瞬移。
        bool preparedTransition = false;
        if (isLyricTransitionPending() && typesetter_.resources().lyricLayout_.Get()) {
            // 保留旧行离场前的滚动位置。新布局后面会把 scroll_.lyric() 重置为 0，
            // 不能让旧的超长歌词因此在转场第一帧跳回开头。
            const float outgoingLyricOffset = scroll_.lyric();
            const float outgoingSecondaryOffset = scroll_.secondary();
            const bool outgoingDoubleLine = lyricLayoutDoubleLine_;
            typesetter_.retainCurrentForTransition(outgoingDoubleLine);
            outgoingLyricEffectLine_ = lyricEffectLayoutLine_;
            outgoingLyricWidth_ = lyricWidth_;
            outgoingLyricHeight_ = lyricHeight_;
            outgoingVerticalLyricWidth_ = verticalLyricWidth_;
            outgoingVerticalLyricHeight_ = verticalLyricHeight_;
            outgoingVerticalLyricRotated_ = verticalLyricRotated_;
            outgoingVerticalLyricColumns_ = verticalLyricColumns_;
            outgoingLyricScrollOffset_ = outgoingLyricOffset;
            transition_.setOutgoingDoubleLine(outgoingDoubleLine);
            outgoingSecondaryWidth_ = outgoingDoubleLine ? 0.0f : secondaryWidth_;
            outgoingSecondaryHeight_ = outgoingDoubleLine ? 0.0f : secondaryHeight_;
            outgoingNextLyricWidth_ = outgoingDoubleLine ? nextLyricWidth_ : 0.0f;
            outgoingNextLyricHeight_ = outgoingDoubleLine ? nextLyricHeight_ : 0.0f;
            if (!outgoingDoubleLine)
                outgoingSecondaryScrollOffset_ = outgoingSecondaryOffset;
            const float outgoingPreviewH =
                outgoingDoubleLine ? typesetter_.resources().outgoingNextLyricLayout_.Get()
                                         ? kLyricPreviewGap + outgoingNextLyricHeight_
                                         : 0.0f
                : typesetter_.resources().outgoingSecondaryLayout_.Get()
                    ? 1.0f + outgoingSecondaryHeight_
                    : 0.0f;
            outgoingLyricBlockHeight_ = outgoingLyricHeight_ + outgoingPreviewH;
            preparedTransition = true;
        } else {
            resetLyricTransition();
            typesetter_.clearCurrentContent();
        }
        // 排版已接管待启动转场；成功构建目标布局后会转换为 Running。

        typesetter_.clearNextContent();
        nextLyricWidth_ = 0.0f;
        nextLyricHeight_ = 0.0f;

        int displayLine = displayLyricLine();
        const int previousDisplayLine = lyricEffectLayoutLine_;
        const float previousVerticalSecondaryHeight = verticalLyricColumns_.secondaryHeight;
        lyricEffectLayoutLine_ = displayLine;
        std::wstring lyric;
        std::wstring secondary;
        if (displayLine >= 0) {
            lyric = lines[(size_t)displayLine].text;
            secondary = selectedSecondary(lines[(size_t)displayLine]);
        } else {
            lyric = statusText;
        }
        LyricTextInput input;
        input.lyric = lyric;
        input.secondary = secondary;
        input.doubleLine = doubleLineLyrics;
        input.vertical = isVerticalTaskbar();
        if (input.vertical) {
            input.verticalPrimaryScrollUp = displayLine < 0 && statusTextIsTrackInfo;
            int width = 0, height = 0;
            clientPixelSize(width, height);
            input.verticalWidth = dip(width);
            if (verticalLyricColumns_.hasSecondary() &&
                std::abs(verticalLyricColumns_.primaryX + verticalLyricColumns_.primaryWidth +
                             verticalLyricColumns_.secondaryX - input.verticalWidth) < 0.5f)
                input.preferredVerticalSecondaryWidth = verticalLyricColumns_.secondaryWidth;
            input.verticalPrimaryEffectPadding = lyricGlow_ ? 4.8f : lyricOutline_ ? 2.4f : 0.0f;
        }
        if (doubleLineLyrics && displayLine >= 0 &&
            static_cast<size_t>(displayLine + 1) < lines.size())
            input.next = lines[static_cast<size_t>(displayLine + 1)].text;
        typesetter_.setFactory(dwrite);
        const auto metrics = typesetter_.buildContent(input);
        lyricWidth_ = metrics.width;
        lyricHeight_ = metrics.height;
        verticalLyricWidth_ = metrics.verticalWidth;
        verticalLyricHeight_ = metrics.verticalHeight;
        verticalLyricRotated_ = metrics.verticalRotated;
        verticalLyricColumns_ = metrics.verticalColumns;
        secondaryWidth_ = metrics.secondaryWidth;
        secondaryHeight_ = metrics.secondaryHeight;
        nextLyricWidth_ = metrics.nextWidth;
        nextLyricHeight_ = metrics.nextHeight;
        if (isMinimalMode()) {
            // 极简只改变显示策略；歌词行和逐字时间轴仍由上游完整读取，
            // 这里只不创建供逐字绘制使用的几何缓存。
            karaoke_.clearGeometry();
        } else {
            karaoke_.build(lines, displayLine, typesetter_.resources().lyricLayout_.Get());
        }
        lyricLayoutDoubleLine_ = doubleLineLyrics;
        if (preparedTransition) {
            if (typesetter_.resources().lyricLayout_.Get()) {
                // 目标布局就绪后才启动动画计时：准备布局的这一帧不消耗过渡时长。
                const auto startMs = monotonicNowMs();
                transition_.setStartMs(startMs);
                if (transition_.kind() == LyricTransitionKind::Scene)
                    sceneTransition_.synchronizeResizeStart(startMs);
                setLyricTransitionActive();
                transition_.setRevision(frameRevision_);
            } else {
                // 目标行没有可绘制布局：放弃过渡，直接回到稳定状态。
                resetLyricTransition();
            }
        }
        const float extent =
            isVerticalTaskbar() && verticalLyricHeight_ > 0.0f ? verticalLyricHeight_ : lyricWidth_;
        scroll_.setLineTiming(lines, currentLine, extent,
                              isVerticalTaskbar() ? kVerticalLyricGap : kTextPadding * 2.0f);
        bool lyricChanged = lyric != lastLyric_;
        const bool verticalSecondaryChanged =
            input.vertical && (preparedTransition || displayLine != previousDisplayLine ||
                               previousVerticalSecondaryHeight !=
                                   verticalLyricColumns_.secondaryHeight);
        const bool secondaryChanged = secondary != lastSecondary_ || verticalSecondaryChanged;
        if (lyricChanged) {
            scroll_.resetLyric();
            lastLyric_ = lyric;
        }
        if (secondaryChanged) {
            scroll_.resetSecondary();
            lastSecondary_ = secondary;
        }
        if (lyricChanged || secondaryChanged) {
            scroll_.resetClock();
        }
    }

    float coverSize() const {
        if (!hwnd)
            return 24.0f;
        int pxW = 0;
        int pxH = 0;
        logicalClientPixelSize(pxW, pxH);
        const float h = dip(isVerticalTaskbar() ? pxW : pxH);
        return h - kCoverPadding * 2.0f;
    }

    float coverSlotWidth(float heightDip) const {
        return albumCoverVisible_ ? heightDip - kCoverPadding : 0.0f;
    }

    bool hitAlbumCover(float clientX, float clientY, RECT* bounds = nullptr) const {
        if (!albumCoverVisible_ || !isSessionVisible() || isStoppedMode() || !onSourceOpen ||
            media.sourceAppUserModelId.empty() || scene_ == DisplayScene::Idle ||
            scene_ == DisplayScene::NoPlayback || drag_.dragging())
            return false;
        float x = embeddedResourceLeftOffset() + kCoverPadding;
        float y = kCoverPadding;
        float size = coverSize();
        if (isVerticalTaskbar()) {
            const auto layout = verticalLayout();
            x = layout.coverX;
            y = layout.coverY;
            size = layout.coverSize;
        }
        const RECT rect{static_cast<LONG>(std::ceil(x * scale())),
                        static_cast<LONG>(std::ceil(y * scale())),
                        static_cast<LONG>(std::floor((x + size) * scale())),
                        static_cast<LONG>(std::floor((y + size) * scale()))};
        if (bounds)
            *bounds = rect;
        return size > 0.0f && clientX >= rect.left && clientX < rect.right &&
               clientY >= rect.top && clientY < rect.bottom;
    }

    float infoStartX() const {
        return embeddedResourceLeftOffset() +
               taskbar_detail::songInfoStart(albumCoverVisible_,
                                             albumCoverVisible_ ? coverSize() : 0.0f);
    }

    float lyricStartPadding() const {
        return songInfoVisible_ && scene_ != DisplayScene::Idle ? kSongInfoLyricGap : kTextPadding;
    }

    float immersiveControlRadius(float heightDip) const {
        return std::clamp(heightDip * kImmersiveControlRadiusFactor, kImmersiveControlMinRadius,
                          kImmersiveControlMaxRadius);
    }

    float immersiveControlsWidth(float heightDip) const {
        const float radius = immersiveControlRadius(heightDip);
        const float pitch = radius * kImmersiveControlPitchFactor;
        const float groupWidth =
            pitch * static_cast<float>(expandedControlCount() - 1) + radius * 2.0f;
        return groupWidth + kTextPadding * 2.0f;
    }

    VerticalLayout verticalLayout() const {
        return verticalLayout(mouseOver_ && !verticalResourceHovered_);
    }

    VerticalLayout verticalLayout(bool hovered) const {
        int width = 0;
        int height = 0;
        clientPixelSize(width, height);
        const float widthDip = dip(width);
        const float resourceHeight =
            resourceMonitoringEnabled() ? resourcePanel_.verticalHeight(widthDip) : 0.0f;
        return taskbar_detail::verticalLayout(
            widthDip, dip(height), scene_, albumCoverVisible_,
            hovered && controlsOnHover_ && hoverControlStyle_ == HoverControlStyle::Inline,
            media.canSwitchSource, resourceHeight, songInfoVisible_,
            resourcePanel_.visibility().placement, verticalResourceOnly(),
            inlineProgressAvailable());
    }

    bool pinnedInlineControlsConfigured() const {
        return !isExpandedView() && !isVerticalTaskbar() && controlsOnHover_ &&
               hoverControlStyle_ == HoverControlStyle::Inline &&
               inlineControlDisplayMode_ == InlineControlDisplayMode::Always;
    }

    bool pinnedInlineControlsForScene(DisplayScene scene) const {
        return pinnedInlineControlsConfigured() && scene != DisplayScene::Idle &&
               scene != DisplayScene::NoPlayback && !embeddedResourceOnly();
    }

    float pinnedInlineLyricFontSize() const {
        if (auto* fmt = typesetter_.resources().fmtLyric_.Get())
            return fmt->GetFontSize();
        return fontSize_ * fluent::font_sizes::taskbar::mainScale;
    }

    taskbar_detail::PinnedInlineControlsLayout
    pinnedInlineControlsLayoutForScene(DisplayScene scene, int pxW, int pxH) const {
        if (!pinnedInlineControlsForScene(scene) || pxW <= 0 || pxH <= 0)
            return {};
        const float width = dip(pxW);
        const float height = dip(pxH);
        return taskbar_detail::planPinnedInlineControls(
            {width, height, scene, songInfoVisible_, albumCoverVisible_, media.canRepeat,
             spectrumExtraForScene(scene, width), coverSlotWidth(height),
             pinnedInlineLyricFontSize(), media.canSwitchSource, embeddedResourceLeftOffset(),
             inlineProgressAvailable()});
    }

    taskbar_detail::PinnedInlineControlsLayout pinnedInlineControlsLayout() const {
        int pxW = 0;
        int pxH = 0;
        logicalClientPixelSize(pxW, pxH);
        return pinnedInlineControlsLayoutForScene(scene_, pxW, pxH);
    }

    LayoutMetrics layoutMetricsForScene(DisplayScene scene, int pxW, int pxH) const {
        const float width = dip(pxW);
        const float height = dip(pxH);
        const bool expandedControls = isExpandedView() && !isVerticalTaskbar();
        float controlsW = expandedControls ? immersiveControlsWidth(height) : 0.0f;
        bool hasSlotControls = expandedControls;
        if (!expandedControls && pinnedInlineControlsForScene(scene)) {
            const auto pinned = pinnedInlineControlsLayoutForScene(scene, pxW, pxH);
            if (pinned.active()) {
                controlsW = pinned.slotWidth;
                hasSlotControls = true;
            }
        }
        return taskbar_detail::horizontalLayout(
            {width, height, scene, isExpandedView(), hasSlotControls, songInfoVisible_,
             albumCoverVisible_, controlsW, spectrumExtraForScene(scene, width),
             isExpandedView() && songInfoVisible_ && albumCoverVisible_ ? coverSize() : 0.0f,
             coverSlotWidth(height), embeddedResourceLeftOffset()});
    }

    LayoutMetrics layoutMetrics(int pxW, int pxH) const {
        return layoutMetricsForScene(scene_, pxW, pxH);
    }

    struct LyricArea {
        float x = 0.0f;
        float w = 0.0f;
    };

    LyricArea lyricAreaForScene(DisplayScene scene, int pxW, int pxH) const {
        const LayoutMetrics layout = layoutMetricsForScene(scene, pxW, pxH);
        const float start =
            songInfoVisible_ && scene != DisplayScene::Idle ? kSongInfoLyricGap : kTextPadding;
        // 这里返回的是“可绘制安全区”，不再把它当作歌词的居中基准。
        // 右边界统一落在频谱之前，左边界统一落在封面/歌曲信息/沉浸控件之后。
        float left = layout.leftW + start + layout.immersiveControlsW;
        if (horizontalImmersiveMode() && scene != DisplayScene::Idle && isSessionVisible() &&
            !isMinimalMode() && !isStoppedMode())
            left += kDockPetSeatW;
        const float right = layout.w - rightSideExtraForScene(scene, layout.w) - kTextPadding;
        return {left, std::max(1.0f, right - left)};
    }

    // 沉浸模式始终以整条任务栏的几何中心作为歌词锚点。封面、歌曲信息、
    // 操作控件、频谱及时钟只参与安全区和裁剪；当空间不足时由绘制路径
    // 把文字约束在安全区内，不能反过来改变正常歌词的中心位置。
    float immersiveLyricCenterX() const {
        if (!isExpandedView() || isVerticalTaskbar())
            return -1.0f;
        int pxW = 0;
        int pxH = 0;
        logicalClientPixelSize(pxW, pxH);
        const LayoutMetrics layout = layoutMetrics(pxW, pxH);
        if (layout.w <= 0.0f || layout.h <= 0.0f)
            return -1.0f;
        return layout.w * 0.5f;
    }

    // 当前行有逐字时间轴且歌词布局对应该行时返回该行；极简模式强制使用普通横向滚动。
    const LyricLine* karaokeLine() const {
        if (isMinimalMode())
            return nullptr;
        if (currentLine < 0 || (size_t)currentLine >= lines.size())
            return nullptr;
        const LyricLine* line = &lines[(size_t)currentLine];
        if (line->chars.empty() || !typesetter_.resources().lyricLayout_.Get() ||
            line->text != lastLyric_)
            return nullptr;
        return line;
    }


    // 逐字填充目标进度 x（布局像素坐标）：已唱边界 + 当前 token 按时长比例推进；
    // durOut 输出当前 token 时长（ms），供平滑时间常数使用。X 坐标在排版时缓存，
    // 播放过程中只做时间定位，不再每帧调用 DirectWrite 命中测试。

    // 平滑步进：过渡时间常数取当前字时长的 1/4（40~200ms），指数趋近目标，
    // 每个字的过渡快慢随其时长自然变化，且同步误差有界（约 τ）。
    // SMTC 锚点校正造成的目标抖动经低通后不再闪烁；非自然切换或大幅 seek 直接对齐，
    // 自然转场中的新行则从首字平滑追赶真实位置。暂停时保留已绘制的高亮。
    float karaokeSmoothStep(const LyricLine&) {
        return karaoke_.advance(currentLine, typesetter_.resources().lyricLayout_.Get(),
                                positionMs_, clientAnimations_ && media.playing,
                                isLyricTransitionActive(),
                                monotonicNowMs());
    }

    // ---------- 图标几何 ----------

    void ensureGeometry() {
        if (!isInvalidated(RenderInvalidation::Geometry))
            return;
        ID2D1Factory* d2d = renderer.d2d();
        if (!d2d)
            return;

        if (coverClip_.Get()) {
            coverClip_.Reset();
        }
        vinylRenderer_.releaseClipGeometry();
        media_control::release(controlGeometry);

        float s = coverSize();
        D2D1_ROUNDED_RECT rr{D2D1::RectF(0, 0, s, s), 4.0f, 4.0f};
        d2d->CreateRoundedRectangleGeometry(rr, coverClip_.ReleaseAndGetAddressOf());
        if (albumCoverEffect_ == AlbumCoverEffect::Vinyl)
            vinylRenderer_.createClipGeometry(d2d, s);

        if (!media_control::create(d2d, controlGeometry))
            requestInvalidation(RenderInvalidation::Geometry);
    }

    // ---------- 渲染 ----------

    // ---------- 内嵌悬浮控件布局 ----------

    bool inlineControlsLayout(float (&centers)[kInlineControlCount], float& cy, float& r) const {
        if (isExpandedView() || isVerticalTaskbar() || scene_ == DisplayScene::Idle ||
            scene_ == DisplayScene::NoPlayback)
            return false;
        if (!controlsOnHover_ || !mouseOver_ || hoverControlStyle_ != HoverControlStyle::Inline)
            return false;
        int pxW = 0;
        int pxH = 0;
        logicalClientPixelSize(pxW, pxH);
        if (pinnedInlineControlsLayoutForScene(scene_, pxW, pxH).active())
            return false;
        LayoutMetrics layout = layoutMetrics(pxW, pxH);
        if (layout.w <= 0.0f || layout.h <= 0.0f)
            return false;
        const float controlsW = std::max(1.0f, layout.rightW - embeddedResourceRightExtra());
        taskbar_detail::layoutInlineHoverControls(layout.leftW, controlsW, layout.h, media.canRepeat,
                                                  media.canSwitchSource, centers, cy, r,
                                                  inlineProgressAvailable());
        return r > 0.0f;
    }

    // 沉浸模式专属控件：始终显示在歌曲信息分隔线右侧，不受普通任务栏歌词
    // 的悬浮控件开关和样式设置影响。控件组的几何区域也会从歌词区预先扣除。
    bool immersiveControlsLayout(D2D1_POINT_2F centers[kExpandedControlCount], float& cy,
                                 float& r) const {
        if (!isExpandedView() || isVerticalTaskbar())
            return false;

        int pxW = 0;
        int pxH = 0;
        logicalClientPixelSize(pxW, pxH);
        const LayoutMetrics layout = layoutMetrics(pxW, pxH);
        if (layout.w <= 0.0f || layout.h <= 0.0f || layout.immersiveControlsW <= 0.0f)
            return false;

        const float start =
            songInfoVisible_ && scene_ != DisplayScene::Idle ? kSongInfoLyricGap : kTextPadding;
        r = immersiveControlRadius(layout.h);
        const float pitch = r * kImmersiveControlPitchFactor;
        const float groupW = pitch * static_cast<float>(expandedControlCount() - 1) + r * 2.0f;
        const float groupLeft =
            layout.leftW + start + std::max(0.0f, (layout.immersiveControlsW - groupW) * 0.5f);
        cy = layout.h * 0.5f;
        constexpr int order[kExpandedControlCount] = {
            kImmersiveControlPrevious, kImmersiveControlPlayPause, kImmersiveControlNext,
            kImmersiveControlRepeat,   kImmersiveControlProgress,  kImmersiveControlVolume,
            kInlineSourceControl,      kImmersiveControlExit,      kDockControlQuickApps,
            kImmersiveControlMenu};
        int visibleIndex = 0;
        for (int slot : order) {
            if (!expandedControlVisible(slot))
                continue;
            centers[slot] = D2D1::Point2F(groupLeft + r + visibleIndex * pitch, cy);
            ++visibleIndex;
        }
        return true;
    }

    // includeDisabled：让出任务栏的判定把暂不可用的按钮也算作控件区域。
    int hitImmersiveControl(float x, float y, bool includeDisabled = false) const {
        D2D1_POINT_2F centers[kExpandedControlCount]{};
        float cy = 0.0f;
        float r = 0.0f;
        if (!immersiveControlsLayout(centers, cy, r))
            return -1;

        float logicalX = 0.0f;
        float logicalY = 0.0f;
        clientPointToLogicalDip(x, y, logicalX, logicalY);
        for (int i = 0; i < kExpandedControlCount; ++i) {
            if (!expandedControlVisible(i))
                continue;
            bool enabled = true;
            switch (i) {
            case kImmersiveControlPrevious:
                enabled = media.canPrev;
                break;
            case kImmersiveControlPlayPause:
                enabled = media.canPlayPause;
                break;
            case kImmersiveControlNext:
                enabled = media.canNext;
                break;
            case kImmersiveControlRepeat:
                enabled = media.canRepeat;
                break;
            case kImmersiveControlProgress:
                enabled = inlineProgressAvailable();
                break;
            case kDockControlQuickApps:
                enabled = idle.quickStartEnabled && static_cast<bool>(onDockQuickApps);
                break;
            case kImmersiveControlMenu:
                enabled = static_cast<bool>(onImmersiveMenu);
                break;
            default:
                break;
            }
            if (!enabled && !includeDisabled)
                continue;
            if (std::hypot(logicalX - centers[i].x, logicalY - centers[i].y) <= r + 4.0f)
                return i;
        }
        return -1;
    }

    void drawExitImmersiveButton(const D2D1_POINT_2F& center, float radius) {
        auto* rt = renderer.renderTarget();
        if (!rt || !brushBtn_.Get())
            return;

        // 视图边框配合两支向内收起的箭头，明确表达“退出沉浸模式”，
        // 同时避开托盘“退出程序”所使用的门与外向箭头语义。
        const float frame = radius * 0.74f;
        const float start = radius * 0.50f;
        const float end = radius * 0.08f;
        const float head = radius * 0.22f;
        const float stroke = std::clamp(radius * 0.14f, 1.1f, 1.7f);

        rt->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(center.x - frame, center.y - frame,
                                                               center.x + frame, center.y + frame),
                                                   radius * 0.15f, radius * 0.15f),
                                 brushBtn_.Get(), stroke);

        const auto topLeftEnd = D2D1::Point2F(center.x - end, center.y - end);
        rt->DrawLine(D2D1::Point2F(center.x - start, center.y - start), topLeftEnd, brushBtn_.Get(),
                     stroke);
        rt->DrawLine(topLeftEnd, D2D1::Point2F(topLeftEnd.x - head, topLeftEnd.y), brushBtn_.Get(),
                     stroke);
        rt->DrawLine(topLeftEnd, D2D1::Point2F(topLeftEnd.x, topLeftEnd.y - head), brushBtn_.Get(),
                     stroke);

        const auto bottomRightEnd = D2D1::Point2F(center.x + end, center.y + end);
        rt->DrawLine(D2D1::Point2F(center.x + start, center.y + start), bottomRightEnd,
                     brushBtn_.Get(), stroke);
        rt->DrawLine(bottomRightEnd, D2D1::Point2F(bottomRightEnd.x + head, bottomRightEnd.y),
                     brushBtn_.Get(), stroke);
        rt->DrawLine(bottomRightEnd, D2D1::Point2F(bottomRightEnd.x, bottomRightEnd.y + head),
                     brushBtn_.Get(), stroke);
    }

    void drawImmersiveControls(float leftW, float h) {
        auto* rt = renderer.renderTarget();
        if (!rt)
            return;

        if (songInfoVisible_)
            drawSongInfoDivider(leftW, h);

        D2D1_POINT_2F centers[kExpandedControlCount]{};
        float cy = 0.0f;
        float r = 0.0f;
        if (!immersiveControlsLayout(centers, cy, r))
            return;

        if (brushHover_.Get()) {
            for (int i = 0; i < kExpandedControlCount; ++i) {
                if (expandedControlVisible(i) && immersiveControlHover_ == i) {
                    const auto color = brushHover_->GetColor();
                    auto fill = color;
                    if (immersiveControlPressed_ == i)
                        fill.a = std::min(1.0f, color.a * 1.8f);
                    brushHover_->SetColor(fill);
                    const float half = r + 4.0f;
                    const float corner = std::min(6.0f, half * 0.4f);
                    rt->FillRoundedRectangle(
                        D2D1::RoundedRect(D2D1::RectF(centers[i].x - half, centers[i].y - half,
                                                      centers[i].x + half, centers[i].y + half),
                                          corner, corner),
                        brushHover_.Get());
                    brushHover_->SetColor(color);
                }
            }
        }
        drawButton(kImmersiveControlPrevious, centers[kImmersiveControlPrevious], r);
        drawButton(kImmersiveControlPlayPause, centers[kImmersiveControlPlayPause], r);
        drawButton(kImmersiveControlNext, centers[kImmersiveControlNext], r);
        if (expandedControlVisible(kImmersiveControlRepeat))
            drawRepeatButton(centers[kImmersiveControlRepeat], r);
        if (expandedControlVisible(kImmersiveControlProgress))
            drawProgressButton(centers[kImmersiveControlProgress], r);
        drawVolumeButton(centers[kImmersiveControlVolume], r);
        if (media.canSwitchSource)
            drawSwitchSourceButton(centers[kInlineSourceControl], r);
        drawExitImmersiveButton(centers[kImmersiveControlExit], r);
        if (expandedControlVisible(kDockControlQuickApps)) {
            settings_icon::draw(rt, settings_icon::Kind::QuickLaunch,
                                D2D1::RectF(centers[kDockControlQuickApps].x - r * 0.82f,
                                            centers[kDockControlQuickApps].y - r * 0.82f,
                                            centers[kDockControlQuickApps].x + r * 0.82f,
                                            centers[kDockControlQuickApps].y + r * 0.82f),
                                onDockQuickApps && idle.quickStartEnabled
                                    ? brushBtn_.Get() : brushBtnDisabled_.Get(), 1.45f);
        }
        if (brushBtn_.Get()) {
            const float dotRadius = std::clamp(r * 0.13f, 1.0f, 1.6f);
            const float dotPitch = r * 0.48f;
            for (int i = -1; i <= 1; ++i) {
                rt->FillEllipse(
                    D2D1::Ellipse(D2D1::Point2F(centers[kImmersiveControlMenu].x + i * dotPitch,
                                                centers[kImmersiveControlMenu].y),
                                  dotRadius, dotRadius),
                    brushBtn_.Get());
            }
        }
    }

    void drawInlineControlBackground(int index, const D2D1_POINT_2F& center, float radius,
                                     bool enabled) {
        auto* rt = drawTarget();
        if (!rt || !brushHover_.Get() || !enabled || isExpandedView() || !mouseOver_ ||
            inlineControlHover_ != index)
            return;
        const bool pressed = drag_.pressed() && !drag_.dragging() && GetCapture() == hwnd;
        const auto color = brushHover_->GetColor();
        auto fill = color;
        if (pressed)
            fill.a = std::min(1.0f, color.a * 1.8f);
        brushHover_->SetColor(fill);
        const float half = radius + 2.0f;
        const float corner = std::min(6.0f, half * 0.4f);
        rt->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(center.x - half, center.y - half,
                                                               center.x + half, center.y + half),
                                                   corner, corner),
                                 brushHover_.Get());
        brushHover_->SetColor(color);
    }

    // 内嵌控件的循环按钮：三档（顺序/单曲/随机）各自一个字形，不靠颜色区分。
    void drawRepeatButton(const D2D1_POINT_2F& c, float r) {
        auto* rt = drawTarget();
        if (!rt)
            return;
        drawInlineControlBackground(3, c, r, true);
        media_control::drawRepeat(rt, controlGeometry, c, r * 0.80f, brushBtn_.Get(),
                                  media.repeatMode);
    }

    // ---------- 点击循环后的模式提示 ----------

    static const wchar_t* repeatModeName(int mode) {
        switch (mode) {
        case 2: return L"单曲循环";
        case 3: return L"随机播放";
        case 1: return L"列表循环";
        default: return L"顺序播放";
        }
    }

    // 点击后先等一小拍再出提示词：此时图标已被本地值切到目标模式，两者一致。
    void armRepeatHint(int targetMode) {
        repeatHintTarget_ = targetMode;
        repeatHintUntil_ = 0;
        if (hwnd)
            SetTimer(hwnd, kRepeatHintTimerId, 350, nullptr);
    }

    void onRepeatHintTimer() {
        if (repeatHintTarget_ >= 0) {
            repeatHintText_ = repeatModeName(repeatHintTarget_);
            repeatHintTarget_ = -1;
            repeatHintUntil_ = GetTickCount64() + kRepeatHintMs;
            if (hwnd)
                SetTimer(hwnd, kRepeatHintTimerId, kRepeatHintMs, nullptr);
            requestFrameAndFlush();
            return;
        }
        if (hwnd)
            KillTimer(hwnd, kRepeatHintTimerId);
        repeatHintUntil_ = 0;
        requestFrameAndFlush();
    }

    bool repeatHintActive() const {
        return repeatHintUntil_ != 0 && GetTickCount64() < repeatHintUntil_;
    }

    void drawRepeatHint(ID2D1RenderTarget* rt, const D2D1_RECT_F& area, ID2D1Brush* brush) {
        auto* format = typesetter_.resources().fmtRepeatHint_.Get();
        if (!rt || !brush || !format || repeatHintText_.empty() || !repeatHintActive())
            return;
        rt->DrawTextW(repeatHintText_.c_str(), static_cast<UINT32>(repeatHintText_.size()),
                      format, area, brush);
    }

    void drawSongInfoSubtitle(float infoX, float infoW, float subtitleY) {
        if (repeatHintActive()) {
            // 副标题行临时改为播放模式提示，省掉额外浮层，也不挤占按钮区。
            const D2D1_RECT_F hintRect =
                D2D1::RectF(infoX, subtitleY, infoX + infoW, subtitleY + artistHeight_);
            drawRepeatHint(drawTarget(), hintRect, brushText_.Get());
        } else {
            drawScrollingText(typesetter_.resources().artistLayout_.Get(), artistWidth_,
                              artistHeight_, infoW, infoX, subtitleY, scroll_.artist(),
                              brushDim_.Get(), nullptr, nullptr, nullptr, 0.0f, 1.0f,
                              LyricAlignment::Center, false, kTextPadding, true, kTextPadding);
        }
    }

    void drawVolumeButton(const D2D1_POINT_2F& c, float r) {
        auto* rt = drawTarget();
        if (!rt)
            return;
        drawInlineControlBackground(4, c, r, appVolume_.available);
        ID2D1SolidColorBrush* brush =
            appVolume_.available ? brushBtn_.Get() : brushBtnDisabled_.Get();
        const int level = !appVolume_.available || appVolume_.muted ? 0
                          : appVolume_.percent == 0                 ? 1
                          : appVolume_.percent < 50                 ? 2
                                                                    : 3;
        media_control::drawVolume(rt, c, r * 0.8f, brush, level);
    }

    void drawProgressButton(const D2D1_POINT_2F& c, float r) {
        auto* rt = drawTarget();
        if (!rt)
            return;
        const bool enabled = inlineProgressAvailable();
        drawInlineControlBackground(kInlineProgressControl, c, r, enabled);
        ID2D1SolidColorBrush* brush = enabled ? brushBtn_.Get() : brushBtnDisabled_.Get();
        media_control::drawProgress(rt, c, r * 0.8f, brush);
    }

    void drawPinnedInlineControls(const taskbar_detail::PinnedInlineControlsLayout& pinned) {
        if (!pinned.active())
            return;
        for (int i = 0; i < 3; ++i) {
            if (pinned.visible[i])
                drawButton(i, D2D1::Point2F(pinned.centerX[i], pinned.centerY), pinned.radius);
        }
        if (pinned.visible[3] && media.canRepeat)
            drawRepeatButton(D2D1::Point2F(pinned.centerX[3], pinned.centerY), pinned.radius);
        if (pinned.visible[kInlineProgressControl] && inlineProgressAvailable())
            drawProgressButton(D2D1::Point2F(pinned.centerX[kInlineProgressControl], pinned.centerY),
                               pinned.radius);
        if (pinned.visible[4])
            drawVolumeButton(D2D1::Point2F(pinned.centerX[4], pinned.centerY), pinned.radius);
        if (pinned.visible[kInlineSourceControl])
            drawSwitchSourceButton(
                D2D1::Point2F(pinned.centerX[kInlineSourceControl], pinned.centerY), pinned.radius);
    }

    void drawSwitchSourceButton(const D2D1_POINT_2F& center, float radius) {
        drawInlineControlBackground(kInlineSourceControl, center, radius, true);
        settings_icon::draw(drawTarget(), settings_icon::Kind::Sync,
                            D2D1::RectF(center.x - radius, center.y - radius,
                                        center.x + radius, center.y + radius),
                            brushBtn_.Get(), 1.6f);
    }

    void drawInlineHoverControls() {
        float centers[kInlineControlCount]{};
        float cy = 0.0f;
        float radius = 0.0f;
        if (!inlineControlsLayout(centers, cy, radius))
            return;
        for (int i = 0; i < 3; ++i)
            drawButton(i, D2D1::Point2F(centers[i], cy), radius);
        if (media.canRepeat)
            drawRepeatButton(D2D1::Point2F(centers[3], cy), radius);
        if (inlineProgressAvailable())
            drawProgressButton(D2D1::Point2F(centers[kInlineProgressControl], cy), radius);
        drawVolumeButton(D2D1::Point2F(centers[4], cy), radius);
        if (media.canSwitchSource)
            drawSwitchSourceButton(D2D1::Point2F(centers[kInlineSourceControl], cy), radius);
    }

    bool pointInPinnedControlsZone(float clientX, float clientY) const {
        const auto pinned = pinnedInlineControlsLayout();
        if (!pinned.active())
            return false;
        int pxW = 0;
        int pxH = 0;
        logicalClientPixelSize(pxW, pxH);
        if (pxW <= 0 || pxH <= 0)
            return false;
        float logicalX = 0.0f;
        float logicalY = 0.0f;
        clientPointToLogicalDip(clientX, clientY, logicalX, logicalY);
        const float h = dip(pxH);
        return logicalX >= pinned.boundsLeft && logicalX <= pinned.boundsRight &&
               logicalY >= 0.0f && logicalY <= h;
    }

    int hitPinnedInlineControl(float clientX, float clientY, bool includeDisabled = false) const {
        const auto pinned = pinnedInlineControlsLayout();
        if (!pinned.active())
            return -1;
        float logicalX = 0.0f;
        float logicalY = 0.0f;
        clientPointToLogicalDip(clientX, clientY, logicalX, logicalY);
        const float hitRadius = pinned.radius + 3.5f;
        int bestIndex = -1;
        float bestDist = hitRadius;
        for (int i = 0; i < kInlineControlCount; ++i) {
            if (!pinned.visible[i])
                continue;
            bool enabled = true;
            switch (i) {
            case 0:
                enabled = media.canPrev;
                break;
            case 1:
                enabled = media.canPlayPause;
                break;
            case 2:
                enabled = media.canNext;
                break;
            case 3:
                enabled = media.canRepeat;
                break;
            case kInlineProgressControl:
                enabled = inlineProgressAvailable();
                break;
            default:
                break;
            }
            if (!enabled && !includeDisabled)
                continue;
            const float dist =
                std::hypot(logicalX - pinned.centerX[i], logicalY - pinned.centerY);
            if (dist <= bestDist) {
                bestDist = dist;
                bestIndex = i;
            }
        }
        return bestIndex;
    }

    bool hitVolumeButton(float x, float y) const {
        if (isExpandedView())
            return hitImmersiveControl(x, y) == kImmersiveControlVolume;
        return hitInlineControl(x, y) == kImmersiveControlVolume;
    }

    bool hitProgressButton(float x, float y) const {
        if (isExpandedView())
            return hitImmersiveControl(x, y) == kImmersiveControlProgress;
        return hitInlineControl(x, y) == kInlineProgressControl;
    }

    // 进度按钮的屏幕坐标矩形（进度滑块浮窗的锚点）
    RECT progressButtonScreenRect() const {
        if (isExpandedView()) {
            D2D1_POINT_2F centers[kExpandedControlCount]{};
            float cy = 0.0f;
            float r = 0.0f;
            if (immersiveControlsLayout(centers, cy, r)) {
                const float s = scale();
                POINT pt = logicalDipToClientPoint(centers[kImmersiveControlProgress].x,
                                                   centers[kImmersiveControlProgress].y);
                ClientToScreen(hwnd, &pt);
                const int half = static_cast<int>(std::lround((r + 6.0f) * s));
                return RECT{pt.x - half, pt.y - half, pt.x + half, pt.y + half};
            }
        }
        if (isVerticalTaskbar()) {
            const VerticalLayout layout = verticalLayout();
            if (layout.showControls && layout.showProgressControl) {
                const float s = scale();
                POINT pt =
                    logicalDipToClientPoint(layout.controlCenters[5].x, layout.controlCenters[5].y);
                ClientToScreen(hwnd, &pt);
                const int half = static_cast<int>(std::lround((layout.controlRadius + 6.0f) * s));
                return RECT{pt.x - half, pt.y - half, pt.x + half, pt.y + half};
            }
        }
        if (const auto pinned = pinnedInlineControlsLayout();
            pinned.active() && pinned.visible[kInlineProgressControl]) {
            const float s = scale();
            POINT pt =
                logicalDipToClientPoint(pinned.centerX[kInlineProgressControl], pinned.centerY);
            ClientToScreen(hwnd, &pt);
            const int half = static_cast<int>(std::lround((pinned.radius + 5.0f) * s));
            return RECT{pt.x - half, pt.y - half, pt.x + half, pt.y + half};
        }
        float centers[kInlineControlCount]{};
        float cy = 0.0f;
        float r = 0.0f;
        if (!inlineControlsLayout(centers, cy, r)) {
            RECT rc{};
            GetWindowRect(hwnd, &rc);
            return rc;
        }
        const float s = scale();
        POINT pt = logicalDipToClientPoint(centers[kInlineProgressControl], cy);
        ClientToScreen(hwnd, &pt);
        const int half = static_cast<int>(std::lround((r + 6.0f) * s));
        return RECT{pt.x - half, pt.y - half, pt.x + half, pt.y + half};
    }

    // 音量按钮的屏幕坐标矩形（音量滑块浮窗的锚点）
    RECT volumeButtonScreenRect() const {
        if (isExpandedView()) {
            D2D1_POINT_2F centers[kExpandedControlCount]{};
            float cy = 0.0f;
            float r = 0.0f;
            if (immersiveControlsLayout(centers, cy, r)) {
                const float s = scale();
                POINT pt = logicalDipToClientPoint(centers[kImmersiveControlVolume].x,
                                                   centers[kImmersiveControlVolume].y);
                ClientToScreen(hwnd, &pt);
                const int half = static_cast<int>(std::lround((r + 6.0f) * s));
                return RECT{pt.x - half, pt.y - half, pt.x + half, pt.y + half};
            }
        }
        if (isVerticalTaskbar()) {
            const VerticalLayout layout = verticalLayout();
            if (layout.showControls) {
                const float s = scale();
                POINT pt =
                    logicalDipToClientPoint(layout.controlCenters[3].x, layout.controlCenters[3].y);
                ClientToScreen(hwnd, &pt);
                const int half = static_cast<int>(std::lround((layout.controlRadius + 6.0f) * s));
                return RECT{pt.x - half, pt.y - half, pt.x + half, pt.y + half};
            }
        }
        if (const auto pinned = pinnedInlineControlsLayout(); pinned.active() && pinned.visible[4]) {
            const float s = scale();
            POINT pt = logicalDipToClientPoint(pinned.centerX[4], pinned.centerY);
            ClientToScreen(hwnd, &pt);
            const int half = static_cast<int>(std::lround((pinned.radius + 5.0f) * s));
            return RECT{pt.x - half, pt.y - half, pt.x + half, pt.y + half};
        }
        float centers[kInlineControlCount]{};
        float cy = 0.0f;
        float r = 0.0f;
        if (!inlineControlsLayout(centers, cy, r)) {
            RECT rc{};
            GetWindowRect(hwnd, &rc);
            return rc;
        }
        const float s = scale();
        POINT pt = logicalDipToClientPoint(centers[4], cy);
        ClientToScreen(hwnd, &pt);
        const int half = static_cast<int>(std::lround((r + 6.0f) * s));
        return RECT{pt.x - half, pt.y - half, pt.x + half, pt.y + half};
    }

    void prepareForExternalPopup() {
        cancelImmersiveYield();
        volumeHover_ = false;
        immersiveControlHover_ = -1;
        volumePopup_.onAnchorLeave();
        volumePopup_.hide();
        hideProgressPopup();
        mediaPopup.onAnchorLeave();
        mediaPopup.hideImmediate();
        requestFrameAndFlush();
    }

    void openTaskbarMenu(POINT screenPoint) {
        if (!onContextMenu)
            return;
        prepareForExternalPopup();
        onContextMenu(screenPoint);
    }

    void openImmersiveMenu(POINT screenPoint) {
        if (!onImmersiveMenu)
            return;
        prepareForExternalPopup();
        onImmersiveMenu(screenPoint);
    }

    // ---------- 沉浸模式让出任务栏 ----------
    bool immersiveYieldAvailable() const {
        return viewMode_ == TaskbarViewMode::Immersive && hwnd && taskbar_ && isWindowVisible();
    }

    // 与 CompositionRenderer::animateRoot 的 smoothstep 曲线一致，用于中途反向时取当前透明度。
    float immersivePresenceOpacity(ULONGLONG now) const { return presence_.opacity(now); }

    // 内容（DComp 根视觉）与模糊背景（Windows.UI.Composition 层）同步淡入淡出。
    void animateImmersivePresence(float from, float to, UINT durationMs) {
        if (!renderer.animateRoot(0.0f, 0.0f, 0.0f, 0.0f, from, to,
                                  static_cast<float>(durationMs) / 1000.0f))
            renderer.setRootOpacity(to);
        renderer.commit();
        dockBackdrop_.animatePresence(from, to, durationMs);
    }

    void startImmersivePresenceTimer() {
        if (immersivePresenceTimerRunning_ || !hwnd)
            return;
        if (SetTimer(hwnd, kImmersivePresenceTimerId, kImmersivePresencePollMs, nullptr))
            immersivePresenceTimerRunning_ = true;
    }

    void stopImmersivePresenceTimer() {
        if (immersivePresenceTimerRunning_ && hwnd)
            KillTimer(hwnd, kImmersivePresenceTimerId);
        immersivePresenceTimerRunning_ = false;
    }

    void cancelImmersiveYield() {
        if (immersiveYieldArmed_ && hwnd)
            KillTimer(hwnd, kImmersiveYieldTimerId);
        immersiveYieldArmed_ = false;
    }

    bool immersiveYieldAllowedState() const {
        return presence_.phase() == ImmersivePresence::Shown ||
               presence_.phase() == ImmersivePresence::Showing;
    }

    // 鼠标移动时更新让出判定：只有停在播放控件以外的区域才开始计时，
    // 划过控件之间的空隙去点按钮不会误触发。
    void updateImmersiveYield(int hoveredControl) {
        if (immersiveYieldAvailable() && (hoveredControl >= 0 || immersiveControlPressed_ >= 0))
            cancelImmersiveHide();
        if (!immersiveYieldAvailable() || !immersiveYieldAllowedState() || hoveredControl >= 0 ||
            immersiveControlPressed_ >= 0) {
            cancelImmersiveYield();
            return;
        }
        if (!immersiveYieldArmed_ &&
            SetTimer(hwnd, kImmersiveYieldTimerId, kImmersiveYieldDelayMs, nullptr))
            immersiveYieldArmed_ = true;
    }

    void onImmersiveYieldTimer() {
        cancelImmersiveYield();
        if (!immersiveYieldAvailable() || !immersiveYieldAllowedState() ||
            immersiveControlPressed_ >= 0)
            return;
        POINT cursor{};
        RECT windowRect{};
        if (!GetCursorPos(&cursor) || !GetWindowRect(hwnd, &windowRect) ||
            !PtInRect(&windowRect, cursor))
            return;
        POINT client = cursor;
        ScreenToClient(hwnd, &client);
        if (hitImmersiveControl(static_cast<float>(client.x), static_cast<float>(client.y), true) >=
            0)
            return;
        beginImmersiveHide();
    }

    void beginImmersiveHide() {
        const ULONGLONG now = GetTickCount64();
        const float from = immersivePresenceOpacity(now);
        presence_.beginHide(now, from);
        volumeHover_ = false;
        immersiveControlHover_ = -1;
        volumePopup_.onAnchorLeave();
        volumePopup_.hide();
        hideProgressPopup();
        animateImmersivePresence(from, 0.0f, kImmersiveHideMs);
        startImmersivePresenceTimer();
        requestFrameAndFlush();
    }

    void cancelImmersiveHide() {
        cancelImmersiveYield();
        if (presence_.phase() != ImmersivePresence::Hiding)
            return;
        const ULONGLONG now = GetTickCount64();
        const float from = immersivePresenceOpacity(now);
        presence_.beginShow(now, from);
        animateImmersivePresence(from, 1.0f, kImmersiveShowMs);
        startImmersivePresenceTimer();
    }

    void finishImmersiveHide() {
        presence_.hide();
        const bool hadPress = immersiveControlPressed_ >= 0;
        immersiveControlPressed_ = -1;
        if (hadPress && GetCapture() == hwnd)
            ReleaseCapture();
        mouseOver_ = false;
        trackingLeave_ = false;
        immersiveControlHover_ = -1;
        volumeHover_ = false;
        volumePopup_.onAnchorLeave();
        volumePopup_.hide();
        hideProgressPopup();
        reconcileWindowVisibility();
    }

    void beginImmersiveShow() {
        presence_.prepareShow();
        // 先以透明状态完成首帧并显示窗口，再启动淡入，避免隐藏期间的旧帧闪现。
        renderer.setRootOpacity(0.0f);
        dockBackdrop_.animatePresence(0.0f, 0.0f, 0);
        requestInvalidation(RenderInvalidation::Paint);
        reconcileWindowVisibility(true);
        if (!isWindowVisible()) {
            // 停止模式、会话不可见等其他原因仍要求隐藏：直接回到常态，交由原有可见性逻辑。
            resetImmersivePresence();
            return;
        }
        presence_.startShow(GetTickCount64());
        animateImmersivePresence(0.0f, 1.0f, kImmersiveShowMs);
        startImmersivePresenceTimer();
    }

    // 鼠标离开任务栏且没有按住按键（例如正往任务栏按钮上拖放文件）时才恢复。
    bool immersiveCursorAwayFromTaskbar() const {
        POINT cursor{};
        if (!GetCursorPos(&cursor))
            return false;
        RECT taskbarRect{};
        if (taskbar_ && GetWindowRect(taskbar_, &taskbarRect) && PtInRect(&taskbarRect, cursor))
            return false;
        for (const int button : {VK_LBUTTON, VK_RBUTTON, VK_MBUTTON}) {
            if ((GetAsyncKeyState(button) & 0x8000) != 0)
                return false;
        }
        return true;
    }

    void onImmersivePresenceTimer() {
        if (viewMode_ != TaskbarViewMode::Immersive) {
            resetImmersivePresence();
            return;
        }
        const bool away =
            presence_.phase() == ImmersivePresence::Hidden && immersiveCursorAwayFromTaskbar();
        switch (presence_.advance(GetTickCount64(), away)) {
        case TaskbarPresence::Action::Hide:
            finishImmersiveHide();
            break;
        case TaskbarPresence::Action::Show:
            beginImmersiveShow();
            break;
        case TaskbarPresence::Action::StopTimer:
            stopImmersivePresenceTimer();
            break;
        case TaskbarPresence::Action::None:
            break;
        }
    }

    // 切换展示模式、窗口销毁等场景立即回到常态（不透明、不让出）。
    void resetImmersivePresence() {
        cancelImmersiveYield();
        stopImmersivePresenceTimer();
        const bool wasShown = presence_.phase() == ImmersivePresence::Shown;
        presence_.reset();
        if (!wasShown) {
            renderer.setRootOpacity(1.0f);
            renderer.commit();
            dockBackdrop_.animatePresence(1.0f, 1.0f, 0);
        }
    }

    void openDockQuickApps(POINT screenPoint) {
        if (!isAppBarView() || !onDockQuickApps || !idle.quickStartEnabled)
            return;
        prepareForExternalPopup();
        onDockQuickApps(screenPoint);
    }

    void activateImmersiveControl(int index) {
        switch (index) {
        case kImmersiveControlPrevious:
        case kImmersiveControlPlayPause:
        case kImmersiveControlNext:
        case kImmersiveControlRepeat:
        case kInlineSourceControl:
            if (index == kImmersiveControlRepeat)
                armRepeatHint(SmtcSnapshot::nextPlayMode(media.repeatMode));
            if (onControl)
                onControl(static_cast<MediaControl>(index), media.sessionGeneration, 0);
            break;
        case kImmersiveControlProgress:
            if (inlineProgressAvailable()) {
                volumeHover_ = false;
                volumePopup_.onAnchorLeave();
                volumePopup_.hide();
                syncProgressPopup();
                progressPopup_.onAnchorEnter();
                progressPopup_.showNear(progressButtonScreenRect(), false, taskbarEdge_ == ABE_LEFT);
            }
            break;
        case kImmersiveControlVolume:
            if (appVolume_.available) {
                hideProgressPopup();
                volumePopup_.onAnchorEnter();
                volumePopup_.showNear(volumeButtonScreenRect(), false, taskbarEdge_ == ABE_LEFT);
            }
            break;
        case kImmersiveControlExit:
            if (onImmersiveExit)
                onImmersiveExit();
            break;
        case kDockControlQuickApps: {
            POINT pt{};
            if (!GetCursorPos(&pt)) {
                RECT rc{};
                GetWindowRect(hwnd, &rc);
                pt = POINT{(rc.left + rc.right) / 2, (rc.top + rc.bottom) / 2};
            }
            openDockQuickApps(pt);
            break;
        }
        case kImmersiveControlMenu: {
            POINT pt{};
            if (!GetCursorPos(&pt)) {
                RECT rc{};
                GetWindowRect(hwnd, &rc);
                pt = POINT{(rc.left + rc.right) / 2, (rc.top + rc.bottom) / 2};
            }
            // 菜单按钮是完整托盘菜单的固定入口，不受“右键显示
            // 任务栏歌词菜单”开关影响，也不复用任务栏歌词的精简菜单。
            openImmersiveMenu(pt);
            break;
        }
        default:
            break;
        }
    }

    void drawButton(int idx, const D2D1_POINT_2F& c, float r) {
        auto* rt = drawTarget();
        if (!rt)
            return;
        bool enabled = idx == 0 ? media.canPrev : idx == 1 ? media.canPlayPause : media.canNext;
        drawInlineControlBackground(idx, c, r, enabled);
        ID2D1SolidColorBrush* brush = enabled ? brushBtn_.Get() : brushBtnDisabled_.Get();

        media_control::draw(rt, controlGeometry, idx, media.playing, c, r, brush);
    }

    int hitInlineControl(float x, float y) const {
        if (isExpandedView())
            return -1;
        if (!isVerticalTaskbar() && pinnedInlineControlsLayout().active())
            return hitPinnedInlineControl(x, y);
        if (!controlsOnHover_ || !mouseOver_ || hoverControlStyle_ != HoverControlStyle::Inline)
            return -1;
        float logicalX = 0.0f;
        float logicalY = 0.0f;
        clientPointToLogicalDip(x, y, logicalX, logicalY);
        const bool enabled[kInlineControlCount] = {
            media.canPrev, media.canPlayPause, media.canNext, media.canRepeat, true,
            media.canSwitchSource, inlineProgressAvailable()};
        int hit = -1;
        if (isVerticalTaskbar()) {
            const VerticalLayout layout = verticalLayout();
            if (!layout.showControls)
                return -1;
            constexpr int slots[] = {0, 1, 2, kImmersiveControlVolume, kInlineSourceControl,
                                     kInlineProgressControl};
            float nearest = layout.controlRadius + 4.0f;
            for (size_t i = 0; i < std::size(slots); ++i) {
                if (slots[i] == kInlineProgressControl && !layout.showProgressControl)
                    continue;
                const auto& center = layout.controlCenters[i];
                const float distance = std::hypot(logicalX - center.x, logicalY - center.y);
                if (enabled[slots[i]] && distance <= nearest) {
                    nearest = distance;
                    hit = slots[i];
                }
            }
            return hit;
        }
        float centers[kInlineControlCount]{};
        float cy = 0.0f;
        float r = 0.0f;
        if (!inlineControlsLayout(centers, cy, r))
            return -1;
        float nearest = r + 4.0f;
        for (int i = 0; i < kInlineControlCount; ++i) {
            const float distance = std::hypot(logicalX - centers[i], logicalY - cy);
            if (enabled[i] && distance <= nearest) {
                nearest = distance;
                hit = i;
            }
        }
        return hit;
    }

    bool hitSourceWheelArea(float x, float y) const {
        if (!media.canSwitchSource || !onControl || isExpandedView() || drag_.pressed() ||
            scene_ == DisplayScene::Idle || scene_ == DisplayScene::NoPlayback)
            return false;
        const bool cardStyle = hoverControlStyle_ == HoverControlStyle::Popup &&
                               mediaPopupEnabledForScene();
        const auto pinned = pinnedInlineControlsLayout();
        if (!cardStyle && !pinned.active())
            return false;

        float logicalX = 0.0f;
        float logicalY = 0.0f;
        clientPointToLogicalDip(x, y, logicalX, logicalY);
        if (isVerticalTaskbar()) {
            const auto layout = verticalLayout();
            return logicalX >= 0.0f && logicalX < layout.w &&
                   logicalY >= layout.lyricY && logicalY < layout.lyricBottom;
        }

        int width = 0;
        int height = 0;
        logicalClientPixelSize(width, height);
        if (logicalY < 0.0f || logicalY >= dip(height))
            return false;
        const auto area = lyricAreaForScene(scene_, width, height);
        if (logicalX >= area.x && logicalX < area.x + area.w)
            return true;

        const float hostWidth = dip(width);
        if (spectrumVisible_ && backgroundWaveEnabled()) {
            const bool overPinnedControls = pinned.active() &&
                logicalX >= pinned.boundsLeft && logicalX < pinned.boundsRight;
            return !overPinnedControls && logicalX >= infoStartX() &&
                   logicalX < backgroundWaveRight(hostWidth);
        }
        const float spectrumWidth = spectrumContentWForScene(scene_, hostWidth);
        const float spectrumRight = hostWidth - embeddedResourceRightExtra() - kTextPadding;
        return spectrumWidth > 0.0f && logicalX >= spectrumRight - spectrumWidth &&
               logicalX < spectrumRight;
    }

    struct LyricTransitionSample {
        float progress = 1.0f;
        float movement = 1.0f;
        float fadeOut = 1.0f;
        float fadeIn = 1.0f;
    };

    static float smoothStep(float t) {
        t = std::clamp(t, 0.0f, 1.0f);
        return t * t * (3.0f - 2.0f * t);
    }

    void resetRootVisual() {
        renderer.resetRoot();
        songRootAnimationInstalled_ = false;
    }

    void cancelSongTransition() {
        setSongTransitionPending(false);
        if (!songRootAnimationInstalled_ && !songContentTransition_)
            return;
        if (songRootAnimationInstalled_)
            resetRootVisual();
        if (immersiveSongContentLayerActive())
            songContentTransition_->finishRequested = true;
        else
            songContentTransition_.reset();
        requestFrameAndFlush();
    }

    void beginImmersiveSongContentTransition() {
        if (!usesSongContentTransition() || isMinimalMode() || !isSongTransitionPending() ||
            songContentTransition_)
            return;
        songContentTransition_ = SongContentTransition{monotonicNowMs()};
    }

    void clearImmersiveSongContentTransition() {
        if (songContentTransition_ && songContentTransition_->compositorLayer)
            renderer.clearTransitionLayers();
        songContentTransition_.reset();
    }

    void finishImmersiveSongContentTransitionIfNeeded() {
        if (!immersiveSongContentLayerActive() || songContentTransition_->finishRequested)
            return;
        const ULONGLONG now = monotonicNowMs();
        if (now < songContentTransition_->startMs ||
            now - songContentTransition_->startMs <
                static_cast<ULONGLONG>(std::ceil(kSongTransitionMs)))
            return;
        renderer.clearTransitionLayers();
        songContentTransition_.reset();
        // 下一帧恢复到交换链绘制最新歌曲内容，并与合成层的结束点衔接。
        requestFrame();
    }

    // D2D 回退路径：沉浸模式的遮罩、控件和频谱保持静止，只把封面、歌曲信息
    // 和歌词作为一组内容在 D2D 坐标系内滑入。
    bool applyImmersiveSongContentTransform(ID2D1DeviceContext* rt) {
        if (!rt || !usesSongContentTransition() || !songContentTransition_ ||
            songContentTransition_->compositorLayer)
            return false;

        ULONGLONG now = monotonicNowMs();
        if (now < songContentTransition_->startMs)
            now = songContentTransition_->startMs;
        const float progress = std::clamp(
            static_cast<float>(now - songContentTransition_->startMs) / kSongTransitionMs, 0.0f,
            1.0f);
        if (progress >= 1.0f) {
            songContentTransition_.reset();
            return false;
        }

        const float offset = kSongTransitionTravelDip * (1.0f - smoothStep(progress));
        rt->SetTransform(D2D1::Matrix3x2F::Translation(offset, 0.0f));
        return true;
    }

    static float rangedSmoothStep(float t, float start, float end) {
        if (end <= start)
            return t >= end ? 1.0f : 0.0f;
        return smoothStep((t - start) / (end - start));
    }

    float lyricTransitionDurationMs() const {
        return transition_.kind() == LyricTransitionKind::Scene ? kSceneTransitionMs
                                                                : kLyricTransitionMs;
    }

    LyricTransitionSample lyricTransitionSample() const {
        LyricTransitionSample sample;
        if (!isLyricTransitionActive() || transition_.startMs() == 0)
            return sample;

        // applyPresentationFrame() 可以在定时器之外直接触发 render()。此时
        // frameNowMs_ 仍可能早于刚设置的起点，不能用 ULONGLONG 直接相减。
        ULONGLONG now = frameNowMs_;
        if (now < transition_.startMs())
            now = monotonicNowMs();
        if (now < transition_.startMs())
            now = transition_.startMs();
        sample.progress = std::clamp(static_cast<float>(now - transition_.startMs()) /
                                         lyricTransitionDurationMs(),
                                     0.0f, 1.0f);
        sample.movement = smoothStep(sample.progress);
        sample.fadeOut = rangedSmoothStep(sample.progress, 0.08f, 0.90f);
        sample.fadeIn = rangedSmoothStep(sample.progress, 0.14f, 1.0f);
        return sample;
    }

    // 绘制可滚动文本：容得下则按 alignment 对齐，容不下则向左无缝滚动。
    // outline/glow 非空时先画 8 方向光晕层和深色描边层，再画主文字。
    // karaokeBrush 非空时启用逐字高亮：整行先用 brush（未播放色）画一遍，
    // 再按像素裁剪出 [文本起点, karaokeX] 区域用 karaokeBrush（已播放色）画第二遍，
    // 实现字内平滑填充（裁剪基于像素，不受滚动偏移影响）
    // 将 glow+outline+本体三层合成到离屏位图（独立的兼容渲染目标，不嵌套主 BeginDraw）。
    // 成功返回可绘制位图（调用方负责 Release），失败返回 nullptr 走直接绘制兜底。
    // 必须在画刷透明度被修改之前调用，缓存内容始终是自然透明度。
    ID2D1Bitmap* textFxBitmap(IDWriteTextLayout* layout, float textW, float textH,
                              ID2D1Brush* brush, ID2D1Brush* outline, ID2D1Brush* glow) {
        return lyricTextRenderer_.textFxBitmap(lyricTextContext(nullptr), layout, textW, textH,
                                               brush, outline, glow);
    }

    // 滚动文本左缘渐隐画笔：固定两停止点（透明 → 不透明），创建一次终身复用，
    // 几何完全由每帧移动渐变轴实现

    ID2D1LinearGradientBrush* ensureSongInfoDividerBrush() {
        if (songInfoDividerBrush_.Get())
            return songInfoDividerBrush_.Get();
        auto* rt = drawTarget();
        if (!rt)
            return nullptr;

        const D2D1_COLOR_F base = brushDim_.Get()
                                      ? brushDim_->GetColor()
                                      : (lightTheme_ ? D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.75f)
                                                     : D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.65f));
        const float peakAlpha = std::clamp(base.a * 0.72f, 0.0f, 1.0f);
        const D2D1_GRADIENT_STOP stops[] = {
            {0.0f, D2D1::ColorF(base.r, base.g, base.b, 0.0f)},
            {0.18f, D2D1::ColorF(base.r, base.g, base.b, peakAlpha * 0.42f)},
            {0.50f, D2D1::ColorF(base.r, base.g, base.b, peakAlpha)},
            {0.82f, D2D1::ColorF(base.r, base.g, base.b, peakAlpha * 0.42f)},
            {1.0f, D2D1::ColorF(base.r, base.g, base.b, 0.0f)},
        };
        ID2D1GradientStopCollection* stopCollection = nullptr;
        if (FAILED(rt->CreateGradientStopCollection(stops, _countof(stops), &stopCollection)) ||
            !stopCollection)
            return nullptr;

        const HRESULT hr =
            rt->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(
                                              D2D1::Point2F(0.0f, 0.0f), D2D1::Point2F(0.0f, 1.0f)),
                                          D2D1::BrushProperties(), stopCollection,
                                          songInfoDividerBrush_.ReleaseAndGetAddressOf());
        stopCollection->Release();
        return FAILED(hr) ? nullptr : songInfoDividerBrush_.Get();
    }

    void drawSongInfoDivider(float dividerStartX, float h, float gapWidth = kSongInfoLyricGap,
                             bool vertical = false) {
        auto* rt = drawTarget();
        if (!rt || scene_ == DisplayScene::Idle || h <= 2.0f || gapWidth <= kSongInfoDividerWidth)
            return;

        const float top = std::min(kSongInfoDividerInset, h * 0.25f);
        const float bottom = std::max(top + kSongInfoDividerWidth, h - top);
        if (bottom <= top)
            return;
        auto* brush = ensureSongInfoDividerBrush();
        if (!brush)
            return;

        brush->SetStartPoint(vertical ? D2D1::Point2F(top, 0.0f) : D2D1::Point2F(0.0f, top));
        brush->SetEndPoint(vertical ? D2D1::Point2F(bottom, 0.0f) : D2D1::Point2F(0.0f, bottom));
        const float x = dividerStartX + (gapWidth - kSongInfoDividerWidth) * 0.5f;
        const auto rect = vertical ? D2D1::RectF(top, x, bottom, x + kSongInfoDividerWidth)
                                   : D2D1::RectF(x, top, x + kSongInfoDividerWidth, bottom);
        rt->FillRoundedRectangle(
            D2D1::RoundedRect(rect, kSongInfoDividerWidth * 0.5f, kSongInfoDividerWidth * 0.5f),
            brush);
    }

    // 区域内左缘渐隐：渐隐带宽度随滚出量从 0 长到 fadeW，且左缘处透明度恒为 0。
    // 若让整个渐隐带按滚出量淡入，则滚出不足 fadeW 时左缘仍有残余不透明度，
    // 半截字符会在边界硬裁剪处留下一条竖线

    // 右缘渐隐带画笔：渐隐带完全位于可视区右缘之外（rightEdge 起 extend 宽），
    // 可读区域内文字始终全不透明，只有越过边界的部分向外渐隐


    // 竖排滚动文本的两侧渐隐几何。竖排跑马灯向下滚动（offset > 0）：内容从底部
    // 滚出、顶部滚入；逐字跟随向上回收（offset < 0）：从顶部滚出、底部滚入。
    // 与横向同一套语义：滚出侧在区域内渐隐（带宽随滚出量从 0 建立；每日一言这类
    // 内容确定的无限循环跑马灯恒定保持，避免绕回时渐隐消失再出现），滚入侧借位
    // 到可视区外渐隐，可读区域内文字始终全不透明


    // 竖排滚出侧渐隐画笔：复用横向左缘画笔（停止点 0=透明 1=不透明），
    // 每帧改设竖向渐变轴；边界处透明度恒为 0，半截字符不会在边界留下亮线

    // 竖排滚入侧借位渐隐画笔：复用横向右缘画笔（停止点 0=不透明 1=透明），
    // 渐隐带完全位于可视区之外（edgeY 起向外 extend 宽）

    bool lyricEffectEnabled(LyricEffect effect) const {
        lyricEffectsRenderer_.setContext(lyricEffectsContext());
        return lyricEffectsRenderer_.lyricEffectEnabled(effect);
    }

    bool lyricStarEnabled(int lineIndex) const {
        lyricEffectsRenderer_.setContext(lyricEffectsContext());
        return lyricEffectsRenderer_.lyricStarEnabled(lineIndex);
    }

    float lyricStarRadius() const { return LyricEffectsRenderer::starRadius(fontSize_); }

    float lyricStarTextShift(IDWriteTextLayout* layout) const {
        const bool outgoing = layout == typesetter_.resources().outgoingLyricLayout_.Get() ||
                              layout == typesetter_.resources().outgoingSecondaryLayout_.Get();
        const bool current = layout == typesetter_.resources().lyricLayout_.Get() ||
                             layout == typesetter_.resources().secondaryLayout_.Get();
        const int line = outgoing  ? outgoingLyricEffectLine_
                         : current ? lyricEffectLayoutLine_
                                   : -1;
        if (!lyricStarEnabled(line))
            return 0.0f;
        const float blockH =
            outgoing
                ? outgoingLyricHeight_ + (typesetter_.resources().outgoingSecondaryLayout_.Get()
                                              ? 1.0f + outgoingSecondaryHeight_
                                              : 0.0f)
                : lyricHeight_ + (typesetter_.resources().secondaryLayout_.Get()
                                      ? 1.0f + secondaryHeight_
                                      : 0.0f);
        const float h = lastPxH_ / scale();
        return std::min(8.0f, std::max(0.0f, (h - blockH) * 0.5f - 2.0f));
    }

    void syncLyricEffectClock(int64_t position, bool playing, bool reset = false) {
        lyricEffectsRenderer_.setContext(lyricEffectsContext(drawTarget()));
        lyricEffectsRenderer_.syncLyricEffectClock(position, playing, reset);
        if (lyricEffectsRenderer_.consumeRedraw())
            requestFrame();
    }

    void setLyricEffects(uint32_t effects) {
        effects &= kLyricEffectsMask;
        if (lyricEffects_ == effects)
            return;
        lyricEffects_ = effects;
        clearLyricEffectGeometry();
        syncLyricEffectClock(positionMs_, media.playing, true);
        if (isLyricDCompActive()) {
            renderer.clearTransitionLayers();
            clearLyricDCompState();
        }
        clearImmersiveSongContentTransition();
        requestFrameAndFlush();
    }

    bool lyricEffectsAnimating() const {
        lyricEffectsRenderer_.setContext(lyricEffectsContext());
        return lyricEffectsRenderer_.lyricEffectsAnimating();
    }

    void clearLyricEffectGeometry() {
        lyricEffectsRenderer_.setContext(lyricEffectsContext(drawTarget()));
        lyricEffectsRenderer_.clearLyricEffectGeometry();
        if (lyricEffectsRenderer_.consumeRedraw())
            requestFrame();
    }

    float lyricEffectProgress(int lineIndex) {
        lyricEffectsRenderer_.setContext(lyricEffectsContext(drawTarget()));
        return lyricEffectsRenderer_.lyricEffectProgress(lineIndex);
    }

    // 所有坐标均在实际文字布局的局部空间，复用调用方的缩放、旋转和渐隐裁剪。
    // rangeStart/rangeSize 把中英混排竖栏的每一段映射回同一句时间轴。
    void drawLyricOverlay(IDWriteTextLayout* layout, int lineIndex, float x, float y, float width,
                          float height, bool vertical, float opacity, float progress,
                          bool foreground, float rangeStart = 0.0f, float rangeSize = 1.0f) {
        lyricEffectsRenderer_.setContext(lyricEffectsContext(drawTarget()));
        lyricEffectsRenderer_.drawLyricOverlay(layout, lineIndex, x, y, width, height, vertical,
                                               opacity, progress, foreground, rangeStart,
                                               rangeSize);
        if (lyricEffectsRenderer_.consumeRedraw())
            requestFrame();
    }

    LyricTextDrawContext lyricTextContext(IDWriteTextLayout* layout) {
        return {drawTarget(),
                &typesetter_.resources(),
                lyricEffectsRenderer_,
                dpi_,
                fontSize_,
                lastPxW_ / scale(),
                layout ? lyricStarTextShift(layout) : 0.0f,
                lyricEffectLayoutLine_,
                outgoingLyricEffectLine_,
                lyricEffects_,
                scene_ == DisplayScene::Lyrics && appearance_ && appearance_->lyricPlayedGradient,
                lyricColor_,
                brushLyric_.Get()};
    }

    void drawScrollingText(IDWriteTextLayout* layout, float textW, float textH, float areaW,
                           float x, float y, float offset, ID2D1Brush* brush,
                           ID2D1Brush* outline = nullptr, ID2D1Brush* glow = nullptr,
                           ID2D1Brush* karaokeBrush = nullptr, float karaokeX = 0.0f,
                           float opacity = 1.0f, LyricAlignment alignment = LyricAlignment::Center,
                           bool singleCopy = false, float rightExtend = 0.0f,
                           bool constantEdgeFade = false, float leftFadeDip = 0.0f,
                           float centerAnchor = -1.0f) {
        const auto context = lyricTextContext(layout);
        lyricEffectsRenderer_.setContext(lyricEffectsContext(drawTarget()));
        lyricTextRenderer_.drawScrollingText(context, layout, textW, textH, areaW, x, y, offset,
                                             brush, outline, glow, karaokeBrush, karaokeX, opacity,
                                             alignment, singleCopy, rightExtend, constantEdgeFade,
                                             leftFadeDip, centerAnchor);
        if (lyricEffectsRenderer_.consumeRedraw())
            requestFrame();
    }

    void drawVerticalScrollingText(
        IDWriteTextLayout* layout, float textW, float textH, float areaH, float x, float y,
        float offset, ID2D1Brush* brush, ID2D1Brush* outline = nullptr, ID2D1Brush* glow = nullptr,
        ID2D1Brush* karaokeBrush = nullptr, float karaokeX = 0.0f, float opacity = 1.0f,
        LyricAlignment alignment = LyricAlignment::Center, bool singleCopy = false,
        bool rotated = false, float topExtend = 0.0f, float bottomExtend = 0.0f,
        bool constantEdgeFade = false, bool skipSlotClip = false, float slotY = -1.0f,
        int effectLine = -1, float effectRangeStart = 0.0f, float effectRangeSize = 1.0f,
        float effectProgress = -1.0f) {
        const auto context = lyricTextContext(layout);
        lyricEffectsRenderer_.setContext(lyricEffectsContext(drawTarget()));
        lyricTextRenderer_.drawVerticalScrollingText(
            context, layout, textW, textH, areaH, x, y, offset, brush, outline, glow, karaokeBrush,
            karaokeX, opacity, alignment, singleCopy, rotated, topExtend, bottomExtend,
            constantEdgeFade, skipSlotClip, slotY, effectLine, effectRangeStart, effectRangeSize,
            effectProgress);
        if (lyricEffectsRenderer_.consumeRedraw())
            requestFrame();
    }

    void drawVerticalLyricParts(const std::vector<VerticalLyricPart>& parts, float blockH,
                                float areaH, float railW, float y, float offset, ID2D1Brush* brush,
                                ID2D1Brush* outline = nullptr, ID2D1Brush* glow = nullptr,
                                ID2D1Brush* karaokeBrush = nullptr, float karaokeProgress = 0.0f,
                                float opacity = 1.0f, bool singleCopy = false,
                                float topExtend = 0.0f, float bottomExtend = 0.0f,
                                bool constantEdgeFade = false, float slotY = -1.0f,
                                float railX = 0.0f, bool scrollUp = false) {
        const auto context = lyricTextContext(nullptr);
        lyricEffectsRenderer_.setContext(lyricEffectsContext(drawTarget()));
        lyricTextRenderer_.drawVerticalLyricParts(
            context, parts, blockH, areaH, railW, y, offset, brush, outline, glow, karaokeBrush,
            karaokeProgress, opacity, singleCopy, topExtend, bottomExtend, constantEdgeFade, slotY,
            railX, scrollUp);
        if (lyricEffectsRenderer_.consumeRedraw())
            requestFrame();
    }

    void drawVerticalLyrics(const VerticalLayout& layout) {
        auto* rt = drawTarget();
        if (!rt || layout.lyricBottom <= layout.lyricY)
            return;

        const float areaH = layout.lyricBottom - layout.lyricY;
        const bool idleScene = scene_ == DisplayScene::Idle;
        ID2D1Brush* primaryBrush =
            idleScene ? static_cast<ID2D1Brush*>(brushText_.Get())
                      : static_cast<ID2D1Brush*>(brushLyric_.Get() ? brushLyric_.Get()
                                                                   : brushText_.Get());
        const bool outgoingIdleScene = sceneTransition_.outgoingScene() == DisplayScene::Idle;
        ID2D1Brush* outgoingBrush =
            outgoingIdleScene ? static_cast<ID2D1Brush*>(brushText_.Get()) : primaryBrush;
        ID2D1Brush* effectOutline = !idleScene && lyricOutline_
                                        ? static_cast<ID2D1Brush*>(brushLyricOutline_.Get())
                                        : nullptr;
        ID2D1Brush* effectGlow =
            !idleScene && lyricGlow_ ? static_cast<ID2D1Brush*>(brushLyricGlow_.Get()) : nullptr;
        ID2D1Brush* outgoingOutline = !outgoingIdleScene && lyricOutline_
                                          ? static_cast<ID2D1Brush*>(brushLyricOutline_.Get())
                                          : nullptr;
        ID2D1Brush* outgoingGlow = !outgoingIdleScene && lyricGlow_
                                       ? static_cast<ID2D1Brush*>(brushLyricGlow_.Get())
                                       : nullptr;
        const LyricAlignment alignment = LyricAlignment::Center;

        auto drawLine = [&](IDWriteTextLayout* textLayout,
                            const std::vector<VerticalLyricPart>* parts, float textW, float textH,
                            const VerticalLyricColumns& columns,
                            float y, float offset, ID2D1Brush* brush, float opacity,
                            bool singleCopy, bool rotated, ID2D1Brush* outline, ID2D1Brush* glow,
                            ID2D1Brush* karaokeBrush, float karaokeProgress, bool constantFade) {
            // 滚入侧借位宽度：与竖排各区间距一致（封面与歌词间、歌词与底部/控件间
            // 均为 6 dip 或窗口边缘内边距），渐隐带不外溢到封面/控件上。
            // slotY 始终传歌词槽顶：转场期间 y 随动画移动，但裁剪/渐隐锚定歌词槽
            const float railW = columns.hasSecondary() ? columns.primaryWidth : layout.w;
            if (parts && !parts->empty()) {
                drawVerticalLyricParts(*parts, textH, areaH, railW, y, offset, brush, outline,
                                       glow, karaokeBrush, karaokeProgress, opacity, singleCopy,
                                       kVerticalLyricGap, kVerticalLyricGap, constantFade,
                                       layout.lyricY, columns.primaryX, columns.primaryScrollUp);
                return;
            }
            // 旋转英文的实际横向宽度通常小于任务栏槽宽，单独居中，避免整句贴在
            // 侧边栏外沿；中文逐字布局本身已经使用完整槽宽。
            const float textX = columns.primaryX + (rotated ? (railW - textW) * 0.5f : 0.0f);
            drawVerticalScrollingText(textLayout, textW, textH, areaH, textX, y, offset, brush,
                                      outline, glow, karaokeBrush, karaokeProgress, opacity,
                                      alignment, singleCopy, rotated, kVerticalLyricGap,
                                      kVerticalLyricGap, constantFade, false, layout.lyricY);
        };

        const auto drawSecondary = [&](const std::vector<VerticalLyricPart>& parts,
                                       const VerticalLyricColumns& columns, float y, float offset,
                                       float opacity, bool singleCopy) {
            if (parts.empty())
                return;
            drawVerticalLyricParts(parts, columns.secondaryHeight, areaH, columns.secondaryWidth,
                                   y, offset, brushDim_.Get(), nullptr, nullptr, nullptr, 0.0f,
                                   opacity, singleCopy, kVerticalLyricGap, kVerticalLyricGap, false,
                                   layout.lyricY, columns.secondaryX, true);
        };

        auto verticalKaraokeProgress = [&](float karaokeX) {
            return lyricWidth_ > 0.0f ? std::clamp(karaokeX / lyricWidth_, 0.0f, 1.0f) : 0.0f;
        };

        // 不再使用整段统一裁剪：各绘制路径自带裁剪/渐隐图层，滚入侧借位内容
        // 需要画出可视槽之外才能形成渐隐带
        if (isLyricTransitionActive() &&
            (typesetter_.resources().outgoingVerticalLyricLayout_.Get() ||
             !typesetter_.resources().outgoingVerticalLyricParts_.empty())) {
            const LyricTransitionSample transition = lyricTransitionSample();
            const float travel = std::clamp(areaH * 0.24f, 18.0f, 48.0f);
            const float direction = transition_.direction() >= 0 ? 1.0f : -1.0f;
            const float oldY = layout.lyricY - direction * travel * transition.movement;
            const float newY = layout.lyricY + direction * travel * (1.0f - transition.movement);
            drawLine(typesetter_.resources().outgoingVerticalLyricLayout_.Get(),
                     &typesetter_.resources().outgoingVerticalLyricParts_,
                     outgoingVerticalLyricWidth_, outgoingVerticalLyricHeight_,
                     outgoingVerticalLyricColumns_, oldY,
                     outgoingLyricScrollOffset_, outgoingBrush, 1.0f - transition.fadeOut, true,
                     outgoingVerticalLyricRotated_, outgoingOutline, outgoingGlow, nullptr, 0.0f,
                     outgoingIdleScene);
            drawSecondary(typesetter_.resources().outgoingVerticalSecondaryParts_,
                          outgoingVerticalLyricColumns_, oldY, outgoingSecondaryScrollOffset_,
                          1.0f - transition.fadeOut, false);

            const LyricLine* incomingLine = !idleScene ? karaokeLine() : nullptr;
            const bool incomingKaraoke = incomingLine && brushLyric_.Get() && brushLyricDim_.Get();
            const float incomingKaraokeX =
                incomingKaraoke ? karaokeSmoothStep(*incomingLine) : 0.0f;
            drawLine(typesetter_.resources().verticalLyricLayout_.Get(),
                     &typesetter_.resources().verticalLyricParts_, verticalLyricWidth_,
                     verticalLyricHeight_, verticalLyricColumns_, newY, scroll_.lyric(),
                     incomingKaraoke ? static_cast<ID2D1Brush*>(brushLyricDim_.Get())
                                     : primaryBrush,
                     transition.fadeIn, true, verticalLyricRotated_, effectOutline, effectGlow,
                     incomingKaraoke ? static_cast<ID2D1Brush*>(brushLyric_.Get()) : nullptr,
                     verticalKaraokeProgress(incomingKaraokeX), idleScene);
            drawSecondary(typesetter_.resources().verticalSecondaryParts_, verticalLyricColumns_,
                          newY, scroll_.secondary(), transition.fadeIn, true);
        } else if (typesetter_.resources().verticalLyricLayout_.Get() ||
                   !typesetter_.resources().verticalLyricParts_.empty()) {
            const LyricLine* currentLine = !idleScene ? karaokeLine() : nullptr;
            const bool karaoke = currentLine && brushLyric_.Get() && brushLyricDim_.Get();
            const float karaokeX = karaoke ? karaokeSmoothStep(*currentLine) : 0.0f;
            drawLine(typesetter_.resources().verticalLyricLayout_.Get(),
                     &typesetter_.resources().verticalLyricParts_, verticalLyricWidth_,
                     verticalLyricHeight_, verticalLyricColumns_, layout.lyricY, scroll_.lyric(),
                     karaoke ? static_cast<ID2D1Brush*>(brushLyricDim_.Get()) : primaryBrush, 1.0f,
                     false, verticalLyricRotated_, effectOutline, effectGlow,
                     karaoke ? static_cast<ID2D1Brush*>(brushLyric_.Get()) : nullptr,
                     verticalKaraokeProgress(karaokeX), idleScene);
            drawSecondary(typesetter_.resources().verticalSecondaryParts_, verticalLyricColumns_,
                          layout.lyricY, scroll_.secondary(), 1.0f, false);
        }
    }

    LyricAlignment activeLyricAlignment() const {
        if (scene_ == DisplayScene::Idle)
            return idleQuoteAlignment_;
        return isExpandedView() ? LyricAlignment::Center : lyricAlignment_;
    }

    LyricAlignment lyricAlignmentForScene(DisplayScene scene) const {
        if (scene == DisplayScene::Idle)
            return idleQuoteAlignment_;
        return isExpandedView() ? LyricAlignment::Center : lyricAlignment_;
    }

    void drawLyricScrollingTextAligned(IDWriteTextLayout* layout, float textW, float textH,
                                       float areaW, float x, float y, float offset,
                                       ID2D1Brush* brush, ID2D1Brush* outline, ID2D1Brush* glow,
                                       ID2D1Brush* karaokeBrush, float karaokeX, float opacity,
                                       LyricAlignment alignment, bool singleCopy = false) {
        // 左缘渐隐宽度分场景：歌曲信息可见时用与分隔间距一致的 8 dip，且渐隐始终
        // 在歌词区域内，文字不会画进信息区；信息区隐藏时用默认宽度
        const float leftFadeDip =
            (songInfoVisible_ || pinnedInlineControlsForScene(scene_)) &&
                    scene_ != DisplayScene::Idle
                ? kTextPadding
                : 0.0f;
        const float centerAnchor = immersiveLyricCenterX();
        // 每日一言与歌曲信息同属内容确定的无限循环跑马灯，是否滚动在内容确定时
        // 已知，渐隐恒定保持，避免每轮循环绕回时左缘渐隐消失再出现
        drawScrollingText(layout, textW, textH, areaW, x, y, offset, brush, outline, glow,
                          karaokeBrush, karaokeX, opacity, alignment, singleCopy, kTextPadding,
                          scene_ == DisplayScene::Idle, leftFadeDip, centerAnchor);
    }

    void drawLyricScrollingText(IDWriteTextLayout* layout, float textW, float textH, float areaW,
                                float x, float y, float offset, ID2D1Brush* brush,
                                ID2D1Brush* outline = nullptr, ID2D1Brush* glow = nullptr,
                                ID2D1Brush* karaokeBrush = nullptr, float karaokeX = 0.0f,
                                float opacity = 1.0f, bool singleCopy = false) {
        drawLyricScrollingTextAligned(layout, textW, textH, areaW, x, y, offset, brush, outline,
                                      glow, karaokeBrush, karaokeX, opacity, activeLyricAlignment(),
                                      singleCopy);
    }

    void drawScaledScrollingText(IDWriteTextLayout* layout, float textW, float textH, float areaW,
                                 float x, float y, float offset, ID2D1Brush* brush, float opacity,
                                 float scale, ID2D1Brush* outline = nullptr,
                                 ID2D1Brush* glow = nullptr, ID2D1Brush* karaokeBrush = nullptr,
                                 float karaokeX = 0.0f, bool singleCopy = false,
                                 float visibleW = 0.0f) {
        auto* rt = drawTarget();
        if (!rt || !layout || areaW <= 0.0f)
            return;
        if (scale >= 0.999f) {
            drawLyricScrollingText(layout, textW, textH, areaW, x, y, offset, brush, outline, glow,
                                   karaokeBrush, karaokeX, opacity, singleCopy);
            return;
        }

        float anchorX = x + areaW * 0.5f;
        if (activeLyricAlignment() == LyricAlignment::Left)
            anchorX = x;
        else if (activeLyricAlignment() == LyricAlignment::Right)
            anchorX = x + areaW;
        else if (const float centerAnchor = immersiveLyricCenterX(); centerAnchor >= 0.0f)
            anchorX = centerAnchor;
        const D2D1_POINT_2F anchor = D2D1::Point2F(anchorX, y + textH * 0.5f);
        // 外层裁剪先在最终窗口坐标中固定下来，再缩放文字。否则裁剪矩形会和文字
        // 一起围绕任务栏中心缩放；沉浸 / Dock 的歌词安全区左右不对称时，右边界
        // 会短暂移入频谱、时钟和控件区域，转场结束恢复普通裁剪时形成闪烁。
        const int effectLine = layout == typesetter_.resources().lyricLayout_.Get()
                                   ? lyricEffectLayoutLine_
                               : layout == typesetter_.resources().outgoingLyricLayout_.Get()
                                   ? outgoingLyricEffectLine_
                                   : -1;
        const float starPad =
            lyricStarEnabled(effectLine) ? lyricStarRadius() * 2.0f + 10.0f : 0.0f;
        const float textShift = lyricStarTextShift(layout);
        lyricEffectsRenderer_.setContext(lyricEffectsContext(rt));
        const float wavePad = lyricEffectsRenderer_.wordWaveEnabled(effectLine) ? 2.0f : 0.0f;
        const D2D1_RECT_F safeClip =
            D2D1::RectF(x, y - starPad - wavePad, x + areaW + kTextPadding,
                         y + textH + textShift + wavePad);
        rt->PushAxisAlignedClip(safeClip, D2D1_ANTIALIAS_MODE_ALIASED);
        // visibleW 是转场期间可见的文本坐标宽度；缩放轴与歌词的实际居中锚点
        // 保持一致，避免新旧两句在入场时发生横向漂移。
        const float drawW = visibleW > 0.0f ? visibleW : areaW;
        D2D1_MATRIX_3X2_F previous{};
        rt->GetTransform(&previous);
        rt->SetTransform(D2D1::Matrix3x2F::Scale(scale, scale, anchor) * previous);
        drawLyricScrollingText(layout, textW, textH, drawW, x, y, offset, brush, outline, glow,
                               karaokeBrush, karaokeX, opacity, singleCopy);
        rt->SetTransform(previous);
        rt->PopAxisAlignedClip();
    }

    void drawDoubleLineLyrics(float lyricAreaX, float lyricAreaW, float h,
                              ID2D1Brush* primaryBrush) {
        if (!typesetter_.resources().lyricLayout_.Get())
            return;

        const float lyricBlockH = lyricHeight_ + (typesetter_.resources().nextLyricLayout_.Get()
                                                      ? kLyricPreviewGap + nextLyricHeight_
                                                      : 0.0f);
        const float coreY = h * 0.5f - lyricBlockH * 0.5f;
        ID2D1Brush* coreBrush = primaryBrush        ? primaryBrush
                                : brushLyric_.Get() ? static_cast<ID2D1Brush*>(brushLyric_.Get())
                                                    : static_cast<ID2D1Brush*>(brushText_.Get());
        ID2D1Brush* previewBrush = brushLyricDim_.Get()
                                       ? static_cast<ID2D1Brush*>(brushLyricDim_.Get())
                                       : static_cast<ID2D1Brush*>(brushDim_.Get());
        ID2D1Brush* effectOutline = scene_ != DisplayScene::Idle && lyricOutline_
                                        ? static_cast<ID2D1Brush*>(brushLyricOutline_.Get())
                                        : nullptr;
        ID2D1Brush* effectGlow = scene_ != DisplayScene::Idle && lyricGlow_
                                     ? static_cast<ID2D1Brush*>(brushLyricGlow_.Get())
                                     : nullptr;
        if (isLyricTransitionActive() && typesetter_.resources().outgoingLyricLayout_.Get()) {
            const LyricTransitionSample transition = lyricTransitionSample();
            const float movementT = transition.movement;
            float outgoingBlockH =
                outgoingLyricBlockHeight_ > 0.0f ? outgoingLyricBlockHeight_ : outgoingLyricHeight_;
            float outgoingY = h * 0.5f - outgoingBlockH * 0.5f;
            float travel = std::max(outgoingLyricHeight_, lyricHeight_) + kLyricPreviewGap;
            float direction = transition_.direction() >= 0 ? 1.0f : -1.0f;
            float oldShift = -direction * travel * movementT;
            float incomingStartY = direction > 0.0f
                                       ? outgoingY + outgoingLyricHeight_ + kLyricPreviewGap
                                       : coreY - travel;
            float incomingY = incomingStartY + (coreY - incomingStartY) * movementT;
            const LyricLine* incomingLine = karaokeLine();
            bool incomingKaraoke = incomingLine && brushLyric_.Get() && brushLyricDim_.Get();
            ID2D1Brush* incomingBrush =
                incomingKaraoke ? static_cast<ID2D1Brush*>(brushLyricDim_.Get()) : coreBrush;
            float incomingProgX = incomingKaraoke ? karaokeSmoothStep(*incomingLine) : 0.0f;
            float incomingScale = kLyricPreviewScale + (1.0f - kLyricPreviewScale) * movementT;
            // y 是主字号布局的位置；缩小时校正半个缩放差，使视觉中心跟随位移而不跳动。
            float scaledIncomingY = incomingY - lyricHeight_ * (1.0f - incomingScale) * 0.5f;

            // 超出部分的消失与第二行上移同步：转场起点取预览期实际可见的右边界
            // （预览放得下=整行可见；放不下=预览渐隐位置，均换算到核心行文本坐标），
            // 随转场进度收敛到核心行可视宽，避免动画第一帧尾部瞬间消失。
            float incomingVisibleW = 0.0f;
            if (lyricWidth_ > lyricAreaW) {
                // 当前缩放比例下恰好覆盖歌词安全区所需的文本坐标宽度。
                // 使用实时比例而不是对两个宽度线性插值，避免中间帧的可见宽度
                // 反而大于安全区，并由外层固定裁剪保证不会侵入右侧固定内容。
                incomingVisibleW = std::min(lyricWidth_, lyricAreaW / incomingScale);
            }

            // 下一行在转场前已经位于核心行下方；转场从这个位置接入核心，避免跳变。
            drawLyricScrollingText(typesetter_.resources().outgoingLyricLayout_.Get(),
                                   outgoingLyricWidth_, outgoingLyricHeight_, lyricAreaW,
                                   lyricAreaX, outgoingY + oldShift, outgoingLyricScrollOffset_,
                                   coreBrush, effectOutline, effectGlow, nullptr, 0.0f,
                                   1.0f - transition.fadeOut);
            if (!transition_.outgoingDoubleLine() &&
                typesetter_.resources().outgoingSecondaryLayout_.Get())
                drawLyricScrollingText(typesetter_.resources().outgoingSecondaryLayout_.Get(),
                                       outgoingSecondaryWidth_, outgoingSecondaryHeight_,
                                       lyricAreaW, lyricAreaX,
                                       outgoingY + outgoingLyricHeight_ + 1.0f + oldShift,
                                       outgoingSecondaryScrollOffset_, brushDim_.Get(), nullptr,
                                       nullptr, nullptr, 0.0f, 1.0f - transition.fadeOut);
            drawScaledScrollingText(
                typesetter_.resources().lyricLayout_.Get(), lyricWidth_, lyricHeight_, lyricAreaW,
                lyricAreaX, scaledIncomingY, scroll_.lyric(), incomingBrush,
                kLyricPreviewOpacity + (1.0f - kLyricPreviewOpacity) * transition.fadeIn,
                incomingScale, effectOutline, effectGlow,
                incomingKaraoke ? static_cast<ID2D1Brush*>(brushLyric_.Get()) : nullptr,
                incomingProgX, true, incomingVisibleW);
            return;
        }

        // 核心行仍保留原有逐字高亮；下一行只作为低透明度预览，不参与逐字填充。
        const LyricLine* curLine = karaokeLine();
        bool karaoke = curLine && brushLyric_.Get() && brushLyricDim_.Get();
        float progX = karaoke ? karaokeSmoothStep(*curLine) : 0.0f;
        drawLyricScrollingText(
            typesetter_.resources().lyricLayout_.Get(), lyricWidth_, lyricHeight_, lyricAreaW,
            lyricAreaX, coreY, scroll_.lyric(),
            karaoke ? static_cast<ID2D1Brush*>(brushLyricDim_.Get()) : coreBrush, effectOutline,
            effectGlow, karaoke ? static_cast<ID2D1Brush*>(brushLyric_.Get()) : nullptr, progX);

        if (typesetter_.resources().nextLyricLayout_.Get()) {
            float nextY = coreY + lyricHeight_ + kLyricPreviewGap;
            drawLyricScrollingText(typesetter_.resources().nextLyricLayout_.Get(), nextLyricWidth_,
                                   nextLyricHeight_, lyricAreaW, lyricAreaX, nextY, 0.0f,
                                   previewBrush, nullptr, nullptr, nullptr, 0.0f,
                                   kLyricPreviewOpacity);
        }
    }

    // 每日一言与歌词属于任务栏的两个展示场景。场景切换时内容层与外层尺寸
    // 使用同一段转场时序，旧内容仍按切换前的内容区绘制，避免水平布局跳变。
    void drawSceneTransition(float w, float lyricAreaX, float lyricAreaW, float h,
                             float lyricBlockH) {
        auto* rt = drawTarget();
        if (!rt || !typesetter_.resources().lyricLayout_.Get() ||
            !typesetter_.resources().outgoingLyricLayout_.Get() || lastPxW_ <= 0 || lastPxH_ <= 0)
            return;

        const int outgoingPxW =
            sceneTransition_.fromWidth() > 0 ? sceneTransition_.fromWidth() : lastLogicalPxW_;
        const int outgoingPxH =
            sceneTransition_.fromHeight() > 0 ? sceneTransition_.fromHeight() : lastLogicalPxH_;
        const LyricArea outgoingArea =
            lyricAreaForScene(sceneTransition_.outgoingScene(), outgoingPxW, outgoingPxH);
        const LyricTransitionSample transition = lyricTransitionSample();
        const float movementT = transition.movement;
        const float direction = transition_.direction() >= 0 ? 1.0f : -1.0f;
        const bool incomingDoubleLine =
            useDoubleLineLyrics() && typesetter_.resources().nextLyricLayout_.Get();
        const float incomingBlockH =
            incomingDoubleLine ? lyricHeight_ + kLyricPreviewGap + nextLyricHeight_ : lyricBlockH;
        const float outgoingPreviewH =
            transition_.outgoingDoubleLine() &&
                    typesetter_.resources().outgoingNextLyricLayout_.Get()
                ? kLyricPreviewGap + outgoingNextLyricHeight_
            : typesetter_.resources().outgoingSecondaryLayout_.Get()
                ? 1.0f + outgoingSecondaryHeight_
                : 0.0f;
        const float outgoingBlockH = outgoingLyricBlockHeight_ > 0.0f
                                         ? outgoingLyricBlockHeight_
                                         : outgoingLyricHeight_ + outgoingPreviewH;
        const float outgoingY = h * 0.5f - outgoingBlockH * 0.5f;
        const float incomingY = h * 0.5f - incomingBlockH * 0.5f;
        const float travel = std::max(incomingBlockH, outgoingBlockH);
        const float oldShift = -direction * travel * movementT;
        const float newShift = direction * travel * (1.0f - movementT);

        auto primaryBrushForScene = [this](DisplayScene scene) -> ID2D1Brush* {
            if (scene == DisplayScene::Idle)
                return brushText_.Get();
            return brushLyric_.Get() ? static_cast<ID2D1Brush*>(brushLyric_.Get())
                                     : static_cast<ID2D1Brush*>(brushText_.Get());
        };
        const bool outgoingEffects = sceneTransition_.outgoingScene() != DisplayScene::Idle;
        const bool incomingEffects = scene_ != DisplayScene::Idle;
        ID2D1Brush* outgoingBrush = primaryBrushForScene(sceneTransition_.outgoingScene());
        ID2D1Brush* incomingBrush = primaryBrushForScene(scene_);
        ID2D1Brush* outgoingOutline =
            outgoingEffects && lyricOutline_ ? brushLyricOutline_.Get() : nullptr;
        ID2D1Brush* outgoingGlow = outgoingEffects && lyricGlow_ ? brushLyricGlow_.Get() : nullptr;
        ID2D1Brush* incomingOutline =
            incomingEffects && lyricOutline_ ? brushLyricOutline_.Get() : nullptr;
        ID2D1Brush* incomingGlow = incomingEffects && lyricGlow_ ? brushLyricGlow_.Get() : nullptr;

        const LyricLine* incomingLine = scene_ == DisplayScene::Lyrics ? karaokeLine() : nullptr;
        const bool incomingKaraoke = incomingLine && brushLyric_.Get() && brushLyricDim_.Get();
        if (incomingKaraoke)
            incomingBrush = brushLyricDim_.Get();
        const float incomingProgX = incomingKaraoke ? karaokeSmoothStep(*incomingLine) : 0.0f;

        rt->PushAxisAlignedClip(D2D1::RectF(0.0f, 0.0f, w, h), D2D1_ANTIALIAS_MODE_ALIASED);
        drawLyricScrollingTextAligned(
            typesetter_.resources().outgoingLyricLayout_.Get(), outgoingLyricWidth_,
            outgoingLyricHeight_, outgoingArea.w, outgoingArea.x, outgoingY + oldShift,
            outgoingLyricScrollOffset_, outgoingBrush, outgoingOutline, outgoingGlow, nullptr, 0.0f,
            1.0f - transition.fadeOut, lyricAlignmentForScene(sceneTransition_.outgoingScene()));
        if (transition_.outgoingDoubleLine() &&
            typesetter_.resources().outgoingNextLyricLayout_.Get()) {
            ID2D1Brush* previewBrush = brushLyricDim_.Get()
                                           ? static_cast<ID2D1Brush*>(brushLyricDim_.Get())
                                           : static_cast<ID2D1Brush*>(brushDim_.Get());
            drawLyricScrollingTextAligned(
                typesetter_.resources().outgoingNextLyricLayout_.Get(), outgoingNextLyricWidth_,
                outgoingNextLyricHeight_, outgoingArea.w, outgoingArea.x,
                outgoingY + outgoingLyricHeight_ + kLyricPreviewGap + oldShift, 0.0f, previewBrush,
                nullptr, nullptr, nullptr, 0.0f, kLyricPreviewOpacity * (1.0f - transition.fadeOut),
                lyricAlignmentForScene(sceneTransition_.outgoingScene()), true);
        } else if (typesetter_.resources().outgoingSecondaryLayout_.Get()) {
            drawLyricScrollingTextAligned(
                typesetter_.resources().outgoingSecondaryLayout_.Get(), outgoingSecondaryWidth_,
                outgoingSecondaryHeight_, outgoingArea.w, outgoingArea.x,
                outgoingY + outgoingLyricHeight_ + 1.0f + oldShift, outgoingSecondaryScrollOffset_,
                brushDim_.Get(), nullptr, nullptr, nullptr, 0.0f, 1.0f - transition.fadeOut,
                lyricAlignmentForScene(sceneTransition_.outgoingScene()));
        }

        drawLyricScrollingTextAligned(
            typesetter_.resources().lyricLayout_.Get(), lyricWidth_, lyricHeight_, lyricAreaW,
            lyricAreaX, incomingY + newShift, scroll_.lyric(), incomingBrush, incomingOutline,
            incomingGlow, incomingKaraoke ? static_cast<ID2D1Brush*>(brushLyric_.Get()) : nullptr,
            incomingProgX, transition.fadeIn, lyricAlignmentForScene(scene_), true);
        if (incomingDoubleLine) {
            ID2D1Brush* previewBrush = brushLyricDim_.Get()
                                           ? static_cast<ID2D1Brush*>(brushLyricDim_.Get())
                                           : static_cast<ID2D1Brush*>(brushDim_.Get());
            drawLyricScrollingTextAligned(
                typesetter_.resources().nextLyricLayout_.Get(), nextLyricWidth_, nextLyricHeight_,
                lyricAreaW, lyricAreaX, incomingY + lyricHeight_ + kLyricPreviewGap + newShift,
                0.0f, previewBrush, nullptr, nullptr, nullptr, 0.0f,
                kLyricPreviewOpacity * transition.fadeIn, lyricAlignmentForScene(scene_), true);
        } else if (typesetter_.resources().secondaryLayout_.Get()) {
            drawLyricScrollingTextAligned(
                typesetter_.resources().secondaryLayout_.Get(), secondaryWidth_, secondaryHeight_,
                lyricAreaW, lyricAreaX, incomingY + lyricHeight_ + 1.0f + newShift,
                scroll_.secondary(), brushDim_.Get(), nullptr, nullptr, nullptr, 0.0f,
                transition.fadeIn, lyricAlignmentForScene(scene_), true);
        }
        rt->PopAxisAlignedClip();
    }

    void drawTransitionTextTo(ID2D1DeviceContext* rt, IDWriteTextLayout* layout, float textW,
                              float textH, float areaW, float x, float y, float offset,
                              ID2D1Brush* brush, ID2D1Brush* outline = nullptr,
                              ID2D1Brush* glow = nullptr, ID2D1Brush* karaokeBrush = nullptr,
                              float karaokeX = 0.0f, bool singleCopy = false) {
        if (!rt || !layout || !brush || areaW <= 0.0f)
            return;
        y += lyricStarTextShift(layout);
        D2D1_RECT_F clip{x, y, x + areaW, y + textH};
        rt->PushAxisAlignedClip(clip, D2D1_ANTIALIAS_MODE_ALIASED);
        float bases[2];
        int n = 0;
        const float centerAnchor = immersiveLyricCenterX();
        auto alignedBase = [&]() {
            float freeW = std::max(0.0f, areaW - textW);
            switch (activeLyricAlignment()) {
            case LyricAlignment::Left:
                return x;
            case LyricAlignment::Right:
                return x + freeW;
            case LyricAlignment::Center:
            default:
                if (centerAnchor >= 0.0f)
                    return std::clamp(centerAnchor - textW * 0.5f, x, x + freeW);
                return x + freeW * 0.5f;
            }
        };
        if (karaokeBrush || singleCopy) {
            bases[n++] = (textW <= areaW) ? alignedBase() : x - offset;
        } else if (textW <= areaW) {
            bases[n++] = alignedBase();
        } else {
            float loopW = textW + kTextPadding * 2.0f;
            bases[n++] = x - offset;
            bases[n++] = x - offset + loopW;
        }
        static constexpr float kDirs[8][2] = {
            {1.0f, 0.0f},  {0.7071f, 0.7071f},   {0.0f, 1.0f},  {-0.7071f, 0.7071f},
            {-1.0f, 0.0f}, {-0.7071f, -0.7071f}, {0.0f, -1.0f}, {0.7071f, -0.7071f}};
        for (int i = 0; i < n; ++i) {
            if (glow) {
                for (auto& d : kDirs)
                    rt->DrawTextLayout(D2D1::Point2F(bases[i] + d[0] * 2.4f, y + d[1] * 2.4f),
                                       layout, glow);
            }
            if (outline) {
                for (auto& d : kDirs)
                    rt->DrawTextLayout(D2D1::Point2F(bases[i] + d[0] * 1.2f, y + d[1] * 1.2f),
                                       layout, outline);
            }
        }
        for (int i = 0; i < n; ++i)
            rt->DrawTextLayout(D2D1::Point2F(bases[i], y), layout, brush);
        if (karaokeBrush && karaokeX > 0.0f) {
            rt->PushAxisAlignedClip(D2D1::RectF(bases[0], y, bases[0] + karaokeX, y + textH),
                                    D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
            rt->DrawTextLayout(D2D1::Point2F(bases[0], y), layout, karaokeBrush);
            rt->PopAxisAlignedClip();
        }
        rt->PopAxisAlignedClip();
    }

    void drawLyricBlockSnapshot(ID2D1DeviceContext* dc, bool outgoing, float lyricAreaX,
                                float lyricAreaW, float h) {
        IDWriteTextLayout* main = outgoing ? typesetter_.resources().outgoingLyricLayout_.Get()
                                           : typesetter_.resources().lyricLayout_.Get();
        IDWriteTextLayout* secondary = outgoing
                                           ? typesetter_.resources().outgoingSecondaryLayout_.Get()
                                           : typesetter_.resources().secondaryLayout_.Get();
        if (!main)
            return;
        const float mainW = outgoing ? outgoingLyricWidth_ : lyricWidth_;
        const float mainH = outgoing ? outgoingLyricHeight_ : lyricHeight_;
        const float secondaryW = outgoing ? outgoingSecondaryWidth_ : secondaryWidth_;
        const float secondaryH = outgoing ? outgoingSecondaryHeight_ : secondaryHeight_;
        const float gap = secondary ? 1.0f : 0.0f;
        const float blockH = mainH + gap + secondaryH;
        const float y = h * 0.5f - blockH * 0.5f;
        ID2D1Brush* mainBrush = scene_ == DisplayScene::Idle
                                    ? static_cast<ID2D1Brush*>(brushText_.Get())
                                    : static_cast<ID2D1Brush*>(
                                          brushLyric_.Get() ? brushLyric_.Get() : brushText_.Get());
        float mainOffset = 0.0f;
        float secondaryOffset = 0.0f;
        ID2D1Brush* karaokeBrush = nullptr;
        float karaokeX = 0.0f;
        if (!outgoing) {
            const LyricLine* incomingLine = karaokeLine();
            const bool incomingKaraoke = incomingLine && brushLyric_.Get() && brushLyricDim_.Get();
            mainBrush =
                incomingKaraoke ? static_cast<ID2D1Brush*>(brushLyricDim_.Get()) : mainBrush;
            if (incomingKaraoke) {
                karaokeBrush = brushLyric_.Get();
                karaokeX = karaokeSmoothStep(*incomingLine);
            }
            mainOffset = scroll_.lyric();
            secondaryOffset = scroll_.secondary();
        } else {
            mainOffset = outgoingLyricScrollOffset_;
            secondaryOffset = outgoingSecondaryScrollOffset_;
        }
        ID2D1Brush* effectOutline = scene_ != DisplayScene::Idle && lyricOutline_
                                        ? static_cast<ID2D1Brush*>(brushLyricOutline_.Get())
                                        : nullptr;
        ID2D1Brush* effectGlow = scene_ != DisplayScene::Idle && lyricGlow_
                                     ? static_cast<ID2D1Brush*>(brushLyricGlow_.Get())
                                     : nullptr;
        drawTransitionTextTo(dc, main, mainW, mainH, lyricAreaW, lyricAreaX, y, mainOffset,
                             mainBrush, effectOutline, effectGlow, karaokeBrush, karaokeX,
                             !outgoing);
        if (secondary)
            drawTransitionTextTo(dc, secondary, secondaryW, secondaryH, lyricAreaW, lyricAreaX,
                                 y + mainH + gap, secondaryOffset, brushDim_.Get(), nullptr,
                                 nullptr, nullptr, 0.0f, !outgoing);
    }

    bool prepareLyricTransitionDComp(float lyricAreaX, float lyricAreaW, float h,
                                     float lyricBlockH) {
        if (isVerticalTaskbar() || useDoubleLineLyrics() || transition_.outgoingDoubleLine() ||
            !isLyricTransitionActive() || !typesetter_.resources().outgoingLyricLayout_.Get() ||
            !typesetter_.resources().lyricLayout_.Get() || lastPxW_ <= 0 || lastPxH_ <= 0)
            return false;
        if (!renderer.ensureTransitionLayers(lastPxW_, lastPxH_, lastPxW_, lastPxH_))
            return false;

        if (ID2D1DeviceContext* dc = renderer.beginTransitionLayerDraw(0)) {
            drawLyricBlockSnapshot(dc, true, lyricAreaX, lyricAreaW, h);
            if (!renderer.endTransitionLayerDraw(0, dc))
                return false;
        } else {
            return false;
        }
        if (ID2D1DeviceContext* dc = renderer.beginTransitionLayerDraw(1)) {
            drawLyricBlockSnapshot(dc, false, lyricAreaX, lyricAreaW, h);
            if (!renderer.endTransitionLayerDraw(1, dc))
                return false;
        } else {
            return false;
        }

        const float outgoingGap =
            typesetter_.resources().outgoingSecondaryLayout_.Get() ? 1.0f : 0.0f;
        const float outgoingBlockH = outgoingLyricHeight_ + outgoingGap + outgoingSecondaryHeight_;
        const float travel = std::max(lyricBlockH, outgoingBlockH) * scale();
        const float direction = transition_.direction() >= 0 ? 1.0f : -1.0f;
        const float durationSec = kLyricTransitionMs / 1000.0f;
        if (!renderer.animateTransitionLayer(0, 0.0f, -direction * travel, 1.0f, 0.0f, durationSec))
            return false;
        if (!renderer.animateTransitionLayer(1, direction * travel, 0.0f, 0.0f, 1.0f, durationSec))
            return false;
        // 不在此单独 Commit：图层上线随 render() 末尾 present() 的 Commit 与底层新帧
        // 同批生效，避免「图层已上屏、底层旧歌词未撤」的双画帧。动画以提交时刻为起点，
        // 延迟到 present 提交仅差一次绘制耗时，不影响时长。
        renderState_.setDCompTransitionPhase(RenderState::DCompTransitionPhase::Active);
        return true;
    }

    void syncLyricTransitionDComp(bool showControls, float lyricAreaX, float lyricAreaW, float h,
                                  float lyricBlockH) {
        // 图层增删不单独 Commit：改动挂起到 render() 末尾 present() 的 Commit，与承载
        // 歌词的底层新帧同批上屏，避免「图层已撤、底层新帧未上屏」的空窗闪烁。
        // 沉浸切歌期间整组歌曲内容占用同一套合成层；歌词行转场不能再抢占
        // transitionLayers_，否则会把正在滑入的歌曲快照替换掉。
        if (songContentTransition_ && songContentTransition_->compositorLayer)
            return;
        if ((isVerticalTaskbar() && isLyricDCompActive()) || isLyricDCompEndRequested() ||
            (showControls && isLyricDCompActive()) || (isLyricDCompActive() && karaokeLine())) {
            renderer.clearTransitionLayers();
            clearLyricDCompState();
        }
        if (showControls)
            return;
        // 逐字高亮需要每帧跟随真实播放位置；DComp 快照是静态位图，不能在入场期间
        // 更新填充边界，因此逐字歌词转场保留 D2D 路径，普通歌词仍使用合成器动画。
        if (transition_.kind() != LyricTransitionKind::Scene && !isLyricDCompActive() &&
            isLyricTransitionActive() && !useDoubleLineLyrics() &&
            !transition_.outgoingDoubleLine() && !karaokeLine() &&
            typesetter_.resources().outgoingLyricLayout_.Get() &&
            typesetter_.resources().lyricLayout_.Get()) {
            if (!prepareLyricTransitionDComp(lyricAreaX, lyricAreaW, h, lyricBlockH)) {
                renderer.clearTransitionLayers();
                clearLyricDCompState();
            }
        }
    }

    void drawHorizontalSongContent(ID2D1DeviceContext* contentRt, float w, float h, float leftW,
                                   float lyricAreaX, float lyricAreaW, float lyricBlockH,
                                   ID2D1Brush* primaryBrush, bool idleScene,
                                   bool lyricEffectsEnabled, bool showControls, bool showSpectrum,
                                   const taskbar_detail::PinnedInlineControlsLayout& pinnedControls) {
        if (!contentRt)
            return;

        ID2D1DeviceContext* previousTarget = drawTargetOverride_;
        drawTargetOverride_ = contentRt;

        // 左侧封面
        const float s = coverSize();
        const float coverX = embeddedResourceLeftOffset() + kCoverPadding;
        const float coverY = (h - s) * 0.5f;
        if (albumCoverVisible_ && !idleScene) {
            if (albumCoverEffect_ == AlbumCoverEffect::Vinyl) {
                drawVinylCover(coverX, coverY, s);
            } else {
                const D2D1_RECT_F coverRect = D2D1::RectF(coverX, coverY, coverX + s, coverY + s);
                if (cover_.bitmap() && coverClip_.Get() && coverLayer_.Get()) {
                    contentRt->PushLayer(
                        D2D1::LayerParameters1(D2D1::InfiniteRect(), coverClip_.Get(),
                                               D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                                               D2D1::Matrix3x2F::Translation(coverX, coverY)),
                        coverLayer_.Get());
                    contentRt->DrawBitmap(cover_.bitmap(), coverRect, 1.0f,
                                          D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
                    contentRt->PopLayer();
                } else {
                    drawFallbackCover(contentRt, coverRect, 4.0f);
                }
            }
            // 平台图标必须在封面之后绘制，保证它位于专辑封面的最顶层。
            drawPlatformIcon(coverX, coverY, s);
        }

        if (songInfoVisible_ && !idleScene) {
            // 左侧歌曲信息（封面显示时位于封面右侧，整体垂直居中，超长自动滚动）
            // 左右边缘都保留渐隐，右缘在分隔线前收束，避免标题/歌手撞到歌词区
            const float infoX = infoStartX();
            const float infoW = std::max(1.0f, leftW - infoX - kTextPadding);
            const float infoGap = taskbar_detail::kSongInfoLineGap;
            const float totalInfoH = titleHeight_ + infoGap + artistHeight_;
            const float infoY = (h - totalInfoH) * 0.5f;
            drawScrollingText(typesetter_.resources().titleLayout_.Get(), titleWidth_, titleHeight_,
                              infoW, infoX, infoY, scroll_.title(), brushText_.Get(), nullptr,
                              nullptr, nullptr, 0.0f, 1.0f, LyricAlignment::Center, false,
                              kTextPadding, true, kTextPadding);
            drawSongInfoSubtitle(infoX, infoW, infoY + titleHeight_ + infoGap);
        }
        if (!idleScene) {
            drawPinnedInlineControls(pinnedControls);
            if (!isExpandedView() && (songInfoVisible_ || pinnedControls.active())) {
                const float dividerStartX =
                    pinnedControls.active() ? pinnedControls.boundsRight : leftW;
                const float dividerGap = songInfoVisible_ ? kSongInfoLyricGap : kTextPadding;
                drawSongInfoDivider(dividerStartX, h, dividerGap);
            }
        }

        if (showControls) {
            drawInlineHoverControls();
        } else {
            if (showSpectrum && !isExpandedView()) {
                const float spectrumW = spectrumContentWForScene(scene_, w);
                drawSpectrum(w - embeddedResourceRightExtra() - spectrumW - kTextPadding, h,
                             spectrumW);
            }
            lyricEffectsRenderer_.setContext(lyricEffectsContext(drawTarget()));
            lyricEffectsRenderer_.drawEasterEggs(
                D2D1::RectF(lyricAreaX, 0.0f, lyricAreaX + lyricAreaW, h));
            if (transition_.kind() == LyricTransitionKind::Scene && isLyricTransitionActive() &&
                typesetter_.resources().outgoingLyricLayout_.Get()) {
                drawSceneTransition(w, lyricAreaX, lyricAreaW, h, lyricBlockH);
            } else if (useDoubleLineLyrics()) {
                drawDoubleLineLyrics(lyricAreaX, lyricAreaW, h, primaryBrush);
            } else if (!isLyricDCompActive()) {
                if (isLyricTransitionActive() &&
                    typesetter_.resources().outgoingLyricLayout_.Get()) {
                    // 位移使用平滑的 ease-in-out，避免一开始就冲得太快。
                    const LyricTransitionSample transition = lyricTransitionSample();
                    const float movementT = transition.movement;
                    const LyricLine* incomingLine = karaokeLine();
                    const bool incomingKaraoke =
                        incomingLine && brushLyric_.Get() && brushLyricDim_.Get();
                    ID2D1Brush* incomingBrush = incomingKaraoke
                                                    ? static_cast<ID2D1Brush*>(brushLyricDim_.Get())
                                                    : primaryBrush;
                    const float incomingProgX =
                        incomingKaraoke ? karaokeSmoothStep(*incomingLine) : 0.0f;
                    const float outgoingPreviewH =
                        transition_.outgoingDoubleLine() &&
                                typesetter_.resources().outgoingNextLyricLayout_.Get()
                            ? kLyricPreviewGap + outgoingNextLyricHeight_
                        : typesetter_.resources().outgoingSecondaryLayout_.Get()
                            ? 1.0f + outgoingSecondaryHeight_
                            : 0.0f;
                    const float outgoingGap =
                        transition_.outgoingDoubleLine() &&
                                typesetter_.resources().outgoingNextLyricLayout_.Get()
                            ? kLyricPreviewGap
                        : typesetter_.resources().outgoingSecondaryLayout_.Get() ? 1.0f
                                                                                 : 0.0f;
                    const float outgoingBlockH = outgoingLyricBlockHeight_ > 0.0f
                                                     ? outgoingLyricBlockHeight_
                                                     : outgoingLyricHeight_ + outgoingPreviewH;
                    const float outgoingY = h * 0.5f - outgoingBlockH * 0.5f;
                    const float travel = std::max(lyricBlockH, outgoingBlockH);
                    const float oldShift =
                        -static_cast<float>(transition_.direction()) * travel * movementT;
                    const float newShift =
                        static_cast<float>(transition_.direction()) * travel * (1.0f - movementT);
                    drawLyricScrollingText(
                        typesetter_.resources().outgoingLyricLayout_.Get(), outgoingLyricWidth_,
                        outgoingLyricHeight_, lyricAreaW, lyricAreaX, outgoingY + oldShift,
                        outgoingLyricScrollOffset_, primaryBrush,
                        lyricEffectsEnabled && lyricOutline_ ? brushLyricOutline_.Get() : nullptr,
                        lyricEffectsEnabled && lyricGlow_ ? brushLyricGlow_.Get() : nullptr,
                        nullptr, 0.0f, 1.0f - transition.fadeOut);
                    if (transition_.outgoingDoubleLine() &&
                        typesetter_.resources().outgoingNextLyricLayout_.Get()) {
                        ID2D1Brush* previewBrush =
                            brushLyricDim_.Get() ? static_cast<ID2D1Brush*>(brushLyricDim_.Get())
                                                 : static_cast<ID2D1Brush*>(brushDim_.Get());
                        drawLyricScrollingText(
                            typesetter_.resources().outgoingNextLyricLayout_.Get(),
                            outgoingNextLyricWidth_, outgoingNextLyricHeight_, lyricAreaW,
                            lyricAreaX,
                            outgoingY + outgoingLyricHeight_ + kLyricPreviewGap + oldShift, 0.0f,
                            previewBrush, nullptr, nullptr, nullptr, 0.0f,
                            kLyricPreviewOpacity * (1.0f - transition.fadeOut));
                    } else if (typesetter_.resources().outgoingSecondaryLayout_.Get()) {
                        drawLyricScrollingText(
                            typesetter_.resources().outgoingSecondaryLayout_.Get(),
                            outgoingSecondaryWidth_, outgoingSecondaryHeight_, lyricAreaW,
                            lyricAreaX, outgoingY + outgoingLyricHeight_ + outgoingGap + oldShift,
                            outgoingSecondaryScrollOffset_, brushDim_.Get(), nullptr, nullptr,
                            nullptr, 0.0f, 1.0f - transition.fadeOut);
                    }
                    // 入场行同步显示真实播放位置的逐字填充，避免转场结束时突然跳色。
                    drawLyricScrollingText(
                        typesetter_.resources().lyricLayout_.Get(), lyricWidth_, lyricHeight_,
                        lyricAreaW, lyricAreaX, h * 0.5f - lyricBlockH * 0.5f + newShift,
                        scroll_.lyric(), incomingBrush,
                        lyricEffectsEnabled && lyricOutline_ ? brushLyricOutline_.Get() : nullptr,
                        lyricEffectsEnabled && lyricGlow_ ? brushLyricGlow_.Get() : nullptr,
                        incomingKaraoke ? brushLyric_.Get() : nullptr, incomingProgX,
                        transition.fadeIn, true);
                    if (typesetter_.resources().secondaryLayout_.Get())
                        drawLyricScrollingText(
                            typesetter_.resources().secondaryLayout_.Get(), secondaryWidth_,
                            secondaryHeight_, lyricAreaW, lyricAreaX,
                            h * 0.5f - lyricBlockH * 0.5f + lyricHeight_ +
                                (typesetter_.resources().secondaryLayout_.Get() ? 1.0f : 0.0f) +
                                newShift,
                            scroll_.secondary(), brushDim_.Get(), nullptr, nullptr, nullptr, 0.0f,
                            transition.fadeIn, true);
                } else {
                    // 逐字高亮：当前行有逐字时间轴且歌词布局对应该行时，
                    // 整行先画未播放色，再按像素进度裁剪出已唱区域画已播放色
                    const LyricLine* curLine = karaokeLine();
                    const bool karaoke = curLine && brushLyric_.Get() && brushLyricDim_.Get();
                    const float progX = karaoke ? karaokeSmoothStep(*curLine) : 0.0f;
                    drawLyricScrollingText(
                        typesetter_.resources().lyricLayout_.Get(), lyricWidth_, lyricHeight_,
                        lyricAreaW, lyricAreaX, h * 0.5f - lyricBlockH * 0.5f, scroll_.lyric(),
                        karaoke ? static_cast<ID2D1Brush*>(brushLyricDim_.Get()) : primaryBrush,
                        lyricEffectsEnabled && lyricOutline_ ? brushLyricOutline_.Get() : nullptr,
                        lyricEffectsEnabled && lyricGlow_ ? brushLyricGlow_.Get() : nullptr,
                        karaoke ? brushLyric_.Get() : nullptr, progX);
                    // 翻译/罗马音仅作整行附属文本，不参与逐字裁剪、描边或光晕。
                    if (typesetter_.resources().secondaryLayout_.Get())
                        drawLyricScrollingText(
                            typesetter_.resources().secondaryLayout_.Get(), secondaryWidth_,
                            secondaryHeight_, lyricAreaW, lyricAreaX,
                            h * 0.5f - lyricBlockH * 0.5f + lyricHeight_ +
                                (typesetter_.resources().secondaryLayout_.Get() ? 1.0f : 0.0f),
                            scroll_.secondary(), brushDim_.Get());
                }
            }
        }

        drawTargetOverride_ = previousTarget;
    }

    D2D1_ROUNDED_RECT taskbarBackgroundRect(float w, float h) const {
        const float radius = std::min(kCornerRadius, w * 0.5f);
        return D2D1_ROUNDED_RECT{D2D1::RectF(0.0f, 0.0f, w, h), radius, radius};
    }

    void drawTaskbarDynamicBackground(float w, float h) {
        const auto regions = taskbarDynamicBackgroundRegions(w, h);
        auto* rt = renderer.renderTarget();
        for (size_t i = 0; i < regions.count; ++i) {
            taskbar_detail::drawIdleQuoteBackground({
                .rt = rt,
                .style = idleQuoteBackground_,
                .width = w,
                .height = h,
                .region = regions.rects[i],
                .frameNowMs = frameNowMs_,
                .brushWarm = brushIdleWarm_.Get(),
                .brushCool = brushIdleCool_.Get(),
                .brushAccent = brushIdleAccent_.Get(),
                .fallbackTextBrush = brushText_.Get(),
                .binaryFontFormat = typesetter_.resources().fmtSecondary_.Get(),
            });
        }
    }

    void drawTaskbarBackground(float w, float h, float progressExtent, bool vertical,
                               ID2D1Effect* coverBlurChain) {
        auto* rt = renderer.renderTarget();
        if (!rt)
            return;

        rt->Clear(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f));
        const D2D1_ROUNDED_RECT bg = taskbarBackgroundRect(w, h);
        if (isAppBarView() && brushBackground_.Get()) {
            // 扩展模式会在下方提前返回；Dock 面向工作区的一侧要在此处绘制分隔线。
            const bool appDark = fluent::isWindowsAppDarkMode();
            brushBackground_->SetColor(appDark ? D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.12f)
                                               : D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.16f));
            const float pixel = 1.0f / std::max(scale(), 1.0f);
            const float top = appBarEdge_ == AppBarEdge::Top ? std::max(0.0f, h - pixel) : 0.0f;
            rt->FillRectangle(D2D1::RectF(0.0f, top, w, std::min(h, top + pixel)),
                              brushBackground_.Get());
        }
        if (isExpandedView()) {
            // 主背景由 Composition 模糊层绘制；动态装饰仍需绘制到透明宿主上。
            drawTaskbarDynamicBackground(w, h);
            return;
        }

        if (coverBlurChain && coverLayer_.Get() && brushBackground_.Get()) {
            ID2D1RoundedRectangleGeometry* clip = nullptr;
            if (auto* factory = renderer.d2d())
                factory->CreateRoundedRectangleGeometry(bg, &clip);
            if (clip) {
                rt->PushLayer(D2D1::LayerParameters1(D2D1::InfiniteRect(), clip,
                                                     D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                                                     D2D1::Matrix3x2F::Identity(),
                                                     coverBackgroundOpacityPct_ / 100.0f),
                              coverLayer_.Get());
                rt->DrawImage(coverBlurChain, D2D1::Point2F(0.0f, 0.0f));
                rt->PopLayer();
                clip->Release();
                brushBackground_->SetColor(lightTheme_ ? D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.45f)
                                                       : D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.45f));
                rt->FillRoundedRectangle(bg, brushBackground_.Get());
            }
        } else if (background_ == TaskbarBackground::Solid && brushBackground_.Get()) {
            brushBackground_->SetColor(lightTheme_ ? D2D1::ColorF(0.97f, 0.97f, 0.97f, 0.85f)
                                                   : D2D1::ColorF(0.13f, 0.13f, 0.13f, 0.85f));
            rt->FillRoundedRectangle(bg, brushBackground_.Get());
        }

        if (!isExpandedView()) {
            rt->FillRoundedRectangle(bg, brushBg_.Get());
            if (mouseOver_ && brushHover_.Get())
                rt->FillRoundedRectangle(bg, brushHover_.Get());
        }

        if (progressBackgroundActive() && brushProgressBg_.Get() && media.durationMs > 0) {
            const float fraction = static_cast<float>(
                std::clamp(static_cast<double>(positionMs_) / static_cast<double>(media.durationMs),
                           0.0, 1.0));
            const float fillExtent = progressExtent * fraction;
            if (fillExtent > 0.5f) {
                const COLORREF c =
                    media.hasDominantColor ? media.dominantColor : fluent::accentColor();
                brushProgressBg_->SetColor(
                    fluent::toD2D(c, progressBackgroundOpacityPct_ / 100.0f));
                const float fillRadius = vertical
                                             ? std::min({bg.radiusX, fillExtent * 0.5f, w * 0.5f})
                                             : std::min({bg.radiusX, fillExtent * 0.5f, h * 0.5f});
                const D2D1_RECT_F fillRect = vertical ? D2D1::RectF(0.0f, 0.0f, w, fillExtent)
                                                      : D2D1::RectF(0.0f, 0.0f, fillExtent, h);
                rt->FillRoundedRectangle(D2D1::RoundedRect(fillRect, fillRadius, fillRadius),
                                         brushProgressBg_.Get());
            }
        }

        drawTaskbarDynamicBackground(w, h);
    }

    bool finishTaskbarFrame(HRESULT hr) {
        if (hr == D2DERR_RECREATE_TARGET) {
            runtime_log::writef(
                L"[taskbar] EndDraw requests device recreate, discarding device resources");
            discardDeviceResources();
            return false;
        }
        if (SUCCEEDED(hr)) {
            const bool startSongTransition = isSongTransitionPending();
            const bool immersiveLayerActive = immersiveSongContentLayerActive();
            const bool finishSongContent =
                immersiveLayerActive && songContentTransition_->finishRequested;
            if (finishSongContent)
                clearImmersiveSongContentTransition();
            if (startSongTransition && !finishSongContent) {
                // 展开视图的根视觉只承载沉浸模式显隐过渡，切歌不能把它复位。
                if (!isExpandedView())
                    resetRootVisual();
                if (!isMinimalMode() && !usesSongContentTransition()) {
                    const float travel = kSongTransitionTravelDip * scale();
                    const bool vertical = isVerticalTaskbar();
                    const float fromX = vertical ? 0.0f : travel;
                    const float fromY =
                        vertical ? (taskbarEdge_ == ABE_LEFT ? -travel : travel) : 0.0f;
                    songRootAnimationInstalled_ =
                        renderer.animateRoot(fromX, 0.0f, fromY, 0.0f, 0.0f, 1.0f,
                                             kSongTransitionMs / 1000.0f);
                    if (!songRootAnimationInstalled_)
                        resetRootVisual();
                }
            }
            // Present 失败（设备丢失/重置）时丢弃设备链，下一帧惰性重建
            if (!renderer.present()) {
                discardDeviceResources();
                return false;
            }
            runtime_log::recordTaskbarFrame(reinterpret_cast<uintptr_t>(hwnd), frameDisplayName_, displayRefreshHz_);
            // 只有真正 Present 成功后才消费该阶段；设备丢失时保留 Pending，
            // 下一次资源重建仍会以同一首歌的完整帧启动转场。
            if (finishSongContent && startSongTransition) {
                requestFrame();
            } else if (startSongTransition) {
                // 合成层已经在运行时，新的切歌请求不能在本帧被旧动画消费；
                // 等当前层收尾后再启动下一张快照。
                if (immersiveLayerActive) {
                    if (songContentTransition_ && !songContentTransition_->pendingConsumed) {
                        setSongTransitionPending(false);
                        songContentTransition_->pendingConsumed = true;
                    }
                } else {
                    setSongTransitionPending(false);
                }
            }
            return true;
        } else {
            runtime_log::writef(L"[taskbar] EndDraw failed: 0x%08X", hr);
            discardDeviceResources();
            return false;
        }
    }

    bool renderVerticalTaskbar(float w, float h, ID2D1Effect* coverBlurChain) {
        auto* rt = renderer.renderTarget();
        if (!rt || w <= 0.0f || h <= 0.0f)
            return false;

        const VerticalLayout layout = verticalLayout();
        if (layout.w <= 0.0f || layout.h <= 0.0f)
            return false;

        rt->BeginDraw();
        rt->SetTransform(D2D1::Matrix3x2F::Identity());
        drawTaskbarBackground(w, h, h, true, coverBlurChain);

        const bool playbackScene =
            scene_ != DisplayScene::Idle && scene_ != DisplayScene::NoPlayback;
        if (layout.coverSize > 0.0f && playbackScene) {
            const float s = layout.coverSize;
            if (albumCoverEffect_ == AlbumCoverEffect::Vinyl) {
                drawVinylCover(layout.coverX, layout.coverY, s);
            } else {
                const D2D1_RECT_F coverRect =
                    D2D1::RectF(layout.coverX, layout.coverY, layout.coverX + s, layout.coverY + s);
                if (cover_.bitmap() && coverClip_.Get() && coverLayer_.Get()) {
                    rt->PushLayer(D2D1::LayerParameters1(
                                      D2D1::InfiniteRect(), coverClip_.Get(),
                                      D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                                      D2D1::Matrix3x2F::Translation(layout.coverX, layout.coverY)),
                                  coverLayer_.Get());
                    rt->DrawBitmap(cover_.bitmap(), coverRect, 1.0f,
                                   D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
                    rt->PopLayer();
                } else {
                    drawFallbackCover(rt, coverRect, std::min(4.0f, s * 0.18f));
                }
            }
            drawPlatformIcon(layout.coverX, layout.coverY, layout.coverSize);
        }

        const float infoHeight = layout.songInfo.bottom - layout.songInfo.top;
        if (infoHeight > 0.0f) {
            const float gap = taskbar_detail::kSongInfoLineGap;
            const float left = (layout.w - titleWidth_ - gap - artistWidth_) * 0.5f;
            const auto drawColumn = [&](const auto& parts, float width, float height, float x, float offset,
                                        ID2D1Brush* brush) {
                const float y = layout.songInfo.top + std::max(0.0f, infoHeight - height) * 0.5f;
                drawVerticalLyricParts(parts, height, infoHeight, width, y,
                                      offset, brush, nullptr, nullptr, nullptr, 0.0f, 1.0f, false,
                                      0.0f, kTextPadding, true, layout.songInfo.top, x, true);
            };
            const auto& text = typesetter_.resources();
            drawColumn(text.verticalTitleParts_, titleWidth_, titleHeight_, left + artistWidth_ + gap, scroll_.title(),
                       brushText_.Get());
            drawColumn(text.verticalArtistParts_, artistWidth_, artistHeight_, left, scroll_.artist(),
                       brushDim_.Get());
            drawSongInfoDivider(layout.lyricY - kSongInfoLyricGap, layout.w, kSongInfoLyricGap, true);
        }

        if (layout.showControls) {
            for (int i = 0; i < 3; ++i)
                drawButton(i, layout.controlCenters[i], layout.controlRadius);
            if (layout.showProgressControl && inlineProgressAvailable())
                drawProgressButton(layout.controlCenters[5], layout.controlRadius);
            drawVolumeButton(layout.controlCenters[3], layout.controlRadius);
            if (media.canSwitchSource)
                drawSwitchSourceButton(layout.controlCenters[4], layout.controlRadius);
        } else {
            drawVerticalLyrics(layout);
        }
        if (layout.resources.bottom > layout.resources.top && ensureResourceFormat())
            resourcePanel_.drawVertical(rt, typesetter_.resources().fmtResource_.Get(),
                                        brushText_.Get(), layout.resources);

        return finishTaskbarFrame(rt->EndDraw());
    }

    bool renderDragPreview(float w, float h) {
        auto* rt = renderer.renderTarget();
        if (!rt || !brushBackground_.Get() || !brushDim_.Get() || !brushText_.Get() ||
            !typesetter_.resources().fmtDragPreview_.Get() || w <= 0.0f || h <= 0.0f)
            return false;

        rt->BeginDraw();
        rt->SetTransform(D2D1::Matrix3x2F::Identity());
        rt->Clear(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f));

        const float inset = 1.5f;
        const float radius = std::max(1.0f, std::min(kCornerRadius, std::min(w, h) * 0.5f - inset));
        const D2D1_ROUNDED_RECT preview = D2D1::RoundedRect(
            D2D1::RectF(inset, inset, std::max(inset, w - inset), std::max(inset, h - inset)),
            radius, radius);
        brushBackground_->SetColor(lightTheme_ ? D2D1::ColorF(0.97f, 0.97f, 0.97f, 0.88f)
                                               : D2D1::ColorF(0.12f, 0.12f, 0.12f, 0.82f));
        rt->FillRoundedRectangle(preview, brushBackground_.Get());
        rt->DrawRoundedRectangle(preview, brushDim_.Get(), 1.25f, dragPreviewStroke_.Get());

        const float textInset = 8.0f;
        const D2D1_RECT_F textRect =
            D2D1::RectF(textInset, textInset, std::max(textInset, w - textInset),
                        std::max(textInset, h - textInset));
        if (isVerticalTaskbar()) {
            constexpr wchar_t verticalText[] = L"松\n手\n固\n定\n到\n这\n里";
            rt->DrawTextW(verticalText, _countof(verticalText) - 1,
                          typesetter_.resources().fmtDragPreview_.Get(), textRect, brushText_.Get(),
                          D2D1_DRAW_TEXT_OPTIONS_CLIP);
        } else {
            rt->DrawTextW(kDragPreviewText, _countof(kDragPreviewText) - 1,
                          typesetter_.resources().fmtDragPreview_.Get(), textRect, brushText_.Get(),
                          D2D1_DRAW_TEXT_OPTIONS_CLIP);
        }
        return finishTaskbarFrame(rt->EndDraw());
    }

    void render() {
        if (updateDepth_ != 0) {
            requestFrame();
            paintPending_ = true;
            return;
        }
        if (rendering_) {
            requestFrame();
            return;
        }
        if (!isWindowVisible() || !hwnd)
            return;

        struct RenderScope {
            bool& active;
            explicit RenderScope(bool& value) : active(value) { active = true; }
            ~RenderScope() { active = false; }
        } renderScope(rendering_);

        int currentWidth = 0;
        int currentHeight = 0;
        clientPixelSize(currentWidth, currentHeight);
        if (currentHeight > 0)
            syncCompactTaskbarLayout(currentHeight);

        if (isExpandedView() && isSceneResizeActive())
            cancelSceneWindowResize();
        if (isSceneResizeActive() && !isLyricTransitionPending())
            updateSceneWindowResize(monotonicNowMs());

        // 先调整窗口，再读取客户区并绑定位图，避免用旧尺寸的位图提交后
        // 把刚刚收缩/扩展的窗口尺寸恢复回去。
        if (isInvalidated(RenderInvalidation::Layout) && !isSceneResizeActive() &&
            !drag_.dragging()) {
            adjustPosition();
            if (!isWindowVisible())
                return;
        }
        ensureImmersiveZOrder();

        int pxW = 0;
        int pxH = 0;
        clientPixelSize(pxW, pxH);
        if (pxW <= 0 || pxH <= 0)
            return;
        const bool vertical = isVerticalTaskbar();
        int logicalPxW = pxW;
        int logicalPxH = pxH;
        if (pxW != lastPxW_ || pxH != lastPxH_) {
            if (isLyricDCompActive())
                requestLyricDCompEnd();
            if (songContentTransition_ && songContentTransition_->compositorLayer)
                clearImmersiveSongContentTransition();
            if (vertical && pxW != lastPxW_) {
                requestInvalidation(RenderInvalidation::SongInfo);
                recreateResourceFormat();
            }
            lastPxW_ = pxW;
            lastPxH_ = pxH;
            requestInvalidation(RenderInvalidation::Geometry);
            if (vertical)
                requestInvalidation(RenderInvalidation::Text);
            // 横向文本布局以超宽无换行创建，度量与窗口尺寸无关；侧边竖排布局
            // 依赖任务栏窄栏宽度，已在上面标记 Text 失效以便重建。
        }
        lastLogicalPxW_ = logicalPxW;
        lastLogicalPxH_ = logicalPxH;

        syncCompactTaskbarLayout(pxH);

        if (!bindRenderer(pxW, pxH))
            return;
        auto* rt = renderer.renderTarget();
        if (!rt)
            return;

        // 预处理和资源创建阶段可能追加失效项；从这里开始才捕获本次提交快照，
        // 避免把本帧刚处理的尺寸/字体失效错误地留到下一帧。
        const InvalidationSnapshot invalidation = renderState_.beginInvalidation();

        if (drag_.dragging()) {
            if (isInvalidated(RenderInvalidation::Text) ||
                isInvalidated(RenderInvalidation::SongInfo))
                buildTextLayouts(0.0f, 0.0f);
            if (updateDragPreviewPlacement()) {
                clientPixelSize(pxW, pxH);
                if (pxW <= 0 || pxH <= 0 || !bindRenderer(pxW, pxH))
                    return;
                rt = renderer.renderTarget();
                if (!rt)
                    return;
                renderer.setDpi(dpi_);
            }
            if (renderDragPreview(dip(pxW), dip(pxH))) {
                // 预览只消费本帧实际处理过的文字/布局/绘制失效；封面、图标与普通
                // 内容几何继续保留，松手恢复歌词后再由完整渲染链处理。
                InvalidationSnapshot previewInvalidation = invalidation;
                previewInvalidation.mask &=
                    toMask(RenderInvalidation::Paint) | toMask(RenderInvalidation::Text) |
                    toMask(RenderInvalidation::SongInfo) | toMask(RenderInvalidation::Layout);
                renderState_.commitInvalidation(previewInvalidation);
            }
            return;
        }

        ensureGeometry();
        if (isInvalidated(RenderInvalidation::Cover))
            decodeCover();
        if (isInvalidated(RenderInvalidation::PlatformIcon))
            decodePlatformIcon();

        if (vertical) {
            if (isInvalidated(RenderInvalidation::Text) ||
                isInvalidated(RenderInvalidation::SongInfo))
                buildTextLayouts(0.0f, 0.0f);
            if (isLyricDCompActive() || isLyricDCompEndRequested()) {
                renderer.clearTransitionLayers();
                clearLyricDCompState();
            }
            ID2D1Effect* coverBlurChain =
                background_ == TaskbarBackground::CoverBlur && cover_.bitmap()
                    ? ensureCoverBlurChain(dip(pxW), dip(pxH))
                    : nullptr;
            if (renderVerticalTaskbar(dip(pxW), dip(pxH), coverBlurChain))
                renderState_.commitInvalidation(invalidation);
            return;
        }

        // 时钟按本地分钟缓存；首帧在这里同步，后续由帧定时器仅在分钟变化时重绘。
        if (!isAppBarView())
            refreshImmersiveClockText();

        LayoutMetrics layout = layoutMetrics(logicalPxW, logicalPxH);
        float w = layout.w;
        float h = layout.h;
        float leftW = layout.leftW;
        float rightW = layout.rightW;

        if (isInvalidated(RenderInvalidation::Text) || isInvalidated(RenderInvalidation::SongInfo))
            buildTextLayouts(leftW, rightW);

        const LyricArea lyricArea = lyricAreaForScene(scene_, logicalPxW, logicalPxH);
        const float lyricAreaX = lyricArea.x;
        const float lyricAreaW = lyricArea.w;
        float secondaryGap = typesetter_.resources().secondaryLayout_.Get() ? 1.0f : 0.0f;
        float lyricBlockH = lyricHeight_ + secondaryGap + secondaryHeight_;
        float lyricY = h * 0.5f - lyricBlockH * 0.5f;
        const bool idleScene = scene_ == DisplayScene::Idle;
        // 每日一言属于任务栏主题正文，不继承上一次播放歌词的用户自定义颜色。
        const bool lyricEffectsEnabled = !idleScene;
        ID2D1Brush* primaryBrush =
            idleScene ? static_cast<ID2D1Brush*>(brushText_.Get())
                      : static_cast<ID2D1Brush*>(brushLyric_.Get() ? brushLyric_.Get()
                                                                   : brushText_.Get());
        // 只有开启悬浮控件且鼠标位于窗口内时才替换歌词，否则保持歌词/频谱视图。
        const auto pinnedControls =
            pinnedInlineControlsLayoutForScene(scene_, logicalPxW, logicalPxH);
        bool showControls =
            !pinnedControls.active() && mouseOver_ && controlsOnHover_ &&
            hoverControlStyle_ == HoverControlStyle::Inline && !idleScene && !isExpandedView() &&
            !(transition_.kind() == LyricTransitionKind::Scene && isLyricTransitionActive() &&
              isTaskbarSceneTransition(sceneTransition_.outgoingScene(), scene_));
        const bool backgroundSpectrum = !idleScene && spectrumVisible_ && backgroundWaveEnabled();
        // 独立频谱占用歌词区右端；背景波浪不改变窗口宽度和歌词布局。
        bool showSpectrum = !idleScene && spectrumVisible_ && !showControls &&
                            !backgroundSpectrum && !integratedSpectrum();
        const bool bottomSpectrum = !idleScene && spectrumVisible_ && !showControls &&
                                    spectrumStyle_ == SpectrumStyle::BottomBand;
        syncLyricTransitionDComp(showControls, lyricAreaX, lyricAreaW, h, lyricBlockH);

        // 模糊效果链涉及资源创建（CreateEffect），与画刷一样放在 BeginDraw 之前
        ID2D1Effect* coverBlurChain = background_ == TaskbarBackground::CoverBlur && cover_.bitmap()
                                          ? ensureCoverBlurChain(w, h)
                                          : nullptr;

        beginImmersiveSongContentTransition();
        bool compositorSongContent =
            immersiveSongContentLayerActive() && !songContentTransition_->finishRequested;
        bool compositorSongContentPending = false;
        if (usesSongContentTransition() && songContentTransition_ &&
            !songContentTransition_->compositorLayer) {
            // 歌词行转场也使用 transitionLayers_。整首歌切换时先交给歌曲内容层，
            // 保证封面、歌曲信息和歌词成为同一张快照，避免两套合成层互相抢占。
            renderer.clearTransitionLayers();
            clearLyricDCompState();
            if (renderer.ensureTransitionLayers(lastPxW_, lastPxH_, 1, 1)) {
                compositorSongContent = true;
                compositorSongContentPending = true;
            }
        }

        rt->BeginDraw();
        rt->SetTransform(D2D1::Matrix3x2F::Identity());
        // 背景、进度和动态底图由横竖两种布局共用，避免后续修复只落在一条路径。
        drawTaskbarBackground(w, h, lyricAreaX + lyricAreaW, false, coverBlurChain);

        if (backgroundSpectrum) {
            const float waveX = infoStartX();
            const float waveRight = backgroundWaveRight(w);
            const float waveW = std::max(1.0f, waveRight - waveX);
            drawBackgroundWaveSpectrum(waveX, h, waveW);
        }
        if (bottomSpectrum)
            drawBottomSpectrum(lyricAreaX, h, lyricAreaW);

        // 沉浸模式的操作组、频谱和时钟都属于固定遮罩层内容，必须在歌曲内容
        // 过渡变换之前绘制；切歌时只让封面、歌曲信息和歌词内容移动。
        if (isExpandedView()) {
            drawImmersiveControls(leftW, h);
            if (!idleScene)
                drawDockPet(w, h, leftW);
            drawImmersiveRightSide(w, h, showSpectrum);
        } else if (embeddedResourceVisible()) {
            const auto resourceRect = horizontalResourceRect(w, h);
            drawDockResourceStatus(resourceRect.left, h, resourceRect.right - resourceRect.left);
        }

        const bool clipSongContent = embeddedResourceVisible();
        const float songContentLeft = embeddedResourceLeftOffset();
        const float songContentRight = w - embeddedResourceRightExtra();
        if (clipSongContent)
            rt->PushAxisAlignedClip(D2D1::RectF(songContentLeft, 0.0f, songContentRight, h),
                                    D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        bool contentTransitionActive = false;
        if (!compositorSongContent) {
            contentTransitionActive = applyImmersiveSongContentTransform(rt);
        }

        // 左侧封面
        if (!compositorSongContent) {
            float s = coverSize();
            float coverX = songContentLeft + kCoverPadding;
            float coverY = (h - s) * 0.5f;
            if (albumCoverVisible_ && !idleScene) {
                if (albumCoverEffect_ == AlbumCoverEffect::Vinyl) {
                    drawVinylCover(coverX, coverY, s);
                } else {
                    D2D1_RECT_F coverRect = D2D1::RectF(coverX, coverY, coverX + s, coverY + s);
                    if (cover_.bitmap() && coverClip_.Get() && coverLayer_.Get()) {
                        rt->PushLayer(
                            D2D1::LayerParameters1(D2D1::InfiniteRect(), coverClip_.Get(),
                                                   D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                                                   D2D1::Matrix3x2F::Translation(coverX, coverY)),
                            coverLayer_.Get());
                        rt->DrawBitmap(cover_.bitmap(), coverRect, 1.0f,
                                       D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
                        rt->PopLayer();
                    } else {
                        drawFallbackCover(rt, coverRect, 4.0f);
                    }
                }
                // 平台图标必须在封面之后绘制，保证它位于专辑封面的最顶层。
                drawPlatformIcon(coverX, coverY, s);
            }

            if (songInfoVisible_ && !idleScene) {
                // 左侧歌曲信息（封面显示时位于封面右侧，整体垂直居中，超长自动滚动）
                // 左右边缘都保留渐隐，右缘在分隔线前收束，避免标题/歌手撞到歌词区
                float infoX = infoStartX();
                float infoW = std::max(1.0f, leftW - infoX - kTextPadding);
                const float infoGap = taskbar_detail::kSongInfoLineGap;
                float totalInfoH = titleHeight_ + infoGap + artistHeight_;
                float infoY = (h - totalInfoH) * 0.5f;
                drawScrollingText(typesetter_.resources().titleLayout_.Get(), titleWidth_,
                                  titleHeight_, infoW, infoX, infoY, scroll_.title(),
                                  brushText_.Get(), nullptr, nullptr, nullptr, 0.0f, 1.0f,
                                  LyricAlignment::Center, false, kTextPadding, true, kTextPadding);
                drawSongInfoSubtitle(infoX, infoW, infoY + titleHeight_ + infoGap);
            }
            if (!idleScene) {
                drawPinnedInlineControls(pinnedControls);
                if (!isExpandedView() && (songInfoVisible_ || pinnedControls.active())) {
                    const float dividerStartX =
                        pinnedControls.active() ? pinnedControls.boundsRight : leftW;
                    const float dividerGap = songInfoVisible_ ? kSongInfoLyricGap : kTextPadding;
                    drawSongInfoDivider(dividerStartX, h, dividerGap);
                }
            }

            if (showControls) {
                drawInlineHoverControls();
            } else {
                if (showSpectrum && !isExpandedView()) {
                    const float spectrumW = spectrumContentWForScene(scene_, w);
                    drawSpectrum(w - embeddedResourceRightExtra() - spectrumW - kTextPadding, h,
                                 spectrumW);
                }
                lyricEffectsRenderer_.setContext(lyricEffectsContext(drawTarget()));
                lyricEffectsRenderer_.drawEasterEggs(
                    D2D1::RectF(lyricAreaX, 0.0f, lyricAreaX + lyricAreaW, h));
                if (transition_.kind() == LyricTransitionKind::Scene && isLyricTransitionActive() &&
                    typesetter_.resources().outgoingLyricLayout_.Get()) {
                    drawSceneTransition(w, lyricAreaX, lyricAreaW, h, lyricBlockH);
                } else if (useDoubleLineLyrics()) {
                    drawDoubleLineLyrics(lyricAreaX, lyricAreaW, h, primaryBrush);
                } else if (!isLyricDCompActive()) {
                    if (isLyricTransitionActive() &&
                        typesetter_.resources().outgoingLyricLayout_.Get()) {
                        // 位移使用平滑的 ease-in-out，避免一开始就冲得太快。
                        const LyricTransitionSample transition = lyricTransitionSample();
                        const float movementT = transition.movement;
                        const LyricLine* incomingLine = karaokeLine();
                        bool incomingKaraoke =
                            incomingLine && brushLyric_.Get() && brushLyricDim_.Get();
                        ID2D1Brush* incomingBrush =
                            incomingKaraoke ? static_cast<ID2D1Brush*>(brushLyricDim_.Get())
                                            : primaryBrush;
                        const float incomingProgX =
                            incomingKaraoke ? karaokeSmoothStep(*incomingLine) : 0.0f;
                        const float outgoingPreviewH =
                            transition_.outgoingDoubleLine() &&
                                    typesetter_.resources().outgoingNextLyricLayout_.Get()
                                ? kLyricPreviewGap + outgoingNextLyricHeight_
                            : typesetter_.resources().outgoingSecondaryLayout_.Get()
                                ? 1.0f + outgoingSecondaryHeight_
                                : 0.0f;
                        float outgoingGap =
                            transition_.outgoingDoubleLine() &&
                                    typesetter_.resources().outgoingNextLyricLayout_.Get()
                                ? kLyricPreviewGap
                            : typesetter_.resources().outgoingSecondaryLayout_.Get() ? 1.0f
                                                                                     : 0.0f;
                        float outgoingBlockH = outgoingLyricBlockHeight_ > 0.0f
                                                   ? outgoingLyricBlockHeight_
                                                   : outgoingLyricHeight_ + outgoingPreviewH;
                        float outgoingY = h * 0.5f - outgoingBlockH * 0.5f;
                        float travel = std::max(lyricBlockH, outgoingBlockH);
                        float oldShift =
                            -static_cast<float>(transition_.direction()) * travel * movementT;
                        float newShift = static_cast<float>(transition_.direction()) * travel *
                                         (1.0f - movementT);
                        drawLyricScrollingText(
                            typesetter_.resources().outgoingLyricLayout_.Get(), outgoingLyricWidth_,
                            outgoingLyricHeight_, lyricAreaW, lyricAreaX, outgoingY + oldShift,
                            outgoingLyricScrollOffset_, primaryBrush,
                            lyricEffectsEnabled && lyricOutline_ ? brushLyricOutline_.Get()
                                                                 : nullptr,
                            lyricEffectsEnabled && lyricGlow_ ? brushLyricGlow_.Get() : nullptr,
                            nullptr, 0.0f, 1.0f - transition.fadeOut);
                        if (transition_.outgoingDoubleLine() &&
                            typesetter_.resources().outgoingNextLyricLayout_.Get()) {
                            ID2D1Brush* previewBrush =
                                brushLyricDim_.Get()
                                    ? static_cast<ID2D1Brush*>(brushLyricDim_.Get())
                                    : static_cast<ID2D1Brush*>(brushDim_.Get());
                            drawLyricScrollingText(
                                typesetter_.resources().outgoingNextLyricLayout_.Get(),
                                outgoingNextLyricWidth_, outgoingNextLyricHeight_, lyricAreaW,
                                lyricAreaX,
                                outgoingY + outgoingLyricHeight_ + kLyricPreviewGap + oldShift,
                                0.0f, previewBrush, nullptr, nullptr, nullptr, 0.0f,
                                kLyricPreviewOpacity * (1.0f - transition.fadeOut));
                        } else if (typesetter_.resources().outgoingSecondaryLayout_.Get())
                            drawLyricScrollingText(
                                typesetter_.resources().outgoingSecondaryLayout_.Get(),
                                outgoingSecondaryWidth_, outgoingSecondaryHeight_, lyricAreaW,
                                lyricAreaX,
                                outgoingY + outgoingLyricHeight_ + outgoingGap + oldShift,
                                outgoingSecondaryScrollOffset_, brushDim_.Get(), nullptr, nullptr,
                                nullptr, 0.0f, 1.0f - transition.fadeOut);
                        // 入场行同步显示真实播放位置的逐字填充，避免转场结束时突然跳色。
                        drawLyricScrollingText(
                            typesetter_.resources().lyricLayout_.Get(), lyricWidth_, lyricHeight_,
                            lyricAreaW, lyricAreaX, lyricY + newShift, scroll_.lyric(),
                            incomingBrush,
                            lyricEffectsEnabled && lyricOutline_ ? brushLyricOutline_.Get()
                                                                 : nullptr,
                            lyricEffectsEnabled && lyricGlow_ ? brushLyricGlow_.Get() : nullptr,
                            incomingKaraoke ? brushLyric_.Get() : nullptr, incomingProgX,
                            transition.fadeIn, true);
                        if (typesetter_.resources().secondaryLayout_.Get())
                            drawLyricScrollingText(typesetter_.resources().secondaryLayout_.Get(),
                                                   secondaryWidth_, secondaryHeight_, lyricAreaW,
                                                   lyricAreaX,
                                                   lyricY + lyricHeight_ + secondaryGap + newShift,
                                                   scroll_.secondary(), brushDim_.Get(), nullptr,
                                                   nullptr, nullptr, 0.0f, transition.fadeIn, true);
                    } else {
                        // 逐字高亮：当前行有逐字时间轴且歌词布局对应该行时，
                        // 整行先画未播放色，再按像素进度裁剪出已唱区域画已播放色
                        const LyricLine* curLine = karaokeLine();
                        bool karaoke = curLine && brushLyric_.Get() && brushLyricDim_.Get();
                        float progX = karaoke ? karaokeSmoothStep(*curLine) : 0.0f;
                        drawLyricScrollingText(
                            typesetter_.resources().lyricLayout_.Get(), lyricWidth_, lyricHeight_,
                            lyricAreaW, lyricAreaX, lyricY, scroll_.lyric(),
                            karaoke ? static_cast<ID2D1Brush*>(brushLyricDim_.Get()) : primaryBrush,
                            lyricEffectsEnabled && lyricOutline_ ? brushLyricOutline_.Get()
                                                                 : nullptr,
                            lyricEffectsEnabled && lyricGlow_ ? brushLyricGlow_.Get() : nullptr,
                            karaoke ? brushLyric_.Get() : nullptr, progX);
                        // 翻译/罗马音仅作整行附属文本，不参与逐字裁剪、描边或光晕。
                        if (typesetter_.resources().secondaryLayout_.Get())
                            drawLyricScrollingText(typesetter_.resources().secondaryLayout_.Get(),
                                                   secondaryWidth_, secondaryHeight_, lyricAreaW,
                                                   lyricAreaX, lyricY + lyricHeight_ + secondaryGap,
                                                   scroll_.secondary(), brushDim_.Get());
                    }
                }
            }
        }

        if (contentTransitionActive)
            rt->SetTransform(D2D1::Matrix3x2F::Identity());
        if (clipSongContent)
            rt->PopAxisAlignedClip();
        // Idle 内容先绘制每日一言，再把宠物放到内容层上方，保证两者同时可见。
        if (isExpandedView() && idleScene)
            drawDockPet(w, h, leftW);

        HRESULT frameHr = rt->EndDraw();
        // 合成器继续负责转场位移，只刷新其中的动态歌词内容，不重启动画。
        if (SUCCEEDED(frameHr) && compositorSongContent && !compositorSongContentPending &&
            lyricEffectsAnimating()) {
            if (auto* layerRt = renderer.beginTransitionLayerDraw(0)) {
                drawHorizontalSongContent(layerRt, w, h, leftW, lyricAreaX, lyricAreaW, lyricBlockH,
                                          primaryBrush, idleScene, lyricEffectsEnabled,
                                          showControls, showSpectrum, pinnedControls);
                if (!renderer.endTransitionLayerDraw(0, layerRt))
                    clearImmersiveSongContentTransition();
            }
        }
        if (SUCCEEDED(frameHr) && compositorSongContentPending) {
            bool layerReady = false;
            if (ID2D1DeviceContext* layerRt = renderer.beginTransitionLayerDraw(0)) {
                drawHorizontalSongContent(layerRt, w, h, leftW, lyricAreaX, lyricAreaW, lyricBlockH,
                                          primaryBrush, idleScene, lyricEffectsEnabled,
                                          showControls, showSpectrum, pinnedControls);
                layerReady = renderer.endTransitionLayerDraw(0, layerRt);
            }
            if (layerReady) {
                const float travelPx = kSongTransitionTravelDip * scale();
                layerReady = renderer.animateTransitionLayerX(
                    0, travelPx, 0.0f, 0.0f, 1.0f, 1.0f, kSongTransitionMs / 1000.0f,
                    clipSongContent && !embeddedResourceAtStart() ? songContentRight * scale()
                                                                  : 0.0f);
            }
            if (layerReady && songContentTransition_) {
                songContentTransition_->compositorLayer = true;
                // DComp 动画的真实起点在本次 Present/Commit 附近；快照绘制可能耗时，
                // 不能沿用创建转场时的时间，否则动画会被提前收尾。
                songContentTransition_->startMs = monotonicNowMs();
            } else {
                renderer.clearTransitionLayers();
                clearLyricDCompState();
                // 合成层创建失败时回到原有 D2D 路径，当前帧仍然完整可见。
                rt->BeginDraw();
                rt->SetTransform(D2D1::Matrix3x2F::Identity());
                drawTaskbarBackground(w, h, lyricAreaX + lyricAreaW, false, coverBlurChain);
                if (backgroundSpectrum) {
                    const float waveX = infoStartX();
                    const float waveRight = backgroundWaveRight(w);
                    const float waveW = std::max(1.0f, waveRight - waveX);
                    drawBackgroundWaveSpectrum(waveX, h, waveW);
                }
                if (bottomSpectrum)
                    drawBottomSpectrum(lyricAreaX, h, lyricAreaW);
                if (isExpandedView()) {
                    drawImmersiveControls(leftW, h);
                    if (!idleScene)
                        drawDockPet(w, h, leftW);
                    drawImmersiveRightSide(w, h, showSpectrum);
                } else if (embeddedResourceVisible()) {
                    const auto resourceRect = horizontalResourceRect(w, h);
                    drawDockResourceStatus(resourceRect.left, h,
                                           resourceRect.right - resourceRect.left);
                }
                if (clipSongContent)
                    rt->PushAxisAlignedClip(D2D1::RectF(songContentLeft, 0.0f, songContentRight, h),
                                            D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
                contentTransitionActive = applyImmersiveSongContentTransform(rt);
                // 失败回退时仍需绘制歌曲内容；这里复用与快照相同的绘制入口，
                // 避免回退路径和正常路径再次产生视觉差异。
                drawHorizontalSongContent(rt, w, h, leftW, lyricAreaX, lyricAreaW, lyricBlockH,
                                          primaryBrush, idleScene, lyricEffectsEnabled,
                                          showControls, showSpectrum, pinnedControls);
                if (contentTransitionActive)
                    rt->SetTransform(D2D1::Matrix3x2F::Identity());
                if (clipSongContent)
                    rt->PopAxisAlignedClip();
                if (isExpandedView() && idleScene)
                    drawDockPet(w, h, leftW);
                frameHr = rt->EndDraw();
            }
        }
        if (finishTaskbarFrame(frameHr))
            renderState_.commitInvalidation(invalidation);
    }

    // ---------- 滚动字幕 ----------

    void updateScroll() {
        const ULONGLONG now = monotonicNowMs();
        frameNowMs_ = now;
        if (isLyricTransitionInProgress() && transition_.revision() != 0 &&
            transition_.revision() != frameRevision_) {
            // 不存在历史过渡队列：版本过期时直接丢弃旧过渡，下一次排版只处理当前行。
            resetLyricTransition();
            requestInvalidation(RenderInvalidation::Text);
        }

        const bool wasTransitioning = isLyricTransitionInProgress();
        if (isLyricTransitionActive() &&
            now - transition_.startMs() >= static_cast<ULONGLONG>(lyricTransitionDurationMs())) {
            finalizeLyricTransition(now);
        }

        TextScrollInput input;
        input.now = now;
        input.animations = clientAnimations_;
        input.wasTransitioning = wasTransitioning;
        input.hold = isLyricTransitionInProgress();
        input.vertical = isVerticalTaskbar();
        input.playing = media.playing;
        input.idle = scene_ == DisplayScene::Idle;
        input.karaoke = karaokeLine() != nullptr;
        input.karaokeProgress = karaoke_.progressForLine(currentLine);
        int width = 0, height = 0;
        logicalClientPixelSize(width, height);
        input.songInfoVisible = songInfoVisible_;
        input.showInlineControls =
            !pinnedInlineControlsLayoutForScene(scene_, width, height).active() && mouseOver_ &&
            controlsOnHover_ && hoverControlStyle_ == HoverControlStyle::Inline;
        input.titleWidth = titleWidth_;
        input.artistWidth = artistWidth_;
        input.lyricWidth = lyricWidth_;
        input.secondaryWidth = secondaryWidth_;
        if (input.vertical) {
            const auto layout = verticalLayout();
            input.infoWidth = layout.songInfo.bottom - layout.songInfo.top;
            input.titleWidth = titleHeight_;
            input.artistWidth = artistHeight_;
            input.area = layout.lyricBottom - layout.lyricY;
            input.verticalExtent = verticalLyricHeight_ > 0.0f ? verticalLyricHeight_ : lyricWidth_;
            input.verticalSecondaryExtent = verticalLyricColumns_.secondaryHeight;
            input.showControls = layout.showControls;
        } else {
            const auto layout = layoutMetrics(width, height);
            input.areaValid = layout.w > 0.0f && layout.h > 0.0f;
            input.infoWidth = std::max(1.0f, layout.leftW - infoStartX() - kTextPadding);
            input.area = lyricAreaForScene(scene_, width, height).w;
        }
        scroll_.advance(input);
    }

    void updateVinylRotation() {
        if (vinyl_.advance(monotonicNowMs(), media.playing,
                           albumCoverEffect_ == AlbumCoverEffect::Vinyl && albumCoverVisible_ &&
                               clientAnimations_))
            requestFrame();
    }

    // ---------- 事件 ----------

    void updateDisplayRefresh() {
        HMONITOR monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
        MONITORINFOEXW info{};
        info.cbSize = sizeof(info);
        if (!GetMonitorInfoW(monitor, &info))
            return;
        frameDisplayName_ = info.szDevice;
        DEVMODEW mode{};
        mode.dmSize = sizeof(mode);
        if (EnumDisplaySettingsW(info.szDevice, ENUM_CURRENT_SETTINGS, &mode) &&
            (mode.dmFields & DM_DISPLAYFREQUENCY) && mode.dmDisplayFrequency >= 24 &&
            displayRefreshHz_ != mode.dmDisplayFrequency) {
            displayRefreshHz_ = mode.dmDisplayFrequency;
            runtime_log::writef(L"[ui][frame-scheduler] monitor=%s refresh=%uHz",
                                info.szDevice, displayRefreshHz_);
            refreshFrameTimer();
        }
    }

    void refreshFrameTimer() {
        if (!timerRunning_ || !hwnd)
            return;
        const double wantMs = desiredFrameMs();
        if (wantMs == timerMs_)
            return;
        frameScheduler_.setInterval(wantMs);
        timerMs_ = wantMs;
    }

    double activeFrameMs() const {
        // 只有低渲染模式固定 ~30fps；极简模式仍使用正常刷新率，歌词帧率不变。
        if (isRenderMode(RenderMode::Low))
            return kTimerPausedMs;
        const UINT hz = displayRefreshHz_ ? displayRefreshHz_ : 60;
        const int preference = appearance_ ? appearance_->frameRate : 1;
        const UINT targetHz = preference == 0 ? std::min(hz, 30u)
                              : preference == 1 ? std::min(hz, 60u) : hz;
        return 1000.0 / targetHz;
    }

    bool hasHighFrequencyAnimation() const {
        const bool songLayerActive = immersiveSongContentLayerActive();
        if (vinyl_.armMoving() && clientAnimations_ && albumCoverVisible_ &&
            albumCoverEffect_ == AlbumCoverEffect::Vinyl && !songLayerActive)
            return true;
        if (horizontalImmersiveMode() && clientAnimations_ && dockPet_.animating())
            return true;
        if (isLyricTransitionInProgress() && !songLayerActive)
            return true;
        if (isSceneResizeActive())
            return true;
        if (songContentTransition_ && !songContentTransition_->compositorLayer)
            return true;
        if (taskbarDynamicBackgroundAnimating() || backgroundWaveAnimating())
            return true;
        if (spectrumVisualsAnimating() || spectrumEffectsAnimating() || lyricEffectsAnimating())
            return true;
        if (resourceMonitoringEnabled() &&
            resourcePanel_.transientLabelsFading(monotonicNowMs(), clientAnimations_))
            return true;
        if (media.playing && scroll_.animating() && !songLayerActive)
            return true;
        if (media.playing && clientAnimations_ && karaokeLine() && !songLayerActive)
            return true;
        if (media.playing && clientAnimations_ && albumCoverVisible_ &&
            albumCoverEffect_ == AlbumCoverEffect::Vinyl && !songLayerActive)
            return true;
        // 播放中的进度背景每帧都在推进
        if (media.playing && progressBackgroundActive())
            return true;
        if (media.playing && spectrumVisible_) {
            for (float v : spectrum_.bands())
                if (v > 0.01f)
                    return true;
        }
        return false;
    }

    double desiredFrameMs() const {
        return hasHighFrequencyAnimation() ? activeFrameMs() : kTimerPausedMs;
    }

    // 帧定时器只在窗口可见时运行：隐藏后的固定间隔 tick（进度插值、滚动、
    // 快照读取）没有任何可见效果，可见性恢复由 SMTC 事件驱动（不依赖定时器）。
    void startFrameTimer() {
        if (!timerRunning_ && hwnd) {
            lastFrameTick_ = 0;
            updateDisplayRefresh();
            const double initialMs = desiredFrameMs();
            timerRunning_ = frameScheduler_.start(hwnd, kFrameMessage, initialMs);
            timerMs_ = timerRunning_ ? initialMs : 0.0;
            if (!timerRunning_)
                runtime_log::writef(L"[ui][frame-scheduler] start failed error=%lu", GetLastError());
        }
    }

    void stopFrameTimer() {
        runtime_log::removeTaskbarFrameRate(reinterpret_cast<uintptr_t>(hwnd));
        if (timerRunning_) {
            frameScheduler_.stop();
            MSG message{};
            while (hwnd && PeekMessageW(&message, hwnd, kFrameMessage, kFrameMessage, PM_REMOVE)) {
            }
            timerRunning_ = false;
            timerMs_ = 0;
            lastFrameTick_ = 0;
        }
    }

    void setRenderMode(RenderMode mode) {
        if (mode == renderState_.mode())
            return;
        const bool leavingStopped = isStoppedMode() && mode != RenderMode::Stopped;
        const bool wasMinimal = isMinimalMode();
        renderState_.setMode(mode);
        if (leavingStopped) {
            if (resourceMonitoringEnabled())
                startResourceMonitoring();
            if (isAppBarView()) {
                renderState_.setVisibilitySuppressed(false);
                setPlacementStatus(TaskbarPlacementStatus::Safe);
            } else {
                // 完全停止期间避让缓存可能已经过期。恢复显示前必须重新等待一次
                // 真实探测，不能按旧的“有空间”结果先显示一帧再隐藏。
                renderState_.setVisibilitySuppressed(true);
                probeReady_ = false;
                placementProbe_.clearResult();
            }
            stopFrameTimer();
            stopPlacementTimer();
            if (!isAppBarView() && !attachment_.embedded())
                scheduleTaskbarAttachRetry();
            reconcileWindowVisibility();
        }
        if (wasMinimal != isMinimalMode()) {
            // 极简关闭逐字绘制后，清掉当前帧的逐字状态；退出时由重新排版恢复几何缓存。
            requestInvalidation(RenderInvalidation::Text);
            karaoke_.clearGeometry();
            karaoke_.reset();
        }
        const bool popupAvailable = mediaPopupAvailable(isSessionVisible());
        mediaPopup.beginPresentationUpdate();
        mediaPopup.setIdleContent(idle, popupAvailable);
        mediaPopup.setMedia(media, popupAvailable);
        mediaPopup.setPresentationMode(scene_, popupAvailable,
                                       hoverControlStyle_ == HoverControlStyle::Inline);
        mediaPopup.endPresentationUpdate();
        syncMediaPopupEnabled();
        if (isMinimalMode()) {
            // 极简仍保留歌词和方形封面，因此不释放主任务栏渲染器；只清掉附加背景链和媒体卡片。
            inlineControlHover_ = -1;
            volumeHover_ = false;
            volumePopup_.hide();
            hideProgressPopup();
            mediaPopupEnabled_ = false;
            mediaPopup.setEnabled(false);
            releaseCoverBackgroundResources();
            // 切歌转场只改变合成器根视觉；进入极简时立即恢复到静止位置。
            setSongTransitionPending(false);
            clearImmersiveSongContentTransition();
            resetRootVisual();
        }
        if (mode == RenderMode::Stopped) {
            // 完全停止：隐藏窗口、停帧定时器、释放 GPU 设备链。此后 SMTC 事件仍更新
            // 内存中的歌词/媒体数据，但 render() 因窗口隐藏直接早退，不占 GPU/CPU
            inlineControlHover_ = -1;
            volumeHover_ = false;
            volumePopup_.hide();
            hideProgressPopup();
            mediaPopupEnabled_ = false;
            mediaPopup.setEnabled(false);
            reconcileWindowVisibility();
            cancelTaskbarAttachRetry();
            stopPlacementTimer();
            stopResourceMonitoring();
            releaseAll();
            return;
        }
        // 帧定时器运行中则立即按新档位调整间隔（正常/极简=跟随刷新率，低渲染=~30fps）
        if (timerRunning_) {
            const double wantMs = desiredFrameMs();
            if (wantMs != timerMs_) {
                frameScheduler_.setInterval(wantMs);
                timerMs_ = wantMs;
            }
        }
        if (!isAppBarView() && renderState_.visibilitySuppressed() && !timerRunning_)
            startPlacementTimer();
        // 从完全停止恢复：按最近会话可见性立即还原窗口；设备链由 render() 惰性重建
        reconcileWindowVisibility();
        if (isWindowVisible())
            requestFrameAndFlush();
    }

    // 静止判定：所有动画源都停止且没有待处理的布局/资源变化时，跳过整帧重绘。
    // 跳过时屏幕上保持上一次 DirectComposition 提交的内容，不会闪烁或丢状态。
    bool needsFrameRender() const {
        const bool songLayerActive = immersiveSongContentLayerActive();
        if (vinyl_.armMoving() && clientAnimations_ && albumCoverVisible_ &&
            albumCoverEffect_ == AlbumCoverEffect::Vinyl && !songLayerActive)
            return true;
        if (renderState_.hasInvalidation())
            return true;
        if (horizontalImmersiveMode() && clientAnimations_ && dockPet_.animating())
            return true;
        if (isLyricTransitionInProgress() && !songLayerActive)
            return true;
        if (isSceneResizeActive())
            return true;
        if (songContentTransition_ && !songContentTransition_->compositorLayer)
            return true;
        if (taskbarDynamicBackgroundAnimating() || backgroundWaveAnimating())
            return true;
        if (scroll_.animating() && !songLayerActive)
            return true;
        // 高亮失效或播放补间未收敛时需要刷新。
        if (karaokeLine() && !karaoke_.settled() && !songLayerActive)
            return true;
        // 播放中的逐字推进和黑胶旋转每帧都在变
        if (media.playing && !songLayerActive &&
            (karaokeLine() || (clientAnimations_ && albumCoverVisible_ &&
                               albumCoverEffect_ == AlbumCoverEffect::Vinyl)))
            return true;
        // 进度背景随播放进度持续推进
        if (media.playing && progressBackgroundActive())
            return true;
        // 频谱柱等静音衰减到阈值以下才算静止
        if (spectrumVisualsAnimating() || spectrumEffectsAnimating() || lyricEffectsAnimating())
            return true;
        if (resourceMonitoringEnabled() &&
            resourcePanel_.transientLabelsFading(monotonicNowMs(), clientAnimations_))
            return true;
        if (spectrumVisible_) {
            for (float v : spectrum_.bands())
                if (v > 0.01f)
                    return true;
        }
        return false;
    }

    void processPlacementProbe() {
        const bool probeWasReady = probeReady_;
        {
            const UpdateScope update(*this);
            updateDisplayRefresh();
            bool changed = detectChanges();
            if (pickProbeResult())
                changed = true;
            if (changed)
                adjustPosition();
        }
        if (!probeWasReady && probeReady_) {
            // 首次真实探测结果到达后再决定是否显示；否则创建期间可能先按
            // 空占用区显示一帧，随后被 UIA/窗口探测结果立即隐藏而产生闪烁。
            if (onPlacementStatusChanged_)
                onPlacementStatusChanged_(placementStatus_);
        }
        if (probeReady_)
            releaseVisibilitySuppression();
    }

    void onTimer() {
        const auto tickNow = monotonicNowMs();
        if (lastFrameTick_ != 0 && tickNow - lastFrameTick_ > std::max(64.0, timerMs_ * 3)) {
            runtime_log::writef(L"[ui][taskbar-frame-gap] elapsed=%llums interval=%.3fms thread=%lu",
                                static_cast<unsigned long long>(tickNow - lastFrameTick_),
                                timerMs_, GetCurrentThreadId());
        }
        lastFrameTick_ = tickNow;
        if (tick)
            tick();
        advancePlaybackClock();
        if (spectrumVisible_ && spectrumReader_)
            spectrum_.setBands(spectrumReader_());
        finishImmersiveSongContentTransitionIfNeeded();
        // TrafficMonitor 可能在两次渲染之间调整自己的任务栏子窗口 Z 序；
        // 即使当前画面静止，也要及时把沉浸宿主恢复到最上层。
        ensureImmersiveZOrder();
        ensureFloatingFallbackZOrder();
        // 只有可见动画源跟随当前显示器刷新率；静态播放降到 ~30fps，
        // 避免歌词没有动画时仍高频轮询和提交。状态事件（悬停、恢复播放、seek）走同步
        // render 路径，不依赖定时器，低档位下也不会延迟显示。
        const double wantMs = desiredFrameMs();
        if (timerRunning_ && wantMs != timerMs_) {
            frameScheduler_.setInterval(wantMs);
            timerMs_ = wantMs;
        }
        frameNowMs_ = monotonicNowMs();
        updateSpectrumVisuals(frameNowMs_);
        updateSpectrumEffects(frameNowMs_);
        const bool lyricAnimating = lyricEffectsAnimating();
        if (lyricAnimating || lyricFxWasAnimating_)
            requestFrame();
        lyricFxWasAnimating_ = lyricAnimating;
        dockPet_.tick(frameNowMs_);
        updateVinylRotation();
        if (refreshResourceSnapshot() ||
            (resourceMonitoringEnabled() &&
             resourcePanel_.tickTransientLabels(frameNowMs_, clientAnimations_)))
            requestFrame();
        // 任务栏位置/DPI/主题跟踪与避让探测结果拾取，放到慢速分支，不跟 60fps 走
        if (++slowTick_ >= kSlowTickInterval) {
            slowTick_ = 0;
            processPlacementProbe();
        }
        updateScroll();
        if (!isAppBarView() && refreshImmersiveClockText())
            requestFrame();
        const bool statusCycleCallbackHandled = scroll_.consumeCompleted();
        if (statusCycleCallbackHandled) {
            if (onStatusTextCycleCompleted_)
                onStatusTextCycleCompleted_();
        }
        // 静止场景跳过整帧重绘：动画源全部停止且无脏状态时，画面保持上一帧内容
        if (!statusCycleCallbackHandled && needsFrameRender())
            requestFrameAndFlush();
    }

    void onPlacementTimer() {
        if (isStoppedMode() || !hwnd)
            return;
        processPlacementProbe();
    }

    void trackMouseLeave() {
        if (trackingLeave_)
            return;
        TRACKMOUSEEVENT tme{};
        tme.cbSize = sizeof(tme);
        tme.dwFlags = TME_LEAVE;
        tme.hwndTrack = hwnd;
        TrackMouseEvent(&tme);
        trackingLeave_ = true;
    }

    LRESULT handle(UINT msg, WPARAM wp, LPARAM lp) {
        switch (msg) {
        case WM_CREATE:
            // 普通创建保持不可见；若正在等待首次避让探测，定时器会由
            // createWindow() 启动，但仍禁止提交可见窗口。
            return 0;
        case kFrameMessage:
            frameScheduler_.acknowledge();
            if (timerRunning_)
                onTimer();
            return 0;
        case WM_TIMER:
            if (wp == kPlacementTimerId)
                onPlacementTimer();
            else if (wp == kTaskbarAttachTimerId)
                retryTaskbarAttach();
            else if (wp == kImmersiveYieldTimerId)
                onImmersiveYieldTimer();
            else if (wp == kImmersivePresenceTimerId)
                onImmersivePresenceTimer();
            else if (wp == kRepeatHintTimerId)
                onRepeatHintTimer();
            return 0;
        case WM_DISPLAYCHANGE: {
            // 显示拓扑/分辨率变化（远程接入、虚拟屏开关等）是设备丢失的前置信号，
            // 记录下来便于和随后的 device lost / 重建日志对照。
            runtime_log::writef(
                L"[display] changed: %lux%lu %lubpp", static_cast<unsigned long>(LOWORD(lp)),
                static_cast<unsigned long>(HIWORD(lp)), static_cast<unsigned long>(wp));
            const HWND previousTaskbar = taskbar_;
            findTaskbar();
            updateDisplayRefresh();
            if (isAppBarView()) {
                if (appBar_.registered())
                    updateAppBarPosition();
            } else {
                if (!attachment_.embedded() && taskbar_ && taskbar_ != previousTaskbar) {
                    attachment_.reset();
                    if (!attachToTaskbar(hwnd))
                        scheduleTaskbarAttachRetry();
                    else
                        cancelTaskbarAttachRetry();
                }
                adjustPosition();
            }
            // 不等待歌词、频谱等下一次内容变化才发现客户区尺寸已改变。立即按新
            // 任务栏几何重算安全区，并让 renderer.bind() 在下一帧同步交换链尺寸。
            requestInvalidation(toMask(RenderInvalidation::Layout) |
                                toMask(RenderInvalidation::Geometry));
            requestFrameAndFlush();
            return 0;
        }
        case WM_SIZE:
            hoverTip_.hide();
            if (wp != SIZE_MINIMIZED) {
                // SetWindowPos/Explorer/AppBar 都可能直接改变客户区；显式唤醒一帧，
                // 避免 DXGI_SCALING_STRETCH 暂时拉伸旧帧直到下一句歌词出现。
                requestInvalidation(RenderInvalidation::Geometry);
                requestFrame();
            }
            if (volumeHover_ && !isExpandedView() && !isVerticalTaskbar() &&
                pinnedInlineControlsConfigured() && !pinnedInlineControlsLayout().visible[4]) {
                volumeHover_ = false;
                volumePopup_.hide();
            }
            if (progressHover_ && !isExpandedView() && !isVerticalTaskbar() &&
                pinnedInlineControlsConfigured() &&
                !pinnedInlineControlsLayout().visible[kInlineProgressControl]) {
                hideProgressPopup();
            }
            return DefWindowProcW(hwnd, msg, wp, lp);
        case WM_SHOWWINDOW:
            if (!wp)
                hoverTip_.hide();
            return DefWindowProcW(hwnd, msg, wp, lp);
        case WM_DPICHANGED:
            hoverTip_.hide();
            if (isAppBarView() && appBar_.registered()) {
                dpi_ = HIWORD(wp) ? HIWORD(wp) : LOWORD(wp);
                renderer.setDpi(dpi_);
                requestInvalidation(toMask(RenderInvalidation::Layout) |
                                    toMask(RenderInvalidation::Text) |
                                    toMask(RenderInvalidation::Cover));
                updateAppBarPosition();
                requestFrameAndFlush();
                return 0;
            }
            return DefWindowProcW(hwnd, msg, wp, lp);
        case WM_ACTIVATE:
            if (appBar_.registered()) {
                APPBARDATA data{};
                data.cbSize = sizeof(data);
                data.hWnd = hwnd;
                data.lParam = LOWORD(wp) != WA_INACTIVE;
                SHAppBarMessage(ABM_ACTIVATE, &data);
            }
            return DefWindowProcW(hwnd, msg, wp, lp);
        case WM_WINDOWPOSCHANGED:
            if (appBar_.registered()) {
                APPBARDATA data{};
                data.cbSize = sizeof(data);
                data.hWnd = hwnd;
                SHAppBarMessage(ABM_WINDOWPOSCHANGED, &data);
            }
            return DefWindowProcW(hwnd, msg, wp, lp);
        case WM_ERASEBKGND:
            return 1;
        case WM_NCHITTEST:
            // 整个窗口区域都视为客户区，让透明背景也能接收鼠标消息
            return HTCLIENT;
        case WM_SETCURSOR:
            if (LOWORD(lp) == HTCLIENT && drag_.dragging()) {
                SetCursor(LoadCursorW(nullptr, IDC_SIZEALL));
                return TRUE;
            }
            if (LOWORD(lp) == HTCLIENT) {
                POINT point{};
                GetCursorPos(&point);
                ScreenToClient(hwnd, &point);
                const float x = static_cast<float>(point.x);
                const float y = static_cast<float>(point.y);
                const int control = isExpandedView() ? hitImmersiveControl(x, y)
                                                     : hitInlineControl(x, y);
                if (hitAlbumCover(x, y) || control == kInlineSourceControl) {
                    SetCursor(LoadCursorW(nullptr, IDC_HAND));
                    return TRUE;
                }
            }
            return DefWindowProcW(hwnd, msg, wp, lp);
        case WM_MOUSEMOVE: {
            RECT coverBounds{};
            const bool coverHovered = !drag_.pressed() && pressedCoverSource_.empty() &&
                hitAlbumCover(static_cast<float>(GET_X_LPARAM(lp)),
                              static_cast<float>(GET_Y_LPARAM(lp)), &coverBounds);
            if (coverHovered)
                hoverTip_.mouseMove(hwnd, coverBounds, wp, lp, L"点击打开当前音乐播放器");
            POINT cursor{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            ClientToScreen(hwnd, &cursor);
            if (drag_.pressed()) {
                const int thresholdX = std::max(GetSystemMetrics(SM_CXDRAG),
                                                (int)std::lround(kLyricDragThresholdDip * scale()));
                const int thresholdY = std::max(GetSystemMetrics(SM_CYDRAG),
                                                (int)std::lround(kLyricDragThresholdDip * scale()));
                if (drag_.move(cursor, thresholdX, thresholdY))
                    beginLyricDrag();
                if (drag_.dragging() && updateDragPreviewPlacement())
                    requestFrameAndFlush();
                return 0;
            }
            const float mouseX = static_cast<float>(GET_X_LPARAM(lp));
            const float mouseY = static_cast<float>(GET_Y_LPARAM(lp));
            const bool inPinnedControls = pointInPinnedControlsZone(mouseX, mouseY);
            const bool wasOverResource = resourceHovered_;
            const bool wasOverVerticalResource = verticalResourceHovered_;
            resourceHovered_ = pointInResourceArea(mouseX, mouseY);
            verticalResourceHovered_ = isVerticalTaskbar() && resourceHovered_;
            const bool triggeredResourceLabels =
                !wasOverResource && resourceHovered_ && resourcePanel_.visibility().hideLabels;
            if (triggeredResourceLabels)
                resourcePanel_.triggerTransientLabels(monotonicNowMs());
            bool wasOver = mouseOver_;
            mouseOver_ = true;
            if (mediaPopupEnabledForScene()) {
                if (inPinnedControls)
                    mediaPopup.onAnchorLeave();
                else
                    mediaPopup.onAnchorEnter();
            }
            if (!wasOver || wasOverVerticalResource != verticalResourceHovered_ ||
                triggeredResourceLabels)
                requestFrameAndFlush();
            trackMouseLeave();
            const int immersiveHover = isExpandedView() ? hitImmersiveControl(mouseX, mouseY) : -1;
            if (immersiveHover != immersiveControlHover_) {
                immersiveControlHover_ = immersiveHover;
                requestFrameAndFlush();
            }
            if (viewMode_ == TaskbarViewMode::Immersive)
                updateImmersiveYield(hitImmersiveControl(mouseX, mouseY, true));
            // 内嵌控件的音量按钮：悬停弹出音量滑块浮窗
            const int inlineHover = hitInlineControl(mouseX, mouseY);
            if (!coverHovered) {
                if ((isExpandedView() ? immersiveHover : inlineHover) == kInlineSourceControl) {
                    RECT bounds{};
                    GetClientRect(hwnd, &bounds);
                    hoverTip_.mouseMove(hwnd, bounds, wp, lp, L"切换到下一个播放来源");
                } else {
                    hoverTip_.hide();
                }
            }
            const bool volHover = (isExpandedView() ? immersiveHover : inlineHover) ==
                                  kImmersiveControlVolume;
            const bool progHover = isExpandedView() ? (immersiveHover == kImmersiveControlProgress)
                                                    : (inlineHover == kInlineProgressControl);
            if (inlineHover != inlineControlHover_) {
                inlineControlHover_ = inlineHover;
                requestFrameAndFlush();
            }
            if (volHover != volumeHover_) {
                volumeHover_ = volHover;
                if (!volHover)
                    volumePopup_.onAnchorLeave();
            }
            if (progHover != progressHover_) {
                progressHover_ = progHover;
                if (!progHover)
                    progressPopup_.onAnchorLeave();
            }
            if (volHover && appVolume_.available && immersiveYieldAllowedState()) {
                if (inPinnedControls)
                    mediaPopup.hideImmediate();
                hideProgressPopup();
                volumePopup_.onAnchorEnter();
                volumePopup_.showNear(volumeButtonScreenRect(), isVerticalTaskbar(),
                                      taskbarEdge_ == ABE_LEFT);
            }
            if (progHover && inlineProgressAvailable() && immersiveYieldAllowedState()) {
                if (inPinnedControls)
                    mediaPopup.hideImmediate();
                volumeHover_ = false;
                volumePopup_.onAnchorLeave();
                volumePopup_.hide();
                syncProgressPopup();
                progressPopup_.onAnchorEnter();
                progressPopup_.showNear(progressButtonScreenRect(), isVerticalTaskbar(),
                                        taskbarEdge_ == ABE_LEFT);
            }
            return 0;
        }
        case WM_MOUSELEAVE:
            hoverTip_.hide();
            sourceWheelRemainder_ = 0;
            if (drag_.pressed() || drag_.dragging())
                return 0;
            cancelImmersiveYield();
            mouseOver_ = false;
            resourceHovered_ = false;
            verticalResourceHovered_ = false;
            trackingLeave_ = false;
            volumeHover_ = false;
            progressHover_ = false;
            immersiveControlHover_ = -1;
            inlineControlHover_ = -1;
            volumePopup_.onAnchorLeave();
            progressPopup_.onAnchorLeave();
            mediaPopup.onAnchorLeave();
            requestFrameAndFlush();
            return 0;
        case WM_MOUSEWHEEL: {
            // 滚轮消息使用屏幕坐标；音量/进度图标上滚动直接微调
            POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            ScreenToClient(hwnd, &pt);
            const int delta = GET_WHEEL_DELTA_WPARAM(wp);
            const bool volumeHit = hitVolumeButton(static_cast<float>(pt.x),
                                                    static_cast<float>(pt.y));
            if (appVolume_.available && onAppVolume && volumeHit) {
                sourceWheelRemainder_ = 0;
                const int steps = delta / WHEEL_DELTA;
                if (steps != 0) {
                    const auto request = AppVolumeRequest::create(
                        std::clamp(appVolume_.percent + steps * 2, 0, 100));
                    appVolume_.percent = request.percent;
                    appVolume_.muted = false;
                    appVolume_.requestRevision = request.revision;
                    volumePopup_.setVolume(appVolume_);
                    mediaPopup.setAppVolume(appVolume_);
                    requestFrameAndFlush();
                    onAppVolume(request);
                }
                return 0;
            }
            if (inlineProgressAvailable() && onControl &&
                hitProgressButton(static_cast<float>(pt.x), static_cast<float>(pt.y))) {
                sourceWheelRemainder_ = 0;
                const int steps = delta / WHEEL_DELTA;
                if (steps != 0) {
                    syncProgressPopup();
                    progressPopup_.seekBy(static_cast<int64_t>(steps) * 5000);
                }
                return 0;
            }
            if (hitDockResourceArea(static_cast<float>(pt.x), static_cast<float>(pt.y))) {
                const int steps = delta / WHEEL_DELTA;
                if (steps != 0)
                    advanceDockResourcePage(steps);
                return 0;
            }
            if (hitSourceWheelArea(static_cast<float>(pt.x), static_cast<float>(pt.y))) {
                const int total = sourceWheelRemainder_ + delta;
                sourceWheelRemainder_ = total % WHEEL_DELTA;
                const int steps = total / WHEEL_DELTA;
                const auto action =
                    steps > 0 ? MediaControl::PreviousSource : MediaControl::NextSource;
                for (int i = 0; i < std::abs(steps); ++i)
                    onControl(action, media.sessionGeneration, 0);
            } else {
                sourceWheelRemainder_ = 0;
            }
            return 0;
        }
        case WM_LBUTTONDOWN: {
            hoverTip_.hide();
            pressedControlSessionGeneration_ = media.sessionGeneration;
            pressedCoverSource_ =
                hitAlbumCover(static_cast<float>(GET_X_LPARAM(lp)),
                              static_cast<float>(GET_Y_LPARAM(lp)))
                    ? media.sourceAppUserModelId : std::wstring{};
            // 点击与拖动从同一次按下并行识别：未越过系统拖动阈值仍按原按钮/卡片
            // 点击处理，越过阈值后才取消点击并进入位置拖拽。
            if (isExpandedView()) {
                immersiveControlPressed_ = hitImmersiveControl(
                    static_cast<float>(GET_X_LPARAM(lp)), static_cast<float>(GET_Y_LPARAM(lp)));
                if (immersiveControlPressed_ >= 0 || !pressedCoverSource_.empty()) {
                    if (viewMode_ == TaskbarViewMode::Immersive)
                        cancelImmersiveHide();
                    SetCapture(hwnd);
                }
                requestFrameAndFlush();
                return 0;
            }
            POINT pressPoint{};
            GetCursorPos(&pressPoint);
            inlineSourcePressed_ =
                hitInlineControl(static_cast<float>(GET_X_LPARAM(lp)),
                                 static_cast<float>(GET_Y_LPARAM(lp))) == kInlineSourceControl;
            drag_.press(pressPoint, -1);
            SetCapture(hwnd);
            if (inlineControlHover_ >= 0)
                requestFrameAndFlush();
            return 0;
        }
        case WM_LBUTTONUP: {
            const auto pressedControlSession = std::exchange(pressedControlSessionGeneration_, 0);
            const bool sourceControlPressed = std::exchange(inlineSourcePressed_, false);
            const auto pressedSource = std::exchange(pressedCoverSource_, std::wstring{});
            if (isExpandedView()) {
                const int pressed = immersiveControlPressed_;
                const int released = hitImmersiveControl(static_cast<float>(GET_X_LPARAM(lp)),
                                                         static_cast<float>(GET_Y_LPARAM(lp)));
                immersiveControlPressed_ = -1;
                if (GetCapture() == hwnd)
                    ReleaseCapture();
                // 沉浸层可能在一次任务栏点击的按下与松开之间出现。只有本窗口
                // 确实收到同一控件的按下事件，才把随后到达的松开解释为点击。
                if (!pressedSource.empty() && pressedSource == media.sourceAppUserModelId &&
                    hitAlbumCover(static_cast<float>(GET_X_LPARAM(lp)),
                                  static_cast<float>(GET_Y_LPARAM(lp))))
                    onSourceOpen(pressedSource);
                else if (pressed >= 0 && pressed == released &&
                         (pressed > kImmersiveControlRepeat ||
                          pressedControlSession == media.sessionGeneration))
                    activateImmersiveControl(pressed);
                requestFrameAndFlush();
                return 0;
            }
            const bool hadPress = drag_.pressed() || drag_.dragging();
            const bool wasDragging = drag_.dragging();
            if (hadPress)
                finishLyricDrag(true);
            if (hadPress && inlineControlHover_ >= 0)
                requestFrameAndFlush();
            if (wasDragging)
                return 0;
            if (hadPress) {
                RECT client{};
                POINT releasePoint{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
                GetClientRect(hwnd, &client);
                if (!PtInRect(&client, releasePoint))
                    return 0;
            }
            float mx = static_cast<float>(GET_X_LPARAM(lp));
            float my = static_cast<float>(GET_Y_LPARAM(lp));
            if (!pressedSource.empty()) {
                if (hadPress && pressedSource == media.sourceAppUserModelId && hitAlbumCover(mx, my))
                    onSourceOpen(pressedSource);
                return 0;
            }
            // 从封面外按下再移入，不把松开误认为封面点击或卡片点击。
            if (hitAlbumCover(mx, my))
                return 0;
            const int btn = hitInlineControl(mx, my);
            if (sourceControlPressed || btn == kInlineSourceControl) {
                if (sourceControlPressed && hadPress && btn == kInlineSourceControl && onControl)
                    onControl(MediaControl::NextSource, media.sessionGeneration, 0);
                return 0;
            }
            if (btn >= 0 && btn <= kImmersiveControlRepeat &&
                (!hadPress || pressedControlSession != media.sessionGeneration))
                return 0;
            // 循环按钮点击后先记下期望模式，等回读结果出来再显示提示词。
            if (btn == static_cast<int>(MediaControl::Repeat))
                armRepeatHint(SmtcSnapshot::nextPlayMode(media.repeatMode));
            if (btn == kInlineProgressControl) {
                if (inlineProgressAvailable()) {
                    volumeHover_ = false;
                    volumePopup_.onAnchorLeave();
                    volumePopup_.hide();
                    syncProgressPopup();
                    progressPopup_.onAnchorEnter();
                    progressPopup_.showNear(progressButtonScreenRect(), isVerticalTaskbar(),
                                            taskbarEdge_ == ABE_LEFT);
                }
                return 0;
            }
            if (btn >= 0 && btn != kImmersiveControlVolume && onControl)
                onControl(static_cast<MediaControl>(btn), media.sessionGeneration, 0);
            // 卡片的当前页面自己判断是否为点击展开；这样无播放时的每日一言卡片
            // 可以独立于媒体控件样式使用点击展开。
            else if (!pointInPinnedControlsZone(mx, my) && !hitVolumeButton(mx, my) &&
                     !hitProgressButton(mx, my) && !isMinimalMode() && !isExpandedView())
                mediaPopup.onAnchorClick();
            return 0;
        }
        case WM_CAPTURECHANGED:
            pressedCoverSource_.clear();
            inlineSourcePressed_ = false;
            immersiveControlPressed_ = -1;
            if (drag_.pressed() || drag_.dragging())
                finishLyricDrag(false);
            return 0;
        case WM_CANCELMODE: {
            hoverTip_.hide();
            inlineSourcePressed_ = false;
            const bool hadCoverPress = !pressedCoverSource_.empty();
            pressedCoverSource_.clear();
            const bool hadImmersivePress = immersiveControlPressed_ >= 0;
            immersiveControlPressed_ = -1;
            if ((hadImmersivePress || hadCoverPress) && GetCapture() == hwnd)
                ReleaseCapture();
            if (drag_.pressed() || drag_.dragging())
                finishLyricDrag(false);
            return 0;
        }
        case WM_CONTEXTMENU: {
            if (!contextMenuEnabled_ || !onContextMenu)
                return 0;

            POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            if (pt.x == -1 && pt.y == -1) {
                RECT rc{};
                GetWindowRect(hwnd, &rc);
                pt = POINT{(rc.left + rc.right) / 2, (rc.top + rc.bottom) / 2};
            }

            openTaskbarMenu(pt);
            return 0;
        }
        case kAppBarCallbackMessage:
            if (!appBar_.registered())
                return 0;
            switch (wp) {
            case ABN_POSCHANGED:
                updateAppBarPosition();
                break;
            case ABN_FULLSCREENAPP:
                appBar_.setFullscreen(lp != FALSE);
                runtime_log::writef(L"[dock] fullscreen %s; z-order=%s",
                                    appBar_.fullscreen() ? L"opened" : L"closed",
                                    appBar_.fullscreen() ? L"bottom" : L"topmost");
                updateAppBarZOrder();
                break;
            case ABN_STATECHANGE:
                updateAppBarZOrder();
                break;
            case ABN_WINDOWARRANGE:
                if (lp)
                    ShowWindow(hwnd, SW_HIDE);
                else if (shouldShowWindow())
                    ShowWindow(hwnd, SW_SHOWNA);
                break;
            default:
                break;
            }
            return 0;
        case WM_QUERYENDSESSION:
            return TRUE;
        case WM_ENDSESSION:
            if (wp) {
                if (onClose_)
                    onClose_();
            }
            return 0;
        case WM_CLOSE:
            if (onClose_)
                onClose_();
            return 0;
        case WM_DESTROY:
            hoverTip_.reset();
            pressedCoverSource_.clear();
            runtime_log::writef(L"[taskbar] WM_DESTROY (visible=%d)", isWindowVisible() ? 1 : 0);
            resetImmersivePresence();
            dockBackdrop_.reset();
            renderState_.setWindowPhase(RenderState::WindowPhase::Hidden);
            unregisterAppBar();
            KillTimer(hwnd, kTaskbarAttachTimerId);
            attachment_.reset();
            floatingZOrderCooldownUntilMs_ = 0;
            stopFrameTimer();
            stopPlacementTimer();
            stopProbe();
            stopResourceMonitoring();
            dragAnchors_.clear();
            drag_.end();
            volumePopup_.destroy();
            progressPopup_.destroy();
            mediaPopup.destroy();
            releaseAll();
            hwnd = nullptr;
            return 0;
        default:
            return DefWindowProcW(hwnd, msg, wp, lp);
        }
    }

    static LRESULT CALLBACK wndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
        Impl* self = nullptr;
        if (msg == WM_NCCREATE) {
            auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
            self = static_cast<Impl*>(cs->lpCreateParams);
            self->hwnd = h;
            SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        } else {
            self = reinterpret_cast<Impl*>(GetWindowLongPtrW(h, GWLP_USERDATA));
        }
        if (self) {
            // 跨线程 SendMessage 等待期间允许重入派发器。即使重入任务释放了
            // 宿主，当前窗口调用栈也必须保有 Impl，直至本次消息处理结束。
            const auto lifetime = self->weak_from_this().lock();
            if (!lifetime)
                return DefWindowProcW(h, msg, wp, lp);
            if (msg == WM_NCDESTROY) {
                SetWindowLongPtrW(h, GWLP_USERDATA, 0);
                if (self->hwnd == h)
                    self->hwnd = nullptr;
                return DefWindowProcW(h, msg, wp, lp);
            }
            if (self->destroying_ && msg != WM_DESTROY)
                return DefWindowProcW(h, msg, wp, lp);
            return self->handle(msg, wp, lp);
        }
        return DefWindowProcW(h, msg, wp, lp);
    }
};

void TaskbarHost::applyAppearance(const TaskbarAppearance& appearance) {
    thread_->invoke([=, this]() mutable {
        impl_->applyAppearance(appearance);
    });
}

TaskbarHost::TaskbarHost()
    : callbacks_(std::make_unique<UiDispatcher>()), thread_(std::make_unique<UiThread>()) {
    thread_->invoke([this] {
        impl_ = std::make_shared<Impl>();
        runtime_log::writef(L"[ui][thread] role=taskbar thread=%lu", GetCurrentThreadId());
    });
}

TaskbarHost::~TaskbarHost() {
    callbacks_->close();
    thread_->invoke([this] {
        impl_->destroying_ = true;
        impl_->stopProbe();
        if (impl_->hwnd)
            DestroyWindow(impl_->hwnd);
        impl_.reset();
    });
}

bool TaskbarHost::create(HINSTANCE inst) {
    return thread_->invoke([=, this]() mutable {
        return impl_->createWindow(inst, L"", TaskbarViewMode::Embedded);
    });
}

bool TaskbarHost::create(HINSTANCE inst, const std::wstring& monitorDeviceName,
                         TaskbarViewMode initialViewMode) {
    return thread_->invoke([=, this]() mutable {
        return impl_->createWindow(inst, monitorDeviceName, initialViewMode);
    });
}

HWND TaskbarHost::hwnd() const {
    return thread_->invoke([=, this]() mutable {
        return impl_ ? impl_->hwnd : nullptr;
    });
}

void TaskbarHost::setTickCallback(std::function<void()> cb) {
    cb = callbacks_->onceCallback(std::move(cb));
    thread_->invoke([=, this]() mutable {
        impl_->tick = std::move(cb);
    });
}

void TaskbarHost::setCloseCallback(std::function<void()> cb) {
    cb = callbacks_->callback(std::move(cb));
    thread_->invoke([this, cb = std::move(cb)]() mutable { impl_->onClose_ = std::move(cb); });
}

void TaskbarHost::applyPresentationFrame(const PresentationFrame& frame) {
    thread_->invoke([=, this]() mutable {
        impl_->applyPresentationFrame(frame);
    });
}

void TaskbarHost::applyPlaybackPatch(const PlaybackPatch& patch) {
    thread_->invoke([=, this]() mutable {
        impl_->acceptPlaybackPatch(patch);
    });
}

void TaskbarHost::applySpectrumPatch(const SpectrumPatch& patch) {
    thread_->invoke([=, this]() mutable {
        impl_->applySpectrumPatch(patch);
    });
}

void TaskbarHost::setControlCallback(std::function<void(MediaControl, uint64_t, int64_t)> cb) {
    cb = callbacks_->callback(std::move(cb));
    thread_->invoke([=, this]() mutable {
        impl_->onControl = cb;
        impl_->mediaPopup.setControlCallback(std::move(cb));
    });
}

void TaskbarHost::setImmersiveExitCallback(std::function<void()> cb) {
    cb = callbacks_->callback(std::move(cb));
    thread_->invoke([=, this]() mutable {
        impl_->onImmersiveExit = std::move(cb);
    });
}

void TaskbarHost::setAppVolume(const AppVolumeState& state) {
    thread_->invoke([=, this]() mutable {
        if (state.available && state.requestRevision < impl_->appVolume_.requestRevision)
            return;
        const bool changed = state.available != impl_->appVolume_.available ||
                             state.percent != impl_->appVolume_.percent ||
                             state.muted != impl_->appVolume_.muted;
        impl_->appVolume_ = state;
        impl_->volumePopup_.setVolume(state);
        impl_->mediaPopup.setAppVolume(state);
        if (!state.available)
            impl_->volumePopup_.hide();
        if (changed) {
            impl_->requestFrame();
            impl_->flushRenderRequest();
        }
    });
}

void TaskbarHost::setAppVolumeCallback(std::function<void(AppVolumeRequest)> cb) {
    if (cb) {
        struct PendingVolume {
            std::mutex mutex;
            std::optional<AppVolumeRequest> request;
        };
        auto pending = std::make_shared<PendingVolume>();
        auto dispatch = callbacks_->onceCallback([pending, function = std::move(cb)] {
            std::optional<AppVolumeRequest> request;
            {
                std::lock_guard<std::mutex> lock(pending->mutex);
                request = std::exchange(pending->request, std::nullopt);
            }
            if (request)
                function(*request);
        });
        cb = [pending, dispatch = std::move(dispatch)](AppVolumeRequest request) {
            {
                std::lock_guard<std::mutex> lock(pending->mutex);
                if (!pending->request || request.revision > pending->request->revision)
                    pending->request = request;
            }
            dispatch();
        };
    }
    thread_->invoke([=, this]() mutable {
        impl_->onAppVolume = cb;
        impl_->volumePopup_.setCallback(cb);
        impl_->mediaPopup.setAppVolumeCallback(std::move(cb));
    });
}

void TaskbarHost::setSourceOpenCallback(std::function<void(const std::wstring&)> cb) {
    cb = callbacks_->callback(std::move(cb));
    thread_->invoke([=, this]() mutable {
        impl_->onSourceOpen = cb;
        impl_->mediaPopup.setSourceOpenCallback(std::move(cb));
    });
}

void TaskbarHost::setIdleAppOpenCallback(std::function<void(const std::wstring&)> cb) {
    cb = callbacks_->callback(std::move(cb));
    thread_->invoke([=, this]() mutable {
        impl_->mediaPopup.setIdleAppOpenCallback(std::move(cb));
    });
}

void TaskbarHost::setIdleTaskOpenCallback(std::function<void(const IdleTaskInfo&)> cb) {
    cb = callbacks_->callback(std::move(cb));
    thread_->invoke([=, this]() mutable {
        impl_->mediaPopup.setIdleTaskOpenCallback(std::move(cb));
    });
}

void TaskbarHost::setIdleTaskCompleteCallback(std::function<void(const IdleTaskInfo&)> cb) {
    cb = callbacks_->callback(std::move(cb));
    thread_->invoke([=, this]() mutable {
        impl_->mediaPopup.setIdleTaskCompleteCallback(std::move(cb));
    });
}

void TaskbarHost::setMediaPopupOpenedCallback(std::function<void()> cb) {
    cb = callbacks_->callback(std::move(cb));
    thread_->invoke([=, this]() mutable {
        impl_->mediaPopup.setPanelOpenedCallback(std::move(cb));
    });
}

void TaskbarHost::setContextMenuCallback(std::function<void(POINT)> cb) {
    cb = callbacks_->callback(std::move(cb));
    thread_->invoke([=, this]() mutable {
        impl_->onContextMenu = std::move(cb);
    });
}

void TaskbarHost::setImmersiveMenuCallback(std::function<void(POINT)> cb) {
    cb = callbacks_->callback(std::move(cb));
    thread_->invoke([=, this]() mutable {
        impl_->onImmersiveMenu = std::move(cb);
    });
}

void TaskbarHost::setDockQuickAppsCallback(std::function<void(POINT)> cb) {
    cb = callbacks_->callback(std::move(cb));
    thread_->invoke([=, this]() mutable {
        impl_->onDockQuickApps = std::move(cb);
    });
}

void TaskbarHost::setAnchorChangedCallback(std::function<void(TaskbarAnchorSpec)> cb) {
    cb = callbacks_->callback(std::move(cb));
    thread_->invoke([=, this]() mutable {
        impl_->onAnchorChanged_ = std::move(cb);
    });
}

void TaskbarHost::setAnchorOptionsChangedCallback(
    std::function<void(std::vector<TaskbarAnchorOption>)> cb) {
    cb = callbacks_->callback(std::move(cb));
    thread_->invoke([=, this]() mutable {
        impl_->onAnchorOptionsChanged_ = std::move(cb);
    });
}

void TaskbarHost::setStatusTextCycleCompletedCallback(std::function<void()> cb) {
    cb = callbacks_->callback(std::move(cb));
    thread_->invoke([=, this]() mutable {
        impl_->onStatusTextCycleCompleted_ = std::move(cb);
    });
}

void TaskbarHost::setPlacementStatusCallback(std::function<void(TaskbarPlacementStatus)> cb) {
    cb = callbacks_->callback(std::move(cb));
    thread_->invoke([=, this]() mutable {
        impl_->onPlacementStatusChanged_ = std::move(cb);
    });
}

void TaskbarHost::setAllowOverlap(bool on) {
    thread_->invoke([=, this]() mutable {
        impl_->allowOverlap_ = on;
        if (impl_->reconcileWindowVisibility() && impl_->isWindowVisible())
            impl_->requestFrameAndFlush();
    });
}

void TaskbarHost::setVisibilitySuppressed(bool on) {
    thread_->invoke([=, this]() mutable {
        impl_->setVisibilitySuppressed(on);
    });
}

TaskbarPlacementStatus TaskbarHost::refreshPlacement() {
    return thread_->invoke([=, this]() mutable {
        return impl_->refreshPlacement();
    });
}

TaskbarPlacementStatus TaskbarHost::placementStatus() const {
    return thread_->invoke([=, this]() mutable {
        return impl_->placementStatus_;
    });
}

bool TaskbarHost::isDisplayed() const {
    return thread_->invoke([=, this]() mutable {
        return impl_ && impl_->isWindowVisible() && impl_->hwnd && IsWindowVisible(impl_->hwnd);
    });
}

std::vector<LyricLine> TaskbarHost::lyrics() const {
    return thread_->invoke([=, this]() mutable {
        return impl_->sourceLines_;
    });
}

void TaskbarHost::show() {
    thread_->invoke([=, this]() mutable {
        impl_->show();
    });
}

void TaskbarHost::hide() {
    thread_->invoke([=, this]() mutable {
        impl_->hide();
    });
}

bool TaskbarHost::isTaskbar() const { return true; }

bool TaskbarHost::isVerticalTaskbar() const {
    return thread_->invoke([=, this]() mutable {
        return impl_ && impl_->isVerticalTaskbar();
    });
}

bool TaskbarHost::isCompactTaskbar() const {
    return thread_->invoke([=, this]() mutable {
        return impl_ && impl_->compactTaskbarHeight_ > 0.0f;
    });
}

std::wstring TaskbarHost::monitorDeviceName() const {
    return thread_->invoke([=, this]() mutable {
        return impl_ ? impl_->targetMonitorDeviceName_ : std::wstring{};
    });
}

int TaskbarHost::currentLine() const {
    return thread_->invoke([=, this]() mutable {
        return impl_->currentLine;
    });
}

std::wstring TaskbarHost::statusText() const {
    return thread_->invoke([=, this]() mutable {
        return impl_->statusText;
    });
}

void TaskbarHost::changeFont(float delta) {
    thread_->invoke([=, this]() mutable {
        impl_->changeFont(delta);
    });
}

LyricFontSizeContext TaskbarHost::fontSizeContext() const {
    return thread_->invoke([=, this]() mutable {
        return impl_->fontSizeContext();
    });
}

void TaskbarHost::setFont(const std::wstring& family, float size, LyricFontStyle style) {
    thread_->invoke([=, this]() mutable {
        impl_->setFont(family, size, style);
    });
}

void TaskbarHost::refreshTheme() {
    thread_->invoke([=, this]() mutable {
        impl_->refreshTheme();
    });
}

void TaskbarHost::setRenderMode(RenderMode mode) {
    thread_->invoke([=, this]() mutable {
        impl_->setRenderMode(mode);
    });
}

void TaskbarHost::setSpectrumVisible(bool on) {
    thread_->invoke([=, this]() mutable {
        impl_->setSpectrumVisible(on);
    });
}

void TaskbarHost::setSpectrumReader(std::function<std::array<float, kSpectrumBands>()> reader) {
    thread_->invoke([this, reader = std::move(reader)]() mutable {
        impl_->spectrumReader_ = std::move(reader);
    });
}

void TaskbarHost::setViewMode(TaskbarViewMode mode) {
    thread_->invoke([=, this]() mutable {
        impl_->setViewMode(mode);
    });
}

void TaskbarHost::setAppBarEdge(AppBarEdge edge) {
    thread_->invoke([=, this]() mutable {
        impl_->setAppBarEdge(edge);
    });
}

bool TaskbarHost::backgroundBlurAvailable() const {
    return thread_->invoke([=, this]() mutable {
        return impl_->backgroundBlurAvailable();
    });
}

void TaskbarHost::setAnchor(TaskbarAnchorSpec anchor) {
    thread_->invoke([=, this]() mutable {
        impl_->setAnchor(anchor);
    });
}

void TaskbarHost::onTaskbarCreated() {
    thread_->invoke([=, this]() mutable {
        impl_->onTaskbarCreated();
    });
}
