#include "ui/settings/sections.h"
#include "ui/settings/section_labels.h"
#include "ui/settings/section_metrics.h"
#include "ui/settings/lyric_section.h"
#include "ui/settings/spectrum_section.h"
#include <algorithm>

namespace settings_ui {
bool SettingsSections::onIdleCommand(int id, Row* row, int option) {
    switch (id) {
    case kIdIdleTextEnabled:
        row->checked = !row->checked;
        state.idleTextEnabled = row->checked;
        updateIdleRowsEnabled();
        updateFloatingCardRowsEnabled();
        if (actions.onIdleTextEnabled)
            actions.onIdleTextEnabled(row->checked);
        break;
    case kIdQuickStartEnabled:
        row->checked = !row->checked;
        state.quickStartEnabled = row->checked;
        updateIdleRowsEnabled();
        updateFloatingCardRowsEnabled();
        if (actions.onQuickStartEnabled)
            actions.onQuickStartEnabled(row->checked);
        break;
    case kIdIdleQuote:
        state.idleTextMode = row->selected;
        updateIdleRowsEnabled();
        if (actions.onIdleTextMode)
            actions.onIdleTextMode(row->selected);
        break;
    case kIdIdleCustomWelcome:
        if (actions.onEditIdleWelcome)
            actions.onEditIdleWelcome();
        break;
    case kIdTickTickEnabled: {
        const bool requested = !row->checked;
        state.tickTickEnabled = requested && state.tickTickApiTokenConfigured;
        row->checked = state.tickTickEnabled;
        updateTickTickRows();
        updateFloatingCardRowsEnabled();
        if (actions.onTickTickEnabled)
            actions.onTickTickEnabled(requested);
        break;
    }
    case kIdTickTickApiToken:
        if (actions.onEditTickTickApiToken)
            actions.onEditTickTickApiToken();
        break;
    case kIdTickTickConnect:
        if (actions.onTickTickConnect)
            actions.onTickTickConnect();
        break;
    case kIdTickTickRefresh:
        if (actions.onTickTickRefresh)
            actions.onTickTickRefresh();
        break;
    case kIdTickTickDisconnect:
        if (actions.onTickTickDisconnect)
            actions.onTickTickDisconnect();
        break;
    case kIdIdleQuoteSource:
        state.idleQuoteSource = row->selected;
        updateIdleQuoteSourceRow();
        if (actions.onIdleQuoteSource)
            actions.onIdleQuoteSource(row->selected);
        break;
    case kIdIdleQuoteRefreshInterval:
        state.idleQuoteRefreshInterval = row->selected;
        if (actions.onIdleQuoteRefreshInterval)
            actions.onIdleQuoteRefreshInterval(row->selected);
        break;
    case kIdIdleQuoteAlignment:
        state.idleQuoteAlignment = row->selected;
        if (actions.onIdleQuoteAlignment)
            actions.onIdleQuoteAlignment(row->selected);
        break;
    case kIdIdleQuoteBackground:
        state.idleQuoteBackground = row->selected;
        if (actions.onIdleQuoteBackground)
            actions.onIdleQuoteBackground(row->selected);
        updateIdleRowsEnabled();
        break;
    case kIdIdleQuoteBackgroundScope:
        state.idleQuoteBackgroundScope = row->selected;
        if (actions.onIdleQuoteBackgroundScope)
            actions.onIdleQuoteBackgroundScope(row->selected);
        break;
    case kIdIdleApps:
        if (isIdleAppDragOption(option)) {
            // 应用条目的普通点击只用于启动拖动，不触发其他操作。
        } else if (option == kIdleAppNamesOption) {
            row->checked = !row->checked;
            state.idleAppNamesVisible = row->checked;
            if (actions.onIdleAppNamesVisible)
                actions.onIdleAppNamesVisible(row->checked);
        } else if (option >= kIdleAppEditOptionBase) {
            const int index = option - kIdleAppEditOptionBase;
            if (index >= 0 && static_cast<size_t>(index) < state.idleApps.size() &&
                actions.onEditIdleApp)
                actions.onEditIdleApp(index);
        } else if (option >= 0) {
            if (actions.onRemoveIdleApp)
                actions.onRemoveIdleApp(option);
        } else if (state.idleApps.size() < kMaxIdleApps && actions.onAddIdleApp) {
            actions.onAddIdleApp();
        }
        break;
    default:
        return false;
    }
    return true;
}

bool SettingsSections::onSpectrumCommand(int id, Row* row) {
    switch (id) {
    case kIdSpectrum:
        row->checked = !row->checked;
        for (int i = 0; i < kSpectrumEffectCount; ++i) {
            if (auto* effect = findRow(kIdSpectrumEffectFirst + i))
                effect->enabled = row->checked;
        }
        if (auto* colorMode = findRow(kIdSpectrumColorMode))
            colorMode->enabled = row->checked;
        if (auto* color = findRow(kIdSpectrumColor))
            color->enabled = row->checked && findRow(kIdSpectrumColorMode) &&
                             findRow(kIdSpectrumColorMode)->selected == 2;
        if (auto* style = findRow(kIdSpectrumStyle))
            style->enabled = row->checked;
        if (auto* gradient = findRow(kIdSpectrumGradient))
            gradient->enabled = row->checked;
        if (auto* background = findRow(kIdSpectrumBackground))
            background->enabled = spectrumBackgroundAvailable();
        if (auto* opacity = findRow(kIdSpectrumOpacity))
            opacity->enabled = spectrumBackgroundAvailable() && findRow(kIdSpectrumBackground) &&
                               findRow(kIdSpectrumBackground)->checked;
        if (actions.onSpectrum)
            actions.onSpectrum(row->checked);
        updateProgressBackgroundRowsEnabled();
        break;
    case kIdSpectrumColorMode:
        row->selected = std::clamp(row->selected, 0, 2);
        state.spectrumCustomColor = row->selected == 2;
        state.spectrumFollowAlbum = row->selected == 1;
        if (auto* color = findRow(kIdSpectrumColor))
            color->enabled =
                state.spectrumCustomColor && findRow(kIdSpectrum) && findRow(kIdSpectrum)->checked;
        if (actions.onSpectrumColorMode)
            actions.onSpectrumColorMode(row->selected);
        break;
    case kIdSpectrumStyle:
        if (auto* background = findRow(kIdSpectrumBackground))
            background->enabled = spectrumBackgroundAvailable();
        if (auto* opacity = findRow(kIdSpectrumOpacity))
            opacity->enabled = spectrumBackgroundAvailable() && findRow(kIdSpectrumBackground) &&
                               findRow(kIdSpectrumBackground)->checked;
        if (actions.onSpectrumStyle)
            actions.onSpectrumStyle(row->selected);
        updateProgressBackgroundRowsEnabled();
        break;
    case kIdSpectrumEffectFirst:
    case kIdSpectrumEffectFirst + 1:
    case kIdSpectrumEffectFirst + 2:
    case kIdSpectrumEffectFirst + 3:
        row->checked = !row->checked;
        if (actions.onSpectrumEffect)
            actions.onSpectrumEffect(row->id - kIdSpectrumEffectFirst, row->checked);
        break;
    case kIdSpectrumGradient:
        row->checked = !row->checked;
        if (actions.onSpectrumGradient)
            actions.onSpectrumGradient(row->checked);
        break;
    case kIdSpectrumBackground:
        row->checked = !row->checked;
        if (auto* opacity = findRow(kIdSpectrumOpacity))
            opacity->enabled = row->checked && spectrumBackgroundAvailable();
        if (actions.onSpectrumBackground)
            actions.onSpectrumBackground(row->checked);
        updateProgressBackgroundRowsEnabled();
        break;
    case kIdSpectrumOpacity:
        if (actions.onSpectrumOpacity)
            actions.onSpectrumOpacity(row->value);
        break;
    default:
        return false;
    }
    return true;
}

bool SettingsSections::onFloatingCardCommand(int id, Row* row) {
    switch (id) {
    case kIdFloatingCardTrigger:
        state.floatingCardTrigger = row->selected;
        if (actions.onFloatingCardTrigger)
            actions.onFloatingCardTrigger(row->selected);
        break;
    case kIdFloatingCardBackground:
        state.floatingCardBackground = row->selected;
        if (actions.onFloatingCardBackground)
            actions.onFloatingCardBackground(row->selected);
        updateFloatingCardRowsEnabled();
        break;
    case kIdFloatingCardFollowAlbum:
        row->checked = !row->checked;
        state.floatingCardFollowAlbum = row->checked;
        if (actions.onFloatingCardFollowAlbum)
            actions.onFloatingCardFollowAlbum(row->checked);
        break;
    case kIdFloatingCardAutoTextContrast:
        row->checked = !row->checked;
        state.floatingCardAutoTextContrast = row->checked;
        if (actions.onFloatingCardAutoTextContrast)
            actions.onFloatingCardAutoTextContrast(row->checked);
        break;
    default:
        return false;
    }
    return true;
}

bool SettingsSections::onSongToastCommand(int id, Row* row) {
    switch (id) {
    case kIdSongToast:
        row->checked = !row->checked;
        state.songToastEnabled = row->checked;
        if (auto* monitor = findRow(kIdSongToastMonitor))
            monitor->enabled = row->checked;
        if (auto* preview = findRow(kIdSongToastPreview))
            preview->enabled = row->checked && state.songToastPreviewAvailable;
        if (auto* duration = findRow(kIdSongToastDuration))
            duration->enabled = row->checked;
        if (auto* skipFullscreen = findRow(kIdSongToastSkipFullscreen))
            skipFullscreen->enabled = row->checked;
        if (auto* position = findRow(kIdSongToastPosition))
            position->enabled = row->checked;
        if (actions.onSongToastEnabled)
            actions.onSongToastEnabled(row->checked);
        break;
    case kIdSongToastDuration:
        if (actions.onSongToastDuration)
            actions.onSongToastDuration(row->value);
        break;
    case kIdSongToastSkipFullscreen:
        row->checked = !row->checked;
        if (actions.onSongToastSkipFullscreen)
            actions.onSongToastSkipFullscreen(row->checked);
        break;
    case kIdSongToastPosition:
        if (actions.onSongToastPosition)
            actions.onSongToastPosition(row->selected);
        break;
    case kIdSongToastMonitor:
        state.songToastFollowMouseMonitor = row->selected == 1;
        if (actions.onSongToastFollowMouseMonitor)
            actions.onSongToastFollowMouseMonitor(state.songToastFollowMouseMonitor);
        break;
    case kIdSongToastPreview:
        if (actions.onPreviewSongToast)
            actions.onPreviewSongToast();
        break;
    default:
        return false;
    }
    return true;
}

void SettingsSections::onCommand(int id, int option) {
    if (id >= kIdOpenDocs && id <= kIdOpenAbout) {
        const auto& action = id == kIdOpenDocs       ? actions.onOpenDocs
                             : id == kIdOpenFeedback ? actions.onOpenFeedback
                                                     : actions.onOpenAbout;
        if (action)
            action();
        return;
    }
    if (id == kIdNav)
        return;
    if (id == kIdIdleAppNames) {
        Row* appRow = findRow(kIdIdleApps);
        if (!appRow || !appRow->enabled)
            return;
        appRow->checked = !appRow->checked;
        state.idleAppNamesVisible = appRow->checked;
        if (actions.onIdleAppNamesVisible)
            actions.onIdleAppNamesVisible(appRow->checked);
        return;
    }
    Row* row = findRow(id);
    if (!row || !row->enabled)
        return;
    if (settings_ui::handleLyricCommand(id, *row, rows[kLyricsPage], state, actions, option)) {
        return;
    }
    if (onIdleCommand(id, row, option) || onSpectrumCommand(id, row) ||
        onFloatingCardCommand(id, row) || onSongToastCommand(id, row)) {
        return;
    }
    switch (id) {
    case kIdSongInfo:
        row->checked = !row->checked;
        if (actions.onSongInfoVisible)
            actions.onSongInfoVisible(row->checked);
        break;
    case kIdAlbumCover:
        row->checked = !row->checked;
        if (actions.onAlbumCoverVisible)
            actions.onAlbumCoverVisible(row->checked);
        if (auto* effect = findRow(kIdCoverEffect))
            effect->enabled = row->checked && !minimalModeActive();
        if (auto* platformIcon = findRow(kIdPlatformIcon))
            platformIcon->enabled = row->checked;
        break;
    case kIdPlatformIcon:
        row->checked = !row->checked;
        if (actions.onPlatformIconVisible)
            actions.onPlatformIconVisible(row->checked);
        break;
    case kIdGenericSmtcEnabled:
        row->checked = !row->checked;
        state.genericSmtcEnabled = row->checked;
        syncPlayerRows(state);
        settings_ui::updateLyricRows(rows[kLyricsPage], state);
        if (actions.onGenericSmtcEnabled)
            actions.onGenericSmtcEnabled(row->checked);
        break;
    case kIdGenericSmtcFilterMode:
        state.genericSmtcFilterMode = std::clamp(row->selected, 0, 2);
        syncPlayerRows(state);
        if (actions.onGenericSmtcFilterMode)
            actions.onGenericSmtcFilterMode(state.genericSmtcFilterMode);
        break;
    case kIdGenericSmtcApps:
        if (option >= 0) {
            if (static_cast<size_t>(option) < state.genericSmtcRules.size() &&
                actions.onRemoveGenericSmtcApp)
                actions.onRemoveGenericSmtcApp(option);
        } else if (state.genericSmtcRules.size() < kMaxGenericSmtcRules &&
                   actions.onAddGenericSmtcApp) {
            actions.onAddGenericSmtcApp();
        }
        break;
    case kIdCoverEffect:
        if (actions.onCoverEffectVinyl)
            actions.onCoverEffectVinyl(row->selected == 1);
        break;
    case kIdTaskbarContextMenu:
        row->checked = !row->checked;
        if (actions.onTaskbarContextMenu)
            actions.onTaskbarContextMenu(row->checked);
        break;
    case kIdTaskbarViewMode:
        if ((row->selected == 0 && !state.editingMonitorHasTaskbar) ||
            (row->selected == 1 && (!state.editingMonitorHasTaskbar || state.editingMonitorVertical))) {
            row->selected = std::clamp(state.taskbarViewMode, 0, 2);
            break;
        }
        state.taskbarViewMode = std::clamp(row->selected, 0, 2);
        updateImmersiveRowsEnabled();
        updateIdleRowsEnabled();
        if (actions.onTaskbarViewMode)
            actions.onTaskbarViewMode(state.taskbarViewMode);
        break;
    case kIdSongTransitionEnabled:
        row->checked = !row->checked;
        state.songTransitionEnabled = row->checked;
        if (actions.onSongTransitionEnabled)
            actions.onSongTransitionEnabled(row->checked);
        break;
    case kIdTaskbarMonitor:
        if (state.multiMonitorConnected && actions.onChooseTaskbarMonitor)
            actions.onChooseTaskbarMonitor();
        break;
    case kIdMonitorEnabled: {
        if (!state.multiMonitorConnected)
            break;
        const bool next = !row->checked;
        if (!next && !state.monitorDisableAllowed)
            break;
        row->checked = next;
        state.monitorEnabled = next;
        updateImmersiveRowsEnabled();
        if (actions.onMonitorEnabled)
            actions.onMonitorEnabled(next);
        break;
    }
    case kIdApplyMonitorToAll:
        if (state.multiMonitorConnected && actions.onApplyMonitorSettingsToAll)
            actions.onApplyMonitorSettingsToAll();
        break;
    case kIdAppBarEdge:
        state.appBarEdge = std::clamp(row->selected, 0, 1);
        if (actions.onAppBarEdge)
            actions.onAppBarEdge(state.appBarEdge);
        break;
    case kIdImmersiveBackgroundAdjustment:
        state.immersiveBackgroundAdjustment = std::clamp(row->selected, 0, 1);
        updateBackgroundAdjustmentPresentation();
        if (actions.onImmersiveBackgroundAdjustment)
            actions.onImmersiveBackgroundAdjustment(state.immersiveBackgroundAdjustment);
        break;
    case kIdDockBackgroundAdjustment:
        state.dockBackgroundAdjustment = std::clamp(row->selected, 0, 1);
        updateBackgroundAdjustmentPresentation();
        if (actions.onDockBackgroundAdjustment)
            actions.onDockBackgroundAdjustment(state.dockBackgroundAdjustment);
        break;
    case kIdImmersiveBackgroundBlur:
        if (actions.onImmersiveBackgroundBlur)
            actions.onImmersiveBackgroundBlur(row->value);
        break;
    case kIdDockBackgroundBlur:
        if (actions.onDockBackgroundBlur)
            actions.onDockBackgroundBlur(row->value);
        break;
    case kIdEmbeddedResourceMonitor:
        row->checked = !row->checked;
        state.embeddedResourceMonitor = row->checked;
        updateImmersiveRowsEnabled();
        updateIdleRowsEnabled();
        if (actions.onEmbeddedResourceMonitor)
            actions.onEmbeddedResourceMonitor(row->checked);
        break;
    case kIdDockResourceMetrics:
        if (option == kDockResourceMetricCpu) {
            state.dockResourceCpuUsage = !state.dockResourceCpuUsage;
            updateImmersiveRowsEnabled();
            if (actions.onDockResourceCpuUsage)
                actions.onDockResourceCpuUsage(state.dockResourceCpuUsage);
        } else if (option == kDockResourceMetricMemory) {
            state.dockResourceMemoryUsage = !state.dockResourceMemoryUsage;
            updateImmersiveRowsEnabled();
            if (actions.onDockResourceMemoryUsage)
                actions.onDockResourceMemoryUsage(state.dockResourceMemoryUsage);
        } else if (option == kDockResourceMetricUpload) {
            state.dockResourceUpload = !state.dockResourceUpload;
            updateImmersiveRowsEnabled();
            if (actions.onDockResourceUpload)
                actions.onDockResourceUpload(state.dockResourceUpload);
        } else if (option == kDockResourceMetricDownload) {
            state.dockResourceDownload = !state.dockResourceDownload;
            updateImmersiveRowsEnabled();
            if (actions.onDockResourceDownload)
                actions.onDockResourceDownload(state.dockResourceDownload);
        } else if (option == kDockResourceMetricGpu) {
            state.dockResourceGpuUsage = !state.dockResourceGpuUsage;
            updateImmersiveRowsEnabled();
            if (actions.onDockResourceGpuUsage)
                actions.onDockResourceGpuUsage(state.dockResourceGpuUsage);
        } else if (option == kDockResourceMetricCpuFrequency) {
            state.dockResourceCpuFrequency = !state.dockResourceCpuFrequency;
            updateImmersiveRowsEnabled();
            if (actions.onDockResourceCpuFrequency)
                actions.onDockResourceCpuFrequency(state.dockResourceCpuFrequency);
        }
        break;
    case kIdDockResourcePlacement:
        state.dockResourcePlacement = std::clamp(row->selected, 0, 1);
        if (actions.onDockResourcePlacement)
            actions.onDockResourcePlacement(state.dockResourcePlacement);
        break;
    case kIdDockResourceSmallFont:
        row->checked = !row->checked;
        state.dockResourceSmallFont = row->checked;
        if (actions.onDockResourceSmallFont)
            actions.onDockResourceSmallFont(row->checked);
        break;
    case kIdDockResourceHideLabels:
        row->checked = !row->checked;
        state.dockResourceHideLabels = row->checked;
        if (actions.onDockResourceHideLabels)
            actions.onDockResourceHideLabels(row->checked);
        break;
    case kIdTaskbarBackground:
        if (auto* opacity = findRow(kIdCoverBackgroundOpacity))
            opacity->enabled = row->selected == 1;
        if (actions.onTaskbarBackground)
            actions.onTaskbarBackground(row->selected);
        break;
    case kIdCoverBackgroundOpacity:
        if (actions.onCoverBackgroundOpacity)
            actions.onCoverBackgroundOpacity(row->value);
        break;
    case kIdProgressBackground:
        row->checked = !row->checked;
        updateProgressBackgroundRowsEnabled();
        if (actions.onProgressBackground)
            actions.onProgressBackground(row->checked);
        break;
    case kIdProgressBackgroundOpacity:
        if (actions.onProgressBackgroundOpacity)
            actions.onProgressBackgroundOpacity(row->value);
        break;
    case kIdHoverControls:
        row->checked = !row->checked;
        state.hoverControls = row->checked;
        if (actions.onHoverControls)
            actions.onHoverControls(row->checked);
        updateMediaControlRowsEnabled();
        updateFloatingCardRowsEnabled();
        break;
    case kIdHoverControlStyle:
        state.hoverControlStyle = std::clamp(row->selected, 0, 1);
        if (actions.onHoverControlStyle)
            actions.onHoverControlStyle(row->selected);
        updateMediaControlRowsEnabled();
        updateFloatingCardRowsEnabled();
        updateIdleRowsEnabled();
        break;
    case kIdInlineControlDisplayMode:
        state.inlineControlDisplayMode = std::clamp(row->selected, 0, 1);
        if (actions.onInlineControlDisplayMode)
            actions.onInlineControlDisplayMode(state.inlineControlDisplayMode);
        break;
    case kIdTaskbarTheme:
        if (actions.onTaskbarTheme)
            actions.onTaskbarTheme(themeModeFromIndex(row->selected));
        break;
    case kIdWindowTheme:
        if (actions.onWindowTheme)
            actions.onWindowTheme(themeModeFromIndex(row->selected));
        break;
    case kIdRenderMode:
        state.renderMode = std::clamp(row->selected, 0, kRenderModeMinimal);
        if (actions.onRenderMode)
            actions.onRenderMode(row->selected);
        break;
    case kIdFrameRate:
        if (actions.onFrameRate)
            actions.onFrameRate(row->selected);
        break;
    case kIdFullscreenRenderModeEnabled:
        row->checked = !row->checked;
        state.fullscreenRenderModeEnabled = row->checked;
        if (auto* monitor = findRow(kIdFullscreenRenderModeMonitor))
            monitor->enabled = row->checked;
        if (auto* target = findRow(kIdFullscreenRenderMode))
            target->enabled = row->checked;
        if (actions.onFullscreenRenderModeEnabled)
            actions.onFullscreenRenderModeEnabled(row->checked);
        break;
    case kIdFullscreenRenderModeMonitor:
        if (actions.onChooseFullscreenRenderModeMonitor)
            actions.onChooseFullscreenRenderModeMonitor();
        break;
    case kIdFullscreenRenderMode:
        state.fullscreenRenderMode = std::clamp(row->selected, 0, 2);
        if (actions.onFullscreenRenderMode)
            actions.onFullscreenRenderMode(state.fullscreenRenderMode);
        break;
    case kIdPickFont:
        if (actions.onPickFont)
            actions.onPickFont();
        break;
    case kIdUiFontSource:
        if (actions.onUiFontSource)
            actions.onUiFontSource(static_cast<UiFontSource>(row->selected));
        break;
    case kIdPickUiFont:
        if (actions.onPickUiFont)
            actions.onPickUiFont();
        break;
    }
}

} // namespace settings_ui
