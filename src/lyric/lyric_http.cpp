#include "lyric/lyric_http.h"

#include "logging/runtime_logger.h"

#include <windows.h>

#include <chrono>
#include <cstdlib>
#include <thread>
#include <utility>

namespace lyric_detail {
namespace {

constexpr char kDefaultUserAgent[] =
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) "
    "Chrome/126.0.0.0 Safari/537.36";
constexpr size_t kMaxLyricResponseBytes = 8 * 1024 * 1024;

std::atomic<bool> g_shutdown{false};
thread_local std::vector<LyricRequestDiagnostic>* g_requestDiagnostics = nullptr;

int curlXferAbort(void*, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
    return g_shutdown.load() ? 1 : 0; // 返回非 0：curl 以 CURLE_ABORTED_BY_CALLBACK 立即返回
}

int curlRequestXferAbort(void* context, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
    return static_cast<const LyricRequest*>(context)->cancelled() ? 1 : 0;
}

size_t curlWrite(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* out = static_cast<std::string*>(userdata);
    if (size == 0 || out->size() > kMaxLyricResponseBytes ||
        nmemb > (kMaxLyricResponseBytes - out->size()) / size)
        return 0;
    const size_t bytes = size * nmemb;
    out->append(ptr, bytes);
    return bytes;
}

} // namespace

void setLyricShutdown(bool shutdown) {
    g_shutdown.store(shutdown);
}

bool isLyricShuttingDown() {
    return g_shutdown.load();
}

RequestDiagnosticsScope::RequestDiagnosticsScope(LyricProvider::ReadyCallback& callback) {
    callback = [callback = std::move(callback), started = std::chrono::steady_clock::now(),
                records = &records_](LyricResult result) mutable {
        result.elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started).count();
        result.diagnostics = *records;
        if (callback)
            callback(std::move(result));
    };
    g_requestDiagnostics = &records_;
}

RequestDiagnosticsScope::~RequestDiagnosticsScope() {
    g_requestDiagnostics = nullptr;
}

std::string toUtf8(const std::wstring& w) {
    if (w.empty())
        return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()),
                                      nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n,
                        nullptr, nullptr);
    return s;
}

std::wstring toWide(const std::string& s) {
    if (s.empty())
        return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()),
                                      nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

std::string jsonString(const nlohmann::json& value) {
    if (value.is_string())
        return value.get<std::string>();
    if (value.is_number_integer())
        return std::to_string(value.get<int64_t>());
    if (value.is_number_unsigned())
        return std::to_string(value.get<uint64_t>());
    return {};
}

std::string xmlInnerText(const std::string& xml, const std::string& tag) {
    const std::string open = "<" + tag;
    size_t begin = xml.find(open);
    if (begin == std::string::npos)
        return {};
    begin = xml.find('>', begin + open.size());
    if (begin == std::string::npos)
        return {};
    ++begin;
    const std::string close = "</" + tag + ">";
    const size_t end = xml.find(close, begin);
    if (end == std::string::npos)
        return {};
    std::string text = xml.substr(begin, end - begin);
    if (text.compare(0, 9, "<![CDATA[") == 0 && text.size() >= 12 &&
        text.compare(text.size() - 3, 3, "]]>") == 0)
        text = text.substr(9, text.size() - 12);
    return text;
}

bool httpGet(CURL* curl, const std::string& url, const char* referer, std::string& out,
             const LyricRequest* request, const wchar_t* sodaOperation, const char* userAgent) {
    if (g_shutdown.load() || (request && request->cancelled()))
        return false;
    curl_easy_reset(curl);
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curlWrite);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &out);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, userAgent ? userAgent : kDefaultUserAgent);
    curl_easy_setopt(curl, CURLOPT_REFERER, referer);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 5000L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 10000L);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L); // 进度回调用于退出时立即中断
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, curlXferAbort);
    if (request) {
        curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, curlRequestXferAbort);
        curl_easy_setopt(curl, CURLOPT_XFERINFODATA, request);
    }
    // 仅汽水对临时网络错误重试一次；其他歌词来源仍执行原有单次请求。
    for (int attempt = 1; attempt <= (sodaOperation ? 2 : 1); ++attempt) {
        if (g_shutdown.load() || (request && request->cancelled()))
            return false;
        out.clear();
        const auto started = std::chrono::steady_clock::now();
        const CURLcode rc = curl_easy_perform(curl);
        long code = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
        if (g_requestDiagnostics) {
            const wchar_t* source = url.starts_with("https://lrclib.net/") ? L"LRCLIB"
                : url.find("music.163.com/") != std::string::npos ? L"网易云音乐"
                : url.find("kugou.com/") != std::string::npos ? L"酷狗音乐"
                : url.find("qishui.com/") != std::string::npos ||
                  url.find("douyin.com/") != std::string::npos ? L"汽水音乐" : L"QQ 音乐";
            g_requestDiagnostics->push_back({source,
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - started).count(), code, static_cast<int>(rc)});
        }
        if (g_shutdown.load() || (request && request->cancelled()))
            return false;
        if (rc == CURLE_OK && code == 200)
            return true;
        if (!sodaOperation)
            return false;
        const bool transient = rc == CURLE_OPERATION_TIMEDOUT || rc == CURLE_COULDNT_CONNECT ||
                               rc == CURLE_COULDNT_RESOLVE_HOST || rc == CURLE_RECV_ERROR ||
                               rc == CURLE_SEND_ERROR ||
                               (rc == CURLE_OK && (code == 429 || (code >= 500 && code < 600)));
        runtime_log::writef(
            L"[lyric][Soda Music] http-failed operation=%ls attempt=%d curl=%d reason=%S http=%ld retry=%d",
            sodaOperation, attempt, static_cast<int>(rc), curl_easy_strerror(rc), code,
            transient && attempt == 1 ? 1 : 0);
        if (!transient || attempt == 2)
            return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
    return false;
}

void attachSecondary(std::vector<LyricLine>& mainLines, const std::vector<LyricLine>& secondary,
                     bool translation) {
    if (mainLines.empty() || secondary.empty())
        return;
    size_t cursor = 0;
    for (const auto& extra : secondary) {
        while (cursor + 1 < mainLines.size() &&
               std::llabs(mainLines[cursor + 1].ms - extra.ms) <=
                   std::llabs(mainLines[cursor].ms - extra.ms))
            ++cursor;
        if (std::llabs(mainLines[cursor].ms - extra.ms) > 1500)
            continue;
        std::wstring& target = translation ? mainLines[cursor].translation
                                           : mainLines[cursor].romanization;
        if (target.empty())
            target = extra.text;
    }
}

} // namespace lyric_detail
