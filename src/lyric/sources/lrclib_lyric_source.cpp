#include "lyric/sources/lrclib_lyric_source.h"

#include "app_info.h"
#include "lyric/lrc_parser.h"
#include "lyric/lyric_matching.h"
#include "lyric/lyricsfile_parser.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <memory>
#include <mutex>

namespace lyric_detail {
namespace {

std::string lrclibEscape(CURL* curl, const std::wstring& value) {
    const auto utf8 = toUtf8(value);
    std::unique_ptr<char, decltype(&curl_free)> encoded(
        curl_easy_escape(curl, utf8.c_str(), static_cast<int>(utf8.size())), curl_free);
    return encoded ? std::string(encoded.get()) : std::string();
}

std::string lrclibQuery(CURL* curl, const std::wstring& title, const std::wstring& artist) {
    return "?track_name=" + lrclibEscape(curl, title) + "&artist_name=" + lrclibEscape(curl, artist);
}

bool lrclibHasWordSync(const nlohmann::json& record) {
    const auto lyricsfile = record.find("lyricsfile");
    return record.contains("hasWordSync") && record["hasWordSync"] == true &&
           lyricsfile != record.end() && lyricsfile->is_string() &&
           !lyricsfile->get_ref<const std::string&>().empty();
}

} // namespace

nlohmann::json getLrclib(CURL* curl, const std::string& resource,
                       const LyricRequest* request) {
    using Clock = std::chrono::steady_clock;
    static std::mutex mutex;
    static Clock::time_point retryAt;
    std::lock_guard<std::mutex> lock(mutex);
    if (Clock::now() < retryAt || isLyricShuttingDown() || (request && request->cancelled()))
        return nullptr;

    static const std::string userAgent = "QQMusicLyric/" + toUtf8(app_info::kVersion) +
        " (https://github.com/H0308/qq-music-lyrics-displayer)";
    std::string body;
    if (httpGet(curl, "https://lrclib.net/api/" + resource, nullptr, body,
                request, nullptr, userAgent.c_str()))
        return nlohmann::json::parse(body, nullptr, false);

    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    if (status == 429 || status == 503) {
        curl_off_t retryAfter = 0;
        curl_easy_getinfo(curl, CURLINFO_RETRY_AFTER, &retryAfter);
        retryAt = Clock::now() + std::chrono::seconds(std::max<curl_off_t>(1, retryAfter));
    }
    return nullptr;
}

std::vector<LyricLine> parseLrclibLyrics(const nlohmann::json& record) {
    if (!record.is_object() || (record.contains("instrumental") && record["instrumental"] == true))
        return {};
    if (lrclibHasWordSync(record)) {
        auto lines = parseLyricsfile(record["lyricsfile"].get_ref<const std::string&>());
        if (!lines.empty())
            return lines;
    }
    const auto lyrics = record.find("syncedLyrics");
    return lyrics != record.end() && lyrics->is_string()
               ? parseLrcText(lyrics->get_ref<const std::string&>())
               : std::vector<LyricLine>{};
}

std::vector<LyricLine> fetchLrclib(const std::wstring& title, const std::wstring& artist,
                                   const std::wstring& album, int64_t durationMs,
                                   const LyricRequest& request, SongInfo& info) {
    if (title.empty() || artist.empty() || durationMs < 1000 || durationMs > 3600000)
        return {};
    std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> curl(curl_easy_init(), curl_easy_cleanup);
    if (!curl)
        return {};
    const auto simpleTitle = simplifiedChinese(title);
    const auto simpleArtist = simplifiedChinese(artist);
    const auto simpleAlbum = simplifiedChinese(album);
    const bool changed = simpleTitle != title || simpleArtist != artist || simpleAlbum != album;
    for (int attempt = 0; attempt < (changed ? 2 : 1); ++attempt) {
        if (request.cancelled())
            return {};
        const auto& queryAlbum = attempt == 0 ? album : simpleAlbum;
        std::string resource = "get" + lrclibQuery(curl.get(), attempt == 0 ? title : simpleTitle,
                                                  attempt == 0 ? artist : simpleArtist) +
                               "&duration=" + std::to_string((durationMs + 500) / 1000);
        if (!queryAlbum.empty())
            resource += "&album_name=" + lrclibEscape(curl.get(), queryAlbum);
        const auto record = getLrclib(curl.get(), resource, &request);
        auto lines = parseLrclibLyrics(record);
        if (!lines.empty()) {
            info.matchedTitle = toWide(record.value("trackName", std::string()));
            info.matchedArtist = toWide(record.value("artistName", std::string()));
            info.matchedDurationMs = static_cast<int64_t>(record.value("duration", 0.0) * 1000.0);
            return lines;
        }
    }
    return {};
}

void searchLrclib(CURL* curl, const std::wstring& title, const std::wstring& artist,
                  std::vector<SearchCandidate>& out) {
    const auto records = getLrclib(curl, "search" + lrclibQuery(curl, title, artist));
    if (!records.is_array())
        return;
    for (const auto& record : records) {
        if (!record.is_object() || !record.contains("id") || !record["id"].is_number_integer() ||
            !record.contains("trackName") || !record["trackName"].is_string() ||
            (record.contains("instrumental") && record["instrumental"] == true))
            continue;
        const bool wordSync = lrclibHasWordSync(record);
        const auto syncedLyrics = record.find("syncedLyrics");
        if (!wordSync && (syncedLyrics == record.end() || !syncedLyrics->is_string() ||
                          syncedLyrics->get_ref<const std::string&>().empty()))
            continue;
        SearchCandidate candidate;
        candidate.source = LyricSource::Lrclib;
        candidate.lrclibId = record["id"].get<int64_t>();
        candidate.lrclibWordSync = wordSync;
        candidate.name = toWide(record["trackName"].get<std::string>());
        if (record.contains("artistName") && record["artistName"].is_string())
            candidate.singer = toWide(record["artistName"].get<std::string>());
        if (record.contains("albumName") && record["albumName"].is_string())
            candidate.album = toWide(record["albumName"].get<std::string>());
        if (record.contains("duration") && record["duration"].is_number()) {
            const double duration = record["duration"].get<double>();
            if (duration > 0 && duration <= 3600)
                candidate.durationMs = static_cast<int64_t>(std::llround(duration * 1000));
        }
        out.push_back(std::move(candidate));
    }
}

} // namespace lyric_detail
