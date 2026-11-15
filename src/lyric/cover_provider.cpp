#include "cover_provider.h"

#include "app_info.h"
#include "logging/runtime_logger.h"

#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

#include <windows.h>

namespace {

constexpr char kUserAgent[] =
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) "
    "Chrome/126.0.0.0 Safari/537.36";
constexpr char kQqReferer[] = "https://y.qq.com/";
constexpr char kNeteaseReferer[] = "https://music.163.com/";

std::string toUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

size_t curlWrite(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* out = static_cast<std::vector<uint8_t>*>(userdata);
    size_t total = size * nmemb;
    size_t oldSize = out->size();
    out->resize(oldSize + total);
    std::memcpy(out->data() + oldSize, ptr, total);
    return total;
}

int curlXferAbort(void*, curl_off_t, curl_off_t, curl_off_t, curl_off_t);

bool httpGet(CURL* curl, const std::string& url, const char* referer,
             std::vector<uint8_t>& data, long& code, std::string& contentType,
             const char* userAgent = kUserAgent) {
    data.clear();
    code = 0;
    contentType.clear();
    curl_easy_reset(curl);
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curlWrite);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &data);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, userAgent);
    curl_easy_setopt(curl, CURLOPT_REFERER, referer);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 5000L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 10000L);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L); // 进度回调用于退出时立即中断
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, curlXferAbort);

    const CURLcode rc = curl_easy_perform(curl);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
    const char* ctype = nullptr;
    curl_easy_getinfo(curl, CURLINFO_CONTENT_TYPE, &ctype);
    contentType = ctype ? ctype : "";
    return rc == CURLE_OK && code == 200 && !data.empty();
}

bool looksLikeImage(const std::vector<uint8_t>& data) {
    if (data.size() >= 3 && data[0] == 0xFF && data[1] == 0xD8 && data[2] == 0xFF)
        return true;
    if (data.size() >= 8 && data[0] == 0x89 && data[1] == 'P' && data[2] == 'N' &&
        data[3] == 'G' && data[4] == 0x0D && data[5] == 0x0A && data[6] == 0x1A &&
        data[7] == 0x0A)
        return true;
    if (data.size() >= 6 && data[0] == 'G' && data[1] == 'I' && data[2] == 'F')
        return true;
    if (data.size() >= 2 && data[0] == 'B' && data[1] == 'M')
        return true;
    return data.size() >= 12 && data[0] == 'R' && data[1] == 'I' && data[2] == 'F' &&
           data[3] == 'F' && data[8] == 'W' && data[9] == 'E' && data[10] == 'B' &&
           data[11] == 'P';
}

std::shared_ptr<const std::vector<uint8_t>> downloadImage(CURL* curl, const std::string& url,
                                                           const char* referer) {
    std::vector<uint8_t> data;
    long code = 0;
    std::string contentType;
    const bool ok = httpGet(curl, url, referer, data, code, contentType) &&
                    looksLikeImage(data);

    char magic[9] = {};
    if (data.size() >= 4) {
        std::snprintf(magic, sizeof(magic), "%02X %02X %02X %02X",
                      data[0], data[1], data[2], data[3]);
    }
    if (!ok) {
        runtime_log::writef(
            L"[cover] download failed: url=%S, http=%ld, type=%S, size=%zu, magic=%S",
            url.c_str(), code, contentType.empty() ? "(none)" : contentType.c_str(), data.size(),
            magic);
        return nullptr;
    }

    runtime_log::writef(L"[cover] download ok: url=%S, type=%S, size=%zu, magic=%S",
                        url.c_str(), contentType.empty() ? "(none)" : contentType.c_str(),
                        data.size(), magic);
    return std::make_shared<const std::vector<uint8_t>>(std::move(data));
}

// 进程退出标志：CoverProvider 析构前置位，curl 进度回调据此立即中断在途下载，
// 避免退出时主线程被 join 堵在最长 10s 的超时上
std::atomic<bool> g_shutdown{false};

int curlXferAbort(void*, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
    return g_shutdown.load() ? 1 : 0; // 返回非 0：curl 以 CURLE_ABORTED_BY_CALLBACK 立即返回
}

std::string lucenePhrase(const std::wstring& text) {
    std::string phrase = "\"";
    for (char ch : toUtf8(text)) {
        if (ch == '\\' || ch == '"')
            phrase += '\\';
        phrase += ch;
    }
    return phrase + '"';
}

bool sameName(const std::string& utf8, const std::wstring& expected) {
    if (utf8.empty() || expected.empty())
        return false;
    const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(),
                                           static_cast<int>(utf8.size()), nullptr, 0);
    if (length <= 0)
        return false;
    std::wstring name(length, L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(),
                        static_cast<int>(utf8.size()), name.data(), length);
    return CompareStringOrdinal(name.data(), length, expected.data(),
                                static_cast<int>(expected.size()), TRUE) == CSTR_EQUAL;
}

std::string jsonText(const nlohmann::json& object, const char* key) {
    const auto it = object.find(key);
    return it != object.end() && it->is_string() ? it->get<std::string>() : std::string();
}

std::string musicBrainzReleaseGroup(CURL* curl, const std::wstring& album,
                                   const std::wstring& albumArtist) {
    const std::string query = "releasegroup:" + lucenePhrase(album) +
                              " AND artist:" + lucenePhrase(albumArtist);
    const std::unique_ptr<char, decltype(&curl_free)> escaped(
        curl_easy_escape(curl, query.c_str(), static_cast<int>(query.size())), curl_free);
    if (!escaped)
        return {};
    const std::string url = "https://musicbrainz.org/ws/2/release-group/?fmt=json&limit=10&query=" +
                            std::string(escaped.get());
    static const std::string userAgent = "QQMusicLyric/" + toUtf8(app_info::kVersion) +
        " (https://github.com/H0308/qq-music-lyrics-displayer)";
    std::vector<uint8_t> data;
    long code = 0;
    std::string contentType;
    {
        // MusicBrainz 公共 API 要求同一客户端平均每秒至多一次请求。
        static std::mutex mutex;
        using Clock = std::chrono::steady_clock;
        static Clock::time_point nextRequest;
        std::lock_guard<std::mutex> lock(mutex);
        if (g_shutdown.load())
            return {};
        std::this_thread::sleep_until(nextRequest);
        if (g_shutdown.load())
            return {};
        nextRequest = Clock::now() + std::chrono::seconds(1);
        if (!httpGet(curl, url, nullptr, data, code, contentType, userAgent.c_str())) {
            runtime_log::writef(L"[cover][MusicBrainz] search failed: http=%ld", code);
            return {};
        }
    }

    const auto root = nlohmann::json::parse(data.begin(), data.end(), nullptr, false);
    const auto groups = root.find("release-groups");
    if (groups == root.end() || !groups->is_array())
        return {};
    std::string matchedId;
    for (const auto& group : *groups) {
        if (!sameName(jsonText(group, "title"), album))
            continue;
        const auto credits = group.find("artist-credit");
        if (credits == group.end() || !credits->is_array())
            continue;
        std::string artist;
        for (const auto& credit : *credits)
            artist += jsonText(credit, "name") + jsonText(credit, "joinphrase");
        if (!sameName(artist, albumArtist))
            continue;

        const auto id = jsonText(group, "id");
        if (id.size() != 36 || id.find_first_not_of("0123456789abcdef-") != std::string::npos)
            continue;
        // 同名同艺人的发行组可能对应不同专辑，例如单曲与录音室专辑。
        if (!matchedId.empty() && matchedId != id)
            return {};
        matchedId = id;
    }
    return matchedId;
}

} // namespace

struct CoverProvider::Impl {
    struct Worker {
        std::thread thread;
        std::shared_ptr<std::atomic<bool>> done = std::make_shared<std::atomic<bool>>(false);
    };

    std::atomic<uint64_t> generation{0};
    std::mutex mtx;
    std::vector<Worker> workers;

    using Fetch = std::function<std::shared_ptr<const std::vector<uint8_t>>()>;

    ~Impl() {
        // 先置退出标志：在途 curl 下载经进度回调立即中断
        g_shutdown.store(true);
        for (auto& w : workers)
            if (w.thread.joinable()) w.thread.join();
    }

    // 回收已结束的线程：在每次新请求入口顺手清理，向量规模稳定在并发中的请求数
    void sweepFinished() {
        std::erase_if(workers, [](Worker& w) {
            if (w.done->load()) {
                w.thread.join();
                return true;
            }
            return false;
        });
    }

    void request(Fetch fetch, ReadyCallback cb) {
        const uint64_t gen = ++generation;
        Impl* impl = this;
        Worker worker;
        auto done = worker.done;
        worker.thread = std::thread(
            [impl, gen, fetch = std::move(fetch), cb = std::move(cb), done]() mutable {
                struct Flag {
                    std::shared_ptr<std::atomic<bool>> d;
                    ~Flag() { d->store(true); }
                } flag{done};

                auto cover = fetch ? fetch() : nullptr;
                {
                    std::lock_guard<std::mutex> lk(impl->mtx);
                    // 被替换的请求已取消，不能把它当成下载失败继续查询下一来源。
                    if (impl->generation != gen)
                        return;
                }
                if (cb) cb(std::move(cover));
            });

        std::lock_guard<std::mutex> lk(mtx);
        sweepFinished();
        workers.push_back(std::move(worker));
    }
};

CoverProvider::CoverProvider() : impl_(std::make_unique<Impl>()) {
    g_shutdown.store(false); // 防御性复位：退出流程只会置位一次
    curl_global_init(CURL_GLOBAL_DEFAULT);
}

CoverProvider::~CoverProvider() = default;

void CoverProvider::requestAsync(const std::wstring& albummid, ReadyCallback cb) {
    if (albummid.empty()) {
        if (cb) cb(nullptr);
        return;
    }

    const std::string url = "https://y.gtimg.cn/music/photo_new/T002R300x300M000" +
                            toUtf8(albummid) + ".jpg";
    impl_->request([url] {
        CURL* curl = curl_easy_init();
        if (!curl)
            return std::shared_ptr<const std::vector<uint8_t>>{};
        auto cover = downloadImage(curl, url, kQqReferer);
        curl_easy_cleanup(curl);
        return cover;
    }, std::move(cb));
}

void CoverProvider::requestNeteaseAsync(const std::wstring& songId, ReadyCallback cb) {
    if (songId.empty()) {
        if (cb) cb(nullptr);
        return;
    }

    const std::string id = toUtf8(songId);
    impl_->request([id] {
        CURL* curl = curl_easy_init();
        if (!curl)
            return std::shared_ptr<const std::vector<uint8_t>>{};

        const std::string detailUrl =
            "https://music.163.com/api/song/detail/?ids=%5B" + id + "%5D";
        std::vector<uint8_t> detail;
        long code = 0;
        std::string contentType;
        if (!httpGet(curl, detailUrl, kNeteaseReferer, detail, code, contentType)) {
            runtime_log::writef(L"[cover][NetEase] detail failed: id=%S, http=%ld", id.c_str(),
                                code);
            curl_easy_cleanup(curl);
            return std::shared_ptr<const std::vector<uint8_t>>{};
        }

        const auto root = nlohmann::json::parse(detail.begin(), detail.end(), nullptr, false);
        std::string imageUrl;
        if (!root.is_discarded() && root.contains("songs") && root["songs"].is_array() &&
            !root["songs"].empty()) {
            const auto& song = root["songs"][0];
            const auto* album = song.contains("album") && song["album"].is_object()
                                    ? &song["album"]
                                : song.contains("al") && song["al"].is_object() ? &song["al"]
                                                                                  : nullptr;
            if (album)
                imageUrl = album->value("picUrl", std::string());
        }
        if (imageUrl.starts_with("//"))
            imageUrl.insert(0, "https:");
        if (imageUrl.empty()) {
            runtime_log::writef(L"[cover][NetEase] no album picUrl: id=%S", id.c_str());
            curl_easy_cleanup(curl);
            return std::shared_ptr<const std::vector<uint8_t>>{};
        }
        imageUrl += imageUrl.find('?') == std::string::npos ? "?param=300y300" : "&param=300y300";

        auto cover = downloadImage(curl, imageUrl, kNeteaseReferer);
        curl_easy_cleanup(curl);
        return cover;
    }, std::move(cb));
}

void CoverProvider::requestMusicBrainzAsync(const std::wstring& album,
                                          const std::wstring& albumArtist,
                                          ReadyCallback cb) {
    if (album.empty() || albumArtist.empty())
        return;
    impl_->request([album, albumArtist] {
        const std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> curl(curl_easy_init(),
                                                                     curl_easy_cleanup);
        if (!curl)
            return std::shared_ptr<const std::vector<uint8_t>>{};
        const auto id = musicBrainzReleaseGroup(curl.get(), album, albumArtist);
        if (id.empty() || g_shutdown.load())
            return std::shared_ptr<const std::vector<uint8_t>>{};
        return downloadImage(curl.get(), "https://coverartarchive.org/release-group/" +
                                         id + "/front-250", nullptr);
    }, std::move(cb));
}
