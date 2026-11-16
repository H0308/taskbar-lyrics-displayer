#pragma once

#include "lyric/lyric_http.h"
#include "lyric/lyric_provider.h"

#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace lyric_detail {

nlohmann::json getLrclib(CURL* curl, const std::string& resource,
                       const LyricRequest* request = nullptr);

std::vector<LyricLine> parseLrclibLyrics(const nlohmann::json& record);

std::vector<LyricLine> fetchLrclib(const std::wstring& title, const std::wstring& artist,
                                   const std::wstring& album, int64_t durationMs,
                                   const LyricRequest& request, SongInfo& info);

void searchLrclib(CURL* curl, const std::wstring& title, const std::wstring& artist,
                  std::vector<SearchCandidate>& out);

} // namespace lyric_detail
