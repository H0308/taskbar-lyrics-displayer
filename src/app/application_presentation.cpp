#include "app/application_internal.h"
#include "media/player_source.h"
#include "settings/display_rules.h"
#include <algorithm>
#include <cstdlib>
namespace lyrics_app {
namespace {
const wchar_t* onlineLyricSourceName(LyricOrigin origin) {
    switch (origin) {
    case LyricOrigin::QqMusic:
        return L"在线歌词（QQ 音乐）";
    case LyricOrigin::Kugou:
        return L"在线歌词（酷狗音乐）";
    case LyricOrigin::Netease:
        return L"在线歌词（网易云音乐）";
    case LyricOrigin::Soda:
        return L"在线歌词（汽水音乐）";
    case LyricOrigin::Lrclib:
        return L"在线歌词（LRCLIB）";
    default:
        return L"在线歌词";
    }
}
}

void App::updateRuntimeLogState(const SmtcSnapshot& snap) {
    const auto& state = playback_.state();
    const auto timeText = [](int64_t value) {
        const auto seconds = std::max<int64_t>(0, value) / 1000;
        return std::to_wstring(seconds / 60) + L":" + (seconds % 60 < 10 ? L"0" : L"") +
               std::to_wstring(seconds % 60);
    };
    const wchar_t* status = !snap.sessionAlive ? L"无会话"
        : snap.status == PlaybackStatus::Playing ? L"播放中"
        : snap.status == PlaybackStatus::Paused ? L"已暂停"
        : snap.status == PlaybackStatus::Stopped ? L"已停止" : L"状态切换中";
    std::wstring player = L"无播放器";
    if (snap.sessionAlive) {
        if (const auto* source = player_source::find(snap.player)) {
            player = source->displayName;
        } else {
            player = platform_icon::resolveSourceDisplayName(snap.sourceAppUserModelId);
            if (player == L"未知应用" && !snap.sourceAppUserModelId.empty())
                player = snap.sourceAppUserModelId;
        }
    }
    player += L" · ";
    player += status;
    const int line = LyricProvider::findLine(state.currentLyrics_, snap.positionMs);
    std::wstring format = L"未加载";
    if (!state.currentLyrics_.empty()) {
        const bool words = std::any_of(state.currentLyrics_.begin(), state.currentLyrics_.end(),
                                       [](const LyricLine& value) { return !value.chars.empty(); });
        format = words ? L"逐字" : L"逐行";
        if (state.currentLyricOrigin_ == LyricOrigin::LocalLrc)
            format += L" LRC";
        else if (state.currentLyricOrigin_ == LyricOrigin::LocalAudio)
            format += L" 音频内嵌";
        else if (state.currentLyricsFromLocal_)
            format += L" QRC";
        else if (state.currentLyricOrigin_ == LyricOrigin::Kugou)
            format += L" KRC";
        else if (state.currentLyricOrigin_ == LyricOrigin::QqMusic)
            format += words ? L" QRC" : L" LRC";
        else if (state.currentLyricOrigin_ == LyricOrigin::Netease)
            format += words ? L" YRC" : L" LRC";
        else if (state.currentLyricOrigin_ == LyricOrigin::Lrclib)
            format += words ? L" Lyricsfile" : L" LRC";
        if (state.currentHasTranslation_)
            format += L" · 翻译";
        if (state.currentHasRomanization_)
            format += L" · 罗马音";
    }
    const auto& info = state.currentLyricInfo_;
    const auto offset = info.timingOffsetMs + info.sourceOffsetMs;
    std::wstring matched = info.matchedTitle.empty() ? L"暂无候选元数据" : info.matchedTitle;
    if (!info.matchedArtist.empty())
        matched += L" · " + info.matchedArtist;
    if (info.matchedDurationMs > 0)
        matched += L" · " + timeText(info.matchedDurationMs);
    std::wstring method = state.currentLyrics_.empty() && !state.lyricLoading_ ? L"未命中"
        : state.lyricLoading_ ? L"加载中"
        : state.currentLyricsFromManual_ ? L"已保存的手动选择"
        : state.currentLyricsFromLocal_
            ? (state.currentLyricOrigin_ == LyricOrigin::LocalLrc     ? L"本地 LRC"
               : state.currentLyricOrigin_ == LyricOrigin::LocalAudio ? L"本地音频内嵌"
                                                                      : L"本地 QRC")
        : state.currentLyricsFromCache_ ? L"内存缓存" : L"在线请求";
    if (!state.lyricLoading_ && !state.currentLyricsFromManual_)
        method += L" · " + std::to_wstring(state.currentLyricLoadMs_) + L" ms";
    std::wstring diagnostics = L"SMTC  " + (snap.sourceAppUserModelId.empty()
        ? std::wstring(L"无会话") : snap.sourceAppUserModelId);
    if (state.lyricDiagnostics_.empty())
        diagnostics += L"\n本次没有在线请求记录";
    for (const wchar_t* provider : {L"酷狗音乐", L"QQ 音乐", L"LRCLIB", L"网易云音乐", L"汽水音乐"}) {
        int count = 0;
        int64_t elapsed = 0;
        long http = 0;
        int error = 0;
        for (const auto& record : state.lyricDiagnostics_) {
            if (record.source == provider) {
                ++count;
                elapsed += record.elapsedMs;
                if (record.networkError != 0 || record.httpStatus != 200) {
                    http = record.httpStatus;
                    error = record.networkError;
                }
            }
        }
        if (count > 0)
            diagnostics += L"\n" + std::wstring(provider) + L"  " + std::to_wstring(count) +
                L" 次 · " + std::to_wstring(elapsed) + L" ms · " +
                (http == 0 && error == 0 ? L"HTTP 200" : L"HTTP " + std::to_wstring(http) +
                 L" / curl " + std::to_wstring(error));
    }
    const wchar_t* mode = isRenderMode(RenderMode::Stopped) ? L"完全停止"
        : isRenderMode(RenderMode::Minimal) ? L"极简"
        : isRenderMode(RenderMode::Low) ? L"低渲染" : L"正常";
    runtimeLogger_.setDetails({
        {L"播放器", player},
        {L"进度与歌词行", timeText(snap.positionMs) + L" / " + timeText(snap.durationMs) +
            L" · " + std::to_wstring(line + 1) + L" / " + std::to_wstring(state.currentLyrics_.size()) + L" 行"},
        {L"歌词类型", format}, {L"匹配曲目", matched}, {L"获取方式", method},
        {L"时间调整", state.currentLyrics_.empty() ? L"未加载"
            : state.currentLyricsFromManual_ && !info.timingOffsetKnown ? L"已保存时间轴，偏移量未记录"
            : offset == 0 ? L"0 ms"
            : (offset < 0 ? std::wstring(L"提前 ") : std::wstring(L"延后 ")) +
              std::to_wstring(std::llabs(offset)) + L" ms"}
    }, std::move(diagnostics), mode);
    if (!snap.sessionAlive) {
        runtimeLogger_.setPlayback({}, {}, 0, false);
        runtimeLogger_.setLyricSource(L"未加载");
        runtimeLogger_.setCoverImage(nullptr);
        return;
    }

    const std::wstring title = snap.title.empty() ? playback_.state().currentTitle : snap.title;
    const std::wstring artist = snap.artist.empty() ? playback_.state().currentArtist : snap.artist;
    const int64_t durationMs =
        !snap.timelineStale
            ? (snap.durationMs > 0 ? snap.durationMs : playback_.state().currentDurationMs)
            : 0;
    runtimeLogger_.setPlayback(title, artist, durationMs, true);
    if (playback_.state().lyricLoading_)
        runtimeLogger_.setLyricSource(L"加载中…");
    else if (playback_.state().currentLyricsFromManual_)
        runtimeLogger_.setLyricSource(L"本地歌词（手动搜索）");
    else if (playback_.state().currentLyricsFromLocal_) {
        const auto origin = playback_.state().currentLyricOrigin_;
        runtimeLogger_.setLyricSource(
            origin == LyricOrigin::LocalLrc     ? L"本地歌词（自定义 LRC）"
            : origin == LyricOrigin::LocalAudio ? L"本地歌词（音频内嵌）"
                                                : L"本地歌词（QQ音乐本地）");
    }
    else if (!playback_.state().currentLyrics_.empty())
        runtimeLogger_.setLyricSource(onlineLyricSourceName(playback_.state().currentLyricOrigin_));
    else
        runtimeLogger_.setLyricSource(L"暂无歌词");
    std::shared_ptr<const std::vector<uint8_t>> cover;
    CoverSource coverSource = CoverSource::None;
    if (playback_.state().lastSmtcThumbnail && !playback_.state().lastSmtcThumbnail->empty()) {
        cover = playback_.state().lastSmtcThumbnail;
        coverSource = CoverSource::Smtc;
    } else if (playback_.state().lastCover_ && !playback_.state().lastCover_->empty()) {
        cover = playback_.state().lastCover_;
        coverSource = playback_.state().lastCoverSource_;
    }
    runtimeLogger_.setCoverImage(cover, coverSourceDisplayName(coverSource));
}

void App::refreshIdleWelcome() {
    const SmtcSnapshot snap = monitor.snapshot();
    if (settings_.idleTextMode_ != IdleTextMode::Welcome || !idle_.idleTextVisible() ||
        snap.sessionAlive || !settings_.idleCustomWelcome_.empty())
        return;
    if (presentation_.frame().idle.sentence == idle_.defaultIdleWelcome())
        return;
    publishPresentationFrame(snap, {.lyricsChanged = true});
}

IdleFrameInput App::buildIdleFrameInput(TaskbarViewMode effectiveMode,
                                        bool embeddedResourceMonitor,
                                        bool hasResourceMetrics, bool quotaVisible) const {
    IdleFrameInput input;
    input.presentation = idle_.currentIdlePresentation();
    input.summaryActive = idle_.state().startupTaskSummaryActive_;
    input.summaryPending = idle_.state().startupTaskSummaryPending_;
    input.tasksEnabled = idle_.tickTickEffectiveEnabled();
    // 额度行与系统指标共用状态区；仅有额度时也要让空闲栏保持可见。
    input.resourcesVisible =
        quotaVisible ||
        (hasResourceMetrics &&
         (effectiveMode == TaskbarViewMode::AppBar ||
          (effectiveMode == TaskbarViewMode::Embedded && embeddedResourceMonitor)));
    input.keepIdleVisible = effectiveMode == TaskbarViewMode::Immersive;
    if (input.summaryActive)
        input.summaryText = idle_.startupTaskSummaryText();
    return input;
}

const PresentationFrame& App::preparePresentationFrame(const SmtcSnapshot& snap,
                                                       PresentationUpdate update) {
    const MonitorTaskbarSettings primaryProfile =
        taskbarHost ? monitorSettingsForHost(taskbarHost.get())
                    : monitorSettingsForDevice(L"");
    const bool primaryVertical = taskbarHost ? taskbarHost->isVerticalTaskbar() : taskbarVertical_;
    const auto effectiveMode =
        display_rules::effectiveTaskbarViewMode(primaryProfile.viewMode, primaryVertical);
    const IdleFrameInput input =
        buildIdleFrameInput(effectiveMode, primaryProfile.embeddedResourceMonitor,
                            primaryProfile.hasAnyResourceMetric(), quota_.displayable());
    return presentation_.update(snap, settings_, playback_.state(), input, update);
}

void App::applyPresentationFrameToHost(TaskbarHost* host, const SmtcSnapshot& snap) {
    if (!host)
        return;
    if (snap.sessionAlive) {
        host->applyPresentationFrame(presentation_.frame());
        return;
    }
    const MonitorTaskbarSettings profile = monitorSettingsForHost(host);
    const auto effectiveMode =
        display_rules::effectiveTaskbarViewMode(profile.viewMode, host->isVerticalTaskbar());
    const IdleFrameInput input =
        buildIdleFrameInput(effectiveMode, profile.embeddedResourceMonitor,
                            profile.hasAnyResourceMetric(), quota_.displayable());
    host->applyPresentationFrame(presentation_.frameForView(snap, playback_.state(), input));
}

void App::publishPresentationFrame(const SmtcSnapshot& snap, PresentationUpdate update,
                                   bool notifyNewSong) {
    if (notifyNewSong)
        cancelSongToastCoverWait();
    // SMTC 状态/控制/封面事件只刷新帧字段；完整歌词仅在内容事务变化时复制。
    preparePresentationFrame(snap, update);
    forEachHost([&](TaskbarHost* host) { applyPresentationFrameToHost(host, snap); });
    // 设置窗口同步由状态变化显式声明，不再借用“歌词内容变化”的含义。
    if (update.refreshSettings)
        refreshSettingsDialog(true);
    const auto& media = presentation_.frame().media;
    if (media.title.empty()) {
        cancelSongToastCoverWait();
        if (songToast_)
            songToast_->setMedia(media);
        return;
    }
    const bool hasCover = media.thumbnail && !media.thumbnail->empty();
    if (notifyNewSong && !hasCover && trayHwnd)
        songToastCoverWaitArmed_ =
            SetTimer(trayHwnd, kTimerSongToastCover, kSongToastCoverWaitMs, nullptr) != 0;
    if (songToastCoverWaitArmed_ && !hasCover)
        return;
    if (notifyNewSong || songToastCoverWaitArmed_) {
        cancelSongToastCoverWait();
        notifySongToast();
    } else if (songToast_) {
        songToast_->setMedia(media);
    }
}

void App::syncHost(TaskbarHost* host) {
    if (!host)
        return;
    const SmtcSnapshot snap = monitor.snapshot();
    if (presentation_.frame().frameRevision == 0) {
        preparePresentationFrame(snap, {.lyricsChanged = true});
    }
    host->applyQuotaReadings(quota_.panelReadings());
    applyPresentationFrameToHost(host, snap);
}

void App::onControl(MediaControl c, uint64_t sessionGeneration, int64_t positionMs) {
    const wchar_t* name = L"unknown";
    switch (c) {
    case MediaControl::Seek:
        name = L"seek";
        monitor.seek(sessionGeneration, positionMs);
        break;
    case MediaControl::Prev:
        name = L"previous";
        monitor.skipPrevious(sessionGeneration);
        break;
    case MediaControl::PlayPause:
        name = L"play-pause";
        monitor.playPause(sessionGeneration);
        break;
    case MediaControl::Next:
        name = L"next";
        monitor.skipNext(sessionGeneration);
        break;
    case MediaControl::Repeat:
        name = L"repeat";
        monitor.cycleRepeatMode(sessionGeneration);
        break;
    case MediaControl::NextSource:
        name = L"next-source";
        monitor.cycleSource(1);
        break;
    case MediaControl::PreviousSource:
        name = L"previous-source";
        monitor.cycleSource(-1);
        break;
    }
    runtime_log::writef(L"[action][media-control] command=%s", name);
}

void App::pushAppVolume() {
    AppVolumeState state;
    state.requestRevision = appVolumeRequestRevision_;
    state.available = appVolume_.query(state.percent, state.muted);
    if (state.requestRevision == appVolumeState_.requestRevision &&
        state.available == appVolumeState_.available &&
        (!state.available ||
         (state.percent == appVolumeState_.percent && state.muted == appVolumeState_.muted)))
        return;
    appVolumeState_ = state;
    forEachHost([&](ILyricHost* host) { host->setAppVolume(state); });
}

void App::syncAppVolumeTarget(const SmtcSnapshot& snap) {
    appVolume_.setTarget(audioTargetForSnapshot(snap));
    pushAppVolume();
}

void App::onSmtcChanged() {
    SmtcSnapshot snap = monitor.snapshot();
    syncTaskbarView(snap);
    syncAppVolumeTarget(snap);
    const bool mediaSessionChanged = mediaSessionAlive_ != snap.sessionAlive;
    mediaSessionAlive_ = snap.sessionAlive;
    if (snap.sessionAlive) {
        // 媒体会话优先于启动任务播报；即使任务请求先返回，也不能覆盖已经在播放的歌词。
        idle_.cancelStartupSummary();
    }
    if (!snap.sessionAlive) {
        if (spectrumSessionAlive_)
            spectrum_.requestReconnect();
        spectrumSessionAlive_ = false;
        spectrumSessionPlaying_ = false;
        spectrumSessionKey_.clear();
        playback_.closeSession();
        idle_.refreshIdleQuote(false);
        publishPresentationFrame(snap,
                                 {.lyricsChanged = true, .refreshSettings = mediaSessionChanged});
        cancelSongToastCoverWait();
        if (songToast_)
            songToast_->hideImmediate();
        updateRuntimeLogState(snap);
        return;
    }
    const AudioSessionTarget audioTarget = audioTargetForSnapshot(snap);
    if (!audioTarget.empty()) {
        spectrum_.setTarget(audioTarget);
        // 把来源应用与曲目键一起纳入频谱会话键：不同应用播放同一首歌时也切换捕获目标。
        // 暂停态切源可能先落到进程扫描，恢复播放时再触发一次重连以切到活动音频会话。
        const std::wstring spectrumKey = snap.sourceAppUserModelId + L'|' + makeTrackKey(snap);
        const bool playing = snap.status == PlaybackStatus::Playing;
        if (!spectrumSessionAlive_ || spectrumSessionKey_ != spectrumKey ||
            (!spectrumSessionPlaying_ && playing))
            spectrum_.requestReconnect();
        spectrumSessionAlive_ = true;
        spectrumSessionPlaying_ = playing;
        spectrumSessionKey_ = spectrumKey;
    } else {
        // 无法映射到具体进程或包族的未知来源：停止旧捕获线程，避免任务栏出现静止频谱空壳。
        spectrumSessionAlive_ = false;
        spectrumSessionPlaying_ = false;
        spectrumSessionKey_.clear();
    }
    syncSpectrumWithMode();

    const std::wstring key = makeTrackKey(snap);
    if (manualSearchDialog && manualSearchDialog->isOpen() && !snap.title.empty()) {
        manualSearchDialog->setTargetSong(
            snap.title, snap.artist, snap.timelineStale ? 0 : snap.durationMs,
            snap.player, snap.player == SmtcPlayerType::NetEase ? snap.neteaseSongId : L"",
            snap.album, snap.sourceAppUserModelId);
        manualSearchTrackKey = key;
    }
    const TrackChange change = playback_.updateTrack(snap);
    const bool trackChanged = change.trackChanged;
    const bool durationOnlyTrackUpdate = change.durationOnly;
    const bool notifyNewSong = trackChanged && change.songIdentityChanged &&
                               settings_.songToastEnabled_ && !isMinimalRenderMode();
    publishPresentationFrame(snap,
                             {.animateTransition = !trackChanged,
                              .lyricsChanged = trackChanged,
                              .durationOnlyUpdate = durationOnlyTrackUpdate,
                              .refreshSettings = mediaSessionChanged || trackChanged},
                             notifyNewSong);
    tryExtractAlbumColor();
    updateRuntimeLogState(snap);
}

void App::onFrame() {
    syncTaskbarOrientation();
    SmtcSnapshot snap = monitor.snapshot();
    if (runtimeLogDialog && runtimeLogDialog->isOpen()) {
        static ULONGLONG lastDiagnosticUpdate = 0;
        const auto now = GetTickCount64();
        if (now - lastDiagnosticUpdate >= 500) {
            lastDiagnosticUpdate = now;
            updateRuntimeLogState(snap);
        }
    }
    if (snap.sessionAlive &&
        (idle_.state().startupTaskSummaryPending_ || idle_.state().startupTaskSummaryActive_)) {
        // 定时器也检查一次，避免媒体事件消息尚未出队时让启动播报多停留一帧。
        idle_.cancelStartupSummary();
        publishPresentationFrame(snap, {.lyricsChanged = true});
    }
    if (!snap.sessionAlive)
        return;
    if (!playback_.matchesCurrentTrack(snap))
        return;
    const PlaybackPatch playback = presentation_.updatePlayback(snap);
    forEachHost([&](ILyricHost* host) {
        // 提前切换显示行，让上下滚动动画在下一句真正开始前完成；
        // 补丁中的 actualPositionMs 仍是真实播放时间，避免逐字高亮跟着提前。
        host->applyPlaybackPatch(playback);
    });
    if (manualSearchDialog && manualSearchDialog->isOpen()) {
        manualSearchDialog->setPlaybackPosition(snap.positionMs);
    }
}

void App::tryExtractAlbumColor() {
    if (!playback_.extractAlbumColor())
        return;
    presentation_.updateAlbumColor(playback_.state().albumColor_);
    const SmtcSnapshot snap = monitor.snapshot();
    forEachHost([&](TaskbarHost* host) { applyPresentationFrameToHost(host, snap); });
    applyFontColors();
}
} // namespace lyrics_app
