#include "ui/settings/sections.h"
#include "settings/display_rules.h"
#include "ui/settings/section_labels.h"
#include "ui/settings/section_metrics.h"
#include "ui/settings/lyric_section.h"
#include "ui/settings/spectrum_section.h"
#include <algorithm>

namespace settings_ui {
Row* SettingsSections::findRow(int id) {
    for (auto& page : rows) {
        for (auto& row : page) {
            if (row.id == id)
                return &row;
        }
    }
    return nullptr;
}

bool SettingsSections::minimalModeActive() const {
    return state.renderMode == kRenderModeMinimal;
}

void SettingsSections::updateFloatingCardRowsEnabled() {
    const auto* controls = findRow(kIdHoverControls);
    const auto* style = findRow(kIdHoverControlStyle);
    const auto* background = findRow(kIdFloatingCardBackground);
    const bool mediaCardEnabled = controls && controls->checked && style && style->selected == 1;
    const bool cardEnabled =
        !minimalModeActive() && (state.idleTextEnabled || state.quickStartEnabled ||
                                state.tickTickEnabled || mediaCardEnabled);
    const bool frosted = cardEnabled && background && background->selected == 1;
    if (auto* row = findRow(kIdFloatingCardTrigger))
        row->enabled = cardEnabled;
    if (auto* row = findRow(kIdFloatingCardBackground))
        row->enabled = cardEnabled;
    if (auto* row = findRow(kIdFloatingCardBackgroundColor))
        row->enabled = frosted;
    if (auto* row = findRow(kIdFloatingCardFollowAlbum))
        row->enabled = frosted;
    if (auto* row = findRow(kIdFloatingCardAutoTextContrast))
        row->enabled = frosted;
}

bool SettingsSections::backgroundWaveActive() const {
    const auto* spectrum = findRow(kIdSpectrum);
    const auto* style = findRow(kIdSpectrumStyle);
    const auto* background = findRow(kIdSpectrumBackground);
    return spectrum && spectrum->checked && style && style->selected == 2 && background &&
           background->checked;
}

bool SettingsSections::spectrumBackgroundAvailable() const {
    const auto* spectrum = findRow(kIdSpectrum);
    const auto* style = findRow(kIdSpectrumStyle);
    return spectrum && spectrum->checked && style && style->selected == 2;
}

void SettingsSections::updateProgressBackgroundRowsEnabled() {
    const bool available = !minimalModeActive() && !backgroundWaveActive();
    if (auto* row = findRow(kIdProgressBackground))
        row->enabled = available;
    if (auto* row = findRow(kIdProgressBackgroundOpacity))
        row->enabled =
            available && findRow(kIdProgressBackground) && findRow(kIdProgressBackground)->checked;
}

void SettingsSections::updateBackgroundAdjustmentPresentation() {
    auto update = [this](int modeId, int sliderId, bool immersive) {
        const auto* modeRow = findRow(modeId);
        const int selected = modeRow ? std::clamp(modeRow->selected, 0, 1) : 0;
        if (auto* slider = findRow(sliderId)) {
            slider->text = backgroundAdjustmentLabel(immersive, selected);
            slider->hint = backgroundAdjustmentHint(state.backgroundBlurSupported);
            slider->showHint = !slider->hint.empty();
            slider->minHeight = kRowTallH;
            slider->height = slider->minHeight;
        }
    };
    update(kIdImmersiveBackgroundAdjustment, kIdImmersiveBackgroundBlur, true);
    update(kIdDockBackgroundAdjustment, kIdDockBackgroundBlur, false);
}

void SettingsSections::updateImmersiveRowsEnabled() {
    auto* mode = findRow(kIdTaskbarViewMode);
    const int selectedMode = mode ? std::clamp(mode->selected, 0, 2) : 0;
    const bool monitorActive = !state.multiMonitorConnected || state.monitorEnabled;
    const bool hasTaskbar = state.editingMonitorHasTaskbar;
    const bool vertical = state.editingMonitorVertical;
    const bool immersiveEnabled = hasTaskbar && !vertical;
    const bool appBarEnabled = monitorActive && selectedMode == 2;
    if (auto* row = findRow(kIdTaskbarMonitor)) {
        row->enabled = state.multiMonitorConnected;
        row->hint = state.multiMonitorConnected
                        ? L"以下展示模式、背景与资源监视配置均仅对当前选中的显示器生效。"
                        : L"当前仅连接一台显示器；连接多台显示器后可分别为每台显示器设置不同模式与效果。";
        row->showHint = true;
    }
    if (auto* row = findRow(kIdMonitorEnabled)) {
        row->checked = monitorActive;
        row->enabled = state.multiMonitorConnected && (!monitorActive || state.monitorDisableAllowed);
        row->hint = state.multiMonitorConnected
                        ? (state.monitorDisableAllowed
                               ? L"关闭后仅隐藏该显示器，并保留它当前的模式与效果设置。"
                               : L"至少保留一台显示器显示；如需全部关闭，请使用右键菜单中的「关闭任务栏歌词」。")
                        : L"单显示器下如需关闭，请使用右键菜单中的「关闭任务栏歌词」。";
        row->showHint = true;
    }
    if (auto* row = findRow(kIdApplyMonitorToAll))
        row->enabled = state.multiMonitorConnected;
    if (mode) {
        mode->enabled = monitorActive;
        mode->optionEnabled.assign(mode->options.size(), monitorActive);
        if (mode->optionEnabled.size() > 0)
            mode->optionEnabled[0] = monitorActive && hasTaskbar;
        if (mode->optionEnabled.size() > 1)
            mode->optionEnabled[1] = monitorActive && immersiveEnabled;
        if (mode->optionEnabled.size() > 2)
            mode->optionEnabled[2] = monitorActive;
        mode->hint = hasTaskbar ? L"" : L"当前显示器未开启系统任务栏，仅可使用 Dock 模式。";
        mode->showHint = !mode->hint.empty();
    }
    if (auto* row = findRow(kIdAppBarEdge))
        row->enabled = appBarEnabled;
    if (auto* row = findRow(kIdImmersiveBackgroundAdjustment)) {
        row->selected = std::clamp(state.immersiveBackgroundAdjustment, 0, 1);
        row->enabled =
            monitorActive && selectedMode == 1 && immersiveEnabled && state.backgroundBlurSupported;
    }
    if (auto* row = findRow(kIdDockBackgroundAdjustment)) {
        row->selected = std::clamp(state.dockBackgroundAdjustment, 0, 1);
        row->enabled = appBarEnabled && state.backgroundBlurSupported;
    }
    if (auto* row = findRow(kIdImmersiveBackgroundBlur)) {
        row->enabled =
            monitorActive && selectedMode == 1 && immersiveEnabled && state.backgroundBlurSupported;
        row->hint = backgroundAdjustmentHint(state.backgroundBlurSupported);
        row->showHint = !row->hint.empty();
        row->minHeight = kRowTallH;
        row->height = row->minHeight;
    }
    if (auto* row = findRow(kIdDockBackgroundBlur)) {
        row->enabled = appBarEnabled && state.backgroundBlurSupported;
        row->hint = backgroundAdjustmentHint(state.backgroundBlurSupported);
        row->showHint = !row->hint.empty();
        row->minHeight = kRowTallH;
        row->height = row->minHeight;
    }
    updateBackgroundAdjustmentPresentation();
    const bool embeddedResources = monitorActive && hasTaskbar && selectedMode == 0;
    const bool resourceExtrasEnabled =
        appBarEnabled || (embeddedResources && state.embeddedResourceMonitor);
    const bool anyResourceMetric = state.dockResourceCpuUsage || state.dockResourceMemoryUsage ||
                                   state.dockResourceDownload || state.dockResourceUpload ||
                                   state.dockResourceGpuUsage || state.dockResourceCpuFrequency;
    if (auto* row = findRow(kIdEmbeddedResourceMonitor)) {
        row->enabled = embeddedResources;
        row->hint =
            (resourceExtrasEnabled && !anyResourceMetric)
                ? (appBarEnabled
                       ? L"Dock 模式下当前未勾选任何监控项，右侧暂不显示资源监视且不进行采样；请在下方“显示监控项”中至少勾选一项。"
                       : L"当前未勾选任何监控项，任务栏暂不显示资源监视且不进行采样；请在下方“显示监控项”中至少勾选一项。")
                : L"仅内嵌模式生效。按下方已勾选的监控项顺序显示，多页可用滚轮切换。";
        row->showHint = true;
    }
    if (auto* row = findRow(kIdDockResourcePlacement))
        row->enabled = embeddedResources && state.embeddedResourceMonitor && anyResourceMetric;
    syncResourceMetricsRow(resourceExtrasEnabled, appBarEnabled);
    if (auto* row = findRow(kIdDockResourceSmallFont))
        row->enabled = resourceExtrasEnabled && anyResourceMetric;
    if (auto* row = findRow(kIdDockResourceHideLabels))
        row->enabled = resourceExtrasEnabled && anyResourceMetric;
    updateMediaControlRowsEnabled();
    if (auto* row = findRow(kIdAlignment))
        row->enabled = state.anyHorizontalEmbeddedHostActive;
}

void SettingsSections::syncResourceMetricsRow(bool enabled, bool appBarEnabled) {
    auto* row = findRow(kIdDockResourceMetrics);
    if (!row)
        return;
    row->enabled = enabled;
    const bool checked[kDockResourceMetricCount] = {
        state.dockResourceCpuUsage,  state.dockResourceMemoryUsage,
        state.dockResourceUpload,    state.dockResourceDownload,
        state.dockResourceGpuUsage,  state.dockResourceCpuFrequency,
    };
    static constexpr const wchar_t* kShortNames[kDockResourceMetricCount] = {
        L"CPU", L"内存", L"上行", L"下行", L"GPU", L"频率",
    };
    std::wstring summary;
    int selectedCount = 0;
    for (int i = 0; i < kDockResourceMetricCount; ++i) {
        if (!checked[i])
            continue;
        if (!summary.empty())
            summary += L"、";
        summary += kShortNames[i];
        ++selectedCount;
    }
    row->controlText =
        selectedCount > 0
            ? (L"已选 " + std::to_wstring(selectedCount) + L"/" +
               std::to_wstring(kDockResourceMetricCount) + L" 项")
            : L"选择监控项…";
    if (selectedCount > 0) {
        row->hint = L"已选 " + std::to_wstring(selectedCount) + L" 项：" + summary;
    } else if (enabled) {
        row->hint =
            appBarEnabled
                ? L"未选择任何监控项，Dock 右侧暂不显示资源监视且不进行采样；请点击右侧按钮至少勾选一项。"
                : L"未选择任何监控项，任务栏暂不显示资源监视且不进行采样；请点击右侧按钮至少勾选一项。";
    } else {
        row->hint = L"未选择任何监控项";
    }
    row->showHint = true;
}

void SettingsSections::updateMediaControlRowsEnabled() {
    const bool anyEmbedded = state.anyEmbeddedHostActive;
    const bool anyHorizontalEmbedded = state.anyHorizontalEmbeddedHostActive;
    const bool minimal = minimalModeActive();
    auto* controls = findRow(kIdHoverControls);
    if (controls)
        controls->enabled = anyEmbedded;
    auto* style = findRow(kIdHoverControlStyle);
    if (style)
        style->enabled = anyEmbedded && controls && controls->checked && !minimal;
    if (auto* inlineMode = findRow(kIdInlineControlDisplayMode)) {
        const bool forcedHoverByMinimal = minimal && state.hoverControlStyle == 1;
        inlineMode->selected = (!anyHorizontalEmbedded || forcedHoverByMinimal)
                                   ? 0
                                   : std::clamp(state.inlineControlDisplayMode, 0, 1);
        inlineMode->enabled = anyHorizontalEmbedded && controls && controls->checked &&
                              state.hoverControlStyle == 0;
    }
}

void SettingsSections::updateIdleAppsRowHeight() {
    if (auto* row = findRow(kIdIdleApps)) {
        const size_t count = std::max<size_t>(1, state.idleApps.size());
        row->minHeight = kIdleAppsBaseH + static_cast<float>(count) * kIdleAppItemH;
        row->height = row->minHeight;
    }
}

void SettingsSections::updateTickTickRows() {
    const bool hasToken = state.tickTickApiTokenConfigured;
    const bool enabled = state.tickTickEnabled && hasToken;
    if (auto* row = findRow(kIdTickTickEnabled)) {
        row->checked = enabled;
        row->enabled = true;
        row->hint = hasToken ? (enabled ? L"已开启自动同步，并在空闲卡片中显示今日任务。"
                                        : L"已关闭自动同步和今日任务入口；API 口令仍保留在本机。")
                             : L"未配置 API 口令，当前处于隐式关闭状态；开启时会先要求填写口令。";
        row->showHint = true;
        row->minHeight = kRowTallH;
        row->height = row->minHeight;
    }
    if (auto* row = findRow(kIdTickTickApiToken)) {
        row->hint = L"在滴答清单网页版的「设置 → 账号与安全 → API 口令」中创建并复制；"
                    L"程序只将口令保存到 Windows 凭据管理器。";
        row->controlText = state.tickTickApiTokenConfigured ? L"已配置" : L"未配置";
        row->showHint = true;
        row->minHeight = kRowTallH;
        row->height = row->minHeight;
        row->enabled = !hasToken || enabled;
    }
    if (auto* row = findRow(kIdTickTickConnect)) {
        row->controlText = state.tickTickConnecting  ? L"连接中…"
                           : state.tickTickConnected ? L"重新连接"
                                                     : L"连接…";
        row->hint = state.tickTickStatus.empty() ? (state.tickTickApiTokenConfigured
                                                        ? L"连接后，卡片会显示今天的未完成任务。"
                                                        : L"请先填写 API 口令。")
                                                 : state.tickTickStatus;
        row->showHint = true;
        row->minHeight = kRowTallH;
        row->height = row->minHeight;
        row->enabled = enabled && !state.tickTickConnecting && !state.tickTickSyncing;
    }
    if (auto* row = findRow(kIdTickTickRefresh)) {
        row->enabled = enabled && state.tickTickConnected && !state.tickTickSyncing;
        row->controlText = state.tickTickSyncing ? L"同步中…" : L"立即同步";
    }
    if (auto* row = findRow(kIdTickTickDisconnect)) {
        row->enabled = hasToken && !state.tickTickConnecting && !state.tickTickSyncing;
    }
}

void SettingsSections::updateIdleQuoteSourceRow() {
    if (auto* row = findRow(kIdIdleQuoteSource)) {
        row->hint = idleQuoteSourceHint(state.idleQuoteSource);
        row->showHint = true;
        row->minHeight = kRowTallH;
        row->height = kRowTallH;
    }
}

int SettingsSections::idleTextMode() const {
    return std::clamp(state.idleTextMode, 0, 1);
}

void SettingsSections::updateIdleRowsEnabled() {
    const bool quoteEnabled = state.idleTextEnabled && idleTextMode() == 0;
    if (auto* row = findRow(kIdIdleQuote)) {
        row->selected = idleTextMode();
        row->enabled = state.idleTextEnabled;
    }
    if (auto* row = findRow(kIdIdleQuoteSource))
        row->enabled = quoteEnabled;
    if (auto* row = findRow(kIdIdleQuoteRefreshInterval))
        row->enabled = quoteEnabled;
    if (auto* row = findRow(kIdIdleQuoteAlignment))
        row->enabled = quoteEnabled && !state.verticalTaskbar;
    if (auto* row = findRow(kIdIdleCustomWelcome))
        row->enabled = state.idleTextEnabled && idleTextMode() == 1;
    if (auto* row = findRow(kIdIdleQuoteBackground))
        row->enabled = !minimalModeActive();
    if (auto* row = findRow(kIdIdleQuoteBackgroundScope))
        row->enabled = !minimalModeActive() && state.idleQuoteBackground != 0;
    if (auto* row = findRow(kIdIdleApps))
        row->enabled = !minimalModeActive() && state.quickStartEnabled;
    if (auto* row = findRow(kIdQuickStartEnabled))
        row->enabled = !minimalModeActive();
}

void SettingsSections::updateCompactLyricRows() {
    settings_ui::updateCompactLyricRows(rows[kLyricsPage], state);
}

void SettingsSections::syncIdleRows(const SettingsState& s) {
    const bool vertical = s.verticalTaskbar;
    if (auto* row = findRow(kIdIdleTextEnabled))
        row->checked = s.idleTextEnabled;
    if (auto* row = findRow(kIdQuickStartEnabled))
        row->checked = s.quickStartEnabled;
    if (auto* row = findRow(kIdIdleQuote))
        row->selected = idleTextMode();
    if (auto* row = findRow(kIdIdleQuoteSource))
        row->selected = std::clamp(s.idleQuoteSource, 0, 2);
    if (auto* row = findRow(kIdIdleQuoteRefreshInterval))
        row->selected = std::clamp(s.idleQuoteRefreshInterval, 0, 2);
    if (auto* row = findRow(kIdIdleQuoteAlignment))
        row->selected = vertical ? 0 : std::clamp(s.idleQuoteAlignment, 0, 2);
    if (auto* row = findRow(kIdIdleQuoteBackground))
        row->selected = std::clamp(s.idleQuoteBackground, 0, 4);
    if (auto* row = findRow(kIdIdleQuoteBackgroundScope))
        row->selected = std::clamp(s.idleQuoteBackgroundScope, 0, 3);
    if (auto* row = findRow(kIdIdleApps))
        row->checked = s.idleAppNamesVisible;
    if (auto* row = findRow(kIdTickTickEnabled))
        row->checked = s.tickTickEnabled;
    updateIdleQuoteSourceRow();
    updateIdleAppsRowHeight();
    updateTickTickRows();
}

void SettingsSections::syncDisplayRows(const SettingsState& s) {
    const bool minimal = s.renderMode == kRenderModeMinimal;
    if (auto* row = findRow(kIdSongInfo))
        row->checked = s.songInfoVisible;
    if (auto* row = findRow(kIdAlbumCover))
        row->checked = s.albumCoverVisible;
    if (auto* row = findRow(kIdPlatformIcon)) {
        row->checked = s.platformIconVisible;
        row->enabled = s.albumCoverVisible;
    }
    if (auto* row = findRow(kIdCoverEffect)) {
        row->selected = minimal ? 0 : (s.coverEffectVinyl ? 1 : 0);
        row->enabled = s.albumCoverVisible && !minimal;
    }
    if (auto* row = findRow(kIdTaskbarContextMenu))
        row->checked = s.taskbarContextMenu;
}

void SettingsSections::syncTaskbarModeRows(const SettingsState& s) {
    const bool vertical = s.editingMonitorVertical;
    const bool monitorActive = !s.multiMonitorConnected || s.monitorEnabled;
    if (auto* row = findRow(kIdTaskbarViewMode))
        row->selected = std::clamp(s.taskbarViewMode, 0, 2);
    if (auto* row = findRow(kIdTaskbarMonitor)) {
        row->controlText = s.taskbarMonitorLabel;
        row->enabled = s.multiMonitorConnected;
    }
    if (auto* row = findRow(kIdMonitorEnabled))
        row->checked = monitorActive;
    if (auto* row = findRow(kIdAppBarEdge))
        row->selected = std::clamp(s.appBarEdge, 0, 1);
    if (auto* row = findRow(kIdImmersiveBackgroundAdjustment))
        row->selected = std::clamp(s.immersiveBackgroundAdjustment, 0, 1);
    if (auto* row = findRow(kIdDockBackgroundAdjustment))
        row->selected = std::clamp(s.dockBackgroundAdjustment, 0, 1);
    if (auto* row = findRow(kIdImmersiveBackgroundBlur))
        row->value = std::clamp(s.immersiveBackgroundBlur, 0, 100);
    if (auto* row = findRow(kIdDockBackgroundBlur))
        row->value = std::clamp(s.dockBackgroundBlur, 0, 100);
    if (auto* row = findRow(kIdEmbeddedResourceMonitor))
        row->checked = s.embeddedResourceMonitor;
    if (auto* row = findRow(kIdDockResourcePlacement)) {
        row->options = vertical ? std::vector<std::wstring>{L"顶部", L"底部"}
                                : std::vector<std::wstring>{L"左侧", L"右侧"};
        row->selected = std::clamp(s.dockResourcePlacement, 0, 1);
    }
    if (auto* row = findRow(kIdDockResourceSmallFont))
        row->checked = s.dockResourceSmallFont;
    if (auto* row = findRow(kIdDockResourceHideLabels))
        row->checked = s.dockResourceHideLabels;
    updateImmersiveRowsEnabled();
    settings_ui::updateSpectrumRows(rows[kSpectrumPage], s);
}

void SettingsSections::syncBackgroundRows(const SettingsState& s) {
    const bool minimal = s.renderMode == kRenderModeMinimal;
    if (auto* row = findRow(kIdTaskbarBackground)) {
        row->selected = minimal ? 0 : std::clamp(s.taskbarBackground, 0, 2);
        row->enabled = !minimal;
    }
    if (auto* row = findRow(kIdCoverBackgroundOpacity)) {
        row->value = std::clamp(s.coverBackgroundOpacity, 0, 100);
        row->enabled = !minimal && s.taskbarBackground == 1;
    }
    if (auto* row = findRow(kIdProgressBackground))
        row->checked = minimal ? false : s.progressBackground;
    if (auto* row = findRow(kIdProgressBackgroundOpacity))
        row->value = std::clamp(s.progressBackgroundOpacity, 0, 100);
    updateProgressBackgroundRowsEnabled();
}

void SettingsSections::syncMediaControlRows(const SettingsState& s) {
    const bool minimal = s.renderMode == kRenderModeMinimal;
    if (auto* row = findRow(kIdHoverControls))
        row->checked = s.hoverControls;
    if (auto* row = findRow(kIdHoverControlStyle))
        row->selected = minimal ? 0 : s.hoverControlStyle;
    if (auto* row = findRow(kIdInlineControlDisplayMode))
        row->selected = std::clamp(s.inlineControlDisplayMode, 0, 1);
    updateMediaControlRowsEnabled();
}

void SettingsSections::syncFloatingCardRows(const SettingsState& s) {
    if (auto* row = findRow(kIdFloatingCardTrigger))
        row->selected = std::clamp(s.floatingCardTrigger, 0, 1);
    if (auto* row = findRow(kIdFloatingCardBackground))
        row->selected = std::clamp(s.floatingCardBackground, 0, 1);
    if (auto* row = findRow(kIdFloatingCardBackgroundColor))
        row->controlText = colorText(s.floatingCardBackgroundColor);
    if (auto* row = findRow(kIdFloatingCardFollowAlbum))
        row->checked = s.floatingCardFollowAlbum;
    if (auto* row = findRow(kIdFloatingCardAutoTextContrast))
        row->checked = s.floatingCardAutoTextContrast;
    updateFloatingCardRowsEnabled();
    updateIdleRowsEnabled();
}

void SettingsSections::syncSongToastRows(const SettingsState& s) {
    const bool minimal = s.renderMode == kRenderModeMinimal;
    if (auto* row = findRow(kIdSongTransitionEnabled))
        row->checked = s.songTransitionEnabled;
    if (auto* row = findRow(kIdSongToast)) {
        row->checked = minimal ? false : s.songToastEnabled;
        row->enabled = !minimal;
    }
    if (auto* row = findRow(kIdSongToastDuration)) {
        row->value = std::clamp(s.songToastDurationSec, 1, 10);
        row->enabled = !minimal && s.songToastEnabled;
    }
    if (auto* row = findRow(kIdSongToastSkipFullscreen)) {
        row->checked = s.songToastSkipFullscreen;
        row->enabled = !minimal && s.songToastEnabled;
    }
    if (auto* row = findRow(kIdSongToastPosition)) {
        row->selected = std::clamp(s.songToastPosition, 0, 1);
        row->enabled = !minimal && s.songToastEnabled;
    }
    if (auto* row = findRow(kIdSongToastMonitor)) {
        row->selected = s.songToastFollowMouseMonitor ? 1 : 0;
        row->enabled = !minimal && s.songToastEnabled;
    }
    if (auto* row = findRow(kIdSongToastPreview))
        row->enabled = !minimal && s.songToastEnabled && s.songToastPreviewAvailable;
}

void SettingsSections::syncThemeFontRows(const SettingsState& s) {
    if (auto* row = findRow(kIdUiFontSource))
        row->selected = static_cast<int>(s.uiFontSource);
    if (auto* row = findRow(kIdPickUiFont)) {
        row->valueText = s.uiFontDesc;
        row->enabled = s.uiFontSource == UiFontSource::Independent;
    }
    if (auto* row = findRow(kIdTaskbarTheme))
        row->selected = themeModeIndex(s.taskbarThemeMode);
    if (auto* row = findRow(kIdWindowTheme))
        row->selected = themeModeIndex(s.windowThemeMode);
    if (auto* row = findRow(kIdRenderMode))
        row->selected = std::clamp(s.renderMode, 0, kRenderModeMinimal);
    if (auto* row = findRow(kIdFrameRate)) {
        row->selected = std::clamp(s.frameRate, 0, 2);
        row->enabled = s.renderMode != static_cast<int>(RenderMode::Low) &&
                       s.renderMode != static_cast<int>(RenderMode::Stopped);
    }
    if (auto* row = findRow(kIdFullscreenRenderModeEnabled))
        row->checked = s.fullscreenRenderModeEnabled;
    if (auto* row = findRow(kIdFullscreenRenderModeMonitor)) {
        row->controlText = s.fullscreenRenderModeMonitorLabel;
        row->enabled = s.fullscreenRenderModeEnabled;
    }
    if (auto* row = findRow(kIdFullscreenRenderMode)) {
        row->selected = std::clamp(s.fullscreenRenderMode, 0, 2);
        row->enabled = s.fullscreenRenderModeEnabled;
    }
    if (auto* row = findRow(kIdPickFont)) {
        row->valueText = s.fontDesc;
        row->hint = kFontSettingNotice;
        row->showHint = true;
        row->minHeight = kRowTallH;
        row->height = row->minHeight;
    }
}

void SettingsSections::syncPlayerRows(const SettingsState& s) {
    const int mode = std::clamp(s.genericSmtcFilterMode, 0, 2);
    if (auto* row = findRow(kIdGenericSmtcEnabled))
        row->checked = s.genericSmtcEnabled;
    if (auto* row = findRow(kIdGenericSmtcFilterMode)) {
        row->selected = mode;
        row->enabled = s.genericSmtcEnabled;
    }
    if (auto* row = findRow(kIdGenericSmtcApps)) {
        row->text = mode == 1   ? L"白名单应用"
                    : mode == 2 ? L"黑名单应用"
                                : L"规则应用列表";
        row->hint =
            mode == 1
                ? L"仅识别列表中的通用 SMTC 应用。"
            : mode == 2
                ? L"忽略列表中的通用 SMTC 应用，其余支持的软件正常识别。"
                : L"切换为“仅白名单”或“排除黑名单”后可管理需要指定识别或排除的软件。";
        row->showHint = true;
        const size_t visibleCount = mode == 0 ? 0 : s.genericSmtcRules.size();
        const size_t slots = std::max<size_t>(1, visibleCount);
        row->minHeight = kIdleAppsBaseH + static_cast<float>(slots) * kIdleAppItemH;
        row->height = row->minHeight;
        row->enabled = s.genericSmtcEnabled && mode != 0;
    }
}

void SettingsSections::update(const SettingsState& s) {
    state = s;
    syncThemeFontRows(s);
    syncIdleRows(s);
    syncDisplayRows(s);
    syncTaskbarModeRows(s);
    settings_ui::updateSpectrumRows(rows[kSpectrumPage], s);
    syncBackgroundRows(s);
    syncMediaControlRows(s);
    syncPlayerRows(s);
    syncFloatingCardRows(s);
    syncSongToastRows(s);
    settings_ui::updateLyricRows(rows[kLyricsPage], s);
    updateImmersiveRowsEnabled();
}

const Row* SettingsSections::findRow(int id) const {
    for (const auto& page : rows) {
        for (const auto& row : page) {
            if (row.id == id)
                return &row;
        }
    }
    return nullptr;
}

} // namespace settings_ui
