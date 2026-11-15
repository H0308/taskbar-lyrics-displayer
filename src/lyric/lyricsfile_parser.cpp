#include "lyric/lyricsfile_parser.h"

#include <yaml-cpp/yaml.h>

#include <windows.h>

#include <algorithm>
#include <unordered_set>
#include <utility>

namespace {

constexpr size_t kMaxDocumentBytes = 512 * 1024;
constexpr size_t kMaxLines = 4096;
constexpr size_t kMaxWordsPerLine = 1024;
constexpr size_t kMaxTextBytes = 4096;

bool isMapping(const YAML::Node& node) {
    if (!node || !node.IsMap())
        return false;
    std::unordered_set<std::string> keys;
    for (const auto& entry : node) {
        if (!entry.first.IsScalar() || !keys.insert(entry.first.Scalar()).second)
            return false;
    }
    return true;
}

std::wstring readText(const YAML::Node& node, size_t& remainingBytes) {
    if (!node || !node.IsScalar())
        return {};
    const auto& utf8 = node.Scalar();
    if (utf8.empty() || utf8.size() > kMaxTextBytes || utf8.size() > remainingBytes)
        return {};
    remainingBytes -= utf8.size();

    const int size = static_cast<int>(utf8.size());
    const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), size,
                                           nullptr, 0);
    if (length <= 0)
        return {};
    std::wstring text(length, L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), size, text.data(), length);
    return text;
}

std::vector<LyricChar> readWords(const YAML::Node& words, int64_t lineEndMs,
                                size_t& remainingBytes) {
    if (!words || !words.IsSequence() || words.size() > kMaxWordsPerLine)
        return {};

    std::vector<LyricChar> result;
    result.reserve(words.size());
    for (const auto& word : words) {
        if (!isMapping(word))
            return {};
        const auto start = word["start_ms"].as<int64_t>(-1);
        const auto endNode = word["end_ms"];
        const bool hasEnd = endNode && !endNode.IsNull();
        const auto end = hasEnd ? endNode.as<int64_t>(-1) : -1;
        auto text = readText(word["text"], remainingBytes);
        if (start < 0 || (hasEnd && end < start) || text.empty())
            return {};
        result.push_back({start, end, std::move(text)});
    }

    // 文本片段必须保持原顺序。缺少结束时间时只为显示推算时间，不修改原始歌词。
    for (size_t i = 0; i < result.size(); ++i) {
        auto& word = result[i];
        if (i > 0 && word.startMs < result[i - 1].startMs)
            return {};
        if (word.endMs < 0)
            word.endMs = std::max(word.startMs, i + 1 < result.size() ? result[i + 1].startMs
                                                                    : lineEndMs);
    }
    return result;
}

} // namespace

std::vector<LyricLine> parseLyricsfile(const std::string& document) {
    if (document.empty() || document.size() > kMaxDocumentBytes)
        return {};
    try {
        const auto documents = YAML::LoadAll(document);
        if (documents.size() != 1 || !isMapping(documents.front()))
            return {};
        const auto& root = documents.front();
        if (root["version"].as<std::string>("") != "1.0")
            return {};
        const auto lines = root["lines"];
        if (!lines || !lines.IsSequence() || lines.size() > kMaxLines)
            return {};

        std::vector<LyricLine> result;
        result.reserve(lines.size());
        size_t remainingBytes = kMaxDocumentBytes;
        for (const auto& node : lines) {
            if (!isMapping(node))
                continue;
            const auto start = node["start_ms"].as<int64_t>(-1);
            if (start < 0)
                continue;
            LyricLine line;
            line.ms = start;
            line.chars = readWords(node["words"], node["end_ms"].as<int64_t>(-1), remainingBytes);
            if (line.chars.empty()) {
                line.text = readText(node["text"], remainingBytes);
            } else {
                for (const auto& word : line.chars)
                    line.text += word.text;
            }
            if (!line.text.empty())
                result.push_back(std::move(line));
        }
        std::stable_sort(result.begin(), result.end(),
                         [](const LyricLine& a, const LyricLine& b) { return a.ms < b.ms; });
        return result;
    } catch (const YAML::Exception&) {
        return {};
    }
}
