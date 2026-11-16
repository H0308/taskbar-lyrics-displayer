#pragma once

#include "lyric/lyric_provider.h"

#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

namespace lyric_detail {

// 进程退出前由 LyricProvider 置位，用于快速中断在途请求与目录扫描。
void setLyricShutdown(bool shutdown);
bool isLyricShuttingDown();

struct LyricRequest {
    const std::atomic<uint64_t>& generation;
    uint64_t expected;

    bool cancelled() const { return isLyricShuttingDown() || generation.load() != expected; }
};

class RequestDiagnosticsScope {
public:
    explicit RequestDiagnosticsScope(LyricProvider::ReadyCallback& callback);
    ~RequestDiagnosticsScope();

    RequestDiagnosticsScope(const RequestDiagnosticsScope&) = delete;
    RequestDiagnosticsScope& operator=(const RequestDiagnosticsScope&) = delete;

private:
    std::vector<LyricRequestDiagnostic> records_;
};

std::string toUtf8(const std::wstring& w);
std::wstring toWide(const std::string& s);
std::string jsonString(const nlohmann::json& value);
std::string xmlInnerText(const std::string& xml, const std::string& tag);

bool httpGet(CURL* curl, const std::string& url, const char* referer, std::string& out,
             const LyricRequest* request = nullptr, const wchar_t* sodaOperation = nullptr,
             const char* userAgent = nullptr);

// 将独立 LRC 时间轴附着到最近的主歌词行（偏差超过 1500ms 忽略）。
void attachSecondary(std::vector<LyricLine>& mainLines, const std::vector<LyricLine>& secondary,
                     bool translation);

} // namespace lyric_detail
