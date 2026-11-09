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

} // namespace player_source
