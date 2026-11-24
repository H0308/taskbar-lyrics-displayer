#include "lyric_provider.h"

#include "logging/runtime_logger.h"
#include "lyric/custom_local_lyric_index.h"
#include "lyric/local_lyric_index.h"
#include "lyric/lyric_http.h"
#include "lyric/lyric_matching.h"
#include "lyric/soda_lyric_timing.h"
#include "lyric/sources/kugou_lyric_source.h"
#include "lyric/sources/lrclib_lyric_source.h"
#include "lyric/sources/netease_lyric_source.h"
#include "lyric/sources/qq_lyric_source.h"
#include "lyric/sources/soda_lyric_source.h"

#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <list>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace {

constexpr size_t kCacheCapacity = 32;

// 线程退出置位守卫：保证工作线程任何 return 路径都能被 sweepFinished 回收
struct DoneFlag {
    std::shared_ptr<std::atomic<bool>> done;
    ~DoneFlag() { done->store(true); }
};

struct CacheEntry {
    std::vector<LyricLine> lines;
    SongInfo info;
};

// ---------- 手动歌词按歌分文件持久化 ----------

// 文件名可读部分：替换 Windows 非法字符，截断防超长
std::wstring sanitizeFileName(const std::wstring& s) {
    std::wstring out;
    out.reserve(s.size());
    for (wchar_t ch : s) {
        if (ch == L'\\' || ch == L'/' || ch == L':' || ch == L'*' || ch == L'?' ||
            ch == L'"' || ch == L'<' || ch == L'>' || ch == L'|' || ch < 0x20)
            out.push_back(L'_');
        else
            out.push_back(ch);
    }
    while (!out.empty() && (out.back() == L' ' || out.back() == L'.'))
        out.pop_back();
    if (out.size() > 50)
        out.resize(50);
    return out.empty() ? L"lyric" : out;
}

// FNV-1a 64 位哈希（对 key 的 UTF-8 字节），避免可读部分截断后重名
uint64_t fnv1a64(const std::string& s) {
    uint64_t h = 14695981039346656037ull;
    for (unsigned char c : s) {
        h ^= c;
        h *= 1099511628211ull;
    }
    return h;
}

// 手动歌词文件名：平台前缀 + 可读部分 + 来源键哈希，
// 如 "qq-歌名 - 歌手-a1b2c3d4e5f60708.json"。
std::wstring manualFileName(const std::wstring& title, const std::wstring& artist,
                            const std::wstring& key, SmtcPlayerType player) {
    const std::wstring readable = sanitizeFileName(title + L" - " + artist);
    wchar_t hex[17];
    std::swprintf(hex, 17, L"%016llx",
                  static_cast<unsigned long long>(fnv1a64(lyric_detail::toUtf8(key))));
    const wchar_t* prefix = player == SmtcPlayerType::Spotify    ? L"spotify-"
                            : player == SmtcPlayerType::SaltPlayer ? L"saltplayer-"
                            : player == SmtcPlayerType::Generic    ? L"generic-"
                            : player == SmtcPlayerType::Soda       ? L"soda-"
                            : player == SmtcPlayerType::NetEase    ? L"netease-"
                                                                   : L"qq-";
    return std::wstring(prefix) + readable + L"-" + hex + L".json";
}

// 旧版本文件名没有平台前缀；保留此函数用于读取已有手动歌词。
std::wstring legacyManualFileName(const std::wstring& title, const std::wstring& artist,
                                  const std::wstring& key) {
    const std::wstring readable = sanitizeFileName(title + L" - " + artist);
    wchar_t hex[17];
    std::swprintf(hex, 17, L"%016llx",
                  static_cast<unsigned long long>(fnv1a64(lyric_detail::toUtf8(key))));
    return readable + L"-" + hex + L".json";
}

} // namespace

struct LyricProvider::Impl {
    mutable std::mutex mtx;
    std::unordered_map<std::wstring, CacheEntry> cache;
    std::unordered_map<std::wstring, CacheEntry> manualOverrides; // 已加载的手动歌词（懒加载缓存）
    std::wstring overrideDir; // 手动歌词持久化目录（每首歌一个 JSON 文件），空表示不持久化
    bool qqLocalLyricsEnabled = false;
    std::wstring qqLocalLyricsDir;
    std::wstring qqLocalLyricOrderDir;
    std::shared_ptr<const lyric_detail::LocalLyricIndex> qqLocalIndex;
    bool qqLocalIndexBuilding = false;
    uint64_t qqLocalConfigGeneration = 0;
    std::chrono::steady_clock::time_point qqLocalIndexRefreshedAt{};
    std::condition_variable qqLocalIndexCv;
    bool customLocalLyricsEnabled = false;
    std::wstring customLocalLyricsDir;
    std::wstring customLocalLyricOrderDir;
    std::shared_ptr<const lyric_detail::CustomLocalLyricIndex> customLocalIndex;
    bool customLocalIndexBuilding = false;
    uint64_t customLocalConfigGeneration = 0;
    std::chrono::steady_clock::time_point customLocalIndexRefreshedAt{};
    std::condition_variable customLocalIndexCv;
    std::list<std::wstring> lru; // 前 = 最近使用
    std::unordered_map<std::wstring, std::list<std::wstring>::iterator> lruIt;
    std::atomic<uint64_t> generation{0};

    struct Worker {
        std::thread thread;
        std::shared_ptr<std::atomic<bool>> done = std::make_shared<std::atomic<bool>>(false);
    };
    std::vector<Worker> workers;

    ~Impl() {
        // 先置退出标志：在途 curl 请求经进度回调立即中断，候选循环提前退出
        lyric_detail::setLyricShutdown(true);
        {
            std::lock_guard<std::mutex> lk(mtx);
            qqLocalIndexBuilding = false;
            qqLocalIndexCv.notify_all();
            customLocalIndexBuilding = false;
            customLocalIndexCv.notify_all();
        }
        for (auto& w : workers) {
            if (w.thread.joinable())
                w.thread.join();
        }
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

    // 调用方需已持有 mtx；目录变化或开关变化时丢弃旧索引并异步重建。
    void rebuildLocalIndexLocked() {
        ++qqLocalConfigGeneration;
        qqLocalIndex.reset();
        if (!qqLocalLyricsEnabled || qqLocalLyricsDir.empty()) {
            qqLocalIndexBuilding = false;
            qqLocalIndexRefreshedAt = {};
            qqLocalIndexCv.notify_all();
            return;
        }

        const uint64_t configGeneration = qqLocalConfigGeneration;
        const std::wstring root = qqLocalLyricsDir;
        const std::wstring orderDir = qqLocalLyricOrderDir;
        qqLocalIndexBuilding = true;
        Worker worker;
        auto done = worker.done;
        Impl* impl = this;
        worker.thread = std::thread([impl, root, orderDir, configGeneration, done]() {
            DoneFlag flag{done};
            auto index = lyric_detail::buildLocalLyricIndex(root, orderDir);
            std::lock_guard<std::mutex> lk(impl->mtx);
            if (impl->qqLocalConfigGeneration != configGeneration)
                return;
            impl->qqLocalIndex = std::move(index);
            impl->qqLocalIndexBuilding = false;
            impl->qqLocalIndexRefreshedAt = std::chrono::steady_clock::now();
            runtime_log::writef(L"[lyric][local-index][QQ] root=\"%ls\" bundles=%zu",
                                root.c_str(), impl->qqLocalIndex->bundles.size());
            impl->qqLocalIndexCv.notify_all();
        });
        workers.push_back(std::move(worker));
    }

    bool acquireLocalIndex(std::shared_ptr<const lyric_detail::LocalLyricIndex>& index) {
        {
            std::unique_lock<std::mutex> lk(mtx);
            while (true) {
                if (lyric_detail::isLyricShuttingDown() || !qqLocalLyricsEnabled ||
                    qqLocalLyricsDir.empty())
                    return false;
                if (!qqLocalIndexBuilding) {
                    index = qqLocalIndex;
                    break;
                }
                qqLocalIndexCv.wait(lk, [this] {
                    return lyric_detail::isLyricShuttingDown() || !qqLocalIndexBuilding;
                });
            }
        }
        return static_cast<bool>(index);
    }

    // QQ 会在程序运行期间继续写入新的 QRC 文件。歌词未命中时允许后台刷新索引，
    // 但限制刷新频率，避免没有本地歌词的歌曲连续触发全目录扫描。
    bool refreshLocalIndex(std::shared_ptr<const lyric_detail::LocalLyricIndex>& index) {
        {
            std::lock_guard<std::mutex> lk(mtx);
            if (lyric_detail::isLyricShuttingDown() || !qqLocalLyricsEnabled ||
                qqLocalLyricsDir.empty())
                return false;

            const auto now = std::chrono::steady_clock::now();
            if (!qqLocalIndexBuilding && qqLocalIndexRefreshedAt.time_since_epoch().count() != 0 &&
                now - qqLocalIndexRefreshedAt < std::chrono::seconds(2)) {
                index = qqLocalIndex;
                return static_cast<bool>(index);
            }
            if (!qqLocalIndexBuilding)
                rebuildLocalIndexLocked();
        }
        return acquireLocalIndex(index);
    }

    bool lookupLocalLyrics(const std::wstring& title, const std::wstring& artist,
                           std::vector<LyricLine>& out, SongInfo& info) {
        std::shared_ptr<const lyric_detail::LocalLyricIndex> index;
        if (!acquireLocalIndex(index))
            return false;
        if (lyric_detail::lookupLocalLyric(*index, title, artist, out, info))
            return true;
        if (!refreshLocalIndex(index))
            return false;
        const bool hit = lyric_detail::lookupLocalLyric(*index, title, artist, out, info);
        if (!hit) {
            runtime_log::writef(L"[lyric][local-no-match][QQ] title=\"%ls\" artist=\"%ls\" "
                                L"indexed-bundles=%zu",
                                title.c_str(), artist.c_str(), index->bundles.size());
        }
        return hit;
    }

    bool localOrderOnlineFirst(const std::wstring& title, const std::wstring& artist) {
        std::shared_ptr<const lyric_detail::LocalLyricIndex> index;
        if (!acquireLocalIndex(index))
            return false;
        auto candidates = lyric_detail::findLocalLyricCandidates(*index, title, artist);
        if (candidates.empty()) {
            if (!refreshLocalIndex(index))
                return false;
            candidates = lyric_detail::findLocalLyricCandidates(*index, title, artist);
        }
        return !candidates.empty() &&
               lyric_detail::readLocalOrderOverride(index->bundles[candidates.front()]);
    }

    bool setLocalOrderOverride(const std::wstring& title, const std::wstring& artist,
                               bool onlineFirst) {
        std::shared_ptr<const lyric_detail::LocalLyricIndex> index;
        if (!acquireLocalIndex(index))
            return false;
        auto candidates = lyric_detail::findLocalLyricCandidates(*index, title, artist);
        if (candidates.empty()) {
            if (!refreshLocalIndex(index))
                return false;
            candidates = lyric_detail::findLocalLyricCandidates(*index, title, artist);
        }
        if (candidates.empty())
            return false;
        return lyric_detail::writeLocalOrderOverride(index->bundles[candidates.front()],
                                                   onlineFirst);
    }

    // 调用方需已持有 mtx；目录或开关变化时异步重建单层目录索引。
    void rebuildCustomLocalIndexLocked() {
        ++customLocalConfigGeneration;
        customLocalIndex.reset();
        if (!customLocalLyricsEnabled || customLocalLyricsDir.empty()) {
            customLocalIndexBuilding = false;
            customLocalIndexRefreshedAt = {};
            customLocalIndexCv.notify_all();
            return;
        }

        const uint64_t configGeneration = customLocalConfigGeneration;
        const std::wstring root = customLocalLyricsDir;
        customLocalIndexBuilding = true;
        Worker worker;
        auto done = worker.done;
        Impl* impl = this;
        worker.thread = std::thread([impl, root, configGeneration, done]() {
            DoneFlag flag{done};
            auto index = lyric_detail::buildCustomLocalLyricIndex(root);
            std::lock_guard<std::mutex> lk(impl->mtx);
            if (impl->customLocalConfigGeneration != configGeneration)
                return;
            impl->customLocalIndex = std::move(index);
            impl->customLocalIndexBuilding = false;
            impl->customLocalIndexRefreshedAt = std::chrono::steady_clock::now();
            runtime_log::writef(L"[lyric][custom-local-index] root=\"%ls\" entries=%zu",
                                root.c_str(), impl->customLocalIndex->entries.size());
            impl->customLocalIndexCv.notify_all();
        });
        workers.push_back(std::move(worker));
    }

    bool acquireCustomLocalIndex(
        std::shared_ptr<const lyric_detail::CustomLocalLyricIndex>& index) {
        {
            std::unique_lock<std::mutex> lk(mtx);
            while (true) {
                if (lyric_detail::isLyricShuttingDown() || !customLocalLyricsEnabled ||
                    customLocalLyricsDir.empty())
                    return false;
                if (!customLocalIndexBuilding) {
                    index = customLocalIndex;
                    break;
                }
                customLocalIndexCv.wait(lk, [this] {
                    return lyric_detail::isLyricShuttingDown() || !customLocalIndexBuilding;
                });
            }
        }
        return static_cast<bool>(index);
    }

    bool refreshCustomLocalIndex(
        std::shared_ptr<const lyric_detail::CustomLocalLyricIndex>& index) {
        {
            std::lock_guard<std::mutex> lk(mtx);
            if (lyric_detail::isLyricShuttingDown() || !customLocalLyricsEnabled ||
                customLocalLyricsDir.empty())
                return false;

            const auto now = std::chrono::steady_clock::now();
            if (!customLocalIndexBuilding &&
                customLocalIndexRefreshedAt.time_since_epoch().count() != 0 &&
                now - customLocalIndexRefreshedAt < std::chrono::seconds(2)) {
                index = customLocalIndex;
                return static_cast<bool>(index);
            }
            if (!customLocalIndexBuilding)
                rebuildCustomLocalIndexLocked();
        }
        return acquireCustomLocalIndex(index);
    }

    bool lookupCustomLocalLyrics(const std::wstring& title, const std::wstring& artist,
                                 int64_t durationMs, std::vector<LyricLine>& out, SongInfo& info) {
        std::shared_ptr<const lyric_detail::CustomLocalLyricIndex> index;
        if (!acquireCustomLocalIndex(index))
            return false;
        if (lyric_detail::lookupCustomLocalLyric(*index, title, artist, durationMs, out, info))
            return true;
        if (!refreshCustomLocalIndex(index))
            return false;
        const bool hit =
            lyric_detail::lookupCustomLocalLyric(*index, title, artist, durationMs, out, info);
        if (!hit) {
            runtime_log::writef(L"[lyric][custom-local-no-match] title=\"%ls\" artist=\"%ls\" "
                                L"indexed-entries=%zu",
                                title.c_str(), artist.c_str(), index->entries.size());
        }
        return hit;
    }

    std::vector<SearchCandidate> searchCustomLocalLyrics(const std::wstring& title,
                                                         const std::wstring& artist) {
        std::shared_ptr<const lyric_detail::CustomLocalLyricIndex> index;
        if (!refreshCustomLocalIndex(index) && !acquireCustomLocalIndex(index))
            return {};
        return lyric_detail::searchCustomLocalCandidates(*index, title, artist);
    }

    bool customLocalOrderOnlineFirst(const std::wstring& key, const std::wstring& title,
                                     const std::wstring& artist, SmtcPlayerType player) {
        std::wstring orderDir;
        {
            std::lock_guard<std::mutex> lk(mtx);
            orderDir = customLocalLyricOrderDir;
        }
        if (orderDir.empty())
            return false;
        return lyric_detail::readCustomLocalOrderOverride(
            lyric_detail::customLocalOrderPath(orderDir, key, title, artist, player));
    }

    bool setCustomLocalOrderOverride(const std::wstring& key, const std::wstring& title,
                                     const std::wstring& artist, SmtcPlayerType player,
                                     bool onlineFirst) {
        std::wstring orderDir;
        {
            std::lock_guard<std::mutex> lk(mtx);
            orderDir = customLocalLyricOrderDir;
        }
        if (orderDir.empty())
            return false;
        return lyric_detail::writeCustomLocalOrderOverride(
            lyric_detail::customLocalOrderPath(orderDir, key, title, artist, player), onlineFirst);
    }

    bool cacheGet(const std::wstring& key, CacheEntry& out) {
        const auto it = cache.find(key);
        if (it == cache.end())
            return false;
        lru.erase(lruIt[key]);
        lru.push_front(key);
        lruIt[key] = lru.begin();
        out = it->second;
        return true;
    }

    void cachePut(const std::wstring& key, CacheEntry entry) {
        cache[key] = std::move(entry);
        const auto it = lruIt.find(key);
        if (it != lruIt.end())
            lru.erase(it->second);
        lru.push_front(key);
        lruIt[key] = lru.begin();
        while (lru.size() > kCacheCapacity) {
            const std::wstring victim = lru.back();
            cache.erase(victim);
            lruIt.erase(victim);
            lru.pop_back();
        }
    }

    // ---------- 手动歌词持久化（每首歌一个 JSON 文件，UTF-8） ----------

    std::wstring overrideFilePath(const std::wstring& key, const std::wstring& title,
                                  const std::wstring& artist, SmtcPlayerType player,
                                  bool legacy) const {
        if (overrideDir.empty())
            return {};
        const std::wstring fileName = legacy
                                          ? legacyManualFileName(title, artist, key)
                                          : manualFileName(title, artist, key, player);
        return overrideDir + L"\\" + fileName;
    }

    bool loadOverrideFile(const std::wstring& path, CacheEntry& entry) {
        try {
            std::ifstream f(std::filesystem::path(path), std::ios::binary);
            if (!f)
                return false;
            auto e = nlohmann::json::parse(f, nullptr, false);
            if (e.is_discarded() || !e.is_object())
                return false;
            entry.info.songmid = lyric_detail::toWide(e.value("songmid", std::string()));
            entry.info.albummid = lyric_detail::toWide(e.value("albummid", std::string()));
            entry.info.neteaseSongId =
                lyric_detail::toWide(e.value("neteaseSongId", std::string()));
            if (e["lines"].is_array()) {
                for (const auto& l : e["lines"]) {
                    if (!l.is_array() || l.size() < 2)
                        continue;
                    LyricLine line{l[0].get<int64_t>(),
                                   lyric_detail::toWide(l[1].get<std::string>()), {}};
                    // 第三项为逐字时间轴（旧格式没有则留空，兼容旧文件）
                    if (l.size() >= 3 && l[2].is_array()) {
                        for (const auto& c : l[2]) {
                            if (!c.is_array() || c.size() < 3)
                                continue;
                            line.chars.push_back(
                                {c[0].get<int64_t>(), c[1].get<int64_t>(),
                                 lyric_detail::toWide(c[2].get<std::string>())});
                        }
                    }
                    // 第四、五项分别为翻译和罗马音；旧文件缺失时保持为空。
                    if (l.size() >= 4 && l[3].is_string())
                        line.translation = lyric_detail::toWide(l[3].get<std::string>());
                    if (l.size() >= 5 && l[4].is_string())
                        line.romanization = lyric_detail::toWide(l[4].get<std::string>());
                    entry.lines.push_back(std::move(line));
                }
            }
            return !entry.lines.empty();
        } catch (...) {
            return false;
        }
    }

    void saveOverrideFile(const std::wstring& path, const std::wstring& title,
                          const std::wstring& artist, int64_t durationMs,
                          const CacheEntry& entry) {
        try {
            std::filesystem::create_directories(std::filesystem::path(path).parent_path());
            nlohmann::json e;
            e["title"] = lyric_detail::toUtf8(title);
            e["artist"] = lyric_detail::toUtf8(artist);
            e["durationMs"] = durationMs;
            e["songmid"] = lyric_detail::toUtf8(entry.info.songmid);
            e["albummid"] = lyric_detail::toUtf8(entry.info.albummid);
            e["neteaseSongId"] = lyric_detail::toUtf8(entry.info.neteaseSongId);
            nlohmann::json arr = nlohmann::json::array();
            for (const auto& l : entry.lines) {
                nlohmann::json chars = nlohmann::json::array();
                for (const auto& c : l.chars)
                    chars.push_back({c.startMs, c.endMs, lyric_detail::toUtf8(c.text)});
                arr.push_back({l.ms, lyric_detail::toUtf8(l.text), std::move(chars),
                               lyric_detail::toUtf8(l.translation),
                               lyric_detail::toUtf8(l.romanization)});
            }
            e["lines"] = std::move(arr);
            std::ofstream f(std::filesystem::path(path), std::ios::binary | std::ios::trunc);
            f << e.dump(2);
        } catch (...) {
        }
    }

    // 查找一首歌的手动歌词：先查内存，未命中再查对应文件（找到则载入内存）
    // 调用方需已持有 mtx
    bool manualOverrideGet(const std::wstring& key, const std::wstring& title,
                           const std::wstring& artist, SmtcPlayerType player, CacheEntry& out) {
        const auto it = manualOverrides.find(key);
        if (it != manualOverrides.end()) {
            out = it->second;
            return true;
        }
        const std::wstring path = overrideFilePath(key, title, artist, player, false);
        if (path.empty())
            return false;
        CacheEntry entry;
        if (!loadOverrideFile(path, entry)) {
            // 只有早期 QQ／网易云保存过无平台前缀的文件；汽水、Spotify、椒盐音乐与 Generic
            // 从一开始就带前缀，不需要回退到旧命名。
            if (player == SmtcPlayerType::Soda || player == SmtcPlayerType::Spotify ||
                player == SmtcPlayerType::SaltPlayer || player == SmtcPlayerType::Generic)
                return false;
            // 兼容改名前已经存在的无前缀文件；新保存的文件只使用带前缀路径。
            const std::wstring legacyPath = overrideFilePath(key, title, artist, player, true);
            if (!loadOverrideFile(legacyPath, entry))
                return false;
        }
        manualOverrides[key] = entry;
        out = std::move(entry);
        return true;
    }

    bool fetchOnline(const std::wstring& title, const std::wstring& artist,
                     const std::wstring& album, int64_t durationMs, std::vector<LyricLine>& out,
                     SongInfo& info, SmtcPlayerType player,
                     const lyric_detail::LyricRequest& request) {
        if (title.empty() || request.cancelled())
            return false;
        out.clear();
        info = {};
        const std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> handle(
            curl_easy_init(), curl_easy_cleanup);
        CURL* curl = handle.get();
        if (!curl)
            return false;

        const lyric_detail::MatchQuery q{title, artist, durationMs,
                                         lyric_detail::extractVersionTags(title)};
        const std::vector<lyric_detail::SearchVariant> variants =
            lyric_detail::buildSearchVariants(q);

        auto tryKugou = [&]() -> bool {
            for (const auto& variant : variants) {
                if (request.cancelled())
                    return false;
                std::vector<lyric_detail::KugouSong> songs;
                if (!lyric_detail::kugouSearchSongs(curl, variant.title, variant.artist, 10, songs))
                    continue;
                const auto ranked = lyric_detail::rankKugouCandidates(q, songs);
                if (ranked.empty())
                    continue;
                for (const auto& [score, song] : ranked) {
                    (void)score;
                    if (request.cancelled())
                        return false;
                    out.clear();
                    const int64_t lyricDuration = q.durationMs > 0 ? q.durationMs
                                                                   : song->durationMs;
                    if (lyric_detail::fetchKrcByHash(curl, song->hash, lyricDuration, out) &&
                        !out.empty()) {
                        if (request.cancelled())
                            return false;
                        lyric_detail::fillQqSongInfo(curl, title, artist, durationMs, info);
                        if (!info.songmid.empty()) {
                            lyric_detail::attachQqSecondary(
                                curl, lyric_detail::toUtf8(info.songmid), out);
                        }
                        runtime_log::writef(L"[lyric][source=kugou] lines=%zu", out.size());
                        info.lyricOrigin = LyricOrigin::Kugou;
                        info.matchedTitle = song->name;
                        info.matchedArtist = song->singer;
                        info.matchedDurationMs = song->durationMs;
                        return true;
                    }
                }
            }
            return false;
        };

        // QRC 未命中时，LRC 复用同一次 QQ 曲目搜索的候选。
        std::vector<std::vector<lyric_detail::QqCandidate>> qqCandidates(variants.size());
        auto tryQq = [&](LyricSource source) -> bool {
            for (size_t i = 0; i < variants.size(); ++i) {
                if (request.cancelled())
                    return false;
                auto& cands = qqCandidates[i];
                if (source == LyricSource::Qrc &&
                    !lyric_detail::searchQqSongs(curl, lyric_detail::makeSearchQuery(variants[i]),
                                                 cands)) {
                    cands.clear();
                    continue;
                }
                const auto ranked = lyric_detail::rankQqCandidates(q, cands);
                if (ranked.empty())
                    continue;
                for (const auto& [score, candidate] : ranked) {
                    (void)score;
                    if (request.cancelled())
                        return false;
                    if (source == LyricSource::Qrc && candidate->songid.empty())
                        continue;
                    if (source == LyricSource::Lrc && candidate->songmid.empty())
                        continue;
                    out.clear();
                    const bool downloaded = source == LyricSource::Qrc
                                                ? lyric_detail::downloadQqQrc(curl, candidate->songid, out)
                                                : lyric_detail::downloadQqLrc(curl, candidate->songmid, out);
                    if (downloaded && !out.empty()) {
                        if (request.cancelled())
                            return false;
                        if (source == LyricSource::Qrc)
                            lyric_detail::attachQqSecondary(curl, candidate->songmid, out);
                        info.songmid = lyric_detail::toWide(candidate->songmid);
                        info.albummid = lyric_detail::toWide(candidate->albummid);
                        info.lyricOrigin = LyricOrigin::QqMusic;
                        info.matchedTitle = candidate->name;
                        info.matchedArtist = candidate->singer;
                        info.matchedDurationMs = candidate->intervalMs;
                        runtime_log::writef(L"[lyric][source=qq] format=%ls lines=%zu",
                                            source == LyricSource::Qrc ? L"QRC" : L"LRC",
                                            out.size());
                        return true;
                    }
                }
            }
            return false;
        };

        const bool found = tryKugou() || tryQq(LyricSource::Qrc) || tryQq(LyricSource::Lrc);
        if (request.cancelled())
            return false;
        if (found)
            return true;
        out = lyric_detail::fetchLrclib(title, artist, album, durationMs, request, info);
        if (request.cancelled())
            return false;
        const bool wordSync = std::any_of(out.begin(), out.end(), [](const LyricLine& line) {
            return !line.chars.empty();
        });
        runtime_log::writef(L"[lyric][source=lrclib] lines=%zu word-sync=%d",
                            out.size(), wordSync ? 1 : 0);
        if (!out.empty()) {
            info.lyricOrigin = LyricOrigin::Lrclib;
            return true;
        }
        if (player != SmtcPlayerType::NetEase &&
            lyric_detail::fetchNeteaseByMetadata(curl, q, out, request, info)) {
            info.lyricOrigin = LyricOrigin::Netease;
            return true;
        }
        if (player != SmtcPlayerType::Soda &&
            lyric_detail::fetchSodaLyrics(title, artist, durationMs, out, request, info)) {
            info.lyricOrigin = LyricOrigin::Soda;
            return true;
        }
        return false;
    }

    bool fetchNetease(const std::wstring& songId, std::vector<LyricLine>& out, SongInfo& info) {
        CURL* curl = curl_easy_init();
        if (!curl)
            return false;
        const bool ok = lyric_detail::downloadNeteaseLyric(curl, songId, out);
        curl_easy_cleanup(curl);
        if (ok) {
            info = SongInfo{}; // 网易云歌词不使用 QQ songmid/albummid 封面兜底
            info.neteaseSongId = songId;
            info.lyricOrigin = LyricOrigin::Netease;
        }
        return ok;
    }
};

LyricProvider::LyricProvider() : impl_(std::make_unique<Impl>()) {
    lyric_detail::setLyricShutdown(false); // 防御性复位：退出流程只会置位一次
    curl_global_init(CURL_GLOBAL_DEFAULT);
}

LyricProvider::~LyricProvider() = default;

void LyricProvider::requestAsync(const std::wstring& title, const std::wstring& artist,
                                 int64_t durationMs, ReadyCallback cb, RequestSource source,
                                 bool persistOrder, const std::wstring& album,
                                 OrderPreference order) {
    // Spotify、椒盐音乐与 Generic 使用带专辑的曲目键走通用多源路径；其它来源沿用 QQ 键。
    const bool sampled = source == RequestSource::Spotify ||
                         source == RequestSource::SaltPlayer ||
                         source == RequestSource::Generic;
    const OrderPreference effectiveOrder =
        order != OrderPreference::Default       ? order
        : source == RequestSource::PreferOnline ? OrderPreference::PreferOnline
        : source == RequestSource::PreferLocal  ? OrderPreference::PreferLocal
                                                : OrderPreference::Default;
    const SmtcPlayerType requestPlayer =
        source == RequestSource::SaltPlayer ? SmtcPlayerType::SaltPlayer
        : source == RequestSource::Spotify  ? SmtcPlayerType::Spotify
        : source == RequestSource::Generic  ? SmtcPlayerType::Generic
                                            : SmtcPlayerType::QQMusic;
    const std::wstring key =
        source == RequestSource::SaltPlayer
            ? makeSaltPlayerKey(title, artist, album, durationMs)
            : source == RequestSource::Spotify
                  ? makeSpotifyKey(title, artist, album, durationMs)
                  : source == RequestSource::Generic
                        ? makeGenericKey(title, artist, album, durationMs)
                        : makeKey(title, artist, durationMs);
    const uint64_t gen = ++impl_->generation;
    Impl* impl = impl_.get();
    Impl::Worker worker;
    auto done = worker.done;
    worker.thread = std::thread([impl, gen, key, title, artist, album, durationMs,
                                 cb = std::move(cb), sampled, effectiveOrder, persistOrder, done,
                                 requestPlayer]() mutable {
        DoneFlag flag{done};
        const lyric_detail::LyricRequest request{impl->generation, gen};
        lyric_detail::RequestDiagnosticsScope diagnostics(cb);
        const wchar_t* requestSource = requestPlayer == SmtcPlayerType::SaltPlayer ? L"SaltPlayer"
                                      : requestPlayer == SmtcPlayerType::Spotify   ? L"Spotify"
                                      : requestPlayer == SmtcPlayerType::Generic   ? L"Generic"
                                                                                   : L"QQ";
        const auto player = requestPlayer;
        runtime_log::writef(L"[lyric][request][%ls] title=\"%ls\" artist=\"%ls\" duration=%lldms",
                            requestSource, title.c_str(), artist.c_str(),
                            static_cast<long long>(durationMs));
        bool customLocalConfigured = false;
        bool qqLocalConfigured = false;
        LyricResult ready;
        {
            std::lock_guard<std::mutex> lk(impl->mtx);
            if (impl->generation != gen)
                return;
            CacheEntry entry;
            if (impl->manualOverrideGet(key, title, artist, player, entry)) {
                ready = {true, std::move(entry.lines), std::move(entry.info), false, true};
                runtime_log::writef(L"[lyric][source=manual][%ls] title=\"%ls\" artist=\"%ls\"",
                                    requestSource, title.c_str(), artist.c_str());
            } else {
                customLocalConfigured =
                    impl->customLocalLyricsEnabled && !impl->customLocalLyricsDir.empty();
                qqLocalConfigured =
                    !sampled && impl->qqLocalLyricsEnabled && !impl->qqLocalLyricsDir.empty();
                if (!customLocalConfigured && !qqLocalConfigured && impl->cacheGet(key, entry)) {
                    ready = {true, std::move(entry.lines), std::move(entry.info)};
                    ready.fromCache = true;
                    runtime_log::writef(L"[lyric][source=online-cache][%ls] title=\"%ls\" artist=\"%ls\"",
                                        requestSource, title.c_str(), artist.c_str());
                }
            }
        }
        if (ready.ok) {
            if (impl->generation == gen && cb)
                cb(std::move(ready));
            return;
        }
        const bool anyLocalConfigured = customLocalConfigured || qqLocalConfigured;
        std::vector<LyricLine> result;
        SongInfo info;
        bool fromCache = false;

        // 本地索引可能仍在构建，顺序标记的读写必须留在工作线程，不能阻塞 UI 线程。
        if (persistOrder && (effectiveOrder == OrderPreference::PreferOnline ||
                             effectiveOrder == OrderPreference::PreferLocal)) {
            const bool wantOnlineFirst = effectiveOrder == OrderPreference::PreferOnline;
            if (customLocalConfigured)
                impl->setCustomLocalOrderOverride(key, title, artist, player, wantOnlineFirst);
            if (qqLocalConfigured)
                impl->setLocalOrderOverride(title, artist, wantOnlineFirst);
        }

        bool onlineFirst = effectiveOrder == OrderPreference::PreferOnline;
        if (effectiveOrder == OrderPreference::Default && anyLocalConfigured) {
            onlineFirst = (customLocalConfigured &&
                           impl->customLocalOrderOnlineFirst(key, title, artist, player)) ||
                          (qqLocalConfigured && impl->localOrderOnlineFirst(title, artist));
        }
        const bool useLocalLyrics = anyLocalConfigured && !onlineFirst;

        auto loadLocal = [&]() -> bool {
            if (customLocalConfigured &&
                impl->lookupCustomLocalLyrics(title, artist, durationMs, result, info)) {
                runtime_log::writef(
                    L"[lyric][source=custom-local][%ls] format=%ls title=\"%ls\" artist=\"%ls\" "
                    L"lines=%zu",
                    requestSource,
                    info.lyricOrigin == LyricOrigin::LocalAudio ? L"audio-tag" : L"lrc",
                    title.c_str(), artist.c_str(), result.size());
                return true;
            }
            if (qqLocalConfigured && impl->lookupLocalLyrics(title, artist, result, info)) {
                runtime_log::writef(L"[lyric][source=local][QQ] title=\"%ls\" artist=\"%ls\" "
                                    L"lines=%zu",
                                    title.c_str(), artist.c_str(), result.size());
                return true;
            }
            return false;
        };

        auto loadOnline = [&]() -> bool {
            {
                std::lock_guard<std::mutex> lk(impl->mtx);
                if (impl->generation != gen)
                    return false;
                CacheEntry cached;
                if (impl->cacheGet(key, cached)) {
                    result = std::move(cached.lines);
                    info = std::move(cached.info);
                    fromCache = true;
                    runtime_log::writef(
                        L"[lyric][source=online-cache][%ls] title=\"%ls\" artist=\"%ls\"",
                        requestSource, title.c_str(), artist.c_str());
                    return true;
                }
            }
            runtime_log::writef(L"[lyric][source=online][%ls] fetching title=\"%ls\" artist=\"%ls\"",
                                requestSource, title.c_str(), artist.c_str());
            const bool ok = impl->fetchOnline(title, artist, album, durationMs, result, info,
                                              player, request);
            runtime_log::writef(L"[lyric][source=online][%ls] result=%ls title=\"%ls\" "
                                L"artist=\"%ls\" lines=%zu",
                                requestSource, ok ? L"success" : L"failed", title.c_str(),
                                artist.c_str(), result.size());
            return ok;
        };

        bool localHit = false;
        bool ok = false;
        if (useLocalLyrics) {
            localHit = loadLocal();
            ok = localHit;
            if (!localHit) {
                runtime_log::writef(L"[lyric][local-miss][%ls] title=\"%ls\" artist=\"%ls\" "
                                    L"fallback=online",
                                    requestSource, title.c_str(), artist.c_str());
                ok = loadOnline();
            }
        } else {
            ok = loadOnline();
            if (!ok && onlineFirst && anyLocalConfigured) {
                {
                    std::lock_guard<std::mutex> lk(impl->mtx);
                    if (impl->generation != gen)
                        return;
                }
                // 在线优先失败时，按“手动 -> 在线 -> 本地”继续尝试本地歌词。
                result.clear();
                info = SongInfo{};
                localHit = loadLocal();
                ok = localHit;
            }
        }

        {
            std::lock_guard<std::mutex> lk(impl->mtx);
            if (impl->generation != gen)
                return;
            if (ok && !localHit)
                impl->cachePut(key, CacheEntry{result, info});
        }
        if (cb)
            cb(LyricResult{ok, std::move(result), std::move(info), localHit, false, fromCache});
    });
    std::lock_guard<std::mutex> lk(impl_->mtx);
    impl_->sweepFinished();
    impl_->workers.push_back(std::move(worker));
}

void LyricProvider::requestNeteaseAsync(const std::wstring& songId,
                                        const std::wstring& title,
                                        const std::wstring& artist,
                                        const std::wstring& album,
                                        int64_t durationMs, ReadyCallback cb,
                                        OrderPreference order, bool persistOrder) {
    runtime_log::writef(L"[lyric][request][Netease] songid=\"%ls\" title=\"%ls\" "
                        L"artist=\"%ls\" duration=%lldms",
                        songId.c_str(), title.c_str(), artist.c_str(),
                        static_cast<long long>(durationMs));
    const std::wstring key = songId.empty() ? L"netease|" + makeKey(title, artist, durationMs) +
                                              L'|' + album
                                            : L"netease|" + songId;
    const uint64_t gen = ++impl_->generation;
    Impl* impl = impl_.get();
    Impl::Worker worker;
    auto done = worker.done;
    worker.thread = std::thread([impl, gen, key, songId, title, artist, album, durationMs,
                                 cb = std::move(cb), order, persistOrder, done]() mutable {
        DoneFlag flag{done};
        const lyric_detail::LyricRequest request{impl->generation, gen};
        lyric_detail::RequestDiagnosticsScope diagnostics(cb);
        bool customLocalConfigured = false;
        LyricResult ready;
        {
            std::lock_guard<std::mutex> lk(impl->mtx);
            if (impl->generation != gen)
                return;
            CacheEntry entry;
            if (!songId.empty() &&
                impl->manualOverrideGet(key, title, artist, SmtcPlayerType::NetEase, entry)) {
                ready = {true, std::move(entry.lines), std::move(entry.info), false, true};
                runtime_log::writef(L"[lyric][source=manual][Netease] songid=\"%ls\"",
                                    songId.c_str());
            } else {
                customLocalConfigured =
                    impl->customLocalLyricsEnabled && !impl->customLocalLyricsDir.empty();
                if (!customLocalConfigured && impl->cacheGet(key, entry)) {
                    ready = {true, std::move(entry.lines), std::move(entry.info)};
                    ready.fromCache = true;
                    runtime_log::writef(L"[lyric][source=online-cache][Netease] songid=\"%ls\"",
                                        songId.c_str());
                }
            }
        }
        if (ready.ok) {
            if (impl->generation == gen && cb)
                cb(std::move(ready));
            return;
        }

        if (persistOrder && customLocalConfigured &&
            (order == OrderPreference::PreferOnline || order == OrderPreference::PreferLocal)) {
            impl->setCustomLocalOrderOverride(key, title, artist, SmtcPlayerType::NetEase,
                                              order == OrderPreference::PreferOnline);
        }

        bool onlineFirst = order == OrderPreference::PreferOnline;
        if (order == OrderPreference::Default && customLocalConfigured) {
            onlineFirst = impl->customLocalOrderOnlineFirst(key, title, artist,
                                                            SmtcPlayerType::NetEase);
        }
        const bool useLocalLyrics = customLocalConfigured && !onlineFirst;

        std::vector<LyricLine> result;
        SongInfo info;
        bool fromCache = false;

        auto loadLocal = [&]() -> bool {
            if (customLocalConfigured &&
                impl->lookupCustomLocalLyrics(title, artist, durationMs, result, info)) {
                runtime_log::writef(
                    L"[lyric][source=custom-local][Netease] format=%ls title=\"%ls\" "
                    L"artist=\"%ls\" lines=%zu",
                    info.lyricOrigin == LyricOrigin::LocalAudio ? L"audio-tag" : L"lrc",
                    title.c_str(), artist.c_str(), result.size());
                return true;
            }
            return false;
        };

        auto loadOnline = [&]() -> bool {
            {
                std::lock_guard<std::mutex> lk(impl->mtx);
                if (impl->generation != gen)
                    return false;
                CacheEntry cached;
                if (impl->cacheGet(key, cached)) {
                    result = std::move(cached.lines);
                    info = std::move(cached.info);
                    fromCache = true;
                    runtime_log::writef(L"[lyric][source=online-cache][Netease] songid=\"%ls\"",
                                        songId.c_str());
                    return true;
                }
            }
            bool ok = !songId.empty() && impl->fetchNetease(songId, result, info);
            if (ok) {
                info.matchedTitle = title;
                info.matchedArtist = artist;
                info.matchedDurationMs = durationMs;
            }
            runtime_log::writef(L"[lyric][source=netease] result=%s songid=\"%ls\" lines=%zu",
                                ok ? L"success" : L"failed", songId.c_str(), result.size());
            if (!ok) {
                ok = impl->fetchOnline(title, artist, album, durationMs, result, info,
                                       SmtcPlayerType::NetEase, request);
            }
            return ok;
        };

        bool localHit = false;
        bool ok = false;
        if (useLocalLyrics) {
            localHit = loadLocal();
            ok = localHit;
            if (!localHit)
                ok = loadOnline();
        } else {
            ok = loadOnline();
            if (!ok && onlineFirst && customLocalConfigured) {
                {
                    std::lock_guard<std::mutex> lk(impl->mtx);
                    if (impl->generation != gen)
                        return;
                }
                result.clear();
                info = SongInfo{};
                localHit = loadLocal();
                ok = localHit;
            }
        }
        {
            std::lock_guard<std::mutex> lk(impl->mtx);
            if (impl->generation != gen)
                return;
            if (ok && !localHit)
                impl->cachePut(key, CacheEntry{result, info});
        }
        if (cb)
            cb(LyricResult{ok, std::move(result), std::move(info), localHit, false, fromCache});
    });
    std::lock_guard<std::mutex> lk(impl_->mtx);
    impl_->sweepFinished();
    impl_->workers.push_back(std::move(worker));
}

std::wstring LyricProvider::makeKey(const std::wstring& title, const std::wstring& artist,
                                    int64_t durationMs) {
    // 时长按 5 秒分桶：区分同名不同版本（专辑版/Live 版等），同时容忍轻微的上报差异；
    // 时长未知固定为 -1
    return title + L'|' + artist + L'|' + std::to_wstring(durationMs > 0 ? durationMs / 5000 : -1);
}

std::wstring LyricProvider::makeSpotifyKey(const std::wstring& title, const std::wstring& artist,
                                           const std::wstring& album, int64_t durationMs) {
    return L"spotify|" + makeKey(title, artist, durationMs) + L'|' + album;
}

std::wstring LyricProvider::makeSaltPlayerKey(const std::wstring& title,
                                              const std::wstring& artist,
                                              const std::wstring& album, int64_t durationMs) {
    return L"saltplayer|" + makeKey(title, artist, durationMs) + L'|' + album;
}

std::wstring LyricProvider::makeGenericKey(const std::wstring& title,
                                           const std::wstring& artist,
                                           const std::wstring& album, int64_t durationMs) {
    return L"generic|" + makeKey(title, artist, durationMs) + L'|' + album;
}

int LyricProvider::findLine(const std::vector<LyricLine>& lines, int64_t positionMs) {
    int lo = 0;
    int hi = static_cast<int>(lines.size()) - 1;
    int ans = -1;
    while (lo <= hi) {
        const int mid = lo + (hi - lo) / 2;
        if (lines[mid].ms <= positionMs) {
            ans = mid;
            lo = mid + 1;
        } else {
            hi = mid - 1;
        }
    }
    return ans;
}

void LyricProvider::searchCandidatesAsync(const std::wstring& title, const std::wstring& artist,
                                          SearchCallback cb, SmtcPlayerType player) {
    runtime_log::writef(L"[action][manual-search] request title=\"%ls\" artist=\"%ls\"",
                        title.c_str(), artist.c_str());
    Impl* impl = impl_.get();
    Impl::Worker worker;
    auto done = worker.done;
    worker.thread = std::thread([impl, title, artist, cb = std::move(cb), done, player]() mutable {
        DoneFlag flag{done};
        std::vector<SearchCandidate> result = impl->searchCustomLocalLyrics(title, artist);
        CURL* curl = curl_easy_init();
        std::vector<lyric_detail::SearchVariant> queries;
        lyric_detail::addSearchVariant(queries, title, artist);
        lyric_detail::addSearchVariant(queries, lyric_detail::simplifiedChinese(title),
                                       lyric_detail::simplifiedChinese(artist));
        for (const auto& search : queries) {
            const auto& searchTitle = search.title;
            const auto& searchArtist = search.artist;
            auto appendSodaCandidates = [&] {
                std::vector<lyric_detail::SodaSong> songs;
                if (lyric_detail::searchSodaSongs(curl, searchTitle, searchArtist, songs)) {
                    for (const auto& song : songs) {
                        SearchCandidate candidate;
                        candidate.name = song.title;
                        candidate.singer = song.artist;
                        candidate.durationMs = song.durationMs;
                        candidate.source = LyricSource::Soda;
                        candidate.sodaSongId = lyric_detail::toWide(song.id);
                        candidate.previewStartMs = song.previewStartMs;
                        candidate.previewDurationMs = song.previewDurationMs;
                        result.push_back(std::move(candidate));
                    }
                }
            };
            if (curl) {
                const std::wstring queryTitle = lyric_detail::stripVersionSuffix(searchTitle);
                std::wstring query = queryTitle;
                if (!searchArtist.empty() &&
                    !lyric_detail::titleEndsWithArtist(searchTitle, searchArtist))
                    query += L' ' + searchArtist;

                auto appendNeteaseCandidates = [&] {
                    std::vector<lyric_detail::NeteaseSong> neteaseSongs;
                    bool neteaseSearchOk =
                        lyric_detail::searchNeteaseSongs(curl, query, 5, neteaseSongs);
                    if (neteaseSongs.empty())
                        neteaseSearchOk =
                            lyric_detail::searchNeteaseSongs(curl, queryTitle, 5, neteaseSongs);
                    if (neteaseSearchOk) {
                        for (const auto& song : neteaseSongs) {
                            result.push_back({{}, {}, {}, {}, song.name, song.singer,
                                              song.durationMs, LyricSource::Yrc,
                                              lyric_detail::toWide(song.id)});
                        }
                    }
                };
                if (player == SmtcPlayerType::Soda)
                    appendSodaCandidates();
                if (player == SmtcPlayerType::NetEase)
                    appendNeteaseCandidates();

                std::vector<lyric_detail::KugouSong> kugouSongs;
                if (lyric_detail::kugouSearchSongs(curl, queryTitle, searchArtist, 5, kugouSongs)) {
                    for (const auto& song : kugouSongs) {
                        result.push_back({{}, {}, {}, lyric_detail::toWide(song.hash), song.name,
                                          song.singer, song.durationMs, LyricSource::Krc});
                    }
                }

                std::vector<lyric_detail::QqCandidate> cands;
                lyric_detail::searchQqSongs(curl, query, cands);
                if (cands.empty())
                    lyric_detail::searchQqSongs(curl, queryTitle, cands);
                for (size_t index = 0; index < cands.size() && index < 5; ++index) {
                    const auto& candidate = cands[index];
                    if (!candidate.songid.empty()) {
                        result.push_back({lyric_detail::toWide(candidate.songmid),
                                          lyric_detail::toWide(candidate.songid),
                                          lyric_detail::toWide(candidate.albummid), {},
                                          candidate.name, candidate.singer, candidate.intervalMs,
                                          LyricSource::Qrc});
                    }
                    if (!candidate.songmid.empty()) {
                        result.push_back({lyric_detail::toWide(candidate.songmid),
                                          lyric_detail::toWide(candidate.songid),
                                          lyric_detail::toWide(candidate.albummid), {},
                                          candidate.name, candidate.singer, candidate.intervalMs,
                                          LyricSource::Lrc});
                    }
                }
                lyric_detail::searchLrclib(curl, searchTitle, searchArtist, result);
                if (player != SmtcPlayerType::NetEase)
                    appendNeteaseCandidates();
                if (player != SmtcPlayerType::Soda)
                    appendSodaCandidates();
            }
        }

        std::unordered_set<std::wstring> seen;
        std::erase_if(result, [&](const SearchCandidate& candidate) {
            std::wstring id;
            switch (candidate.source) {
            case LyricSource::Krc:
                id = candidate.kugouHash;
                break;
            case LyricSource::Qrc:
                id = candidate.songid;
                break;
            case LyricSource::Lrc:
                id = candidate.songmid;
                break;
            case LyricSource::Yrc:
                id = candidate.neteaseSongId;
                break;
            case LyricSource::Soda:
                id = candidate.sodaSongId;
                break;
            case LyricSource::Lrclib:
                id = std::to_wstring(candidate.lrclibId);
                break;
            case LyricSource::LocalLrc:
            case LyricSource::LocalAudio:
                id = candidate.localPath;
                break;
            }
            return !seen.insert(std::to_wstring(static_cast<int>(candidate.source)) + L"|" + id)
                        .second;
        });
        if (curl)
            curl_easy_cleanup(curl);
        runtime_log::writef(L"[action][manual-search] candidates title=\"%ls\" artist=\"%ls\" "
                            L"count=%zu",
                            title.c_str(), artist.c_str(), result.size());
        if (cb)
            cb(std::move(result));
    });
    std::lock_guard<std::mutex> lk(impl_->mtx);
    impl_->sweepFinished();
    impl_->workers.push_back(std::move(worker));
}

void LyricProvider::requestQqSongInfoAsync(const std::wstring& title,
                                           const std::wstring& artist,
                                           int64_t durationMs,
                                           SongInfoCallback cb) {
    runtime_log::writef(L"[cover][QQ] song-info request title=\"%ls\" artist=\"%ls\" "
                        L"duration=%lldms",
                        title.c_str(), artist.c_str(), static_cast<long long>(durationMs));
    Impl::Worker worker;
    auto done = worker.done;
    worker.thread = std::thread(
        [title, artist, durationMs, cb = std::move(cb), done]() mutable {
            DoneFlag flag{done};
            SongInfo info;
            CURL* curl = curl_easy_init();
            if (curl) {
                lyric_detail::fillQqSongInfo(curl, title, artist, durationMs, info);
                curl_easy_cleanup(curl);
            }
            runtime_log::writef(L"[cover][QQ] song-info result title=\"%ls\" artist=\"%ls\" "
                                L"albummid=%ls",
                                title.c_str(), artist.c_str(),
                                info.albummid.empty() ? L"(empty)" : info.albummid.c_str());
            if (cb)
                cb(info);
        });
    std::lock_guard<std::mutex> lk(impl_->mtx);
    impl_->sweepFinished();
    impl_->workers.push_back(std::move(worker));
}

void LyricProvider::fetchLyricAsync(const SearchCandidate& cand, FetchCallback cb,
                                    int64_t playbackDurationMs) {
    runtime_log::writef(L"[action][manual-search] preview name=\"%ls\" artist=\"%ls\" "
                        L"source=%d",
                        cand.name.c_str(), cand.singer.c_str(), static_cast<int>(cand.source));
    Impl::Worker worker;
    auto done = worker.done;
    worker.thread = std::thread([cand, cb = std::move(cb), done, playbackDurationMs]() mutable {
        DoneFlag flag{done};
        if (cand.source == LyricSource::LocalLrc || cand.source == LyricSource::LocalAudio) {
            std::vector<LyricLine> lines;
            SongInfo info;
            const bool ok = lyric_detail::loadCustomLocalLyricFile(
                std::filesystem::path(cand.localPath),
                cand.source == LyricSource::LocalLrc ? lyric_detail::CustomLocalKind::LrcFile
                                                     : lyric_detail::CustomLocalKind::AudioEmbedded,
                lines, info);
            if (info.matchedTitle.empty())
                info.matchedTitle = cand.name;
            if (info.matchedArtist.empty())
                info.matchedArtist = cand.singer;
            runtime_log::writef(L"[action][manual-search] preview-local name=\"%ls\" "
                                L"source=%d result=%s lines=%zu",
                                cand.name.c_str(), static_cast<int>(cand.source),
                                ok ? L"success" : L"failed", lines.size());
            if (cb)
                cb(ok, std::move(lines), info);
            return;
        }
        CURL* curl = curl_easy_init();
        std::vector<LyricLine> lines;
        bool ok = false;
        if (curl) {
            if (cand.source == LyricSource::Lrclib) {
                if (cand.lrclibId > 0) {
                    lines = lyric_detail::parseLrclibLyrics(
                        lyric_detail::getLrclib(curl, "get/" + std::to_string(cand.lrclibId)));
                }
                ok = !lines.empty();
            } else if (cand.source == LyricSource::Soda) {
                const lyric_detail::SodaSong song{lyric_detail::toUtf8(cand.sodaSongId), cand.name,
                                                  cand.singer, cand.durationMs, cand.previewStartMs,
                                                  cand.previewDurationMs};
                ok = lyric_detail::downloadSodaLyrics(curl, song, playbackDurationMs, lines);
            } else if (cand.source == LyricSource::Qrc) {
                ok = lyric_detail::downloadQqQrc(curl, lyric_detail::toUtf8(cand.songid), lines);
            } else if (cand.source == LyricSource::Krc) {
                ok = lyric_detail::fetchKrcByHash(curl, lyric_detail::toUtf8(cand.kugouHash),
                                                  cand.durationMs, lines);
            } else if (cand.source == LyricSource::Yrc) {
                ok = lyric_detail::downloadNeteaseLyric(curl, cand.neteaseSongId, lines);
            } else {
                ok = lyric_detail::downloadQqLrc(curl, lyric_detail::toUtf8(cand.songmid), lines);
            }
            if (ok && cand.source != LyricSource::Lrc && !cand.songmid.empty())
                lyric_detail::attachQqSecondary(curl, lyric_detail::toUtf8(cand.songmid), lines);
            curl_easy_cleanup(curl);
        }
        runtime_log::writef(L"[action][manual-search] preview-result name=\"%ls\" "
                            L"source=%d result=%s lines=%zu",
                            cand.name.c_str(), static_cast<int>(cand.source),
                            ok ? L"success" : L"failed", lines.size());
        SongInfo info{cand.songmid, cand.albummid, cand.neteaseSongId};
        info.matchedTitle = cand.name;
        info.matchedArtist = cand.singer;
        info.matchedDurationMs = cand.durationMs;
        if (cand.source == LyricSource::Soda) {
            info.sourceOffsetMs = -soda_lyrics::previewOffset(
                cand.durationMs, cand.previewStartMs, cand.previewDurationMs, playbackDurationMs);
        }
        switch (cand.source) {
        case LyricSource::Qrc:
        case LyricSource::Lrc:
            info.lyricOrigin = LyricOrigin::QqMusic;
            break;
        case LyricSource::Krc:
            info.lyricOrigin = LyricOrigin::Kugou;
            break;
        case LyricSource::Yrc:
            info.lyricOrigin = LyricOrigin::Netease;
            break;
        case LyricSource::Soda:
            info.lyricOrigin = LyricOrigin::Soda;
            break;
        case LyricSource::Lrclib:
            info.lyricOrigin = LyricOrigin::Lrclib;
            break;
        case LyricSource::LocalLrc:
            info.lyricOrigin = LyricOrigin::LocalLrc;
            break;
        case LyricSource::LocalAudio:
            info.lyricOrigin = LyricOrigin::LocalAudio;
            break;
        }
        if (cb)
            cb(ok, std::move(lines), info);
    });
    std::lock_guard<std::mutex> lk(impl_->mtx);
    impl_->sweepFinished();
    impl_->workers.push_back(std::move(worker));
}

void LyricProvider::requestSodaAsync(const std::wstring& title, const std::wstring& artist,
                                     const std::wstring& album, int64_t durationMs,
                                     ReadyCallback cb, OrderPreference order, bool persistOrder) {
    const std::wstring key = L"soda|" + makeKey(title, artist, durationMs);
    const uint64_t gen = ++impl_->generation;
    Impl* impl = impl_.get();
    Impl::Worker worker;
    auto done = worker.done;
    worker.thread = std::thread([impl, gen, key, title, artist, album, durationMs,
                                 cb = std::move(cb), order, persistOrder, done]() mutable {
        DoneFlag flag{done};
        const lyric_detail::LyricRequest request{impl->generation, gen};
        lyric_detail::RequestDiagnosticsScope diagnostics(cb);
        bool customLocalConfigured = false;
        LyricResult ready;
        {
            std::lock_guard<std::mutex> lock(impl->mtx);
            if (impl->generation != gen)
                return;
            CacheEntry entry;
            if (impl->manualOverrideGet(key, title, artist, SmtcPlayerType::Soda, entry)) {
                ready = {true, std::move(entry.lines), std::move(entry.info), false, true};
            } else {
                customLocalConfigured =
                    impl->customLocalLyricsEnabled && !impl->customLocalLyricsDir.empty();
                if (!customLocalConfigured && impl->cacheGet(key, entry)) {
                    ready = {true, std::move(entry.lines), std::move(entry.info)};
                    ready.fromCache = true;
                }
            }
        }
        if (!ready.ok) {
            if (persistOrder && customLocalConfigured &&
                (order == OrderPreference::PreferOnline || order == OrderPreference::PreferLocal)) {
                impl->setCustomLocalOrderOverride(key, title, artist, SmtcPlayerType::Soda,
                                                  order == OrderPreference::PreferOnline);
            }
            bool onlineFirst = order == OrderPreference::PreferOnline;
            if (order == OrderPreference::Default && customLocalConfigured) {
                onlineFirst = impl->customLocalOrderOnlineFirst(key, title, artist,
                                                                SmtcPlayerType::Soda);
            }
            const bool useLocalLyrics = customLocalConfigured && !onlineFirst;

            auto loadLocal = [&]() -> bool {
                if (customLocalConfigured &&
                    impl->lookupCustomLocalLyrics(title, artist, durationMs, ready.lines,
                                                  ready.info)) {
                    ready.fromLocal = true;
                    runtime_log::writef(
                        L"[lyric][source=custom-local][Soda Music] format=%ls title=\"%ls\" "
                        L"artist=\"%ls\" lines=%zu",
                        ready.info.lyricOrigin == LyricOrigin::LocalAudio ? L"audio-tag" : L"lrc",
                        title.c_str(), artist.c_str(), ready.lines.size());
                    return true;
                }
                return false;
            };

            auto loadOnline = [&]() -> bool {
                {
                    std::lock_guard<std::mutex> lock(impl->mtx);
                    if (request.cancelled())
                        return false;
                    CacheEntry cached;
                    if (impl->cacheGet(key, cached)) {
                        ready.lines = std::move(cached.lines);
                        ready.info = std::move(cached.info);
                        ready.fromCache = true;
                        return true;
                    }
                }
                // Soda 的歌名和真实时长通常分两批到达，合并这段事件再联网。
                // 缓存、本地和手动覆盖不等待；新代次同时让旧请求在 curl 中止或循环边界退出。
                std::this_thread::sleep_for(std::chrono::milliseconds(250));
                if (request.cancelled())
                    return false;
                runtime_log::writef(L"[lyric][request][Soda Music] title=\"%ls\" artist=\"%ls\" duration=%lldms",
                                    title.c_str(), artist.c_str(), static_cast<long long>(durationMs));
                bool ok = lyric_detail::fetchSodaLyrics(title, artist, durationMs, ready.lines,
                                                        request, ready.info);
                if (ok)
                    ready.info.lyricOrigin = LyricOrigin::Soda;
                if (!ok) {
                    ok = impl->fetchOnline(title, artist, album, durationMs, ready.lines,
                                           ready.info, SmtcPlayerType::Soda, request);
                }
                return ok;
            };

            if (useLocalLyrics) {
                ready.ok = loadLocal();
                if (!ready.ok)
                    ready.ok = loadOnline();
            } else {
                ready.ok = loadOnline();
                if (!ready.ok && onlineFirst && customLocalConfigured && !request.cancelled()) {
                    ready.lines.clear();
                    ready.info = SongInfo{};
                    ready.ok = loadLocal();
                }
            }
            std::lock_guard<std::mutex> lock(impl->mtx);
            if (request.cancelled())
                return;
            if (ready.ok && !ready.fromLocal)
                impl->cachePut(key, CacheEntry{ready.lines, ready.info});
        }
        runtime_log::writef(L"[lyric][Soda Music] result=%ls source=%ls lines=%zu",
                            ready.ok ? L"success" : L"failed",
                            ready.fromManual  ? L"manual"
                            : ready.fromLocal ? L"local"
                                              : L"online",
                            ready.lines.size());
        if (!request.cancelled() && cb)
            cb(std::move(ready));
    });
    std::lock_guard<std::mutex> lock(impl_->mtx);
    impl_->sweepFinished();
    impl_->workers.push_back(std::move(worker));
}

void LyricProvider::setManualOverride(const std::wstring& title, const std::wstring& artist,
                                      int64_t durationMs, std::vector<LyricLine> lines,
                                      SongInfo info, SmtcPlayerType player,
                                      const std::wstring& album) {
    std::lock_guard<std::mutex> lk(impl_->mtx);
    const std::wstring key =
        player == SmtcPlayerType::SaltPlayer
            ? makeSaltPlayerKey(title, artist, album, durationMs)
            : player == SmtcPlayerType::Spotify ? makeSpotifyKey(title, artist, album, durationMs)
            : player == SmtcPlayerType::Generic ? makeGenericKey(title, artist, album, durationMs)
            : player == SmtcPlayerType::NetEase ? L"netease|" + info.neteaseSongId
            : (player == SmtcPlayerType::Soda ? L"soda|" : L"") + makeKey(title, artist, durationMs);
    auto& stored = impl_->manualOverrides[key]; // 同一首歌重复手动选择时覆盖旧记录
    stored = CacheEntry{std::move(lines), std::move(info)};
    // 写入这首歌自己的文件
    const std::wstring path = impl_->overrideFilePath(key, title, artist, player, false);
    if (!path.empty())
        impl_->saveOverrideFile(path, title, artist, durationMs, stored);
    runtime_log::writef(L"[action][manual-search] applied title=\"%ls\" artist=\"%ls\" "
                        L"duration=%lldms lines=%zu path=\"%ls\"",
                        title.c_str(), artist.c_str(), static_cast<long long>(durationMs),
                        stored.lines.size(),
                        path.c_str());
}

void LyricProvider::setManualOverrideDir(const std::wstring& dir) {
    std::lock_guard<std::mutex> lk(impl_->mtx);
    impl_->overrideDir = dir;
}

void LyricProvider::setQqLocalLyricsConfig(bool enabled, const std::wstring& dir) {
    std::lock_guard<std::mutex> lk(impl_->mtx);
    if (impl_->qqLocalLyricsEnabled == enabled && impl_->qqLocalLyricsDir == dir)
        return;
    impl_->qqLocalLyricsEnabled = enabled;
    impl_->qqLocalLyricsDir = dir;
    ++impl_->generation;
    impl_->sweepFinished();
    impl_->rebuildLocalIndexLocked();
}

void LyricProvider::setQqLyricOrderDir(const std::wstring& dir) {
    std::lock_guard<std::mutex> lk(impl_->mtx);
    if (impl_->qqLocalLyricOrderDir == dir)
        return;
    impl_->qqLocalLyricOrderDir = dir;
    impl_->rebuildLocalIndexLocked();
}

void LyricProvider::setCustomLocalLyricsConfig(bool enabled, const std::wstring& dir) {
    std::lock_guard<std::mutex> lk(impl_->mtx);
    if (impl_->customLocalLyricsEnabled == enabled && impl_->customLocalLyricsDir == dir)
        return;
    impl_->customLocalLyricsEnabled = enabled;
    impl_->customLocalLyricsDir = dir;
    ++impl_->generation;
    impl_->sweepFinished();
    impl_->rebuildCustomLocalIndexLocked();
}

void LyricProvider::setCustomLocalLyricOrderDir(const std::wstring& dir) {
    std::lock_guard<std::mutex> lk(impl_->mtx);
    impl_->customLocalLyricOrderDir = dir;
}
