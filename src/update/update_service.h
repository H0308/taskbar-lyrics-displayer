#pragma once

#include "app_info.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace update_service {

enum class UpdateState {
    Current,
    Available,
    NoRelease,
    Failed,
};

enum class UpdateSource {
    GitHub,
    Gitee,
};

struct ReleaseInfo {
    std::wstring version;
    std::wstring publishedAt;
    std::wstring body;
    std::wstring releaseUrl;
    std::wstring installerUrl;
    std::wstring installerName;
    int64_t installerSize = 0;
};

struct UpdateCheckResult {
    uint64_t requestId = 0;
    UpdateState state = UpdateState::Failed;
    std::wstring version;
    std::wstring releaseUrl;
    std::wstring detail;
    std::optional<ReleaseInfo> release;
};

struct DownloadCallbacks {
    std::function<void(int percent)> onProgress;
    std::function<void(bool success, bool cancelled, std::wstring path, std::wstring detail)>
        onFinished;
};

const wchar_t* updateStateName(UpdateState state);
const wchar_t* updateSourceName(UpdateSource source);
const char* updateSourceApi(UpdateSource source);
const wchar_t* updateSourceReleasePage(UpdateSource source);
int updateSourceIndex(UpdateSource source);
UpdateSource updateSourceFromIndex(int index);

std::optional<std::vector<int>> parseVersion(const std::wstring& value);
int compareVersions(const std::vector<int>& lhs, const std::vector<int>& rhs);
std::wstring displayVersion(const std::wstring& version);
std::wstring displayReleaseDate(const std::wstring& value);

std::wstring selectHttpsProxy(const std::wstring& proxyList);
std::string proxyUrlForCurl(const std::wstring& value);
std::string systemProxyForUrl(const std::wstring& url);

// 解析 GitHub/Gitee Release JSON 响应并比较版本号，返回结构化检查结果。
UpdateCheckResult parseLatestReleaseResponse(uint64_t requestId, UpdateSource source,
                                             long responseCode, const std::string& body,
                                             const std::wstring& currentVersion = app_info::kVersion);

uint64_t nextRequestId();
std::wstring makeTempInstallerPath(const std::wstring& installerName);

// 异步请求最新版本信息并在后台线程回调。
void requestLatestRelease(uint64_t requestId, UpdateSource source,
                          std::function<void(UpdateCheckResult)> onResult);

// 异步下载安装器到指定路径并报告进度与最终结果；cancelled 置位后主动中断并清理临时文件。
void downloadInstaller(std::wstring url, std::wstring outputPath, int64_t expectedBytes,
                       std::wstring sourceName, std::shared_ptr<std::atomic_bool> cancelled,
                       DownloadCallbacks callbacks);

} // namespace update_service
