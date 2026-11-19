#include "media/player_source.h"

#include <cwchar>
#include <string_view>

namespace player_source {
namespace {

constexpr const wchar_t* kQqSessionIds[] = {L"QQMusic.exe", L"Tencent.QQMusic"};
constexpr const wchar_t* kQqProcesses[] = {L"QQMusic.exe"};
constexpr const wchar_t* kNeteaseSessionIds[] = {L"cloudmusic.exe", L"NeteaseBridge.exe"};
constexpr const wchar_t* kNeteaseProcesses[] = {L"cloudmusic.exe", L"NeteaseBridge.exe"};
constexpr const wchar_t* kSodaSessionIds[] = {L"汽水音乐"};
constexpr const wchar_t* kSodaProcesses[] = {L"SodaMusic.exe"};
constexpr const wchar_t* kSpotifySessionIds[] = {L"Spotify.exe", L"Spotify"};
constexpr const wchar_t* kSpotifyProcesses[] = {L"Spotify.exe"};
// 椒盐音乐（Salt Player for Windows）：微软商店版发布会话，AUMID 形如
// Sakawish.SaltPlayerforWindows_<发布者哈希>!SaltPlayerforWindows，
// 由 isSaltPlayerPackageSession 按前后缀匹配；Steam 版等非打包版本直接以进程名作为会话标识。
constexpr const wchar_t* kSaltSessionIds[] = {L"Salt Player for Windows.exe"};
constexpr const wchar_t* kSaltProcesses[] = {L"Salt Player for Windows.exe"};

constexpr Definition kDefinitions[] = {
    {SmtcPlayerType::QQMusic, L"QQ音乐", kQqSessionIds, false, kQqProcesses},
    {SmtcPlayerType::NetEase, L"网易云音乐", kNeteaseSessionIds, true, kNeteaseProcesses},
    {SmtcPlayerType::Soda, L"汽水音乐", kSodaSessionIds, false, kSodaProcesses},
    {SmtcPlayerType::Spotify, L"Spotify", kSpotifySessionIds, true, kSpotifyProcesses},
    {SmtcPlayerType::SaltPlayer, L"椒盐音乐", kSaltSessionIds, true, kSaltProcesses},
};

bool isSpotifyPackageSession(std::wstring_view sessionId) noexcept {
    constexpr std::wstring_view prefix = L"SpotifyAB.SpotifyMusic_";
    constexpr std::wstring_view suffix = L"!Spotify";
    if (sessionId.size() <= prefix.size() + suffix.size() ||
        _wcsnicmp(sessionId.data(), prefix.data(), prefix.size()) != 0 ||
        _wcsnicmp(sessionId.data() + sessionId.size() - suffix.size(),
                  suffix.data(), suffix.size()) != 0)
        return false;
    const auto publisher = sessionId.substr(prefix.size(),
                                            sessionId.size() - prefix.size() - suffix.size());
    return publisher.find_first_not_of(L"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789") ==
           std::wstring_view::npos;
}

constexpr std::wstring_view kSaltPackagePrefix = L"Sakawish.SaltPlayerforWindows_";
constexpr std::wstring_view kSaltPackageSuffix = L"!SaltPlayerforWindows";

// 商店包会话：<PackageFamilyName 主体>_<发布者哈希>!<AppId>。
// 只认发布者哈希由字母数字组成的形式，避免把任意同前缀的桌面会话误判。
bool isSaltPlayerPackageSession(std::wstring_view sessionId) noexcept {
    if (sessionId.size() <= kSaltPackagePrefix.size() + kSaltPackageSuffix.size() ||
        _wcsnicmp(sessionId.data(), kSaltPackagePrefix.data(), kSaltPackagePrefix.size()) != 0 ||
        _wcsnicmp(sessionId.data() + sessionId.size() - kSaltPackageSuffix.size(),
                  kSaltPackageSuffix.data(), kSaltPackageSuffix.size()) != 0)
        return false;
    const auto publisher = sessionId.substr(
        kSaltPackagePrefix.size(), sessionId.size() - kSaltPackagePrefix.size() -
                                       kSaltPackageSuffix.size());
    return !publisher.empty() &&
           publisher.find_first_not_of(L"abcdefghijklmnopqrstuvwxyz0123456789") ==
               std::wstring_view::npos;
}

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
    if (isSpotifyPackageSession(sessionId))
        return find(SmtcPlayerType::Spotify);
    if (isSaltPlayerPackageSession(sessionId))
        return find(SmtcPlayerType::SaltPlayer);
    return nullptr;
}

} // namespace player_source
