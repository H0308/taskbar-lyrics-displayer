#include "media/player_source.h"

#include <cwchar>

namespace player_source {
namespace {

constexpr const wchar_t* kQqSessionIds[] = {L"QQMusic.exe", L"Tencent.QQMusic"};
constexpr const wchar_t* kQqProcesses[] = {L"QQMusic.exe"};
constexpr const wchar_t* kNeteaseSessionIds[] = {L"cloudmusic.exe", L"NeteaseBridge.exe"};
constexpr const wchar_t* kNeteaseProcesses[] = {L"cloudmusic.exe", L"NeteaseBridge.exe"};
constexpr const wchar_t* kSodaSessionIds[] = {L"汽水音乐"};
constexpr const wchar_t* kSodaProcesses[] = {L"SodaMusic.exe"};

constexpr Definition kDefinitions[] = {
    {SmtcPlayerType::QQMusic, L"QQ音乐", kQqSessionIds, false, kQqProcesses},
    {SmtcPlayerType::NetEase, L"网易云音乐", kNeteaseSessionIds, true, kNeteaseProcesses},
    {SmtcPlayerType::Qishui, L"汽水音乐", kSodaSessionIds, false, kSodaProcesses},
};

} // namespace

const Definition* find(SmtcPlayerType player) noexcept {
    for (const auto& definition : kDefinitions) {
        if (definition.player == player)
            return &definition;
    }
    return nullptr;
}

const Definition* find(const std::wstring& sessionId) noexcept {
    for (const auto& definition : kDefinitions) {
        for (const wchar_t* candidate : definition.sessionIds) {
            const bool matches = definition.ignoreSessionIdCase
                                     ? _wcsicmp(sessionId.c_str(), candidate) == 0
                                     : sessionId == candidate;
            if (matches)
                return &definition;
        }
    }
    return nullptr;
}

} // namespace player_source
