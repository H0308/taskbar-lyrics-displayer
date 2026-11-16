#include "lyric/sources/kugou_lyric_source.h"

#include "lyric/lyric_http.h"
#include "util/base64.h"

#include <nlohmann/json.hpp>
#include <zlib.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <sstream>
#include <unordered_set>

namespace lyric_detail {
namespace {

// KRC 解密：酷狗客户端硬编码的 16 字节 XOR 密钥
constexpr uint8_t kKrcXorKey[16] = {0x40, 0x47, 0x61, 0x77, 0x5E, 0x32, 0x74, 0x47,
                                    0x51, 0x36, 0x31, 0x2D, 0xCE, 0xD2, 0x6E, 0x69};

void replaceAll(std::string& s, const std::string& from, const std::string& to) {
    size_t pos = 0;
    while ((pos = s.find(from, pos)) != std::string::npos) {
        s.replace(pos, from.size(), to);
        pos += to.size();
    }
}

// 行内无 <...> token 时按字符数均分行时长生成逐字
void buildFallbackChars(const std::wstring& text, int64_t lineStart, int64_t lineDuration,
                        std::vector<LyricChar>& out) {
    if (text.empty())
        return;
    const int64_t total = std::max<int64_t>(lineDuration, static_cast<int64_t>(text.size()) * 120);
    for (size_t i = 0; i < text.size(); ++i) {
        const int64_t start = lineStart + static_cast<int64_t>(i * total / text.size());
        const int64_t end = lineStart + static_cast<int64_t>((i + 1) * total / text.size());
        out.push_back({start, std::max(end, start + 1), text.substr(i, 1)});
    }
}

void deduplicateKugouCandidates(std::vector<KugouSong>& candidates) {
    std::unordered_set<std::string> seen;
    std::vector<KugouSong> unique;
    unique.reserve(candidates.size());
    for (auto& candidate : candidates) {
        if (candidate.hash.empty() || !seen.insert(candidate.hash).second)
            continue;
        unique.push_back(std::move(candidate));
    }
    candidates = std::move(unique);
}

bool kugouSearchLyric(CURL* curl, const std::string& hash, int64_t durationMs,
                      std::vector<std::pair<std::string, std::string>>& out) {
    out.clear();
    const std::string url = "http://lyrics.kugou.com/search?ver=1&man=yes&client=pc&hash=" + hash +
                            "&duration=" + std::to_string(durationMs);
    std::string body;
    if (!httpGet(curl, url, nullptr, body))
        return false;
    auto j = nlohmann::json::parse(body, nullptr, false);
    if (j.is_discarded())
        return false;
    auto& cands = j["candidates"];
    if (!cands.is_array())
        return false;
    for (auto& c : cands) {
        std::string id = c.value("id", std::string());
        std::string accesskey = c.value("accesskey", std::string());
        if (!id.empty() && !accesskey.empty())
            out.emplace_back(std::move(id), std::move(accesskey));
        if (out.size() >= 3)
            break;
    }
    return !out.empty();
}

bool kugouDownloadKrc(CURL* curl, const std::string& id, const std::string& accesskey,
                      std::vector<LyricLine>& out) {
    const std::string url = "http://lyrics.kugou.com/download?ver=1&client=pc&id=" + id +
                            "&accesskey=" + accesskey + "&fmt=krc&charset=utf8";
    std::string body;
    if (!httpGet(curl, url, nullptr, body))
        return false;
    auto j = nlohmann::json::parse(body, nullptr, false);
    if (j.is_discarded())
        return false;
    std::string krcText;
    if (!krcDecode(j.value("content", std::string()), krcText))
        return false;
    return parseKrc(krcText, out);
}

} // namespace

bool krcDecode(const std::string& b64, std::string& out) {
    const std::string raw = base64Decode(b64);
    if (raw.size() <= 4 || raw.compare(0, 4, "krc1") != 0)
        return false;
    std::string xored = raw.substr(4);
    for (size_t i = 0; i < xored.size(); ++i)
        xored[i] = static_cast<char>(static_cast<uint8_t>(xored[i]) ^ kKrcXorKey[i % 16]);

    z_stream zs{};
    if (inflateInit(&zs) != Z_OK)
        return false;
    zs.next_in = reinterpret_cast<Bytef*>(xored.data());
    zs.avail_in = static_cast<uInt>(xored.size());
    out.clear();
    char buf[16384];
    int ret = Z_OK;
    while (ret == Z_OK) {
        zs.next_out = reinterpret_cast<Bytef*>(buf);
        zs.avail_out = sizeof(buf);
        ret = inflate(&zs, Z_NO_FLUSH);
        if (ret != Z_OK && ret != Z_STREAM_END) {
            inflateEnd(&zs);
            return false;
        }
        out.append(buf, sizeof(buf) - zs.avail_out);
    }
    inflateEnd(&zs);
    return ret == Z_STREAM_END && !out.empty();
}

bool parseKrc(std::string content, std::vector<LyricLine>& out) {
    if (!content.empty() && content.compare(0, 3, "\xEF\xBB\xBF") == 0)
        content.erase(0, 3); // 去 BOM
    replaceAll(content, "&apos;", "'");
    replaceAll(content, "&quot;", "\"");
    replaceAll(content, "&amp;", "&");

    std::vector<LyricLine> lines;
    std::string languagePayload;
    std::istringstream ss(content);
    std::string raw;
    while (std::getline(ss, raw)) {
        if (!raw.empty() && raw.back() == '\r')
            raw.pop_back();
        if (raw.compare(0, 10, "[language:") == 0 && raw.size() > 11 && raw.back() == ']') {
            languagePayload = raw.substr(10, raw.size() - 11);
            continue;
        }
        // 行头必须是 [数字,数字]，否则为元数据行（ti:/ar:/language: 等）
        if (raw.size() < 3 || raw[0] != '[' || !std::isdigit(static_cast<unsigned char>(raw[1])))
            continue;
        const size_t close = raw.find(']');
        if (close == std::string::npos)
            continue;
        const std::string header = raw.substr(1, close - 1);
        const size_t comma = header.find(',');
        if (comma == std::string::npos)
            continue;
        char* end = nullptr;
        const long long lineStart = std::strtoll(header.c_str(), &end, 10);
        if (end != header.c_str() + comma)
            continue;
        const long long lineDuration = std::strtoll(header.c_str() + comma + 1, nullptr, 10);

        // 逐字 token：<偏移,时长,0>文本
        std::vector<LyricChar> chars;
        std::wstring text;
        size_t pos = close + 1;
        while (pos < raw.size()) {
            if (raw[pos] != '<')
                break;
            const size_t gt = raw.find('>', pos);
            if (gt == std::string::npos)
                break;
            const std::string tag = raw.substr(pos + 1, gt - pos - 1);
            const size_t c1 = tag.find(',');
            const size_t c2 = c1 == std::string::npos ? c1 : tag.find(',', c1 + 1);
            if (c2 == std::string::npos)
                break;
            const long long charOff = std::strtoll(tag.c_str(), nullptr, 10);
            const long long charDur = std::strtoll(tag.c_str() + c1 + 1, nullptr, 10);
            const size_t next = raw.find('<', gt + 1);
            const std::wstring token = toWide(raw.substr(gt + 1, next - gt - 1));
            if (!token.empty()) {
                const int64_t start = lineStart + charOff;
                chars.push_back({start, start + std::max<long long>(charDur, 1), token});
                text += token;
            }
            pos = next;
        }
        if (chars.empty()) {
            // 无逐字 token：去掉残留标签后按均分兜底
            const std::string plain = raw.substr(close + 1);
            std::string cleaned;
            for (size_t i = 0; i < plain.size();) {
                if (plain[i] == '<') {
                    const size_t gt = plain.find('>', i);
                    if (gt == std::string::npos)
                        break;
                    i = gt + 1;
                } else {
                    cleaned += plain[i++];
                }
            }
            const std::wstring t = trimW(toWide(cleaned));
            if (t.empty())
                continue;
            buildFallbackChars(t, lineStart, lineDuration, chars);
            text = t;
        }
        while (!text.empty() && (text.back() == L' ' || text.back() == L'\t'))
            text.pop_back();
        if (text.empty())
            continue;
        lines.push_back({static_cast<int64_t>(lineStart), text, std::move(chars)});
    }
    if (lines.empty())
        return false;
    std::sort(lines.begin(), lines.end(),
              [](const LyricLine& a, const LyricLine& b) { return a.ms < b.ms; });
    // KRC 的 language 元数据是 Base64 JSON；type=0 为音译，type=1 为翻译，
    // lyricContent 按主歌词行号对应，不另带时间戳。
    if (!languagePayload.empty()) {
        auto lang = nlohmann::json::parse(base64Decode(languagePayload), nullptr, false);
        if (!lang.is_discarded() && lang["content"].is_array()) {
            for (const auto& block : lang["content"]) {
                const int type = block.value("type", -1);
                if (type != 0 && type != 1)
                    continue;
                const auto& contentLines = block["lyricContent"];
                if (!contentLines.is_array())
                    continue;
                const size_t count = std::min(lines.size(), contentLines.size());
                for (size_t i = 0; i < count; ++i) {
                    if (!contentLines[i].is_array())
                        continue;
                    std::wstring text;
                    for (const auto& token : contentLines[i]) {
                        if (!token.is_string())
                            continue;
                        std::wstring part = toWide(token.get<std::string>());
                        if (type == 0 && !text.empty() && !part.empty() && text.back() != L' ' &&
                            part.front() != L' ')
                            text += L' ';
                        text += part;
                    }
                    text = trimW(text);
                    if (type == 0)
                        lines[i].romanization = std::move(text);
                    else
                        lines[i].translation = std::move(text);
                }
            }
        }
    }
    out = std::move(lines);
    return true;
}

bool kugouSearchSongs(CURL* curl, const std::wstring& title, const std::wstring& artist, int maxN,
                      std::vector<KugouSong>& out) {
    out.clear();
    std::wstring query = title;
    if (!artist.empty() && !titleEndsWithArtist(title, artist))
        query += L' ' + artist;
    const std::string keyword = toUtf8(query);
    char* esc = curl_easy_escape(curl, keyword.c_str(), static_cast<int>(keyword.size()));
    const std::string url = "http://mobilecdn.kugou.com/api/v3/search/song?format=json&keyword=" +
                            std::string(esc ? esc : "") + "&page=1&pagesize=" +
                            std::to_string(std::max(maxN, 1)) + "&showtype=1";
    if (esc)
        curl_free(esc);
    std::string body;
    if (!httpGet(curl, url, nullptr, body))
        return false;
    auto j = nlohmann::json::parse(body, nullptr, false);
    if (j.is_discarded())
        return false;
    auto& info = j["data"]["info"];
    if (!info.is_array())
        return false;
    for (auto& s : info) {
        KugouSong song;
        song.hash = s.value("hash", std::string());
        if (song.hash.empty())
            continue;
        song.name = toWide(s.value("songname", std::string()));
        song.singer = toWide(s.value("singername", std::string()));
        song.durationMs = static_cast<int64_t>(s.value("duration", 0)) * 1000;
        out.push_back(std::move(song));
        if (static_cast<int>(out.size()) >= maxN)
            break;
    }
    return true;
}

std::vector<std::pair<int, const KugouSong*>> rankKugouCandidates(
    const MatchQuery& q, std::vector<KugouSong>& candidates) {
    deduplicateKugouCandidates(candidates);
    std::vector<std::pair<int, const KugouSong*>> ranked;
    for (const auto& candidate : candidates) {
        const int score = scoreCandidate(q, candidate.name, candidate.singer, candidate.durationMs);
        if (score >= kScoreThreshold)
            ranked.push_back({score, &candidate});
    }
    std::stable_sort(ranked.begin(), ranked.end(),
                     [](const auto& left, const auto& right) { return left.first > right.first; });
    return ranked;
}

bool fetchKrcByHash(CURL* curl, const std::string& hash, int64_t durationMs,
                    std::vector<LyricLine>& out) {
    std::vector<std::pair<std::string, std::string>> cands;
    if (!kugouSearchLyric(curl, hash, durationMs, cands))
        return false;
    for (const auto& [id, accesskey] : cands) {
        out.clear();
        if (kugouDownloadKrc(curl, id, accesskey, out) && !out.empty())
            return true;
    }
    return false;
}

} // namespace lyric_detail
