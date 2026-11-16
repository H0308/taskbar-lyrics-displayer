#pragma once

#include "lyric/lyric_matching.h"
#include "lyric/lyric_provider.h"

#include <curl/curl.h>

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace lyric_detail {

struct QqCandidate {
    std::string songmid;
    std::string songid;
    std::string albummid;
    std::wstring name;
    std::wstring singer;
    int64_t intervalMs = 0;
};

bool searchQqSongs(CURL* curl, const std::wstring& query, std::vector<QqCandidate>& out);
std::vector<std::pair<int, const QqCandidate*>> rankQqCandidates(
    const MatchQuery& q, std::vector<QqCandidate>& candidates);

bool downloadQqQrc(CURL* curl, const std::string& songid, std::vector<LyricLine>& out);
bool downloadQqLrc(CURL* curl, const std::string& songmid, std::vector<LyricLine>& out);
void attachQqSecondary(CURL* curl, const std::string& songmid, std::vector<LyricLine>& out);
void fillQqSongInfo(CURL* curl, const std::wstring& title, const std::wstring& artist,
                    int64_t durationMs, SongInfo& info);

} // namespace lyric_detail
