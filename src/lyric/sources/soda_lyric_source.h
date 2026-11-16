#pragma once

#include "lyric/lyric_http.h"
#include "lyric/lyric_provider.h"

#include <curl/curl.h>

#include <cstdint>
#include <string>
#include <vector>

namespace lyric_detail {

struct SodaSong {
    std::string id;
    std::wstring title;
    std::wstring artist;
    int64_t durationMs = 0;
    int64_t previewStartMs = 0;
    int64_t previewDurationMs = 0;
};

bool searchSodaSongs(CURL* curl, const std::wstring& title, const std::wstring& artist,
                     std::vector<SodaSong>& out, const LyricRequest* request = nullptr);

bool downloadSodaLyrics(CURL* curl, const SodaSong& song, int64_t playbackDurationMs,
                        std::vector<LyricLine>& out, const LyricRequest* request = nullptr);

bool fetchSodaLyrics(const std::wstring& title, const std::wstring& artist,
                     int64_t durationMs, std::vector<LyricLine>& out, const LyricRequest& request,
                     SongInfo& info);

} // namespace lyric_detail
