#pragma once

#include "lyric/lyric_http.h"
#include "lyric/lyric_matching.h"
#include "lyric/lyric_provider.h"

#include <curl/curl.h>

#include <cstdint>
#include <string>
#include <vector>

namespace lyric_detail {

struct NeteaseSong {
    std::string id;
    std::wstring name;
    std::wstring singer;
    int64_t durationMs = 0;
};

std::vector<LyricLine> parseYrc(std::string content);

bool searchNeteaseSongs(CURL* curl, const std::wstring& query, int maxN,
                        std::vector<NeteaseSong>& out, const LyricRequest* request = nullptr);

bool downloadNeteaseLyric(CURL* curl, const std::wstring& songId,
                          std::vector<LyricLine>& out, const LyricRequest* request = nullptr);

bool fetchNeteaseByMetadata(CURL* curl, const MatchQuery& query,
                            std::vector<LyricLine>& out, const LyricRequest& request,
                            SongInfo& info);

} // namespace lyric_detail
