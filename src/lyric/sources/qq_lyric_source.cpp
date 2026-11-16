#include "lyric/sources/qq_lyric_source.h"

#include "lyric/lrc_parser.h"
#include "lyric/lyric_http.h"
#include "lyric/qrc_decoder.h"
#include "util/base64.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <unordered_set>

namespace lyric_detail {
namespace {

void deduplicateQqCandidates(std::vector<QqCandidate>& candidates) {
    std::unordered_set<std::string> seen;
    std::vector<QqCandidate> unique;
    unique.reserve(candidates.size());
    for (auto& candidate : candidates) {
        const std::string id = !candidate.songmid.empty()
                                   ? "mid:" + candidate.songmid
                                   : "id:" + candidate.songid;
        if (candidate.songmid.empty() && candidate.songid.empty())
            continue;
        if (!seen.insert(id).second)
            continue;
        unique.push_back(std::move(candidate));
    }
    candidates = std::move(unique);
}

} // namespace

bool searchQqSongs(CURL* curl, const std::wstring& query, std::vector<QqCandidate>& out) {
    out.clear();
    const std::string keyword = toUtf8(query);
    char* esc = curl_easy_escape(curl, keyword.c_str(), static_cast<int>(keyword.size()));
    const std::string url = "https://c.y.qq.com/soso/fcgi-bin/client_search_cp?w=" +
                            std::string(esc ? esc : "") + "&n=10&p=1&format=json&new_json=1";
    if (esc)
        curl_free(esc);

    std::string body;
    if (!httpGet(curl, url, "https://y.qq.com/", body))
        return false;
    auto j = nlohmann::json::parse(body, nullptr, false);
    if (j.is_discarded())
        return false;
    auto& list = j["data"]["song"]["list"];
    if (!list.is_array())
        return false;
    for (auto& s : list) {
        QqCandidate c;
        c.songmid = s.value("mid", s.value("songmid", ""));
        if (s.contains("id"))
            c.songid = jsonString(s["id"]);
        if (c.songid.empty() && s.contains("songid"))
            c.songid = jsonString(s["songid"]);
        c.albummid = s.value("album", nlohmann::json::object())
                         .value("mid", s.value("albummid", ""));
        c.name = toWide(s.value("name", s.value("songname", "")));
        std::wstring singers;
        if (s["singer"].is_array()) {
            for (auto& sg : s["singer"]) {
                if (!singers.empty())
                    singers += L'/';
                singers += toWide(sg.value("name", ""));
            }
        }
        c.singer = singers;
        c.intervalMs = static_cast<int64_t>(s.value("interval", 0)) * 1000;
        if (!c.songmid.empty() || !c.songid.empty())
            out.push_back(std::move(c));
    }
    return true;
}

std::vector<std::pair<int, const QqCandidate*>> rankQqCandidates(
    const MatchQuery& q, std::vector<QqCandidate>& candidates) {
    deduplicateQqCandidates(candidates);
    std::vector<std::pair<int, const QqCandidate*>> ranked;
    for (const auto& candidate : candidates) {
        const int score =
            scoreCandidate(q, candidate.name, candidate.singer, candidate.intervalMs);
        if (score >= kScoreThreshold)
            ranked.push_back({score, &candidate});
    }
    std::stable_sort(ranked.begin(), ranked.end(),
                     [](const auto& left, const auto& right) { return left.first > right.first; });
    return ranked;
}

bool downloadQqQrc(CURL* curl, const std::string& songid, std::vector<LyricLine>& out) {
    if (songid.empty())
        return false;
    std::string body;
    const std::string url = "https://c.y.qq.com/qqmusic/fcgi-bin/lyric_download.fcg"
                            "?musicid=" + songid + "&version=15&miniversion=82&lrctype=4";
    if (!httpGet(curl, url, "https://c.y.qq.com/", body))
        return false;
    const std::string encrypted = xmlInnerText(body, "content");
    if (encrypted.empty())
        return false;
    return decodeQrcLyrics(encrypted, out);
}

bool downloadQqLrc(CURL* curl, const std::string& songmid, std::vector<LyricLine>& out) {
    const std::string url = "https://c.y.qq.com/lyric/fcgi-bin/fcg_query_lyric_new.fcg?songmid=" +
                            songmid + "&g_tk=5381&format=json&nobase64=0";
    std::string body;
    if (!httpGet(curl, url, "https://y.qq.com/portal/player.html", body))
        return false;

    auto j = nlohmann::json::parse(body, nullptr, false);
    if (j.is_discarded() || j.value("retcode", -1) != 0)
        return false;
    const std::string lrc = base64Decode(j.value("lyric", std::string()));
    if (lrc.empty())
        return false;
    out = parseLrcText(lrc);
    const std::string trans = base64Decode(j.value("trans", std::string()));
    const std::string roma = base64Decode(j.value("roma", std::string()));
    if (!trans.empty())
        attachSecondary(out, parseLrcText(trans), true);
    if (!roma.empty())
        attachSecondary(out, parseLrcText(roma), false);
    return !out.empty();
}

void attachQqSecondary(CURL* curl, const std::string& songmid, std::vector<LyricLine>& out) {
    if (songmid.empty() || out.empty())
        return;
    std::vector<LyricLine> qqLines;
    if (!downloadQqLrc(curl, songmid, qqLines))
        return;
    std::vector<LyricLine> trans;
    std::vector<LyricLine> roma;
    for (const auto& line : qqLines) {
        if (!line.translation.empty())
            trans.push_back({line.ms, line.translation});
        if (!line.romanization.empty())
            roma.push_back({line.ms, line.romanization});
    }
    attachSecondary(out, trans, true);
    attachSecondary(out, roma, false);
}

void fillQqSongInfo(CURL* curl, const std::wstring& title, const std::wstring& artist,
                    int64_t durationMs, SongInfo& info) {
    const MatchQuery q{title, artist, durationMs, extractVersionTags(title)};
    for (const auto& variant : buildSearchVariants(q)) {
        std::vector<QqCandidate> cands;
        if (!searchQqSongs(curl, makeSearchQuery(variant), cands))
            continue;
        const auto ranked = rankQqCandidates(q, cands);
        if (!ranked.empty()) {
            const QqCandidate* best = ranked.front().second;
            info.songmid = toWide(best->songmid);
            info.albummid = toWide(best->albummid);
            return;
        }
    }
}

} // namespace lyric_detail
