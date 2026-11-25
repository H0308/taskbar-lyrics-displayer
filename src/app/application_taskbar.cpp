#include "app/application_internal.h"
#include "settings/display_rules.h"
namespace lyrics_app {
namespace {
const wchar_t* viewModeShortName(TaskbarViewMode mode) {
    switch (mode) {
    case TaskbarViewMode::Immersive:
        return L"沉浸";
    case TaskbarViewMode::AppBar:
        return L"Dock 模式";
    case TaskbarViewMode::Embedded:
    default:
        return L"内嵌";
    }
}

bool isTaskbarWindowReady(HWND taskbar) {
    RECT taskbarRect{};
    return taskbar && IsWindow(taskbar) && GetWindowRect(taskbar, &taskbarRect) &&
           taskbarRect.right > taskbarRect.left && taskbarRect.bottom > taskbarRect.top;
}
} // namespace

MonitorTaskbarSettings App::monitorSettingsForDisplay(const TaskbarDisplayInfo& display) const {
    return settings_.monitorTaskbarSettingsFor(display.deviceName, display.primary);
}

MonitorTaskbarSettings App::monitorSettingsForDevice(const std::wstring& deviceName) const {
    const auto displays = enumerateTaskbarDisplays();
    if (!deviceName.empty()) {
        const auto found = std::find_if(displays.begin(), displays.end(), [&](const auto& item) {
            return item.deviceName == deviceName;
        });
        if (found != displays.end())
            return settings_.monitorTaskbarSettingsFor(found->deviceName, found->primary);
        return settings_.monitorTaskbarSettingsFor(deviceName, false);
    }
    const auto primary = std::find_if(displays.begin(), displays.end(),
                                      [](const auto& item) { return item.primary; });
    if (primary != displays.end())
        return settings_.monitorTaskbarSettingsFor(primary->deviceName, true);
    if (!displays.empty())
        return settings_.monitorTaskbarSettingsFor(displays.front().deviceName,
                                                   displays.front().primary);
    return settings_.defaultMonitorTaskbarSettings(L"", true);
}

MonitorTaskbarSettings App::monitorSettingsForHost(const TaskbarHost* host) const {
    return monitorSettingsForDevice(host ? host->monitorDeviceName() : std::wstring{});
}

TaskbarHost* App::findHostForMonitor(const std::wstring& deviceName) const {
    if (deviceName.empty())
        return taskbarHost.get();
    if (taskbarHost && taskbarHost->monitorDeviceName() == deviceName)
        return taskbarHost.get();
    for (const auto& host : additionalTaskbarHosts_) {
        if (host && host->monitorDeviceName() == deviceName)
            return host.get();
    }
    return nullptr;
}

std::wstring App::resolveEditingMonitorDeviceName(
    const std::vector<TaskbarDisplayInfo>& displays) const {
    if (displays.empty())
        return {};
    if (!settingsEditingMonitorId_.empty()) {
        const auto found = std::find_if(displays.begin(), displays.end(), [this](const auto& item) {
            return item.deviceName == settingsEditingMonitorId_;
        });
        if (found != displays.end())
            return found->deviceName;
    }
    const auto primary = std::find_if(displays.begin(), displays.end(),
                                      [](const auto& item) { return item.primary; });
    return primary != displays.end() ? primary->deviceName : displays.front().deviceName;
}

void App::materializeConnectedMonitorSettings(const std::vector<TaskbarDisplayInfo>& displays) {
    if (!settings_.monitorTaskbarSettings_.empty() || displays.empty())
        return;
    std::vector<MonitorTaskbarSettings> initial;
    initial.reserve(displays.size());
    for (const auto& display : displays)
        initial.push_back(settings_.defaultMonitorTaskbarSettings(display.deviceName, display.primary));
    settings_.monitorTaskbarSettings_ = std::move(initial);
}

bool App::updateMonitorSettings(const std::wstring& deviceName,
                                const std::function<void(MonitorTaskbarSettings&)>& mutator) {
    const auto displays = enumerateTaskbarDisplays();
    if (displays.empty())
        return false;
    auto target = displays.end();
    if (!deviceName.empty()) {
        target = std::find_if(displays.begin(), displays.end(),
                              [&](const auto& item) { return item.deviceName == deviceName; });
    }
    if (target == displays.end()) {
        target = std::find_if(displays.begin(), displays.end(),
                              [](const auto& item) { return item.primary; });
    }
    if (target == displays.end())
        target = displays.begin();

    materializeConnectedMonitorSettings(displays);
    auto& profile = settings_.ensureMonitorTaskbarSettings(target->deviceName, target->primary);
    mutator(profile);
    if (target->primary)
        settings_.syncLegacyTaskbarFieldsFromMonitor(profile);
    return true;
}

bool App::anyEmbeddedHostConfigured() const {
    const auto displays = desiredTaskbarDisplays(monitor.snapshot());
    if (displays.empty())
        return settings_.taskbarViewMode_ == TaskbarViewMode::Embedded;
    for (const auto& display : displays) {
        const auto profile = monitorSettingsForDisplay(display);
        const bool vertical = !display.taskbarWindow || !display.horizontalTaskbar;
        if (display_rules::effectiveTaskbarViewMode(profile.viewMode, vertical) ==
            TaskbarViewMode::Embedded)
            return true;
    }
    return false;
}

bool App::anyHorizontalEmbeddedHostConfigured() const {
    const auto displays = desiredTaskbarDisplays(monitor.snapshot());
    if (displays.empty())
        return settings_.taskbarViewMode_ == TaskbarViewMode::Embedded && !taskbarVertical_;
    for (const auto& display : displays) {
        const auto profile = monitorSettingsForDisplay(display);
        const bool vertical = !display.taskbarWindow || !display.horizontalTaskbar;
        if (!vertical &&
            display_rules::effectiveTaskbarViewMode(profile.viewMode, false) ==
                TaskbarViewMode::Embedded)
            return true;
    }
    return false;
}

std::vector<TaskbarDisplayInfo> App::desiredTaskbarDisplaysFrom(
    const std::vector<TaskbarDisplayInfo>& displays) const {
    if (displays.empty())
        return {};
    auto primary = std::find_if(displays.begin(), displays.end(),
                                [](const auto& item) { return item.primary; });
    if (primary == displays.end())
        primary = displays.begin();

    // 单显示器（含副屏断开或宕机后仅剩一台显示器）始终强制保持启用，
    // 防止用户把副屏设为主用后因显示器宕机导致所有屏幕都不可见。
    if (displays.size() == 1)
        return {*primary};

    std::vector<TaskbarDisplayInfo> selected;
    selected.reserve(displays.size());
    for (const auto& display : displays) {
        const MonitorTaskbarSettings profile = monitorSettingsForDisplay(display);
        if (!profile.enabled)
            continue;
        if (profile.viewMode != TaskbarViewMode::AppBar &&
            !isTaskbarWindowReady(display.taskbarWindow))
            continue;
        selected.push_back(display);
    }
    // 兜底：若所有已连接且启用的显示器都不可承载（例如唯一启用的副屏宕机或被关闭系统任务栏），
    // 自动回退到主显示器，但不改写已持久化的每显示器配置。
    if (selected.empty())
        return {*primary};
    return selected;
}

std::vector<TaskbarDisplayInfo> App::desiredTaskbarDisplays(const SmtcSnapshot&) const {
    return desiredTaskbarDisplaysFrom(enumerateTaskbarDisplays());
}

bool App::hasMissingEnabledTaskbarDisplay(const std::vector<TaskbarDisplayInfo>& allDisplays) const {
    auto isActive = [this](const std::wstring& deviceName) {
        return std::find(activeTaskbarDisplayIds_.begin(), activeTaskbarDisplayIds_.end(),
                         deviceName) != activeTaskbarDisplayIds_.end();
    };
    for (const auto& display : allDisplays) {
        if (monitorSettingsForDisplay(display).enabled && !isActive(display.deviceName))
            return true;
    }
    for (const auto& profile : settings_.monitorTaskbarSettings_) {
        if (profile.enabled && !profile.deviceName.empty() && !isActive(profile.deviceName))
            return true;
    }
    return false;
}

std::wstring App::taskbarDisplayLabel(const TaskbarDisplayInfo& display) const {
    std::wstring label = display.displayName;
    if (display.displayName != display.deviceName)
        label += L"（" + display.deviceName + L"）";
    if (display.primary)
        label += L"（主显示器）";
    return label;
}

std::wstring App::taskbarMonitorLabel() const {
    const auto compactDeviceName = [](std::wstring deviceName) {
        if (deviceName.rfind(L"\\\\.\\", 0) == 0)
            deviceName.erase(0, 4);
        return deviceName;
    };
    const auto displays = enumerateTaskbarDisplays();
    if (displays.empty())
        return L"主显示器";
    const std::wstring targetId = resolveEditingMonitorDeviceName(displays);
    const auto selected = std::find_if(displays.begin(), displays.end(), [&](const auto& item) {
        return item.deviceName == targetId;
    });
    if (selected == displays.end())
        return L"主显示器";
    return selected->primary ? L"主显示器" : compactDeviceName(selected->deviceName);
}

void App::selectTaskbarMonitor(const std::wstring& deviceName) {
    if (settingsEditingMonitorId_ == deviceName)
        return;
    settingsEditingMonitorId_ = deviceName;
    refreshSettingsDialog(true);
}

void App::showTaskbarMonitorMenu() {
    const auto displays = enumerateTaskbarDisplays();
    if (displays.empty())
        return;
    const std::wstring editingId = resolveEditingMonitorDeviceName(displays);
    std::vector<fluent::FluentMenuItem> items;
    taskbarMonitorMenuItems_.clear();
    const bool singleDisplay = displays.size() <= 1;
    for (size_t i = 0; i < displays.size(); ++i) {
        const auto& display = displays[i];
        const auto profile = monitorSettingsForDisplay(display);
        fluent::FluentMenuItem item;
        item.id = kCmdTaskbarMonitorBase + static_cast<int>(i);
        const bool activeEnabled = singleDisplay || profile.enabled;
        std::wstring suffix = activeEnabled ? (std::wstring(L" · ") + viewModeShortName(profile.viewMode))
                                            : std::wstring(L" · 未启用");
        if (activeEnabled && profile.viewMode != TaskbarViewMode::AppBar && !display.taskbarWindow)
            suffix += L"（无任务栏）";
        item.text = taskbarDisplayLabel(display) + suffix;
        item.icon = settings_icon::Kind::Display;
        item.checked = display.deviceName == editingId;
        taskbarMonitorMenuItems_.emplace_back(item.id, display.deviceName);
        items.push_back(std::move(item));
    }
    POINT point{};
    GetCursorPos(&point);
    fluent::FluentMenu::show(trayHwnd, point, std::move(items), [this](int command) {
        const auto found =
            std::find_if(taskbarMonitorMenuItems_.begin(), taskbarMonitorMenuItems_.end(),
                         [command](const auto& item) { return item.first == command; });
        if (found != taskbarMonitorMenuItems_.end())
            selectTaskbarMonitor(found->second);
    });
}

void App::applyMonitorEnabled(bool on) {
    const auto displays = enumerateTaskbarDisplays();
    if (displays.size() <= 1) {
        refreshSettingsDialog(true);
        return;
    }
    const std::wstring targetId = resolveEditingMonitorDeviceName(displays);
    if (!on) {
        int otherEnabledCount = 0;
        for (const auto& display : displays) {
            if (display.deviceName == targetId)
                continue;
            const auto profile = monitorSettingsForDisplay(display);
            if (profile.enabled &&
                (profile.viewMode == TaskbarViewMode::AppBar || display.taskbarWindow != nullptr))
                ++otherEnabledCount;
        }
        if (otherEnabledCount == 0) {
            refreshSettingsDialog(true);
            return;
        }
    }
    if (!updateMonitorSettings(targetId,
                               [on](MonitorTaskbarSettings& profile) { profile.enabled = on; }))
        return;
    settingsEditingMonitorId_ = targetId;
    if (taskbarHost || taskbarPendingDisplayRebuild_) {
        taskbarMissingMonitorChecks_ = 0;
        syncTaskbarView(monitor.snapshot());
    }
    logSettingBool(L"monitor-enabled", on);
    saveSettings();
    refreshSettingsDialog(true);
}

void App::applyMonitorSettingsToAll() {
    const auto displays = enumerateTaskbarDisplays();
    if (displays.size() <= 1)
        return;
    const std::wstring sourceId = resolveEditingMonitorDeviceName(displays);
    const MonitorTaskbarSettings source = monitorSettingsForDevice(sourceId);
    materializeConnectedMonitorSettings(displays);
    for (const auto& display : displays) {
        auto& profile = settings_.ensureMonitorTaskbarSettings(display.deviceName, display.primary);
        profile.enabled = true;
        profile.viewMode = source.viewMode;
        profile.appBarEdge = source.appBarEdge;
        profile.immersiveBackgroundBlur = source.immersiveBackgroundBlur;
        profile.dockBackgroundBlur = source.dockBackgroundBlur;
        profile.immersiveBackgroundAdjustment = source.immersiveBackgroundAdjustment;
        profile.dockBackgroundAdjustment = source.dockBackgroundAdjustment;
        profile.embeddedResourceMonitor = source.embeddedResourceMonitor;
        profile.dockResourceCpuUsage = source.dockResourceCpuUsage;
        profile.dockResourceMemoryUsage = source.dockResourceMemoryUsage;
        profile.dockResourceGpuUsage = source.dockResourceGpuUsage;
        profile.dockResourceDownload = source.dockResourceDownload;
        profile.dockResourceUpload = source.dockResourceUpload;
        profile.dockResourceCpuFrequency = source.dockResourceCpuFrequency;
        profile.dockResourceSmallFont = source.dockResourceSmallFont;
        profile.dockResourceHideLabels = source.dockResourceHideLabels;
        profile.dockResourcePlacement = source.dockResourcePlacement;
        if (display.primary)
            settings_.syncLegacyTaskbarFieldsFromMonitor(profile);
    }
    if (taskbarHost || taskbarPendingDisplayRebuild_) {
        taskbarMissingMonitorChecks_ = 0;
        publishTaskbarAppearance();
        syncTaskbarView(monitor.snapshot());
        if (taskbarHost) {
            syncTaskbarOrientation();
            applyEffectiveTaskbarSettings();
            publishPresentationFrame(monitor.snapshot(), {.lyricsChanged = true});
        }
    }
    runtime_log::writef(L"[action][setting] apply-monitor-settings-to-all source=%s",
                        sourceId.c_str());
    saveSettings();
    refreshSettingsDialog(true);
}

bool App::fullscreenTargetAllMonitors() const {
    return settings_.fullscreenRenderModeMonitorId_.empty() ||
           settings_.fullscreenRenderModeMonitorId_ == L"all";
}

std::wstring App::fullscreenRenderModeMonitorLabel() const {
    if (fullscreenTargetAllMonitors())
        return L"全部显示器";

    const auto compactDeviceName = [](std::wstring deviceName) {
        if (deviceName.rfind(L"\\\\.\\", 0) == 0)
            deviceName.erase(0, 4);
        return deviceName;
    };
    const auto displays = enumerateTaskbarDisplays();
    const auto selected = std::find_if(displays.begin(), displays.end(), [this](const auto& item) {
        return item.deviceName == settings_.fullscreenRenderModeMonitorId_;
    });
    if (selected == displays.end())
        return compactDeviceName(settings_.fullscreenRenderModeMonitorId_) + L" 断开";
    return selected->primary ? L"主显示器" : compactDeviceName(selected->deviceName);
}

void App::selectFullscreenRenderModeMonitor(const std::wstring& deviceName) {
    const std::wstring normalized = deviceName.empty() ? std::wstring(L"all") : deviceName;
    if (settings_.fullscreenRenderModeMonitorId_ == normalized)
        return;
    settings_.fullscreenRenderModeMonitorId_ = normalized;
    runtime_log::writef(L"[action][setting] fullscreen-render-mode-monitor=%s",
                        settings_.fullscreenRenderModeMonitorId_.c_str());
    syncFullscreenRenderMode(true);
    saveSettings();
    refreshSettingsDialog(true);
}

void App::showFullscreenRenderModeMonitorMenu() {
    const auto displays = enumerateTaskbarDisplays();
    std::vector<fluent::FluentMenuItem> items;
    taskbarMonitorMenuItems_.clear();
    fluent::FluentMenuItem all;
    all.id = kCmdTaskbarMonitorAll;
    all.text = L"全部显示器";
    all.icon = settings_icon::Kind::Display;
    all.checked = fullscreenTargetAllMonitors();
    items.push_back(std::move(all));
    for (size_t i = 0; i < displays.size(); ++i) {
        const auto& display = displays[i];
        fluent::FluentMenuItem item;
        item.id = kCmdTaskbarMonitorBase + static_cast<int>(i);
        item.text = taskbarDisplayLabel(display);
        item.icon = settings_icon::Kind::Display;
        item.checked = !fullscreenTargetAllMonitors() &&
                       settings_.fullscreenRenderModeMonitorId_ == display.deviceName;
        taskbarMonitorMenuItems_.emplace_back(item.id, display.deviceName);
        items.push_back(std::move(item));
    }
    POINT point{};
    GetCursorPos(&point);
    fluent::FluentMenu::show(trayHwnd, point, std::move(items), [this](int command) {
        if (command == kCmdTaskbarMonitorAll) {
            selectFullscreenRenderModeMonitor(L"all");
            return;
        }
        const auto found =
            std::find_if(taskbarMonitorMenuItems_.begin(), taskbarMonitorMenuItems_.end(),
                         [command](const auto& item) { return item.first == command; });
        if (found != taskbarMonitorMenuItems_.end())
            selectFullscreenRenderModeMonitor(found->second);
    });
}

bool App::taskbarExpandedConfigured() const {
    return !anyEmbeddedHostConfigured();
}

void App::requestTaskbarRebuild() {
    if (shutdownRequested_ || taskbarRebuildPending_)
        return;
    taskbarRebuildPending_ = true;
    // 这里只唤醒主消息循环，窗口过程和模态消息循环都不能直接销毁宿主。
    // SetWindowPos/SHAppBarMessage 等调用可能重入 WM_DISPLAYCHANGE，
    // 必须等原来的窗口调用栈退出后才能释放它仍在使用的对象。
    if (trayHwnd && !PostMessageW(trayHwnd, kMsgTaskbarRebuild, 0, 0))
        runtime_log::writef(L"[display] failed to wake main loop for taskbar rebuild");
}

void App::cancelTaskbarDisplayRebuildRetry() {
    taskbarPendingDisplayRebuild_ = false;
    taskbarRebuildAllowOverlap_ = false;
    taskbarDisplayRebuildRetries_ = 0;
    if (trayHwnd)
        KillTimer(trayHwnd, kTimerTaskbarDisplayRebuildRetry);
}

void App::scheduleTaskbarDisplayRebuildRetry(bool allowOverlap) {
    if (shutdownRequested_ || isRenderMode(RenderMode::Stopped))
        return;
    taskbarPendingDisplayRebuild_ = true;
    taskbarRebuildAllowOverlap_ = allowOverlap;
    if (taskbarDisplayRebuildRetries_ >= kTaskbarDisplayRebuildMaxRetries)
        return;
    ++taskbarDisplayRebuildRetries_;
    if (trayHwnd)
        SetTimer(trayHwnd, kTimerTaskbarDisplayRebuildRetry, kTaskbarDisplayRebuildRetryMs,
                 nullptr);
}

void App::cancelMissingMonitorCheck() {
    taskbarMissingMonitorChecks_ = 0;
    if (trayHwnd)
        KillTimer(trayHwnd, kTimerTaskbarMissingMonitorCheck);
}

void App::scheduleMissingMonitorCheck() {
    if (shutdownRequested_ || !taskbarHost || isRenderMode(RenderMode::Stopped))
        return;
    if (taskbarMissingMonitorChecks_ >= kTaskbarMissingMonitorMaxChecks)
        return;
    if (trayHwnd)
        SetTimer(trayHwnd, kTimerTaskbarMissingMonitorCheck, kTaskbarMissingMonitorCheckMs,
                 nullptr);
}

void App::onMissingMonitorCheckTimer() {
    if (trayHwnd)
        KillTimer(trayHwnd, kTimerTaskbarMissingMonitorCheck);
    if (shutdownRequested_ || !taskbarHost || isRenderMode(RenderMode::Stopped)) {
        cancelMissingMonitorCheck();
        return;
    }
    ++taskbarMissingMonitorChecks_;
    const auto allDisplays = enumerateTaskbarDisplays();
    const auto desired = desiredTaskbarDisplaysFrom(allDisplays);
    std::vector<std::wstring> desiredIds;
    desiredIds.reserve(desired.size());
    for (const auto& display : desired)
        desiredIds.push_back(display.deviceName);
    if (desiredIds != activeTaskbarDisplayIds_) {
        runtime_log::writef(
            L"[display] missing taskbar display check=%d/%d triggered rebuild (active=%zu desired=%zu)",
            taskbarMissingMonitorChecks_, kTaskbarMissingMonitorMaxChecks,
            activeTaskbarDisplayIds_.size(), desiredIds.size());
        requestTaskbarRebuild();
        return;
    }
    if (!hasMissingEnabledTaskbarDisplay(allDisplays) ||
        taskbarMissingMonitorChecks_ >= kTaskbarMissingMonitorMaxChecks) {
        if (hasMissingEnabledTaskbarDisplay(allDisplays)) {
            runtime_log::writef(L"[display] missing taskbar display check stopped after %d attempts",
                                taskbarMissingMonitorChecks_);
        }
        cancelMissingMonitorCheck();
        return;
    }
    scheduleMissingMonitorCheck();
}

void App::rebuildTaskbarIfPending() {
    if (!taskbarRebuildPending_)
        return;
    taskbarRebuildPending_ = false;
    if (shutdownRequested_ || (!taskbarHost && !taskbarPendingDisplayRebuild_))
        return;

    const bool allowOverlap = taskbarHost ? taskbarAllowOverlap_ : taskbarRebuildAllowOverlap_;
    runtime_log::writef(L"[display] taskbar rebuild started");
    destroyTaskbar();
    if (shutdownRequested_)
        return;
    const TaskbarCreateResult result = createTaskbar(GetModuleHandleW(nullptr), allowOverlap);
    if (result == TaskbarCreateResult::Failed)
        scheduleTaskbarDisplayRebuildRetry(allowOverlap);
    else if (result != TaskbarCreateResult::Pending)
        cancelTaskbarDisplayRebuildRetry();
    refreshSettingsDialog(true);
}

void App::syncTaskbarView(const SmtcSnapshot& snap) {
    if (!taskbarHost && !taskbarPendingDisplayRebuild_)
        return;

    const auto displays = desiredTaskbarDisplays(snap);
    std::vector<std::wstring> desiredIds;
    desiredIds.reserve(displays.size());
    for (const auto& display : displays)
        desiredIds.push_back(display.deviceName);
    if (!taskbarHost || desiredIds != activeTaskbarDisplayIds_) {
        requestTaskbarRebuild();
        return;
    }

    forEachHost([&](TaskbarHost* host) {
        const auto profile = monitorSettingsForHost(host);
        host->setAppBarEdge(profile.appBarEdge);
        const auto configuredMode = profile.viewMode;
        const bool vertical =
            configuredMode == TaskbarViewMode::Immersive && host->isVerticalTaskbar();
        host->setViewMode(display_rules::effectiveTaskbarViewMode(configuredMode, vertical));
    });
    syncTaskbarOrientation();
}

void App::syncSpectrumWithMode() {
    bool anyHostActive = false;
    if (!taskbarSpectrumSuppressedForNoSpace_) {
        forEachHost([&](TaskbarHost* host) {
            if (display_rules::spectrumRequested(settings_, effectiveRenderModeForHost(host),
                                                 host->isVerticalTaskbar())) {
                anyHostActive = true;
            }
        });
    }
    const SmtcSnapshot snap = monitor.snapshot();
    const AudioSessionTarget audioTarget = audioTargetForSnapshot(snap);
    // 播放中但确实解析不到音频进程/包族的未知来源临时抑制频谱显示，不覆盖用户开关偏好。
    const bool sourceBlocked = snap.sessionAlive && audioTarget.empty();
    if (anyHostActive && !sourceBlocked) {
        if (!audioTarget.empty())
            spectrum_.setTarget(audioTarget);
        spectrum_.start();
        forEachHost([&](TaskbarHost* host) {
            host->setSpectrumVisible(display_rules::spectrumRequested(
                settings_, effectiveRenderModeForHost(host), host->isVerticalTaskbar()));
        });
    } else {
        spectrum_.stop();
        forEachHost([&](TaskbarHost* host) { host->setSpectrumVisible(false); });
    }
}

void App::tryRestoreSpectrumAfterNoSpace() {
    if (!taskbarSpectrumSuppressedForNoSpace_ || taskbarSpectrumRestoring_ ||
        !display_rules::spectrumRequested(settings_, effectiveRenderModeForHost(taskbarHost.get()),
                                          taskbarVertical_) ||
        !taskbarHost)
        return;

    // 频谱本身会扩大任务栏歌词窗口；恢复空间时先试着恢复用户设置，
    // 如果再次判定无空间，状态回调会重新保留抑制标记并关闭频谱。
    taskbarSpectrumRestoring_ = true;
    taskbarSpectrumSuppressedForNoSpace_ = false;
    syncSpectrumWithMode();
    taskbarSpectrumRestoring_ = false;
}

void App::syncTaskbarOrientation() {
    if (!taskbarHost)
        return;
    const bool compact = taskbarHost->isCompactTaskbar();
    if (compact != taskbarCompact_) {
        taskbarCompact_ = compact;
        refreshSettingsDialog(true);
    }
    const bool vertical = taskbarHost->isVerticalTaskbar();
    if (vertical == taskbarVertical_)
        return;

    taskbarVertical_ = vertical;
    // 方向只影响实际显示与采样，保留用户配置，回到横向后重新应用。
    forEachHost([&](TaskbarHost* host) {
        const auto profile = monitorSettingsForHost(host);
        const auto configuredMode = profile.viewMode;
        const bool hostVertical =
            configuredMode == TaskbarViewMode::Immersive && host->isVerticalTaskbar();
        host->setViewMode(display_rules::effectiveTaskbarViewMode(configuredMode, hostVertical));
    });
    applyEffectiveTaskbarSettings();
    syncSpectrumWithMode();
    publishPresentationFrame(monitor.snapshot(), {.lyricsChanged = true});
    refreshSettingsDialog(true);
}

void App::armTaskbarAutoRestore() {
    if (shutdownRequested_ || isRenderMode(RenderMode::Stopped) || taskbarHost)
        return;
    taskbarAutoRestorePending_ = true;
    if (trayHwnd && !SetTimer(trayHwnd, kTimerTaskbarAutoRestore, kTaskbarAutoRestoreMs, nullptr)) {
        runtime_log::writef(L"[taskbar] failed to schedule auto-restore");
    }
}

void App::cancelTaskbarAutoRestore() {
    taskbarAutoRestorePending_ = false;
    if (trayHwnd)
        KillTimer(trayHwnd, kTimerTaskbarAutoRestore);
}

void App::tryRestoreTaskbarAfterNoSpace() {
    if (!taskbarAutoRestorePending_)
        return;
    if (shutdownRequested_ || isRenderMode(RenderMode::Stopped)) {
        cancelTaskbarAutoRestore();
        return;
    }
    if (taskbarHost) {
        cancelTaskbarAutoRestore();
        return;
    }

    const TaskbarCreateResult result = createTaskbar(GetModuleHandleW(nullptr));
    if (result == TaskbarCreateResult::Created) {
        cancelTaskbarAutoRestore();
        // 不要仅凭宿主创建成功就关闭提示。此时首次真实的空间探测可能
        // 尚未完成，必须等 onTaskbarPlacementStatus() 确认有可用空间后再关。
        runtime_log::writef(L"[taskbar] auto-restore result=created, awaiting-placement");
    } else if (result == TaskbarCreateResult::Failed) {
        // 真正的宿主创建失败不弹出“空间不足”提示；等待 Explorer 的
        // TaskbarCreated 广播重新触发一次恢复探测。
        cancelTaskbarAutoRestore();
        runtime_log::writef(L"[taskbar] auto-restore result=failed");
    } else if (result == TaskbarCreateResult::Pending) {
        // 系统任务栏未就绪时改由重建机制接管，不并行运行两套恢复定时器。
        cancelTaskbarAutoRestore();
        runtime_log::writef(L"[taskbar] auto-restore result=pending-taskbar");
    }
    // NoSpace 会保留宿主，由 TaskbarHost 的低频避让探测继续等待空间释放。
}

void App::configureTaskbarHostCallbacks(TaskbarHost* host, bool primary) {
    if (!host)
        return;
    host->setCloseCallback([this] { requestQuit(); });
    host->setSpectrumReader([source = &spectrum_] { return source->bands(); });
    host->setTickCallback([this, host] {
        TaskbarHost* tickOwner = nullptr;
        forEachHost([&](TaskbarHost* candidate) {
            if (!tickOwner && effectiveRenderModeForHost(candidate) != RenderMode::Stopped)
                tickOwner = candidate;
        });
        if (tickOwner == host)
            onFrame();
    });
    if (primary) {
        host->setStatusTextCycleCompletedCallback(
            [this] { idle_.onStartupTaskSummaryCompleted(); });
        host->setPlacementStatusCallback(
            [this, host](TaskbarPlacementStatus status) {
                // 状态通知异步回到主线程后，不能用旧的空间判定覆盖当前宿主状态。
                if (taskbarHost.get() == host && host->placementStatus() == status)
                    onTaskbarPlacementStatus(status);
            });
    }
    host->setControlCallback([this](MediaControl c, uint64_t sessionGeneration, int64_t positionMs) {
        onControl(c, sessionGeneration, positionMs);
    });
    host->setImmersiveExitCallback([this, host] {
        if (isLiveTaskbarHost(host))
            applyTaskbarViewModeForMonitor(host->monitorDeviceName(), 0);
    });
    host->setDockQuickAppsCallback([this](POINT pt) { showDockQuickApps(pt); });
    host->setImmersiveMenuCallback([this, host](POINT pt) {
        if (isLiveTaskbarHost(host))
            activeMenuMonitorId_ = host->monitorDeviceName();
        showTrayMenu(pt, false);
    });
    host->setContextMenuCallback([this, host](POINT pt) { showTaskbarMenu(pt, host); });
    host->setAnchorChangedCallback([this, host](TaskbarAnchorSpec anchor) {
        if (isLiveTaskbarHost(host))
            applyTaskbarAnchor(anchor, L"taskbar-position-drag", host, host->monitorDeviceName());
    });
    host->setAnchorOptionsChangedCallback([this, host](std::vector<TaskbarAnchorOption> options) {
        if (isLiveTaskbarHost(host))
            taskbarAnchorOptions_[host] = std::move(options);
    });
    host->setAppVolumeCallback([this](AppVolumeRequest request) {
        if (request.revision <= appVolumeRequestRevision_)
            return;
        appVolumeRequestRevision_ = request.revision;
        const bool ok = appVolume_.setVolumePercent(request.percent);
        runtime_log::writef(L"[action][volume] set percent=%d result=%s", request.percent,
                            ok ? L"ok" : L"unavailable");
        pushAppVolume();
    });
    host->setSourceOpenCallback([](const std::wstring& source) {
        const auto result = platform_icon::launchSourceApp(source);
        runtime_log::writef(L"[action][player] open-source source=%s result=%s", source.c_str(),
                             platform_icon::appLaunchResultName(result));
    });
    host->setIdleAppOpenCallback([this](const std::wstring& path) { launchIdleApp(path); });
    host->setIdleTaskOpenCallback(
        [this](const IdleTaskInfo& task) { idle_.openTickTickTask(task); });
    host->setIdleTaskCompleteCallback(
        [this](const IdleTaskInfo& task) { idle_.completeTickTickTask(task); });
    host->setMediaPopupOpenedCallback([this] { idle_.refreshTickTickTasks(); });
}

bool App::isLiveTaskbarHost(const TaskbarHost* host) const {
    if (!host)
        return false;
    if (taskbarHost.get() == host)
        return true;
    return std::any_of(additionalTaskbarHosts_.begin(), additionalTaskbarHosts_.end(),
                       [host](const auto& item) { return item.get() == host; });
}

void App::applyTaskbarAnchor(const TaskbarAnchorSpec& anchor, const wchar_t* logSource,
                             const TaskbarHost* alreadyAppliedHost,
                             const std::wstring& targetMonitorDeviceName) {
    const std::wstring targetId =
        !targetMonitorDeviceName.empty()
            ? targetMonitorDeviceName
            : (alreadyAppliedHost ? alreadyAppliedHost->monitorDeviceName()
                                  : (taskbarHost ? taskbarHost->monitorDeviceName() : std::wstring{}));
    const MonitorTaskbarSettings before = monitorSettingsForDevice(targetId);
    if (before.anchor() == anchor)
        return;
    TaskbarAnchorSpec saved = anchor;
    if (!updateMonitorSettings(targetId, [&](MonitorTaskbarSettings& profile) {
            profile.setAnchor(anchor);
            saved = profile.anchor();
        })) {
        settings_.setTaskbarAnchor(anchor);
        saved = settings_.taskbarAnchor();
    }
    runtime_log::writef(L"[setting] %s monitor=%s mode=%d ratio=%.4f align=%d", logSource,
                        targetId.c_str(), saved.mode, saved.ratio,
                        saved.edge == TaskbarAnchorEdge::Start ? 0 : 1);
    if (TaskbarHost* host = findHostForMonitor(targetId); host && host != alreadyAppliedHost)
        host->setAnchor(saved);
    saveSettings();
}

TaskbarCreateResult App::createTaskbar(HINSTANCE inst, bool allowOverlap) {
    if (taskbarHost) {
        if (!allowOverlap)
            return TaskbarCreateResult::Created;

        // 空间不足时保留现有宿主和探测线程；用户确认强制开启时，
        // 直接把同一个宿主切到允许重叠，避免重新创建窗口和等待首次探测。
        taskbarAllowOverlap_ = true;
        taskbarAutoClosedForNoSpace_ = false;
        taskbarSpectrumSuppressedForNoSpace_ = false;
        cancelTaskbarAutoRestore();
        cancelTaskbarDisplayRebuildRetry();
        cancelMissingMonitorCheck();
        forEachHost([](TaskbarHost* host) { host->setAllowOverlap(true); });
        const TaskbarPlacementStatus status = taskbarHost->refreshPlacement();
        if (status == TaskbarPlacementStatus::Unavailable) {
            taskbarAllowOverlap_ = false;
            runtime_log::writef(L"[taskbar] forced-open result=unavailable");
            return TaskbarCreateResult::Failed;
        }
        const SmtcSnapshot forcedSnap = monitor.snapshot();
        forEachHost([&](TaskbarHost* host) {
            host->show();
            applyPresentationFrameToHost(host, forcedSnap);
            host->refreshPlacement();
        });
        syncSpectrumWithMode();
        updateTrayIcon();
        return TaskbarCreateResult::Created;
    }

    const SmtcSnapshot snap = monitor.snapshot();
    const auto allDisplays = enumerateTaskbarDisplays();
    const auto displays = desiredTaskbarDisplaysFrom(allDisplays);
    auto formatDisplaysSummary = [&] {
        std::wstring summary;
        for (size_t i = 0; i < allDisplays.size(); ++i) {
            if (i > 0)
                summary += L", ";
            const auto& display = allDisplays[i];
            const auto profile = monitorSettingsForDisplay(display);
            summary += display.deviceName;
            summary += L"(p=";
            summary += display.primary ? L"1" : L"0";
            summary += L",en=";
            summary += profile.enabled ? L"1" : L"0";
            summary += L",m=";
            summary += std::to_wstring(static_cast<int>(profile.viewMode));
            summary += L",tb=";
            summary += display.taskbarWindow ? L"1" : L"0";
            summary += L",ok=";
            summary += isTaskbarWindowReady(display.taskbarWindow) ? L"1" : L"0";
            summary += L")";
        }
        return summary;
    };
    auto waitForTaskbar = [&] {
        scheduleTaskbarDisplayRebuildRetry(allowOverlap);
        updateTrayIcon();
        runtime_log::writef(L"[taskbar] waiting for system taskbar displays=[%s]",
                            formatDisplaysSummary().c_str());
        return TaskbarCreateResult::Pending;
    };
    if (displays.empty())
        return waitForTaskbar();
    auto systemTaskbarReady = [&] {
        for (const auto& display : displays) {
            const auto profile = monitorSettingsForDisplay(display);
            if (profile.viewMode == TaskbarViewMode::AppBar)
                return true;
            if (isTaskbarWindowReady(display.taskbarWindow))
                return true;
        }
        return false;
    };
    if (!systemTaskbarReady())
        return waitForTaskbar();
    taskbarAllowOverlap_ = allowOverlap;
    activeTaskbarDisplayIds_.clear();

    auto createHost = [&](const TaskbarDisplayInfo& display, bool primary) {
        auto host = std::make_unique<TaskbarHost>();
        host->setVisibilitySuppressed(true);
        const auto profile = monitorSettingsForDisplay(display);
        const auto initialMode = display_rules::effectiveTaskbarViewMode(
            profile.viewMode, !display.taskbarWindow || !display.horizontalTaskbar);
        if (!host->create(inst, display.deviceName, initialMode)) {
            runtime_log::writef(L"failed to create taskbar host for %s",
                                display.deviceName.c_str());
            return std::unique_ptr<TaskbarHost>{};
        }
        host->setAllowOverlap(allowOverlap);
        configureTaskbarHostCallbacks(host.get(), primary);
        // 完全停止模式下新宿主从创建开始就保持隐藏，避免同步首帧后闪现。
        host->setRenderMode(effectiveRenderModeForHost(host.get()));
        host->setAppBarEdge(profile.appBarEdge);
        return host;
    };

    // 逐台创建宿主：单台显示器在热拔插或宕机瞬间创建失败时，不阻断其余健康显示器。
    for (const auto& display : displays) {
        const bool primaryHost = !taskbarHost;
        auto host = createHost(display, primaryHost);
        if (!host)
            continue;
        activeTaskbarDisplayIds_.push_back(display.deviceName);
        if (primaryHost)
            taskbarHost = std::move(host);
        else
            additionalTaskbarHosts_.push_back(std::move(host));
    }
    if (!taskbarHost) {
        activeTaskbarDisplayIds_.clear();
        taskbarAllowOverlap_ = false;
        // 创建跨 UI 线程执行，期间 Explorer 仍可能撤下刚枚举到的任务栏。
        if (!systemTaskbarReady())
            return waitForTaskbar();
        return TaskbarCreateResult::Failed;
    }
    runtime_log::writef(L"[display] taskbar hosts ready: active=%zu/%zu displays=[%s]",
                        activeTaskbarDisplayIds_.size(), allDisplays.size(),
                        formatDisplaysSummary().c_str());
    auto syncMissingMonitorCheck = [&] {
        if (hasMissingEnabledTaskbarDisplay(allDisplays)) {
            if (taskbarMissingMonitorChecks_ == 0) {
                runtime_log::writef(
                    L"[display] partial taskbar hosts active=%zu, scheduling missing-display check",
                    activeTaskbarDisplayIds_.size());
            }
            scheduleMissingMonitorCheck();
        } else {
            cancelMissingMonitorCheck();
        }
    };

    forEachHost([&](TaskbarHost* host) {
        const auto profile = monitorSettingsForHost(host);
        host->setAppBarEdge(profile.appBarEdge);
        syncHost(host);
        if (settings_.hasUserFont_)
            host->setFont(settings_.fontFamily_, settings_.fontSize_, settings_.fontStyle_);
        host->applyAppearance(appearanceForHost(host));

        host->setAnchor(profile.anchor());
        host->setAppVolume(appVolumeState_);
    });
    applyFontAppearance();
    syncTaskbarOrientation();
    applyEffectiveTaskbarSettings();
    syncSpectrumWithMode();
    bool anyHostHasSpace = false;
    bool anyHostUnavailable = false;
    forEachHost([&](TaskbarHost* host) {
        const TaskbarPlacementStatus status = host->refreshPlacement();
        if (status != TaskbarPlacementStatus::NoSpace &&
            status != TaskbarPlacementStatus::Unavailable)
            anyHostHasSpace = true;
        if (status == TaskbarPlacementStatus::Unavailable)
            anyHostUnavailable = true;
    });
    if (!anyHostHasSpace && !anyHostUnavailable) {
        taskbarAutoClosedForNoSpace_ = true;
        taskbarSpectrumSuppressedForNoSpace_ = true;
        spectrum_.stop();
        const bool wasRestoringSpectrum = taskbarSpectrumRestoring_;
        taskbarSpectrumRestoring_ = true;
        forEachHost([](TaskbarHost* host) { host->setSpectrumVisible(false); });
        taskbarSpectrumRestoring_ = wasRestoringSpectrum;
        bool recovered = false;
        forEachHost([&](TaskbarHost* host) {
            if (host->placementStatus() != TaskbarPlacementStatus::NoSpace &&
                host->placementStatus() != TaskbarPlacementStatus::Unavailable)
                recovered = true;
        });
        if (recovered) {
            // 释放频谱占用后已有足够空间，继续保留宿主但不需要进入无空间状态。
            taskbarAutoClosedForNoSpace_ = false;
            taskbarSpectrumSuppressedForNoSpace_ = false;
            cancelTaskbarAutoRestore();
            syncMissingMonitorCheck();
            updateTrayIcon();
            return TaskbarCreateResult::Created;
        }
        cancelTaskbarAutoRestore();
        runtime_log::writef(L"[taskbar] auto-hide reason=no-space");
        cancelTaskbarDisplayRebuildRetry();
        syncMissingMonitorCheck();
        updateTrayIcon();
        return TaskbarCreateResult::NoSpace;
    }
    if (!anyHostHasSpace && anyHostUnavailable) {
        runtime_log::writef(L"[taskbar] placement unavailable after creation");
        bool allAppBar = true;
        forEachHost([&](TaskbarHost* host) {
            if (monitorSettingsForHost(host).viewMode != TaskbarViewMode::AppBar)
                allAppBar = false;
        });
        destroyTaskbar();
        // AppBar 的注册失败属于真正的创建失败，不作为 Explorer 未就绪处理。
        return allAppBar ? TaskbarCreateResult::Failed : waitForTaskbar();
    }
    taskbarAutoClosedForNoSpace_ = false;
    cancelTaskbarAutoRestore();
    cancelTaskbarDisplayRebuildRetry();
    syncMissingMonitorCheck();
    // 宿主会在首次避让探测完成后自行解除显示抑制；强制开启仍重新提交当前帧，
    // 确保“开启”选项立即生效而不必等待下一次媒体事件。
    if (allowOverlap) {
        forEachHost([&](TaskbarHost* host) {
            host->show();
            applyPresentationFrameToHost(host, snap);
        });
    }
    updateTrayIcon();
    return TaskbarCreateResult::Created;
}

void App::destroyTaskbar() {
    taskbarRebuildPending_ = false;
    if (trayHwnd)
        KillTimer(trayHwnd, kTimerTaskbarMissingMonitorCheck);
    spectrum_.stop(); // 频谱只画在任务栏上，宿主销毁时捕获线程一并停
    taskbarAllowOverlap_ = false;
    taskbarSpectrumSuppressedForNoSpace_ = false;
    taskbarSpectrumRestoring_ = false;
    taskbarNoSpaceClosePending_ = false;
    // 任何销毁路径都结束一次“手动开启待确认”请求，避免旧请求在后续
    // 的自动探测消息中误把自动关闭当成手动开启失败并弹框。
    taskbarManualOpenPending_ = false;
    additionalTaskbarHosts_.clear();
    auto host = std::move(taskbarHost);
    host.reset();
    taskbarAnchorOptions_.clear();
    activeTaskbarAnchorMenuSpecs_.clear();
    activeTaskbarDisplayIds_.clear();
    updateTrayIcon();
}

bool App::taskbarEnabledForUserAction() {
    if (isRenderMode(RenderMode::Stopped))
        return false;
    if (!taskbarHost)
        return taskbarPendingDisplayRebuild_;
    bool anyAvailable = false;
    bool anyDisplayed = false;
    bool anyAppBar = false;
    forEachHost([&](TaskbarHost* host) {
        const TaskbarPlacementStatus status = host->placementStatus();
        if (status != TaskbarPlacementStatus::NoSpace &&
            status != TaskbarPlacementStatus::Unavailable) {
            anyAvailable = true;
            if (monitorSettingsForHost(host).viewMode == TaskbarViewMode::AppBar)
                anyAppBar = true;
            if (host->isDisplayed())
                anyDisplayed = true;
        }
    });
    if (!anyAvailable)
        return false;
    // AppBar 在无媒体且关闭空闲入口时会主动隐藏并释放工作区，
    // 但宿主与用户选择仍是开启状态，托盘操作不能把它误判为关闭。
    if (anyAppBar)
        return true;
    if (anyDisplayed) {
        taskbarEnabledBeforeStopped_ = false;
        return true;
    }
    return taskbarEnabledBeforeStopped_;
}

void App::toggleTaskbar() {
    runtime_log::writef(L"[action][taskbar] toggle current=%s",
                        taskbarEnabledForUserAction() ? L"enabled" : L"disabled");
    if (taskbarEnabledForUserAction()) {
        taskbarAutoClosedForNoSpace_ = false;
        taskbarManualOpenPending_ = false;
        cancelTaskbarAutoRestore();
        cancelTaskbarDisplayRebuildRetry();
        cancelMissingMonitorCheck();
        destroyTaskbar();
    } else {
        bool anyHostNoSpace = false;
        forEachHost([&](TaskbarHost* host) {
            if (host->placementStatus() == TaskbarPlacementStatus::NoSpace)
                anyHostNoSpace = true;
        });
        const bool noSpaceAlreadyKnown =
            !isRenderMode(RenderMode::Stopped) &&
            (anyHostNoSpace ||
             (!taskbarHost && taskbarAutoClosedForNoSpace_ && taskbarAutoRestorePending_));
        if (noSpaceAlreadyKnown) {
            // 无空间状态已经由当前宿主确认，保留宿主直接提示；用户确认后
            // 由 createTaskbar(..., true) 把同一个宿主切换为允许重叠。
            taskbarAutoClosedForNoSpace_ = true;
            cancelTaskbarAutoRestore();
            showTaskbarSpacePrompt();
            runtime_log::writef(L"[action][taskbar] toggle result=no-space-prompt");
            return;
        }
        // 仅在宿主不是“已确认无空间”的状态下才重建；无空间宿主会保留
        // 后台探测线程，不走销毁/重建路径。
        if (taskbarHost)
            destroyTaskbar();
        cancelTaskbarDisplayRebuildRetry();
        cancelMissingMonitorCheck();
        if (isRenderMode(RenderMode::Stopped)) {
            message_dialog::showModal(
                GetModuleHandleW(nullptr), trayHwnd, L"任务栏歌词提示",
                L"当前已开启性能-完全停止模式，如果要显示任务栏歌词，可以在性能设置中选择其他模式",
                L"确定");
            runtime_log::writef(L"[action][taskbar] toggle result=stopped");
            return;
        }
        taskbarManualOpenPending_ = true;
        const TaskbarCreateResult result = createTaskbar(GetModuleHandleW(nullptr));
        if (result == TaskbarCreateResult::NoSpace) {
            taskbarManualOpenPending_ = false;
            showTaskbarSpacePrompt();
        } else if (result == TaskbarCreateResult::Failed) {
            taskbarManualOpenPending_ = false;
            runtime_log::writef(L"[action][taskbar] toggle result=failed");
        } else if (result == TaskbarCreateResult::Pending) {
            taskbarManualOpenPending_ = false;
            runtime_log::writef(L"[action][taskbar] toggle result=pending-taskbar");
            return;
        }
    }
    runtime_log::writef(L"[action][taskbar] toggle result=%s",
                        taskbarHost ? L"enabled" : L"disabled");
}

void App::onTaskbarPlacementStatus(TaskbarPlacementStatus status) {
    if (status != TaskbarPlacementStatus::NoSpace) {
        taskbarNoSpaceClosePending_ = false;
        taskbarManualOpenPending_ = false;
        if (status != TaskbarPlacementStatus::Unavailable) {
            taskbarAutoClosedForNoSpace_ = false;
            if (!taskbarSpectrumRestoring_) {
                if (taskbarSpectrumSuppressedForNoSpace_) {
                    if (display_rules::spectrumRequested(
                            settings_, effectiveRenderModeForHost(taskbarHost.get()),
                            taskbarVertical_))
                        tryRestoreSpectrumAfterNoSpace();
                    else
                        // 频谱因用户设置、竖向任务栏或性能模式关闭时，
                        // 无需继续保留“空间不足抑制”状态；回到正常模式时
                        // 由普通同步逻辑按用户设置重新决定是否开启。
                        taskbarSpectrumSuppressedForNoSpace_ = false;
                } else if (taskbarHost && !isRenderMode(RenderMode::Stopped))
                    syncSpectrumWithMode();
            }
        }
        return;
    }
    if (!taskbarHost || taskbarAllowOverlap_ || taskbarNoSpaceClosePending_)
        return;

    taskbarAutoClosedForNoSpace_ = true;
    // 空间不足时只隐藏宿主；后台探测线程继续运行，空间恢复后可直接复用窗口和
    // 渲染资源。频谱停止和提示处理延迟到托盘消息，避免在探测回调内 join 捕获线程。
    taskbarSpectrumSuppressedForNoSpace_ = true;

    // 状态回调可能来自 TaskbarHost 的 WM_TIMER；延迟到托盘窗口消息中处理手动
    // 开启提示，避免在探测/重排调用栈内直接创建模态窗口。
    taskbarNoSpaceClosePending_ = true;
    if (!trayHwnd || !PostMessageW(trayHwnd, kMsgTaskbarNoSpace, 0, 0)) {
        taskbarNoSpaceClosePending_ = false;
        runtime_log::writef(L"[taskbar] failed to schedule no-space state handling");
    }
}

void App::handleTaskbarNoSpace() {
    if (!taskbarNoSpaceClosePending_)
        return;
    taskbarNoSpaceClosePending_ = false;
    const bool shouldPrompt = taskbarManualOpenPending_;
    taskbarManualOpenPending_ = false;
    if (!taskbarHost || taskbarAllowOverlap_)
        return;

    // 多显示器内嵌时，只要还有其他屏幕存在可用空间，就不停掉全局频谱或弹窗，
    // 空间不足的单块屏由其自身宿主安全隐藏并在空间恢复后自动重现。
    bool anyOtherHostAvailable = false;
    for (const auto& host : additionalTaskbarHosts_) {
        const TaskbarPlacementStatus status = host->placementStatus();
        if (status != TaskbarPlacementStatus::NoSpace &&
            status != TaskbarPlacementStatus::Unavailable) {
            anyOtherHostAvailable = true;
            break;
        }
    }
    if (anyOtherHostAvailable) {
        taskbarAutoClosedForNoSpace_ = false;
        taskbarSpectrumSuppressedForNoSpace_ = false;
        syncSpectrumWithMode();
        return;
    }

    taskbarAutoClosedForNoSpace_ = true;
    spectrum_.stop();
    const bool wasRestoringSpectrum = taskbarSpectrumRestoring_;
    taskbarSpectrumRestoring_ = true;
    forEachHost([](TaskbarHost* host) { host->setSpectrumVisible(false); });
    taskbarSpectrumRestoring_ = wasRestoringSpectrum;
    // 关闭频谱本身可能已经释放了占用空间；此时不再进入无空间处理，
    // 避免把刚恢复的宿主再次标记为自动隐藏。
    bool anyHostRecovered = false;
    forEachHost([&](TaskbarHost* host) {
        const TaskbarPlacementStatus status = host->placementStatus();
        if (status != TaskbarPlacementStatus::NoSpace &&
            status != TaskbarPlacementStatus::Unavailable)
            anyHostRecovered = true;
    });
    if (anyHostRecovered) {
        taskbarAutoClosedForNoSpace_ = false;
        taskbarSpectrumSuppressedForNoSpace_ = false;
        return;
    }
    cancelTaskbarAutoRestore();
    runtime_log::writef(L"[taskbar] auto-hide reason=no-space");
    if (shouldPrompt)
        showTaskbarSpacePrompt();
}

void App::showTaskbarSpacePrompt() {
    runtime_log::writef(L"[action][taskbar] prompt reason=no-space auto-closed=%d",
                        taskbarAutoClosedForNoSpace_ ? 1 : 0);
    // 系统模态框自带标准按钮和关闭行为；显示期间暂停自动恢复定时器，
    // 避免用户尚未选择时后台又创建宿主并改变当前状态。
    cancelTaskbarAutoRestore();
    const message_dialog::Result result = message_dialog::showModal(
        GetModuleHandleW(nullptr), trayHwnd, L"任务栏歌词提示",
        L"当前任务栏可用空间不足，开启任务栏歌词可能遮挡任务栏按钮并影响正常使用。\n\n"
        L"是否仍要开启任务栏歌词？",
        L"开启", L"取消开启", false);
    if (result == message_dialog::Result::Primary) {
        const TaskbarCreateResult createResult = createTaskbar(GetModuleHandleW(nullptr), true);
        if (createResult == TaskbarCreateResult::Failed)
            runtime_log::writef(L"[action][taskbar] forced-open result=failed");
        else if (createResult == TaskbarCreateResult::NoSpace)
            runtime_log::writef(L"[action][taskbar] forced-open result=no-space");
        else if (createResult == TaskbarCreateResult::Pending)
            runtime_log::writef(L"[action][taskbar] forced-open result=pending-taskbar");
        return;
    }

    if (!shutdownRequested_ && !isRenderMode(RenderMode::Stopped) && taskbarAutoClosedForNoSpace_ &&
        !taskbarHost)
        armTaskbarAutoRestore();
}
} // namespace lyrics_app
