#pragma once

namespace app_info {

// 发布新版本时同步修改这里，并使用相同的版本号创建 GitHub/Gitee Release 标签。
inline constexpr wchar_t kVersion[] = L"2.13.0";
inline constexpr wchar_t kProjectUrl[] =
    L"https://github.com/H0308/taskbar-lyrics-displayer";
inline constexpr wchar_t kGiteeProjectUrl[] =
    L"https://gitee.com/EPSDA/taskbar-lyrics-displayer";
inline constexpr wchar_t kGiteeLatestReleasePage[] =
    L"https://gitee.com/EPSDA/taskbar-lyrics-displayer/releases";
inline constexpr wchar_t kLatestReleasePage[] =
    L"https://github.com/H0308/taskbar-lyrics-displayer/releases/latest";
inline constexpr char kLatestReleaseApi[] =
    "https://api.github.com/repos/H0308/taskbar-lyrics-displayer/releases/latest";
inline constexpr char kGiteeLatestReleaseApi[] =
    "https://gitee.com/api/v5/repos/EPSDA/taskbar-lyrics-displayer/releases/latest";

} // namespace app_info
