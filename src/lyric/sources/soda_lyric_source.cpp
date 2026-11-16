#include "lyric/sources/soda_lyric_source.h"

#include "logging/runtime_logger.h"
#include "lyric/lrc_parser.h"
#include "lyric/lyric_matching.h"
#include "lyric/soda_lyric_timing.h"
#include "lyric/sources/kugou_lyric_source.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <memory>
#include <unordered_set>
#include <utility>

namespace lyric_detail {

bool searchSodaSongs(CURL* curl, const std::wstring& title, const std::wstring& artist,
                     std::vector<SodaSong>& out, const LyricRequest* request) {
    out.clear();
    if (!curl || isLyricShuttingDown())
        return false;
    const std::string keyword = toUtf8(makeSearchQuery({title, artist}));
    char* escaped = curl_easy_escape(curl, keyword.c_str(), static_cast<int>(keyword.size()));
    if (!escaped)
        return false;
    const std::string searchUrl = "https://api.qishui.com/luna/search/track?q=" +
                                  std::string(escaped) + "&cursor=0&count=20&aid=386088";
    curl_free(escaped);
    std::string body;
    if (!httpGet(curl, searchUrl, "https://music.douyin.com/", body, request, L"search"))
        return false;
    try {
        const auto root = nlohmann::json::parse(body, nullptr, false);
        if (!root.is_object() || !root.contains("result_groups") || !root["result_groups"].is_array()) {
            runtime_log::writef(L"[lyric][Soda Music] invalid search response title=\"%ls\" artist=\"%ls\"",
                                title.c_str(), artist.c_str());
            return false;
        }
        for (const auto& group : root["result_groups"]) {
            if (!group.is_object() || group.value("id", std::string()) != "tracks" ||
                !group.contains("data") || !group["data"].is_array())
                continue;
            for (const auto& item : group["data"]) {
                if (!item.is_object() || !item.contains("entity") || !item["entity"].is_object())
                    continue;
                const auto& entity = item["entity"];
                if (!entity.contains("track") || !entity["track"].is_object())
                    continue;
                const auto& track = entity["track"];
                SodaSong song;
                if (track.contains("id"))
                    song.id = jsonString(track["id"]);
                if (song.id.empty() || song.id.find_first_not_of("0123456789") != std::string::npos)
                    continue;
                song.title = toWide(track.value("name", std::string()));
                song.durationMs = track.value("duration", int64_t{0});
                if (track.contains("artists") && track["artists"].is_array()) {
                    for (const auto& singer : track["artists"]) {
                        const std::wstring name = toWide(singer.value("name", std::string()));
                        if (!name.empty()) {
                            if (!song.artist.empty())
                                song.artist += L'/';
                            song.artist += name;
                        }
                    }
                }
                if (track.contains("preview") && track["preview"].is_object()) {
                    song.previewStartMs = track["preview"].value("start", int64_t{0});
                    song.previewDurationMs = track["preview"].value("duration", int64_t{0});
                }
                out.push_back(std::move(song));
            }
        }
        runtime_log::writef(L"[lyric][Soda Music] search title=\"%ls\" artist=\"%ls\" candidates=%zu",
                            title.c_str(), artist.c_str(), out.size());
        return true;
    } catch (const nlohmann::json::exception&) {
        runtime_log::writef(L"[lyric][Soda Music] invalid native API response");
    }
    return false;
}

bool downloadSodaLyrics(CURL* curl, const SodaSong& song, int64_t playbackDurationMs,
                        std::vector<LyricLine>& out, const LyricRequest* request) {
    out.clear();
    if (song.id.empty() || song.id.find_first_not_of("0123456789") != std::string::npos ||
        isLyricShuttingDown())
        return false;
    const std::string url = "https://beta-luna.douyin.com/luna/h5/seo_track?track_id=" +
                            song.id + "&device_platform=web";
    std::string body;
    if (!httpGet(curl, url, "https://music.douyin.com/", body, request, L"lyrics"))
        return false;
    try {
        const auto detail = nlohmann::json::parse(body, nullptr, false);
        if (!detail.is_object() || !detail.contains("seo_track") ||
            !detail["seo_track"].is_object() || !detail["seo_track"].contains("track") ||
            !detail["seo_track"]["track"].is_object() || !detail.contains("lyric") ||
            !detail["lyric"].is_object()) {
            runtime_log::writef(L"[lyric][Soda Music] missing lyric data id=%S", song.id.c_str());
            return false;
        }
        const auto& track = detail["seo_track"]["track"];
        if (!track.contains("id") || jsonString(track["id"]) != song.id)
            return false;
        const auto& lyric = detail["lyric"];
        const std::string content = lyric.value("content", std::string());
        const std::string type = lyric.value("type", std::string());
        std::vector<LyricLine> lines;
        if (type == "krc")
            parseKrc(content, lines);
        else if (type == "lrc")
            lines = parseLrcText(content);
        if (lines.empty()) {
            runtime_log::writef(L"[lyric][Soda Music] empty or unsupported lyrics id=%S type=%S bytes=%zu",
                                song.id.c_str(), type.c_str(), content.size());
            return false;
        }
        if (lyric.contains("translations") && lyric["translations"].is_object()) {
            const std::string translation = lyric["translations"].value("cn", std::string());
            attachSecondary(lines, parseLrcText(translation), true);
        }
        const int64_t offsetMs = soda_lyrics::previewOffset(
            song.durationMs, song.previewStartMs, song.previewDurationMs, playbackDurationMs);
        soda_lyrics::alignPreview(lines, offsetMs, playbackDurationMs, song.durationMs);
        if (lines.empty()) {
            runtime_log::writef(L"[lyric][Soda Music] empty aligned lyrics id=%S offset=%lldms duration=%lldms",
                                song.id.c_str(), static_cast<long long>(offsetMs),
                                static_cast<long long>(playbackDurationMs));
            return false;
        }
        runtime_log::writef(L"[lyric][Soda Music] native id=%S type=%S offset=%lldms lines=%zu",
                            song.id.c_str(), type.c_str(),
                            static_cast<long long>(offsetMs), lines.size());
        out = std::move(lines);
        return true;
    } catch (const nlohmann::json::exception&) {
        runtime_log::writef(L"[lyric][Soda Music] invalid lyric response");
        return false;
    }
}

bool fetchSodaLyrics(const std::wstring& title, const std::wstring& artist,
                     int64_t durationMs, std::vector<LyricLine>& out, const LyricRequest& request,
                     SongInfo& info) {
    out.clear();
    std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> curl(curl_easy_init(), &curl_easy_cleanup);
    if (!curl || request.cancelled())
        return false;
    const MatchQuery query{title, artist, durationMs, extractVersionTags(title)};
    std::unordered_set<std::wstring> searched;
    std::unordered_set<std::string> attempted;
    for (const auto& variant : buildSearchVariants(query)) {
        if (request.cancelled())
            return false;
        if (!searched.insert(makeSearchQuery(variant)).second)
            continue;
        std::vector<SodaSong> songs;
        // 网络失败不是关键词不匹配，不继续增加搜索请求。
        if (!searchSodaSongs(curl.get(), variant.title, variant.artist, songs, &request))
            return false;
        std::vector<std::pair<int, const SodaSong*>> ranked;
        int bestScore = 0;
        for (const auto& song : songs) {
            const int64_t matchedDurationMs =
                soda_lyrics::previewOffset(song.durationMs, song.previewStartMs,
                                           song.previewDurationMs, durationMs) > 0
                    ? song.previewDurationMs : song.durationMs;
            // 搜索可以简化关键词，录用仍用原始歌曲的版本、歌手和时长校验。
            const int score = scoreCandidate(query, song.title, song.artist, matchedDurationMs);
            bestScore = std::max(bestScore, score);
            if (score >= kScoreThreshold && !attempted.contains(song.id))
                ranked.emplace_back(score, &song);
        }
        runtime_log::writef(L"[lyric][Soda Music] match title=\"%ls\" candidates=%zu eligible=%zu best=%d duration=%lldms",
                            title.c_str(), songs.size(), ranked.size(), bestScore,
                            static_cast<long long>(durationMs));
        std::stable_sort(ranked.begin(), ranked.end(),
                         [](const auto& a, const auto& b) { return a.first > b.first; });
        for (size_t i = 0; i < std::min<size_t>(ranked.size(), 3); ++i) {
            if (request.cancelled())
                return false;
            attempted.insert(ranked[i].second->id);
            if (downloadSodaLyrics(curl.get(), *ranked[i].second, durationMs, out, &request)) {
                info.matchedTitle = ranked[i].second->title;
                info.matchedArtist = ranked[i].second->artist;
                info.matchedDurationMs = ranked[i].second->durationMs;
                info.sourceOffsetMs = -soda_lyrics::previewOffset(
                    ranked[i].second->durationMs, ranked[i].second->previewStartMs,
                    ranked[i].second->previewDurationMs, durationMs);
                return !request.cancelled();
            }
        }
    }
    return false;
}

} // namespace lyric_detail
