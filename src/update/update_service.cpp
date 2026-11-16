#include "update/update_service.h"

#include <windows.h>
#include <winhttp.h>

#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <thread>
#include <utility>

namespace update_service {
namespace {

std::atomic<uint64_t> gNextRequestId{0};

std::wstring wideOf(const std::string& s) {
    if (s.empty())
        return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0)
        return {};
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

std::string utf8Of(const std::wstring& w) {
    if (w.empty())
        return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()),
                                         nullptr, 0, nullptr, nullptr);
    if (size <= 0)
        return {};
    std::string s(size, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), size,
                        nullptr, nullptr);
    return s;
}

std::wstring trimProxyValue(std::wstring value) {
    const size_t begin = value.find_first_not_of(L" \t");
    if (begin == std::wstring::npos)
        return {};
    const size_t end = value.find_last_not_of(L" \t");
    return value.substr(begin, end - begin + 1);
}

void freeProxyConfig(WINHTTP_CURRENT_USER_IE_PROXY_CONFIG& config) {
    if (config.lpszAutoConfigUrl)
        GlobalFree(config.lpszAutoConfigUrl);
    if (config.lpszProxy)
        GlobalFree(config.lpszProxy);
    if (config.lpszProxyBypass)
        GlobalFree(config.lpszProxyBypass);
    config = {};
}

void freeProxyInfo(WINHTTP_PROXY_INFO& info) {
    if (info.lpszProxy)
        GlobalFree(info.lpszProxy);
    if (info.lpszProxyBypass)
        GlobalFree(info.lpszProxyBypass);
    info = {};
}

size_t curlWrite(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* out = static_cast<std::string*>(userdata);
    const size_t total = size * nmemb;
    constexpr size_t kMaxResponseBytes = 4 * 1024 * 1024;
    if (out->size() > kMaxResponseBytes || total > kMaxResponseBytes - out->size())
        return 0;
    out->append(ptr, total);
    return total;
}

size_t curlFileWrite(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* out = static_cast<std::ofstream*>(userdata);
    const size_t total = size * nmemb;
    out->write(ptr, static_cast<std::streamsize>(total));
    return out->good() ? total : 0;
}

struct DownloadContext {
    int64_t expectedBytes = 0;
    std::shared_ptr<std::atomic_bool> cancelled;
    std::function<void(int)> onProgress;
    std::atomic<int> lastPercent{-1};
};

int downloadProgress(void* userdata, curl_off_t dltotal, curl_off_t dlnow,
                     curl_off_t, curl_off_t) {
    auto* context = static_cast<DownloadContext*>(userdata);
    if (context->cancelled && context->cancelled->load(std::memory_order_relaxed))
        return 1;

    const curl_off_t total = dltotal > 0 ? dltotal : context->expectedBytes;
    if (total <= 0)
        return 0;

    const int percent = static_cast<int>(std::clamp<curl_off_t>(dlnow * 100 / total, 0, 100));
    if (context->lastPercent.exchange(percent, std::memory_order_relaxed) != percent &&
        context->onProgress) {
        context->onProgress(percent);
    }
    return 0;
}

} // namespace

const wchar_t* updateStateName(UpdateState state) {
    switch (state) {
    case UpdateState::Current: return L"current";
    case UpdateState::Available: return L"available";
    case UpdateState::NoRelease: return L"no-release";
    case UpdateState::Failed: return L"failed";
    default: return L"unknown";
    }
}

const wchar_t* updateSourceName(UpdateSource source) {
    return source == UpdateSource::Gitee ? L"Gitee" : L"GitHub";
}

const char* updateSourceApi(UpdateSource source) {
    return source == UpdateSource::Gitee ? app_info::kGiteeLatestReleaseApi
                                         : app_info::kLatestReleaseApi;
}

const wchar_t* updateSourceReleasePage(UpdateSource source) {
    return source == UpdateSource::Gitee ? app_info::kGiteeLatestReleasePage
                                         : app_info::kLatestReleasePage;
}

int updateSourceIndex(UpdateSource source) {
    return source == UpdateSource::Gitee ? 1 : 0;
}

UpdateSource updateSourceFromIndex(int index) {
    return index == 1 ? UpdateSource::Gitee : UpdateSource::GitHub;
}

std::optional<std::vector<int>> parseVersion(const std::wstring& value) {
    if (value.empty())
        return std::nullopt;

    const size_t begin = (value.front() == L'v' || value.front() == L'V') ? 1 : 0;
    if (begin == value.size())
        return std::nullopt;

    std::vector<int> parts;
    size_t pos = begin;
    while (pos < value.size()) {
        size_t end = value.find(L'.', pos);
        if (end == std::wstring::npos)
            end = value.size();
        if (end == pos)
            return std::nullopt;

        int number = 0;
        for (size_t i = pos; i < end; ++i) {
            if (value[i] < L'0' || value[i] > L'9')
                return std::nullopt;
            number = number * 10 + static_cast<int>(value[i] - L'0');
        }
        parts.push_back(number);
        pos = end == value.size() ? value.size() : end + 1;
    }
    return parts.empty() ? std::nullopt : std::optional<std::vector<int>>(std::move(parts));
}

int compareVersions(const std::vector<int>& lhs, const std::vector<int>& rhs) {
    const size_t count = std::max(lhs.size(), rhs.size());
    for (size_t i = 0; i < count; ++i) {
        const int l = i < lhs.size() ? lhs[i] : 0;
        const int r = i < rhs.size() ? rhs[i] : 0;
        if (l != r)
            return l < r ? -1 : 1;
    }
    return 0;
}

std::wstring displayVersion(const std::wstring& version) {
    if (!version.empty() && (version.front() == L'v' || version.front() == L'V'))
        return version.substr(1);
    return version;
}

std::wstring displayReleaseDate(const std::wstring& value) {
    return value.size() >= 10 ? value.substr(0, 10) : value;
}

std::wstring selectHttpsProxy(const std::wstring& proxyList) {
    std::wstring fallback;
    size_t begin = 0;
    while (begin <= proxyList.size()) {
        const size_t separator = proxyList.find(L';', begin);
        const size_t length = separator == std::wstring::npos
                                  ? proxyList.size() - begin
                                  : separator - begin;
        const std::wstring item = trimProxyValue(proxyList.substr(begin, length));
        if (!item.empty()) {
            const size_t equals = item.find(L'=');
            if (equals == std::wstring::npos) {
                if (fallback.empty())
                    fallback = item;
            } else {
                const std::wstring scheme = trimProxyValue(item.substr(0, equals));
                const std::wstring value = trimProxyValue(item.substr(equals + 1));
                if (_wcsicmp(scheme.c_str(), L"https") == 0 && !value.empty())
                    return value;
                if (_wcsicmp(scheme.c_str(), L"http") == 0 && fallback.empty())
                    fallback = value;
                if (_wcsicmp(scheme.c_str(), L"socks") == 0 && fallback.empty() &&
                    !value.empty())
                    fallback = L"socks5h://" + value;
            }
        }
        if (separator == std::wstring::npos)
            break;
        begin = separator + 1;
    }
    return fallback;
}

std::string proxyUrlForCurl(const std::wstring& value) {
    const std::wstring proxy = trimProxyValue(value);
    if (proxy.empty())
        return {};
    if (proxy.find(L"://") != std::wstring::npos)
        return utf8Of(proxy);
    return "http://" + utf8Of(proxy);
}

std::string systemProxyForUrl(const std::wstring& url) {
    WINHTTP_CURRENT_USER_IE_PROXY_CONFIG config{};
    if (!WinHttpGetIEProxyConfigForCurrentUser(&config))
        return {};

    std::wstring selectedProxy;
    const bool hasAutoProxy = config.fAutoDetect || config.lpszAutoConfigUrl != nullptr;
    if (hasAutoProxy) {
        HINTERNET session = WinHttpOpen(L"QQMusicLyric", WINHTTP_ACCESS_TYPE_NO_PROXY,
                                        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (session) {
            WinHttpSetTimeouts(session, 3000, 3000, 3000, 3000);
            WINHTTP_AUTOPROXY_OPTIONS options{};
            if (config.fAutoDetect) {
                options.dwFlags |= WINHTTP_AUTOPROXY_AUTO_DETECT;
                options.dwAutoDetectFlags = WINHTTP_AUTO_DETECT_TYPE_DHCP |
                                            WINHTTP_AUTO_DETECT_TYPE_DNS_A;
            }
            if (config.lpszAutoConfigUrl) {
                options.dwFlags |= WINHTTP_AUTOPROXY_CONFIG_URL;
                options.lpszAutoConfigUrl = config.lpszAutoConfigUrl;
            }

            WINHTTP_PROXY_INFO info{};
            if (WinHttpGetProxyForUrl(session, url.c_str(), &options, &info)) {
                if (info.dwAccessType == WINHTTP_ACCESS_TYPE_NAMED_PROXY && info.lpszProxy)
                    selectedProxy = selectHttpsProxy(info.lpszProxy);
                freeProxyInfo(info);
                WinHttpCloseHandle(session);
                freeProxyConfig(config);
                return proxyUrlForCurl(selectedProxy);
            }
            WinHttpCloseHandle(session);
        }
    }

    if (config.lpszProxy)
        selectedProxy = selectHttpsProxy(config.lpszProxy);
    freeProxyConfig(config);
    return proxyUrlForCurl(selectedProxy);
}

UpdateCheckResult parseLatestReleaseResponse(uint64_t requestId, UpdateSource source,
                                             long responseCode, const std::string& body,
                                             const std::wstring& currentVersion) {
    const std::wstring sourceName = updateSourceName(source);
    auto failResult = [&](UpdateState state, std::wstring detail) {
        return UpdateCheckResult{requestId, state, {}, {}, std::move(detail), std::nullopt};
    };

    if (responseCode == 404)
        return failResult(UpdateState::NoRelease, sourceName + L" 尚未发布可供检查的正式版本");
    if (responseCode != 200)
        return failResult(UpdateState::Failed,
                          sourceName + L" 返回 HTTP " + std::to_wstring(responseCode));

    auto json = nlohmann::json::parse(body, nullptr, false);
    if (json.is_discarded() || !json.is_object())
        return failResult(UpdateState::Failed, L"无法读取 " + sourceName + L" 返回的数据");
    if (!json.contains("tag_name") || !json["tag_name"].is_string())
        return failResult(UpdateState::Failed, L"最新版本信息不完整");

    const std::string versionTag = json["tag_name"].get<std::string>();
    ReleaseInfo latestRelease;
    latestRelease.version = wideOf(versionTag);
    // Gitee v5 的 latest Release 响应可能没有 html_url，使用已知发布页作为回退。
    if (json.contains("html_url") && json["html_url"].is_string())
        latestRelease.releaseUrl = wideOf(json["html_url"].get<std::string>());
    else
        latestRelease.releaseUrl = updateSourceReleasePage(source);
    if (json.contains("published_at") && json["published_at"].is_string())
        latestRelease.publishedAt = wideOf(json["published_at"].get<std::string>());
    if (json.contains("body") && json["body"].is_string())
        latestRelease.body = wideOf(json["body"].get<std::string>());

    std::string installerVersion = versionTag;
    if (!installerVersion.empty() &&
        (installerVersion.front() == 'v' || installerVersion.front() == 'V'))
        installerVersion.erase(installerVersion.begin());
    const std::string expectedInstaller =
        "WindowsTaskbarLyrics-" + installerVersion + "-Setup.exe";
    if (json.contains("assets") && json["assets"].is_array()) {
        for (const auto& asset : json["assets"]) {
            if (!asset.is_object() || !asset.contains("name") ||
                !asset["name"].is_string() ||
                asset["name"].get<std::string>() != expectedInstaller ||
                !asset.contains("browser_download_url") ||
                !asset["browser_download_url"].is_string())
                continue;
            latestRelease.installerName = wideOf(asset["name"].get<std::string>());
            latestRelease.installerUrl =
                wideOf(asset["browser_download_url"].get<std::string>());
            if (asset.contains("size") && asset["size"].is_number_integer())
                latestRelease.installerSize = asset["size"].get<int64_t>();
            break;
        }
    }

    const std::wstring version = latestRelease.version;
    const std::wstring releaseUrl = latestRelease.releaseUrl;
    const auto remoteVersion = parseVersion(version);
    const auto localVersion = parseVersion(currentVersion);
    if (version.empty() || !remoteVersion || !localVersion)
        return failResult(UpdateState::Failed, L"最新版本标签格式无法识别");

    const UpdateState state = compareVersions(*remoteVersion, *localVersion) > 0
                                  ? UpdateState::Available
                                  : UpdateState::Current;
    return UpdateCheckResult{requestId, state, version, releaseUrl, {}, std::move(latestRelease)};
}

uint64_t nextRequestId() {
    return gNextRequestId.fetch_add(1, std::memory_order_relaxed) + 1;
}

std::wstring makeTempInstallerPath(const std::wstring& installerName) {
    if (installerName.empty())
        return {};

    wchar_t tempDir[MAX_PATH]{};
    const DWORD length = GetTempPathW(static_cast<DWORD>(std::size(tempDir)), tempDir);
    if (length == 0 || length >= std::size(tempDir))
        return {};

    std::wstring path = tempDir;
    if (!path.empty() && path.back() != L'\\')
        path += L'\\';
    path += installerName;
    DeleteFileW(path.c_str());
    return path;
}

void requestLatestRelease(uint64_t requestId, UpdateSource source,
                          std::function<void(UpdateCheckResult)> onResult) {
    std::thread([requestId, source, onResult = std::move(onResult)] {
        const auto publish = [&](UpdateCheckResult result) {
            if (onResult)
                onResult(std::move(result));
        };
        CURL* curl = curl_easy_init();
        if (!curl) {
            publish(UpdateCheckResult{requestId, UpdateState::Failed, {}, {},
                                      L"无法初始化网络请求", std::nullopt});
            return;
        }

        std::string body;
        constexpr char kUserAgent[] = "QQMusicLyric";
        curl_slist* headers = nullptr;
        if (source == UpdateSource::GitHub) {
            headers = curl_slist_append(headers, "Accept: application/vnd.github+json");
            headers = curl_slist_append(headers, "X-GitHub-Api-Version: 2022-11-28");
        }

        curl_easy_setopt(curl, CURLOPT_URL, updateSourceApi(source));
        // 版本 API 保持直连；只让体积较大的安装器下载读取系统代理，避免代理节点对
        // 代码托管平台的 API 返回异常。空字符串会显式禁用环境变量代理。
        curl_easy_setopt(curl, CURLOPT_PROXY, "");
        if (headers)
            curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curlWrite);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
        curl_easy_setopt(curl, CURLOPT_USERAGENT, kUserAgent);
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 3L);
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 5000L);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 10000L);

        const CURLcode rc = curl_easy_perform(curl);
        long responseCode = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &responseCode);
        if (headers)
            curl_slist_free_all(headers);
        curl_easy_cleanup(curl);

        if (rc != CURLE_OK) {
            publish(UpdateCheckResult{requestId, UpdateState::Failed, {}, {},
                                      L"网络请求失败：" + wideOf(curl_easy_strerror(rc)),
                                      std::nullopt});
            return;
        }
        publish(parseLatestReleaseResponse(requestId, source, responseCode, body));
    }).detach();
}

void downloadInstaller(std::wstring url, std::wstring outputPath, int64_t expectedBytes,
                       std::wstring sourceName, std::shared_ptr<std::atomic_bool> cancelled,
                       DownloadCallbacks callbacks) {
    std::thread([url = std::move(url), outputPath = std::move(outputPath), expectedBytes,
                 sourceName = std::move(sourceName), cancelled = std::move(cancelled),
                 callbacks = std::move(callbacks)]() mutable {
        const auto finish = [&](bool success, bool wasCancelled, std::wstring path,
                                std::wstring detail) {
            if (callbacks.onFinished)
                callbacks.onFinished(success, wasCancelled, std::move(path), std::move(detail));
        };

        const std::string urlUtf8 = utf8Of(url);
        if (urlUtf8.empty()) {
            DeleteFileW(outputPath.c_str());
            finish(false, false, {}, L"更新安装器下载地址无效");
            return;
        }
        const std::string proxy = systemProxyForUrl(url);

        std::ofstream output(std::filesystem::path(outputPath),
                             std::ios::binary | std::ios::trunc);
        if (!output) {
            finish(false, false, {}, L"无法创建临时安装器文件");
            return;
        }

        CURL* curl = curl_easy_init();
        if (!curl) {
            output.close();
            DeleteFileW(outputPath.c_str());
            finish(false, false, {}, L"无法初始化下载请求");
            return;
        }

        DownloadContext context{expectedBytes, cancelled, callbacks.onProgress};
        constexpr char kUserAgent[] = "QQMusicLyric";
        curl_slist* headers = nullptr;
        headers = curl_slist_append(headers, "Accept: application/octet-stream");

        curl_easy_setopt(curl, CURLOPT_URL, urlUtf8.c_str());
        curl_easy_setopt(curl, CURLOPT_PROXY, proxy.c_str());
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curlFileWrite);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &output);
        curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, downloadProgress);
        curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &context);
        curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(curl, CURLOPT_USERAGENT, kUserAgent);
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 10000L);
        curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1024L);
        curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 60L);

        const CURLcode rc = curl_easy_perform(curl);
        long responseCode = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &responseCode);
        if (headers)
            curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
        output.close();

        const bool wasCancelled = cancelled && cancelled->load(std::memory_order_relaxed);
        if (wasCancelled || rc == CURLE_ABORTED_BY_CALLBACK) {
            DeleteFileW(outputPath.c_str());
            finish(false, true, {}, {});
            return;
        }
        if (rc != CURLE_OK) {
            DeleteFileW(outputPath.c_str());
            finish(false, false, {}, L"下载失败：" + wideOf(curl_easy_strerror(rc)));
            return;
        }
        if (responseCode < 200 || responseCode >= 300) {
            DeleteFileW(outputPath.c_str());
            finish(false, false, {},
                   L"下载失败：" + sourceName + L" 返回 HTTP " + std::to_wstring(responseCode));
            return;
        }

        std::error_code ec;
        const auto actualBytes = std::filesystem::file_size(std::filesystem::path(outputPath), ec);
        if (ec || actualBytes == 0 ||
            (expectedBytes > 0 && actualBytes != static_cast<uintmax_t>(expectedBytes))) {
            DeleteFileW(outputPath.c_str());
            finish(false, false, {}, L"下载文件大小校验失败");
            return;
        }

        if (callbacks.onProgress)
            callbacks.onProgress(100);
        finish(true, false, std::move(outputPath), {});
    }).detach();
}

} // namespace update_service
