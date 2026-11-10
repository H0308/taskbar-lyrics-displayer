#include "app/presentation_model.h"
#include "settings/app_settings.h"
#include "media/playback_controller.h"

namespace {
constexpr int64_t kLyricTransitionLeadMs = 100;

int64_t lyricSelectionPosition(const SmtcSnapshot& snap, const std::vector<LyricLine>& lines) {
    // 汽水首句前显示歌曲信息，到真实行起点才交接；后续行继续使用既有提前量。
    if (snap.player == SmtcPlayerType::Soda && !lines.empty() && snap.positionMs < lines.front().ms)
        return snap.positionMs;
    return snap.positionMs + kLyricTransitionLeadMs;
}

const wchar_t* notRunningStatus(const PlaybackState& playback) {
    if (playback.lastPlayer_ == SmtcPlayerType::Soda)
        return L"汽水音乐未运行";
    return playback.lastPlayer_ == SmtcPlayerType::NetEase ? L"网易云音乐未运行" : L"QQ 音乐未运行";
}

OverlayMediaInfo makeMediaInfo(const SmtcSnapshot& snap, const PlaybackState& playback,
                               const PresentationFrame& previous) {
    OverlayMediaInfo mi;
    const bool retainPrevious = snap.sessionAlive && snap.title.empty() && snap.artist.empty() &&
                                previous.frameRevision != 0 &&
                                previous.trackKey == playback.currentKey;
    if (retainPrevious) {
        // SMTC 的过渡事件可能暂时没有完整文案，保留当前曲目的媒体字段，
        // 避免用一份不完整快照拼出“旧歌词 + 空标题”的中间画面。
        mi = previous.media;
    } else {
        mi.title = snap.title.empty() ? playback.currentTitle : snap.title;
        mi.artist = snap.artist.empty() ? playback.currentArtist : snap.artist;
        if (!snap.album.empty()) {
            mi.artist += L" · ";
            mi.artist += snap.album;
        }
        mi.sourceAppUserModelId = snap.sourceAppUserModelId;
    }
    if (snap.thumbnail && !snap.thumbnail->empty())
        mi.thumbnail = snap.thumbnail;
    else if (playback.lastSmtcThumbnail && !playback.lastSmtcThumbnail->empty())
        mi.thumbnail = playback.lastSmtcThumbnail;
    else
        mi.thumbnail = playback.lastCover_;
    mi.hasDominantColor = playback.hasAlbumColor_;
    mi.dominantColor = playback.albumColor_;
    // QQ 切歌时旧时间线会暂时残留，不能把上一首的总时长带进媒体卡片。
    mi.durationMs = !snap.timelineStale && snap.durationMs > 0 ? snap.durationMs : 0;
    mi.canPrev = snap.canPrev;
    mi.canPlayPause = snap.canPlayPause;
    mi.canNext = snap.canNext;
    mi.playing = snap.status == PlaybackStatus::Playing;
    return mi;
}

DisplayScene displaySceneFor(const SmtcSnapshot& snap, const AppSettings& settings,
                             const PlaybackState& playback, bool idleVisible) {
    if (!snap.sessionAlive)
        return idleVisible ? DisplayScene::Idle : DisplayScene::NoPlayback;
    if (playback.lyricLoading_)
        return DisplayScene::Searching;
    if (!playback.currentLyrics_.empty())
        return DisplayScene::Lyrics;
    return settings.spectrumOn_ ? DisplayScene::Spectrum : DisplayScene::Message;
}

void fillPresentationFrame(PresentationFrame& frame, const SmtcSnapshot& snap,
                           bool animateTransition, bool copyLyrics, bool durationOnlyUpdate,
                           const AppSettings& settings, const PlaybackState& playback,
                           const IdleFrameInput& idle) {
    frame.requestGeneration = playback.requestGeneration_;
    frame.trackKey = playback.currentKey;
    const bool idleVisible = idle.presentation.hasContent() || idle.resourcesVisible ||
                             idle.summaryActive || (idle.summaryPending && idle.tasksEnabled);
    frame.scene = displaySceneFor(snap, settings, playback, idleVisible);
    frame.media = makeMediaInfo(snap, playback, frame);
    frame.idle = idle.presentation;
    if (copyLyrics)
        frame.lyrics = playback.currentLyrics_;
    frame.actualPositionMs = snap.positionMs;
    frame.lineSelectionPositionMs = lyricSelectionPosition(snap, frame.lyrics);
    frame.currentLine = LyricProvider::findLine(frame.lyrics, frame.lineSelectionPositionMs);
    frame.visible = snap.sessionAlive || idleVisible;
    frame.animateTransition = animateTransition;
    frame.durationOnlyUpdate = durationOnlyUpdate;
    const bool showStartupTaskSummary =
        idle.summaryActive && !snap.sessionAlive && frame.scene == DisplayScene::Idle;
    const bool showStartupTaskLoading = idle.summaryPending && idle.tasksEnabled &&
                                        !snap.sessionAlive && frame.scene == DisplayScene::Idle;
    frame.statusTextOneShot = showStartupTaskSummary;
    if (showStartupTaskSummary) {
        frame.statusText = idle.summaryText;
        frame.statusTextOneShot = true;
    } else if (showStartupTaskLoading) {
        frame.statusText = L"正在同步今日任务…";
    } else if (frame.scene == DisplayScene::Idle) {
        if (idle.presentation.showText)
            frame.statusText = frame.idle.sentence;
        else if (!idle.resourcesVisible && idle.presentation.quickStartEnabled)
            frame.statusText = L"快捷启动";
        else if (!idle.resourcesVisible && idle.presentation.todayTasksEnabled)
            frame.statusText = L"今日任务";
        else
            frame.statusText.clear();
    } else if (!snap.sessionAlive)
        frame.statusText = notRunningStatus(playback);
    else if (playback.lyricLoading_)
        frame.statusText = L"歌词加载中…";
    else if (playback.currentLyrics_.empty())
        frame.statusText = playback.currentKey.empty() ? L"等待播放…" : L"暂无歌词";
    else if (snap.player == SmtcPlayerType::Soda) {
        // 独立保留前奏展示文案，不向歌词插入伪造行，也不混入专辑字段。
        frame.statusText = snap.title.empty() ? playback.currentTitle : snap.title;
        const auto& artist = snap.artist.empty() ? playback.currentArtist : snap.artist;
        if (!artist.empty())
            frame.statusText += L" - " + artist;
    } else
        frame.statusText.clear();
}
} // namespace

const PresentationFrame& PresentationModel::update(const SmtcSnapshot& snap,
                                                   const AppSettings& settings,
                                                   const PlaybackState& playback,
                                                   const IdleFrameInput& idle,
                                                   PresentationUpdate update) {
    fillPresentationFrame(frame_, snap, update.animateTransition,
                          revision_ == 0 || update.lyricsChanged, update.durationOnlyUpdate,
                          settings, playback, idle);
    frame_.frameRevision = ++revision_;
    return frame_;
}

PlaybackPatch PresentationModel::updatePlayback(const SmtcSnapshot& snap) {
    frame_.actualPositionMs = snap.positionMs;
    frame_.lineSelectionPositionMs = lyricSelectionPosition(snap, frame_.lyrics);
    frame_.currentLine = LyricProvider::findLine(frame_.lyrics, frame_.lineSelectionPositionMs);
    frame_.media.playing = snap.status == PlaybackStatus::Playing;
    PlaybackPatch playback;
    playback.frameRevision = frame_.frameRevision;
    playback.requestGeneration = frame_.requestGeneration;
    playback.actualPositionMs = frame_.actualPositionMs;
    playback.lineSelectionPositionMs = frame_.lineSelectionPositionMs;
    playback.currentLine = frame_.currentLine;
    playback.playing = frame_.media.playing;
    return playback;
}

const PresentationFrame& PresentationModel::updateAlbumColor(COLORREF color) {
    frame_.media.hasDominantColor = true;
    frame_.media.dominantColor = color;
    frame_.frameRevision = ++revision_;
    frame_.animateTransition = false;
    return frame_;
}
