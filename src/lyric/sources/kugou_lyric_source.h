#pragma once

#include "lyric/lyric_matching.h"
#include "lyric/lyric_provider.h"

#include <curl/curl.h>

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace lyric_detail {

struct KugouSong {
    std::string hash;
    std::wstring name;
    std::wstring singer;
    int64_t durationMs = 0;
};

bool krcDecode(const std::string& b64, std::string& out);
bool parseKrc(std::string content, std::vector<LyricLine>& out);

bool kugouSearchSongs(CURL* curl, const std::wstring& title, const std::wstring& artist, int maxN,
                      std::vector<KugouSong>& out);
std::vector<std::pair<int, const KugouSong*>> rankKugouCandidates(
    const MatchQuery& q, std::vector<KugouSong>& candidates);
bool fetchKrcByHash(CURL* curl, const std::string& hash, int64_t durationMs,
                    std::vector<LyricLine>& out);

} // namespace lyric_detail
