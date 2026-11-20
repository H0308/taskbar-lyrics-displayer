#pragma once

#include "media/smtc_monitor.h"

#include <span>
#include <string>

namespace player_source {

// 固定播放器信息集中在此模块；会话内容校验仍由各 SMTC 适配器负责。
struct Definition {
    SmtcPlayerType player;
    const wchar_t* displayName;
    std::span<const wchar_t* const> sessionIds;
    bool ignoreSessionIdCase;
    // 首项为真实播放器进程，供存活判断与频谱使用；其余为关联音频进程。
    std::span<const wchar_t* const> processNames;
};

const Definition* find(SmtcPlayerType player) noexcept;
const Definition* find(const std::wstring& sessionId) noexcept;

inline SmtcPlayerType resolvePlayerType(const std::wstring& sessionId) noexcept {
    if (const auto* source = find(sessionId))
        return source->player;
    return sessionId.empty() ? SmtcPlayerType::Unknown : SmtcPlayerType::Generic;
}

constexpr bool showsTrackInfoBeforeLyrics(SmtcPlayerType player) noexcept {
    return player == SmtcPlayerType::QQMusic || player == SmtcPlayerType::NetEase ||
           player == SmtcPlayerType::Soda || player == SmtcPlayerType::Spotify ||
           player == SmtcPlayerType::SaltPlayer || player == SmtcPlayerType::Generic;
}

// Spotify、椒盐音乐与 Generic 会话均无原生平台歌词接口：使用带专辑的曲目键、
// 读取 AlbumArtist、走通用在线多源歌词与 QQ／MusicBrainz 封面回退，无歌词时保持
// 展示曲目信息。Generic 会话以 SMTC 会话为准（无进程存活概念），丢失时走稳定窗口。
constexpr bool usesMetadataLyricPath(SmtcPlayerType player) noexcept {
    return player == SmtcPlayerType::Spotify || player == SmtcPlayerType::SaltPlayer ||
           player == SmtcPlayerType::Generic;
}

} // namespace player_source
