#pragma once

#include "media/audio_target.h"
#include "media/smtc_monitor.h"

#include <windows.h>

#include <string>
#include <vector>

namespace platform_icon {

// 从已有 HICON 读取预乘 BGRA 像素；不接管图标句柄所有权。
bool readHiconPixels(HICON icon, std::vector<BYTE>& pixels, UINT& width, UINT& height);

// 从媒体来源标识对应的应用（AppsFolder AUMID、运行中进程或可执行路径）读取预乘 BGRA 图标像素。
bool readSourceIconPixels(const std::wstring& sourceAppUserModelId,
                          std::vector<BYTE>& pixels, UINT& width, UINT& height);

// 解析媒体来源的应用展示名（优先白名单，其次系统 AppsFolder 名称与 EXE 文件描述，兜底“未知应用”）。
std::wstring resolveSourceDisplayName(const std::wstring& sourceAppUserModelId);

// 解析媒体来源对应的本地可执行文件路径（可供菜单提取原生 HICON），找不到时返回空。
std::wstring resolveSourceExePath(const std::wstring& sourceAppUserModelId);

// 解析媒体来源对应的音频目标（进程名列表与可选的 UWP/MSIX PackageFamilyName），供音量与频谱复用。
AudioSessionTarget resolveSourceAudioTarget(SmtcPlayerType player,
                                            const std::wstring& sourceAppUserModelId);

// 从用户配置的 EXE 路径读取正常大小的图标与文件描述。
bool readExeIconPixels(const std::wstring& path, std::vector<BYTE>& pixels, UINT& width,
                       UINT& height);
bool readExeDisplayName(const std::wstring& path, std::wstring& name);

enum class AppLaunchResult {
    Failed,
    ShowRequested,
    ForegroundRequested,
    ForegroundDenied,
    LaunchRequested,
};
const wchar_t* appLaunchResultName(AppLaunchResult result);

// 激活媒体来源对应的已有播放器主窗口（含网易云托盘隐藏主窗口）。
AppLaunchResult launchSourceApp(const std::wstring& sourceAppUserModelId);

// 按配置的 EXE 路径查找已有窗口，必要时由系统打开该路径。
AppLaunchResult launchConfiguredExe(const std::wstring& path);

// 使用 Windows Shell 打开 URL 或已注册的自定义 URI 协议。
bool launchUri(const std::wstring& uri);

} // namespace platform_icon
