#include "media/track_identity.h"
#include "lyric/lyric_provider.h"

namespace {
constexpr wchar_t kSpotifyTrackPrefix[] = L"spotify|";
constexpr wchar_t kSaltPlayerTrackPrefix[] = L"saltplayer|";
}

std::wstring makeTrackKey(const SmtcSnapshot& snap) {
    if (snap.player == SmtcPlayerType::NetEase && !snap.neteaseSongId.empty())
        return L"netease|" + snap.neteaseSongId;
    // QQ 切歌窗口可能把上一首的时间线残留带进当前快照；它对展示和歌词匹配
    // 都是不可信的，不能让这段时长参与曲目身份，否则真实时长到达时会被误判为换歌。
    const int64_t durationMs = snap.timelineStale ? 0 : snap.durationMs;
    if (snap.player == SmtcPlayerType::Spotify)
        return LyricProvider::makeSpotifyKey(snap.title, snap.artist, snap.album, durationMs);
    if (snap.player == SmtcPlayerType::SaltPlayer)
        return LyricProvider::makeSaltPlayerKey(snap.title, snap.artist, snap.album, durationMs);
    const wchar_t* prefix = snap.player == SmtcPlayerType::Soda ? L"soda|" : L"qq|";
    return prefix + LyricProvider::makeKey(snap.title, snap.artist, durationMs);
}

bool snapshotMatchesTrackKey(const SmtcSnapshot& snap, const std::wstring& key) {
    if (!snap.sessionAlive)
        return false;
    // SMTC 的 Changing/Opened 过渡可能暂时没有完整文案；此时保留当前会话，
    // 待下一份完整快照再做曲目身份判断。
    if (snap.title.empty() && snap.neteaseSongId.empty()) {
        if (snap.player == SmtcPlayerType::Spotify)
            return key.empty() || key.starts_with(kSpotifyTrackPrefix);
        if (snap.player == SmtcPlayerType::SaltPlayer)
            return key.empty() || key.starts_with(kSaltPlayerTrackPrefix);
        return key.empty() || key.starts_with(L"qq|") || key.starts_with(L"soda|");
    }
    return key.empty() || makeTrackKey(snap) == key;
}
