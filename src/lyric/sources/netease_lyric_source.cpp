#include "lyric/sources/netease_lyric_source.h"

#include "logging/runtime_logger.h"
#include "lyric/lrc_parser.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdlib>
#include <sstream>
#include <unordered_set>
#include <utility>

namespace lyric_detail {
namespace {

bool parseIntegerField(const std::string& value, int64_t& out) {
    if (value.empty())
        return false;
    char* end = nullptr;
    const long long parsed = std::strtoll(value.c_str(), &end, 10);
    if (end != value.c_str() + value.size())
        return false;
    out = static_cast<int64_t>(parsed);
    return true;
}

std::string lyricField(const nlohmann::json& root, const char* name) {
    if (!root.is_object())
        return {};
    const auto it = root.find(name);
    if (it == root.end() || !it->is_object())
        return {};
    return it->value("lyric", std::string());
}

} // namespace

std::vector<LyricLine> parseYrc(std::string content) {
    if (!content.empty() && content.compare(0, 3, "\xEF\xBB\xBF") == 0)
        content.erase(0, 3); // 去 BOM

    std::vector<LyricLine> lines;
    std::istringstream ss(content);
    std::string raw;
    while (std::getline(ss, raw)) {
        if (!raw.empty() && raw.back() == '\r')
            raw.pop_back();
        const size_t begin = raw.find_first_not_of(" \t");
        if (begin == std::string::npos || raw[begin] != '[')
            continue; // YRC 可能包含 JSON 信息行

        const size_t close = raw.find(']', begin + 1);
        if (close == std::string::npos)
            continue;
        const std::string header = raw.substr(begin + 1, close - begin - 1);
        const size_t comma = header.find(',');
        if (comma == std::string::npos) {
            // 个别接口响应会在 yrc 字段中混入普通 LRC 行，保留它们作为非逐字行。
            int64_t lineStart = 0;
            if (!parseLrcTimestamp(header, lineStart))
                continue;
            std::wstring plain = toWide(raw.substr(close + 1));
            while (!plain.empty() && (plain.back() == L' ' || plain.back() == L'\t'))
                plain.pop_back();
            if (!plain.empty())
                lines.push_back({lineStart, std::move(plain)});
            continue;
        }
        int64_t lineStart = 0;
        int64_t lineDuration = 0;
        if (!parseIntegerField(header.substr(0, comma), lineStart) ||
            !parseIntegerField(header.substr(comma + 1), lineDuration) || lineStart < 0)
            continue;
        (void)lineDuration;

        std::vector<LyricChar> chars;
        std::wstring text;
        size_t pos = close + 1;
        while (pos < raw.size() && raw[pos] == '(') {
            const size_t tokenClose = raw.find(')', pos + 1);
            if (tokenClose == std::string::npos)
                break;
            const std::string token = raw.substr(pos + 1, tokenClose - pos - 1);
            const size_t comma1 = token.find(',');
            const size_t comma2 = comma1 == std::string::npos ? comma1 : token.find(',', comma1 + 1);
            if (comma2 == std::string::npos)
                break;

            int64_t charOffset = 0;
            int64_t charDuration = 0;
            if (!parseIntegerField(token.substr(0, comma1), charOffset) ||
                !parseIntegerField(token.substr(comma1 + 1, comma2 - comma1 - 1), charDuration))
                break;

            size_t nextToken = std::string::npos;
            for (size_t candidate = raw.find('(', tokenClose + 1);
                 candidate != std::string::npos;
                 candidate = raw.find('(', candidate + 1)) {
                const size_t candidateClose = raw.find(')', candidate + 1);
                if (candidateClose == std::string::npos)
                    break;
                const std::string candidateToken =
                    raw.substr(candidate + 1, candidateClose - candidate - 1);
                const size_t candidateComma1 = candidateToken.find(',');
                const size_t candidateComma2 = candidateComma1 == std::string::npos
                                                   ? candidateComma1
                                                   : candidateToken.find(',', candidateComma1 + 1);
                int64_t ignoredStart = 0;
                int64_t ignoredDuration = 0;
                if (candidateComma2 != std::string::npos &&
                    parseIntegerField(candidateToken.substr(0, candidateComma1),
                                      ignoredStart) &&
                    parseIntegerField(candidateToken.substr(candidateComma1 + 1,
                                                             candidateComma2 - candidateComma1 - 1),
                                      ignoredDuration)) {
                    nextToken = candidate;
                    break;
                }
            }
            const std::string tokenText = raw.substr(
                tokenClose + 1,
                nextToken == std::string::npos ? std::string::npos : nextToken - tokenClose - 1);
            const std::wstring tokenWide = toWide(tokenText);
            if (!tokenWide.empty()) {
                // YRC 的字时间戳是相对整首歌曲的绝对时间；例如行首和首字通常相同。
                const int64_t start = charOffset;
                const int64_t end = start + std::max<int64_t>(charDuration, 1);
                chars.push_back({start, end, tokenWide});
                text += tokenWide;
            }
            if (nextToken == std::string::npos)
                break;
            pos = nextToken;
        }

        while (!text.empty() && (text.back() == L' ' || text.back() == L'\t'))
            text.pop_back();
        if (text.empty() || chars.empty())
            continue;
        lines.push_back({lineStart, std::move(text), std::move(chars)});
    }

    std::sort(lines.begin(), lines.end(),
              [](const LyricLine& a, const LyricLine& b) { return a.ms < b.ms; });
    return lines;
}

bool searchNeteaseSongs(CURL* curl, const std::wstring& query, int maxN,
                        std::vector<NeteaseSong>& out, const LyricRequest* request) {
    out.clear();
    const std::string keyword = toUtf8(query);
    char* esc = curl_easy_escape(curl, keyword.c_str(), static_cast<int>(keyword.size()));
    const std::string url = "https://music.163.com/api/cloudsearch/pc?csrf_token=&s=" +
                            std::string(esc ? esc : "") + "&type=1&offset=0&limit=" +
                            std::to_string(maxN);
    if (esc)
        curl_free(esc);

    std::string body;
    if (!httpGet(curl, url, "https://music.163.com/", body, request))
        return false;
    auto root = nlohmann::json::parse(body, nullptr, false);
    if (root.is_discarded() || !root.is_object())
        return false;
    auto resultIt = root.find("result");
    if (resultIt == root.end() || !resultIt->is_object())
        return false;
    auto songsIt = resultIt->find("songs");
    if (songsIt == resultIt->end() || !songsIt->is_array())
        return false;

    for (const auto& item : *songsIt) {
        if (!item.is_object())
            continue;
        NeteaseSong song;
        auto idIt = item.find("id");
        if (idIt != item.end())
            song.id = jsonString(*idIt);
        song.name = toWide(item.value("name", std::string()));
        if (song.name.empty() || song.id.empty())
            continue;

        auto artistsIt = item.find("ar");
        if (artistsIt != item.end() && artistsIt->is_array()) {
            for (const auto& artist : *artistsIt) {
                if (!artist.is_object())
                    continue;
                const std::wstring name = toWide(artist.value("name", std::string()));
                if (!name.empty()) {
                    if (!song.singer.empty())
                        song.singer += L'/';
                    song.singer += name;
                }
            }
        }
        song.durationMs = static_cast<int64_t>(item.value("dt", 0LL));
        if (song.durationMs <= 0)
            song.durationMs = static_cast<int64_t>(item.value("duration", 0LL));
        out.push_back(std::move(song));
        if (static_cast<int>(out.size()) >= maxN)
            break;
    }
    return true;
}

bool downloadNeteaseLyric(CURL* curl, const std::wstring& songId,
                          std::vector<LyricLine>& out, const LyricRequest* request) {
    if (songId.empty())
        return false;

    std::string body;
    const std::string id = toUtf8(songId);
    const std::string url = "https://music.163.com/api/song/lyric?id=" + id +
                            "&lv=1&kv=1&tv=1&yv=1&ytv=1&rv=1&yrv=1";
    if (!httpGet(curl, url, "https://music.163.com/", body, request))
        return false;

    auto root = nlohmann::json::parse(body, nullptr, false);
    if (root.is_discarded() || !root.is_object())
        return false;

    std::vector<LyricLine> lines;
    const std::string yrc = lyricField(root, "yrc");
    if (!yrc.empty())
        lines = parseYrc(yrc);
    if (lines.empty()) {
        const std::string lrc = lyricField(root, "lrc");
        if (lrc.empty())
            return false;
        lines = parseLrcText(lrc);
    }
    if (lines.empty())
        return false;

    // YRC 主歌词也可以附着网易云返回的翻译/罗马音；新字段优先，
    // 旧字段为空时使用兼容字段。
    std::string translation = lyricField(root, "ytlrc");
    if (translation.empty())
        translation = lyricField(root, "tlyric");
    std::string romanization = lyricField(root, "yromalrc");
    if (romanization.empty())
        romanization = lyricField(root, "romalrc");
    if (!translation.empty()) {
        auto translatedLines = parseYrc(translation);
        if (translatedLines.empty())
            translatedLines = parseLrcText(translation);
        attachSecondary(lines, translatedLines, true);
    }
    if (!romanization.empty()) {
        auto romanizedLines = parseYrc(romanization);
        if (romanizedLines.empty())
            romanizedLines = parseLrcText(romanization);
        attachSecondary(lines, romanizedLines, false);
    }

    out = std::move(lines);
    return true;
}

bool fetchNeteaseByMetadata(CURL* curl, const MatchQuery& query,
                            std::vector<LyricLine>& out, const LyricRequest& request,
                            SongInfo& info) {
    std::unordered_set<std::wstring> searched;
    std::unordered_set<std::string> attempted;
    for (const auto& variant : buildSearchVariants(query)) {
        if (request.cancelled())
            return false;
        const auto keyword = makeSearchQuery(variant);
        if (!searched.insert(keyword).second)
            continue;
        std::vector<NeteaseSong> songs;
        if (!searchNeteaseSongs(curl, keyword, 5, songs, &request))
            return false;
        std::vector<std::pair<int, const NeteaseSong*>> ranked;
        for (const auto& song : songs) {
            const int score = scoreCandidate(query, song.name, song.singer, song.durationMs);
            if (score >= kScoreThreshold && !attempted.contains(song.id))
                ranked.emplace_back(score, &song);
        }
        std::stable_sort(ranked.begin(), ranked.end(),
                         [](const auto& left, const auto& right) { return left.first > right.first; });
        for (const auto& [score, song] : ranked) {
            (void)score;
            if (request.cancelled())
                return false;
            if (!attempted.insert(song->id).second)
                continue;
            if (downloadNeteaseLyric(curl, toWide(song->id), out, &request)) {
                info.matchedTitle = song->name;
                info.matchedArtist = song->singer;
                info.matchedDurationMs = song->durationMs;
                runtime_log::writef(L"[lyric][source=netease] matched songid=%S lines=%zu",
                                    song->id.c_str(), out.size());
                return true;
            }
        }
    }
    return false;
}

} // namespace lyric_detail
