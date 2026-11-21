#include "media/playback_controller.h"
#include "media/player_source.h"
#include "media/track_identity.h"
#include "logging/runtime_logger.h"
#include "util/dominant_color.h"
#include <utility>

namespace {
constexpr UINT_PTR kTimerLyricDebounce = 3;
constexpr UINT kLyricDebounceMs = 300;
constexpr UINT kSaltLyricDebounceMs = 200;

const wchar_t* coverSourceName(CoverSource source) {
    switch (source) {
    case CoverSource::None:
        return L"none";
    case CoverSource::Smtc:
        return L"smtc";
    case CoverSource::QqApi:
        return L"qq-api";
    case CoverSource::NeteaseApi:
        return L"netease-api";
    case CoverSource::MusicBrainz:
        return L"musicbrainz";
    default:
        return L"unknown";
    }
}

const wchar_t* playerName(SmtcPlayerType player) {
    switch (player) {
    case SmtcPlayerType::QQMusic:
        return L"QQ Music";
    case SmtcPlayerType::NetEase:
        return L"NetEase Music";
    case SmtcPlayerType::Soda:
        return L"Soda Music";
    case SmtcPlayerType::Spotify:
        return L"Spotify";
    case SmtcPlayerType::SaltPlayer:
        return L"Salt Player";
    case SmtcPlayerType::Generic:
        return L"Generic SMTC";
    default:
        return L"Unknown";
    }
}

const wchar_t* statusName(PlaybackStatus s) {
    switch (s) {
    case PlaybackStatus::Playing:
        return L"Playing";
    case PlaybackStatus::Paused:
        return L"Paused";
    case PlaybackStatus::Stopped:
        return L"Stopped";
    default:
        return L"Other";
    }
}
} // namespace

PlaybackController::PlaybackController(
    std::function<SmtcSnapshot()> snapshot,
    std::function<void(const SmtcSnapshot&, PresentationUpdate)> publish,
    std::function<void(const SmtcSnapshot&)> logState, std::function<void()> coverChanged)
    : snapshot_(std::move(snapshot)), publish_(std::move(publish)), logState_(std::move(logState)),
      coverChanged_(std::move(coverChanged)) {}

void PlaybackController::closeSession() {
    if (!state_.currentKey.empty() || state_.lastStatus != PlaybackStatus::Stopped)
        runtime_log::writef(L"[smtc] %s session closed", playerName(state_.lastPlayer_));
    state_.currentKey.clear();
    currentSourceAppId_.clear();
    currentSessionGeneration_ = 0;
    state_.currentTitle.clear();
    state_.currentArtist.clear();
    state_.currentDurationMs = 0;
    state_.lastStatus = PlaybackStatus::Stopped;
    state_.lyricLoading_ = false;
    cancelLyricDebounce();
    ++state_.requestGeneration_;
    releaseCurrentResources();
    state_.hasAlbumColor_ = false;
}

bool PlaybackController::matchesCurrentTrack(const SmtcSnapshot& snap) const {
    return snap.sessionGeneration == currentSessionGeneration_ &&
           snap.sourceAppUserModelId == currentSourceAppId_ &&
           snapshotMatchesTrackKey(snap, state_.currentKey);
}

TrackChange PlaybackController::updateTrack(const SmtcSnapshot& snap) {
    const SmtcPlayerType previousPlayer = state_.lastPlayer_;
    state_.lastPlayer_ = snap.player;
    const bool sourceChanged = snap.sessionGeneration != currentSessionGeneration_ ||
                               snap.sourceAppUserModelId != currentSourceAppId_;
    if (sourceChanged) {
        currentSourceAppId_ = snap.sourceAppUserModelId;
        currentSessionGeneration_ = snap.sessionGeneration;
        cancelLyricDebounce();
        ++state_.requestGeneration_;
    }
    const std::wstring key = makeTrackKey(snap);
    // 切入 Spotify 或椒盐音乐时即使曲目文案还没到，也要释放前一个播放器的歌词和封面。
    const bool enteringSampled = player_source::usesMetadataLyricPath(snap.player) &&
                                 snap.player != previousPlayer;
    const bool trackChanged =
        sourceChanged || (key != state_.currentKey && (!snap.title.empty() || enteringSampled));
    // 同一播放器、来源和歌曲文案下的时长补齐保留封面及专辑色。
    // 汽水和元数据路径播放器（Spotify／椒盐音乐）的曲目与时间线分别更新，已知时长也可能需要修正。
    const auto matchesPreviousDurationKey = [&]() {
        if (!player_source::usesMetadataLyricPath(snap.player))
            return true;
        SmtcSnapshot previousDurationSnap = snap;
        previousDurationSnap.durationMs = state_.currentDurationMs;
        previousDurationSnap.timelineStale = false;
        return state_.currentKey == makeTrackKey(previousDurationSnap);
    };
    const bool durationOnlyTrackUpdate =
        trackChanged && !sourceChanged && snap.player == previousPlayer && !snap.timelineStale &&
        (state_.currentDurationMs <= 0 || snap.player == SmtcPlayerType::Soda ||
         player_source::usesMetadataLyricPath(snap.player)) &&
        snap.durationMs > 0 && snap.title == state_.currentTitle &&
        snap.artist == state_.currentArtist &&
        matchesPreviousDurationKey();
    // QQ 的曲目键包含时长，切歌后时间线更新会让 trackChanged 再触发一次；
    // 切歌弹窗只认标题/艺术家身份变化，避免同一首歌弹两次。
    const bool songIdentityChanged = !snap.title.empty() && (snap.title != state_.currentTitle ||
                                                             snap.artist != state_.currentArtist);
    const bool newSmtcThumbnail =
        snap.thumbnail && !snap.thumbnail->empty() &&
        (!state_.lastSmtcThumbnail || state_.lastSmtcThumbnail != snap.thumbnail);
    // 同曲属性事件补齐的新封面也需要重新取色，相同图片无需重复取色。
    const bool nativeCoverChanged =
        (snap.player == SmtcPlayerType::Soda ||
         player_source::usesMetadataLyricPath(snap.player)) &&
        newSmtcThumbnail &&
        (!state_.lastCover_ || (snap.thumbnail != state_.lastCover_ &&
                                *snap.thumbnail != *state_.lastCover_));
    if (snap.status != state_.lastStatus) {
        runtime_log::writef(L"[smtc] %s status: %s", playerName(snap.player),
                            statusName(snap.status));
        state_.lastStatus = snap.status;
    }
    if (trackChanged) {
        // 时长补齐或修正仍需重建歌词请求，但媒体卡片保留当前封面和专辑色。
        releaseCurrentLyrics();
        if (!durationOnlyTrackUpdate)
            releaseCurrentCover();
        state_.currentKey = key;
        state_.currentTitle = snap.title;
        state_.currentArtist = snap.artist;
        state_.currentDurationMs = snap.timelineStale ? 0 : snap.durationMs;
        if (snap.player == SmtcPlayerType::NetEase) {
            runtime_log::writef(L"[smtc] %s track: %s - %s [%s] (%lld ms)", playerName(snap.player),
                                snap.title.c_str(), snap.artist.c_str(), snap.neteaseSongId.c_str(),
                                snap.durationMs);
        } else {
            runtime_log::writef(L"[smtc] %s track: %s - %s (%lld ms)", playerName(snap.player),
                                snap.title.c_str(), snap.artist.c_str(), snap.durationMs);
        }
        if (snap.thumbnail && !snap.thumbnail->empty()) {
            state_.lastCover_ = snap.thumbnail;
            state_.lastCoverSource_ = CoverSource::Smtc;
            logCoverCreated(CoverSource::Smtc, state_.lastCover_);
        }
        if (!durationOnlyTrackUpdate)
            state_.hasAlbumColor_ = false;
        // 跟随专辑开启时不回退配置色：沿用上一首的专辑色直到新封面取色完成，
        // 避免歌词加载窗口期配置色闪一下再切回专辑色
        state_.lyricLoading_ = !player_source::usesMetadataLyricPath(snap.player);
        if (snap.title.empty() && snap.neteaseSongId.empty() && sourceChanged) {
            state_.lyricLoading_ = false;
            state_.lyricRequestStale_ = false;
        } else if (snap.player == SmtcPlayerType::SaltPlayer) {
            cancelLyricDebounce();
            if (!snap.title.empty()) {
                // 椒盐音乐切歌时常先更新时长、几十毫秒后再更新歌名，短防抖合并同一批事件。
                state_.lyricLoading_ = true;
                state_.lyricRequestStale_ = false;
                ++state_.requestGeneration_;
                if (timerWindow_) {
                    state_.lyricDebounceDeadline_ = GetTickCount64() + 2000;
                    state_.lyricDebounceArmed_ =
                        SetTimer(timerWindow_, kTimerLyricDebounce, kSaltLyricDebounceMs, nullptr) !=
                        0;
                }
                if (!state_.lyricDebounceArmed_)
                    startLyricRequest(snap);
            } else {
                state_.lyricRequestStale_ = false;
                ++state_.requestGeneration_;
            }
        } else if (snap.player == SmtcPlayerType::Spotify ||
                   snap.player == SmtcPlayerType::Generic) {
            // Spotify 与 Generic 会话均来自 SMTC 快照，无切歌事件批合并问题，直接请求；
            // 不能落入下方 QQ 防抖分支（onLyricDebounce 只放行 QQ/椒盐）。
            cancelLyricDebounce();
            if (!snap.title.empty()) {
                startLyricRequest(snap);
            } else {
                state_.lyricRequestStale_ = false;
                ++state_.requestGeneration_;
            }
        } else if (snap.player == SmtcPlayerType::NetEase) {
            cancelLyricDebounce();
            startLyricRequest(snap);
            if (!snap.thumbnail || snap.thumbnail->empty())
                requestNeteaseCoverFallback(snap.neteaseSongId);
        } else if (snap.player == SmtcPlayerType::Soda) {
            cancelLyricDebounce();
            startLyricRequest(snap);
        } else if (timerWindow_) {
            // QQ：等 SMTC 切歌事件批合并后再请求；期间 state_.lyricLoading_ 保持 true，
            // 界面停留在「歌词加载中…」而不是闪「暂无歌词」。
            if (state_.lyricDebounceArmed_)
                KillTimer(timerWindow_, kTimerLyricDebounce);
            state_.lyricDebounceDeadline_ = GetTickCount64() + 8000;
            state_.lyricDebounceArmed_ =
                SetTimer(timerWindow_, kTimerLyricDebounce, kLyricDebounceMs, nullptr) != 0;
            if (!state_.lyricDebounceArmed_)
                startLyricRequest(snap); // 定时器创建失败时按原逻辑立即请求
        } else {
            startLyricRequest(snap);
        }
    } else {
        if (!snap.timelineStale && snap.durationMs > 0)
            state_.currentDurationMs = snap.durationMs;
        if (newSmtcThumbnail) {
            if (state_.lastSmtcThumbnail && !state_.lastSmtcThumbnail->empty()) {
                runtime_log::writef(L"[resource][event] cover-released source=smtc bytes=%zu",
                                    state_.lastSmtcThumbnail->size());
            } else if (state_.lastCover_ && !state_.lastCover_->empty() &&
                       state_.lastCover_ != snap.thumbnail) {
                runtime_log::writef(L"[resource][event] cover-released source=%s bytes=%zu",
                                    coverSourceName(state_.lastCoverSource_),
                                    state_.lastCover_->size());
            }
            logCoverCreated(CoverSource::Smtc, snap.thumbnail);
        }
        if (snap.thumbnail && !snap.thumbnail->empty()) {
            state_.lastCover_ = snap.thumbnail;
            state_.lastCoverSource_ = CoverSource::Smtc;
        }
    }
    state_.lastSmtcThumbnail = snap.thumbnail;
    if (nativeCoverChanged)
        state_.hasAlbumColor_ = false;
    // 时间线从残留恢复可信（stale 1→0）：若当前歌词请求是在不可信期间发出的
    // （等待超时被迫发出），用可信时长立即重发，覆盖可能已被时长过滤淘汰的结果。
    if (!trackChanged && snap.player == SmtcPlayerType::QQMusic && state_.lyricRequestStale_ &&
        !snap.timelineStale && !state_.lyricDebounceArmed_ && state_.lyricLoading_ &&
        state_.currentLyrics_.empty()) {
        startLyricRequest(snap);
    }

    return {trackChanged, durationOnlyTrackUpdate, songIdentityChanged};
}

void PlaybackController::applyManualLyrics(LyricResult result, const SmtcSnapshot& snap) {
    // 手动覆盖是新的内容事务，使仍在队列中的自动歌词/封面结果失效。
    ++state_.requestGeneration_;
    cancelLyricDebounce(); // 防止挂起的切歌防抖请求到点后覆盖手动选择
    releaseCurrentLyrics();
    state_.currentLyrics_ = std::move(result.lines);
    state_.currentLyricsFromLocal_ = false;
    state_.currentLyricsFromManual_ = true;
    state_.currentLyricOrigin_ = result.info.lyricOrigin;
    state_.currentLyricInfo_ = result.info;
    state_.lyricLoading_ = false;
    updateLyricCapabilities(state_.currentLyrics_);
    logLyricsCreated(L"manual");
    publish_(snap, {.animateTransition = true, .lyricsChanged = true, .refreshSettings = true});
    runtime_log::writef(L"[lyric] manual override applied: %s", state_.currentKey.c_str());
    logState_(snap);
    // 手动选择后同样兜底封面；网易云不依赖手动候选，仍使用当前播放歌曲 ID。
    if (!state_.lastSmtcThumbnail || state_.lastSmtcThumbnail->empty()) {
        if (snap.player == SmtcPlayerType::NetEase)
            requestNeteaseCoverFallback(snap.neteaseSongId);
        else if (snap.player == SmtcPlayerType::QQMusic ||
                 player_source::usesMetadataLyricPath(snap.player)) {
            if (!result.info.albummid.empty())
                requestQqCoverFallback(result.info.albummid, snap.player);
            else
                requestQqCoverInfoFallback(snap);
        }
    }
}

bool PlaybackController::extractAlbumColor() {
    if (state_.hasAlbumColor_ || !state_.lastCover_ || state_.lastCover_->empty())
        return false;
    auto color = extractDominantColor(*state_.lastCover_);
    if (!color)
        return false;
    state_.albumColor_ = *color;
    state_.hasAlbumColor_ = true;
    runtime_log::writef(L"[color] album dominant: #%02X%02X%02X : %s", GetRValue(*color),
                        GetGValue(*color), GetBValue(*color), state_.currentKey.c_str());
    return true;
}

void PlaybackController::logLyricsCreated(const wchar_t* source) {
    if (!state_.currentLyrics_.empty())
        runtime_log::writef(L"[resource][event] lyrics-created source=%s lines=%zu", source,
                            state_.currentLyrics_.size());
}

void PlaybackController::releaseCurrentLyrics() {
    if (!state_.currentLyrics_.empty()) {
        const wchar_t* source = state_.currentLyricsFromManual_  ? L"manual"
                                : state_.currentLyricsFromLocal_ ? L"local"
                                                                 : L"online";
        runtime_log::writef(L"[resource][event] lyrics-released source=%s lines=%zu", source,
                            state_.currentLyrics_.size());
    }
    state_.currentLyrics_.clear();
    state_.currentLyricsFromLocal_ = false;
    state_.currentLyricsFromManual_ = false;
    state_.currentLyricOrigin_ = LyricOrigin::Unknown;
    state_.currentLyricInfo_ = {};
    state_.currentLyricsFromCache_ = false;
    state_.currentLyricLoadMs_ = 0;
    state_.lyricDiagnostics_.clear();
    updateLyricCapabilities({});
}

void PlaybackController::logCoverCreated(CoverSource source,
                                         const std::shared_ptr<const std::vector<uint8_t>>& cover) {
    if (!cover || cover->empty())
        return;
    const wchar_t* sourceName = coverSourceName(source);
    runtime_log::writef(L"[resource][event] cover-created source=%s bytes=%zu", sourceName,
                        cover->size());
    runtime_log::writef(L"[cover] selected source=%s track=%s bytes=%zu", sourceName,
                        state_.currentKey.c_str(), cover->size());
}

CoverProvider::ReadyCallback PlaybackController::coverReadyCallback(CoverSource source,
                                                                   bool reportFailure) {
    const std::wstring key = state_.currentKey;
    const uint64_t generation = state_.requestGeneration_;
    const DWORD thread = mainThread_;
    return [thread, key, generation, source, reportFailure](
               std::shared_ptr<const std::vector<uint8_t>> cover) {
        if ((!cover || cover->empty()) && !reportFailure)
            return;
        auto* payload = new CoverPayload{key, generation, source, std::move(cover)};
        if (!PostThreadMessageW(thread, kMsgCoverReady, 1, reinterpret_cast<LPARAM>(payload)))
            delete payload;
    };
}

void PlaybackController::requestQqCoverFallback(const std::wstring& albummid,
                                                SmtcPlayerType player) {
    if (!albummid.empty())
        coverProvider.requestAsync(
            albummid, coverReadyCallback(CoverSource::QqApi,
                                         player_source::usesMetadataLyricPath(player)));
}

void PlaybackController::requestMusicBrainzCover(const SmtcSnapshot& snap) {
    // 椒盐音乐与 Spotify 同样缺少可靠的第三方专辑标识，按专辑名查询 MusicBrainz。
    if (!player_source::usesMetadataLyricPath(snap.player))
        return;
    const auto& artist = snap.albumArtist.empty() ? snap.artist : snap.albumArtist;
    coverProvider.requestMusicBrainzAsync(snap.album, artist,
                                         coverReadyCallback(CoverSource::MusicBrainz));
}

void PlaybackController::requestQqCoverInfoFallback(const SmtcSnapshot& snap) {
    const std::wstring key = state_.currentKey;
    const uint64_t generation = state_.requestGeneration_;
    const DWORD targetThread = mainThread_;
    const int64_t durationMs = snap.timelineStale ? 0 : snap.durationMs;
    provider.requestQqSongInfoAsync(
        snap.title, snap.artist, durationMs, [targetThread, key, generation](const SongInfo& info) {
            auto* payload = new QqCoverInfoPayload{key, generation, info.albummid};
            if (!PostThreadMessageW(targetThread, kMsgQqCoverInfoReady, 0,
                                    reinterpret_cast<LPARAM>(payload)))
                delete payload;
        });
}

void PlaybackController::requestNeteaseCoverFallback(const std::wstring& songId) {
    if (!songId.empty())
        coverProvider.requestNeteaseAsync(songId, coverReadyCallback(CoverSource::NeteaseApi));
}

void PlaybackController::releaseCurrentCover() {
    if (state_.lastCover_ && !state_.lastCover_->empty())
        runtime_log::writef(L"[resource][event] cover-released source=%s bytes=%zu",
                            coverSourceName(state_.lastCoverSource_), state_.lastCover_->size());
    else if (state_.lastSmtcThumbnail && !state_.lastSmtcThumbnail->empty())
        runtime_log::writef(L"[resource][event] cover-released source=smtc bytes=%zu",
                            state_.lastSmtcThumbnail->size());
    state_.lastCover_.reset();
    state_.lastCoverSource_ = CoverSource::None;
    state_.lastSmtcThumbnail.reset();
}

void PlaybackController::releaseCurrentResources() {
    releaseCurrentLyrics();
    releaseCurrentCover();
}

void PlaybackController::updateLyricCapabilities(const std::vector<LyricLine>& lines) {
    state_.currentHasTranslation_ = false;
    state_.currentHasRomanization_ = false;
    for (const auto& line : lines) {
        state_.currentHasTranslation_ = state_.currentHasTranslation_ || !line.translation.empty();
        state_.currentHasRomanization_ =
            state_.currentHasRomanization_ || !line.romanization.empty();
    }
}

void PlaybackController::startLyricRequest(const SmtcSnapshot& snap, bool forceOnline,
                                           bool forceLocal, bool persistOrder) {
    state_.lyricLoading_ = true;
    state_.currentLyricsFromLocal_ = false;
    state_.currentLyricsFromManual_ = false;
    state_.lyricRequestStale_ = snap.timelineStale;
    state_.lyricDiagnostics_.clear();
    state_.currentLyricLoadMs_ = 0;
    const int64_t durationMs = snap.timelineStale ? 0 : snap.durationMs;
    ++state_.requestGeneration_;
    logState_(snap);
    const std::wstring requestKey = state_.currentKey;
    const uint64_t generation = state_.requestGeneration_;
    auto postLyricResult = [threadId = mainThread_, requestKey, generation](LyricResult result) {
        auto* payload = new LyricPayload{requestKey, generation, std::move(result)};
        if (!PostThreadMessageW(threadId, kMsgLyricReady, 0, reinterpret_cast<LPARAM>(payload)))
            delete payload;
    };
    if (snap.player == SmtcPlayerType::NetEase) {
        provider.requestNeteaseAsync(snap.neteaseSongId, snap.title, snap.artist, snap.album,
                                     durationMs, postLyricResult);
    } else if (snap.player == SmtcPlayerType::Soda) {
        provider.requestSodaAsync(snap.title, snap.artist, snap.album, durationMs, postLyricResult);
    } else {
        using Source = LyricProvider::RequestSource;
        const Source source = snap.player == SmtcPlayerType::SaltPlayer ? Source::SaltPlayer
                              : snap.player == SmtcPlayerType::Spotify  ? Source::Spotify
                              : snap.player == SmtcPlayerType::Generic  ? Source::Generic
                              : forceLocal                              ? Source::PreferLocal
                              : forceOnline                             ? Source::PreferOnline
                                                                        : Source::Default;
        provider.requestAsync(snap.title, snap.artist, durationMs, postLyricResult, source,
                              persistOrder, snap.album);
    }
}

void PlaybackController::cancelLyricDebounce() {
    if (state_.lyricDebounceArmed_ && timerWindow_)
        KillTimer(timerWindow_, kTimerLyricDebounce);
    state_.lyricDebounceArmed_ = false;
}

void PlaybackController::onLyricDebounce() {
    if (!state_.lyricDebounceArmed_)
        return;
    state_.lyricDebounceArmed_ = false;
    SmtcSnapshot snap = snapshot_();
    if (!snap.sessionAlive || snap.sessionGeneration != currentSessionGeneration_ ||
        snap.sourceAppUserModelId != currentSourceAppId_ ||
        (snap.player != SmtcPlayerType::QQMusic && snap.player != SmtcPlayerType::SaltPlayer))
        return;
    if (snap.title.empty()) {
        if (snap.player == SmtcPlayerType::SaltPlayer) {
            if (timerWindow_ && GetTickCount64() < state_.lyricDebounceDeadline_) {
                state_.lyricDebounceArmed_ =
                    SetTimer(timerWindow_, kTimerLyricDebounce, kSaltLyricDebounceMs, nullptr) != 0;
                if (state_.lyricDebounceArmed_)
                    return;
            }
            state_.lyricLoading_ = false;
            publish_(snap, {});
            logState_(snap);
        }
        return;
    }
    if (makeTrackKey(snap) != state_.currentKey)
        return;
    if (snap.player == SmtcPlayerType::QQMusic && snap.timelineStale &&
        GetTickCount64() < state_.lyricDebounceDeadline_) {
        // 时间线仍是旧歌残留：durationMs 不可信， lyric 请求的时长过滤会
        // 把正确候选全部淘汰（失败会先闪「暂无歌词」）。继续等新时间线。
        if (timerWindow_)
            state_.lyricDebounceArmed_ =
                SetTimer(timerWindow_, kTimerLyricDebounce, kLyricDebounceMs, nullptr) != 0;
        return;
    }
    startLyricRequest(snap);
}

void PlaybackController::onLyricReady(std::unique_ptr<LyricPayload> payload) {
    if (!payload || payload->key != state_.currentKey ||
        payload->requestGeneration != state_.requestGeneration_)
        return;
    SmtcSnapshot snap = snapshot_();
    if (!matchesCurrentTrack(snap))
        return;

    state_.lyricLoading_ = false;
    std::wstring coverAlbummid;
    state_.currentLyricLoadMs_ = payload->result.elapsedMs;
    state_.lyricDiagnostics_ = payload->result.diagnostics;
    if (payload->result.ok) {
        state_.currentLyrics_ = std::move(payload->result.lines);
        state_.currentLyricsFromLocal_ =
            snap.player == SmtcPlayerType::QQMusic && payload->result.fromLocal;
        state_.currentLyricsFromManual_ = payload->result.fromManual;
        state_.currentLyricOrigin_ = payload->result.info.lyricOrigin;
        state_.currentLyricInfo_ = payload->result.info;
        state_.currentLyricsFromCache_ = payload->result.fromCache;
        updateLyricCapabilities(state_.currentLyrics_);
        logLyricsCreated(state_.currentLyricsFromManual_  ? L"manual"
                         : state_.currentLyricsFromLocal_ ? L"local"
                                                          : L"online");
        runtime_log::writef(L"[lyric] loaded %zu lines: %s", state_.currentLyrics_.size(),
                            state_.currentKey.c_str());
        coverAlbummid = std::move(payload->result.info.albummid);
    } else {
        if (state_.lyricRequestStale_) {
            // 时间线不可信期间发出的请求：失败大概率是旧时长触发候选的 15 秒
            // 时长过滤，不能据此判定「暂无歌词」。保持「加载中」，等新时间线
            // 可信后由 onSmtcChanged 的重试分支用正确时长重发。
            state_.lyricLoading_ = true;
            publish_(snap, {.refreshSettings = true});
            logState_(snap);
            return;
        }
        releaseCurrentLyrics();
        state_.currentLyricLoadMs_ = payload->result.elapsedMs;
        state_.lyricDiagnostics_ = payload->result.diagnostics;
        runtime_log::writef(L"[lyric] not found: %s", state_.currentKey.c_str());
    }
    // SMTC 没有封面时，使用已匹配的专辑标识或单独查询曲目信息。
    if ((snap.player == SmtcPlayerType::QQMusic ||
         player_source::usesMetadataLyricPath(snap.player)) &&
        (!snap.thumbnail || snap.thumbnail->empty()) &&
        (!state_.lastSmtcThumbnail || state_.lastSmtcThumbnail->empty()) &&
        (!state_.lastCover_ || state_.lastCover_->empty())) {
        if (!coverAlbummid.empty())
            requestQqCoverFallback(coverAlbummid, snap.player);
        else
            requestQqCoverInfoFallback(snap);
    }
    publish_(snap, {.animateTransition = true, .lyricsChanged = true, .refreshSettings = true});
    logState_(snap);
}

void PlaybackController::onQqCoverInfoReady(std::unique_ptr<QqCoverInfoPayload> payload) {
    if (!payload || payload->key != state_.currentKey ||
        payload->requestGeneration != state_.requestGeneration_)
        return;
    SmtcSnapshot snap = snapshot_();
    if (!matchesCurrentTrack(snap) ||
        (snap.thumbnail && !snap.thumbnail->empty()) ||
        (state_.lastSmtcThumbnail && !state_.lastSmtcThumbnail->empty()) ||
        (state_.lastCover_ && !state_.lastCover_->empty()))
        return;
    if (payload->albummid.empty()) {
        runtime_log::writef(L"[cover][QQ] no albummid from song-info: %s",
                            state_.currentKey.c_str());
        requestMusicBrainzCover(snap);
        return;
    }
    requestQqCoverFallback(payload->albummid, snap.player);
}

void PlaybackController::onCoverReady(std::unique_ptr<CoverPayload> payload) {
    if (!payload || payload->key != state_.currentKey ||
        payload->requestGeneration != state_.requestGeneration_)
        return; // 已切歌或重新加载，丢弃过期封面
    SmtcSnapshot snap = snapshot_();
    if (!matchesCurrentTrack(snap))
        return;
    if ((snap.thumbnail && !snap.thumbnail->empty()) ||
        (state_.lastSmtcThumbnail && !state_.lastSmtcThumbnail->empty()))
        return; // SMTC 已提供有效封面，优先使用
    if (!payload->cover || payload->cover->empty()) {
        if (payload->source == CoverSource::QqApi && !state_.lastCover_)
            requestMusicBrainzCover(snap);
        return;
    }
    if (payload->source == CoverSource::MusicBrainz && state_.lastCover_)
        return;
    state_.lastCover_ = payload->cover;
    state_.lastCoverSource_ = payload->source;
    logCoverCreated(payload->source, state_.lastCover_);
    publish_(snap, {.animateTransition = true});
    coverChanged_();
    logState_(snap);
}

void PlaybackController::reloadCurrentQqLyrics(bool forceOnline, bool forceLocal,
                                               bool persistOrder) {
    const SmtcSnapshot snap = snapshot_();
    if (!snap.sessionAlive || snap.sessionGeneration != currentSessionGeneration_ ||
        snap.sourceAppUserModelId != currentSourceAppId_ ||
        snap.player != SmtcPlayerType::QQMusic || state_.currentKey.empty() ||
        snap.title.empty() || snap.timelineStale || makeTrackKey(snap) != state_.currentKey ||
        (forceOnline && !state_.currentLyricsFromLocal_) ||
        (forceLocal && (state_.currentLyricsFromLocal_ || state_.currentLyricsFromManual_)))
        return;

    runtime_log::writef(L"[action][lyrics] reload force-online=%d force-local=%d persist-order=%d",
                        forceOnline ? 1 : 0, forceLocal ? 1 : 0, persistOrder ? 1 : 0);
    cancelLyricDebounce();
    releaseCurrentLyrics();
    state_.lyricLoading_ = true;
    startLyricRequest(snap, forceOnline, forceLocal, persistOrder);
    publish_(snap, {.lyricsChanged = true, .refreshSettings = true});
}
