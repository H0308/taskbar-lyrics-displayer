#pragma once
#include "model/presentation_update.h"
#include "app/messages.h"
#include "media/smtc_monitor.h"
#include "lyric/cover_provider.h"
#include "lyric/lyric_provider.h"
#include <functional>

enum class CoverSource { None, Smtc, QqApi, NeteaseApi, MusicBrainz };

struct CoverPayload {
    std::wstring key;
    uint64_t requestGeneration = 0;
    CoverSource source = CoverSource::Smtc;
    std::shared_ptr<const std::vector<uint8_t>> cover;
};

struct QqCoverInfoPayload {
    std::wstring key;
    uint64_t requestGeneration = 0;
    std::wstring albummid;
};

struct LyricPayload {
    std::wstring key;
    uint64_t requestGeneration = 0;
    LyricResult result;
};

struct PlaybackState {
    std::wstring currentKey;
    std::wstring currentTitle;
    std::wstring currentArtist;
    int64_t currentDurationMs = 0;
    PlaybackStatus lastStatus = PlaybackStatus::Stopped;
    SmtcPlayerType lastPlayer_ = SmtcPlayerType::Unknown;
    bool lyricLoading_ = false;
    bool lyricDebounceArmed_ = false;     // QQ / 椒盐音乐切歌请求防抖定时器已挂起
    ULONGLONG lyricDebounceDeadline_ = 0; // 防抖续等截止时刻（QQ 等新时间线，椒盐音乐等空标题恢复）
    bool lyricRequestStale_ = false;      // 当前在途/最近一次歌词请求是否发于时间线不可信期间
    std::vector<LyricLine> currentLyrics_;
    bool currentLyricsFromLocal_ = false;
    bool currentLyricsFromManual_ = false;
    LyricOrigin currentLyricOrigin_ = LyricOrigin::Unknown;
    SongInfo currentLyricInfo_;
    bool currentLyricsFromCache_ = false;
    int64_t currentLyricLoadMs_ = 0;
    std::vector<LyricRequestDiagnostic> lyricDiagnostics_;
    uint64_t requestGeneration_ = 0;
    std::shared_ptr<const std::vector<uint8_t>> lastSmtcThumbnail;
    bool currentHasTranslation_ = false;
    bool currentHasRomanization_ = false;
    bool hasAlbumColor_ = false; // 当前曲目是否已提取到主色调（切歌后失效）
    COLORREF albumColor_ = RGB(49, 194, 124);
    std::shared_ptr<const std::vector<uint8_t>>
        lastCover_; // 当前曲目有效封面（SMTC 优先，API 兜底）
    CoverSource lastCoverSource_ = CoverSource::None;
};

struct TrackChange {
    bool trackChanged = false;
    bool durationOnly = false;
    bool songIdentityChanged = false;
};

// 独占当前曲目的状态及请求代次；窗口只能读取快照或提交明确的操作。
class PlaybackController {
public:
    PlaybackController(std::function<SmtcSnapshot()> snapshot,
                       std::function<void(const SmtcSnapshot&, PresentationUpdate)> publish,
                       std::function<void(const SmtcSnapshot&)> logState,
                       std::function<void()> coverChanged);
    const PlaybackState& state() const { return state_; }
    LyricProvider& lyricProvider() { return provider; }
    void setTimerWindow(HWND window) { timerWindow_ = window; }
    TrackChange updateTrack(const SmtcSnapshot& snap);
    bool matchesCurrentTrack(const SmtcSnapshot& snap) const;
    void closeSession();
    void applyManualLyrics(LyricResult result, const SmtcSnapshot& snap);
    bool extractAlbumColor();
    void logLyricsCreated(const wchar_t* source);
    void releaseCurrentLyrics();
    void logCoverCreated(CoverSource source,
                         const std::shared_ptr<const std::vector<uint8_t>>& cover);
    void releaseCurrentCover();
    void releaseCurrentResources();
    void updateLyricCapabilities(const std::vector<LyricLine>& lines);
    void startLyricRequest(const SmtcSnapshot& snap, bool forceOnline = false,
                           bool forceLocal = false, bool persistOrder = false);
    void cancelLyricDebounce();
    void onLyricDebounce();
    void onLyricReady(std::unique_ptr<LyricPayload> payload);
    void onQqCoverInfoReady(std::unique_ptr<QqCoverInfoPayload> payload);
    void onCoverReady(std::unique_ptr<CoverPayload> payload);
    void reloadCurrentQqLyrics(bool forceOnline = false, bool forceLocal = false,
                               bool persistOrder = false);

private:
    CoverProvider::ReadyCallback coverReadyCallback(CoverSource source, bool reportFailure = false);
    void requestQqCoverFallback(const std::wstring& albummid, SmtcPlayerType player);
    void requestQqCoverInfoFallback(const SmtcSnapshot& snap);
    void requestNeteaseCoverFallback(const std::wstring& songId);
    void requestMusicBrainzCover(const SmtcSnapshot& snap);

    PlaybackState state_;
    std::wstring currentSourceAppId_;
    DWORD mainThread_ = GetCurrentThreadId();
    HWND timerWindow_ = nullptr;
    std::function<SmtcSnapshot()> snapshot_;
    std::function<void(const SmtcSnapshot&, PresentationUpdate)> publish_;
    std::function<void(const SmtcSnapshot&)> logState_;
    std::function<void()> coverChanged_;
    LyricProvider provider;
    CoverProvider coverProvider;
};
