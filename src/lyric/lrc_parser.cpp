#include "lyric/lrc_parser.h"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <sstream>
#include <utility>

namespace {

constexpr size_t kMaxLocalLyricBytes = 2 * 1024 * 1024;
constexpr int64_t kMaxOffsetMs = 600000;

std::wstring toWide(const std::string& text) {
    if (text.empty())
        return {};
    const int length = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                                           nullptr, 0);
    if (length <= 0)
        return {};
    std::wstring result(static_cast<size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(),
                        length);
    return result;
}

std::string wideToUtf8(const std::wstring& text) {
    if (text.empty())
        return {};
    const int length = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                                           nullptr, 0, nullptr, nullptr);
    if (length <= 0)
        return {};
    std::string result(static_cast<size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(),
                        length, nullptr, nullptr);
    return result;
}

bool decodeCodePageToWide(UINT codePage, DWORD flags, const std::string& raw, std::wstring& out) {
    out.clear();
    if (raw.empty())
        return true;
    const int length =
        MultiByteToWideChar(codePage, flags, raw.data(), static_cast<int>(raw.size()), nullptr, 0);
    if (length <= 0)
        return false;
    out.resize(static_cast<size_t>(length));
    return MultiByteToWideChar(codePage, flags, raw.data(), static_cast<int>(raw.size()),
                               out.data(), length) == length;
}

std::string trimAscii(const std::string& s) {
    const size_t first = s.find_first_not_of(" \t\r\n");
    if (first == std::string::npos)
        return {};
    const size_t last = s.find_last_not_of(" \t\r\n");
    return s.substr(first, last - first + 1);
}

std::wstring trimWide(std::wstring text) {
    while (!text.empty() && (text.back() == L' ' || text.back() == L'\t'))
        text.pop_back();
    const size_t first = text.find_first_not_of(L" \t");
    if (first == std::wstring::npos)
        return {};
    return text.substr(first);
}

std::string lowerAscii(std::string s) {
    for (char& ch : s)
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return s;
}

bool allDigits(const std::string& s) {
    if (s.empty())
        return false;
    for (char ch : s) {
        if (!std::isdigit(static_cast<unsigned char>(ch)))
            return false;
    }
    return true;
}

bool parseTimeStamp(const std::string& text, int64_t& milliseconds) {
    const size_t colon = text.find(':');
    if (colon == std::string::npos || colon == 0)
        return false;
    for (size_t i = 0; i < colon; ++i) {
        if (!std::isdigit(static_cast<unsigned char>(text[i])))
            return false;
    }

    char* end = nullptr;
    const double seconds = std::strtod(text.c_str() + colon + 1, &end);
    if (end == text.c_str() + colon + 1)
        return false;
    const long long minutes = std::strtoll(text.c_str(), nullptr, 10);
    milliseconds = static_cast<int64_t>((minutes * 60.0 + seconds) * 1000.0 + 0.5);
    return true;
}

// 兼容标准 [mm:ss.xx]、[mm:ss.xxx]、[mm:ss] 与冒号分隔毫秒 [mm:ss:xx]。
bool parseFlexibleTimestamp(const std::string& text, int64_t& milliseconds) {
    const size_t colon = text.find(':');
    if (colon == std::string::npos || colon == 0 || colon + 1 >= text.size())
        return false;
    const std::string minPart = text.substr(0, colon);
    if (!allDigits(minPart))
        return false;

    const std::string rest = text.substr(colon + 1);
    const size_t fracSep = rest.find_first_of(".:");
    std::string secPart;
    std::string fracPart;
    if (fracSep == std::string::npos) {
        secPart = rest;
    } else {
        secPart = rest.substr(0, fracSep);
        fracPart = rest.substr(fracSep + 1);
        if (!allDigits(fracPart))
            return false;
    }
    if (!allDigits(secPart))
        return false;

    const long long minutes = std::strtoll(minPart.c_str(), nullptr, 10);
    const long long seconds = std::strtoll(secPart.c_str(), nullptr, 10);
    int64_t fracMs = 0;
    if (!fracPart.empty()) {
        std::string padded = fracPart.substr(0, 3);
        while (padded.size() < 3)
            padded.push_back('0');
        fracMs = std::strtoll(padded.c_str(), nullptr, 10);
    }
    milliseconds = minutes * 60000 + seconds * 1000 + fracMs;
    return true;
}

bool parseMetadataTag(const std::string& tagContent, ParsedLrcDocument& doc) {
    const size_t colon = tagContent.find(':');
    if (colon == std::string::npos || colon == 0)
        return false;
    const std::string key = lowerAscii(trimAscii(tagContent.substr(0, colon)));
    const std::string value = trimAscii(tagContent.substr(colon + 1));
    if (value.empty())
        return false;
    if (key == "ti" && doc.title.empty()) {
        doc.title = toWide(value);
        return true;
    }
    if (key == "ar" && doc.artist.empty()) {
        doc.artist = toWide(value);
        return true;
    }
    if (key == "al" && doc.album.empty()) {
        doc.album = toWide(value);
        return true;
    }
    if (key == "offset" && !doc.hasOffset) {
        char* end = nullptr;
        const long long rawOffset = std::strtoll(value.c_str(), &end, 10);
        if (end && *end == '\0') {
            const int64_t clamped = std::clamp<int64_t>(rawOffset, -kMaxOffsetMs, kMaxOffsetMs);
            // LRC 规范中正 offset 表示歌词提前，对应时间戳减去 offset（软件约定负值为提前）。
            doc.appliedOffsetMs = -clamped;
            doc.hasOffset = true;
            return true;
        }
    }
    return false;
}

struct EnhancedSegment {
    int64_t startMs = 0;
    std::wstring text;
};

// 解析增强型 LRC 行内词级标签 <mm:ss.xx>；未包含有效词级标签时返回 false。
bool parseEnhancedWordSegments(const std::string& body, int64_t baseLineMs,
                               std::vector<EnhancedSegment>& segmentsOut,
                               int64_t& explicitEndMsOut) {
    segmentsOut.clear();
    explicitEndMsOut = -1;
    bool foundTag = false;
    int64_t currentStartMs = baseLineMs;
    size_t pos = 0;
    size_t textStart = 0;

    auto flushText = [&](size_t textEnd, bool allowEmpty) {
        if (textEnd <= textStart && !allowEmpty)
            return;
        const std::wstring piece = toWide(body.substr(textStart, textEnd - textStart));
        if (!piece.empty())
            segmentsOut.push_back({currentStartMs, piece});
    };

    while (pos < body.size()) {
        const size_t open = body.find('<', pos);
        if (open == std::string::npos)
            break;
        const size_t close = body.find('>', open + 1);
        if (close == std::string::npos)
            break;
        int64_t wordMs = 0;
        if (!parseFlexibleTimestamp(body.substr(open + 1, close - open - 1), wordMs)) {
            pos = open + 1;
            continue;
        }
        flushText(open, false);
        foundTag = true;
        currentStartMs = wordMs;
        textStart = close + 1;
        pos = close + 1;
    }
    if (!foundTag)
        return false;

    if (textStart < body.size()) {
        flushText(body.size(), false);
    } else if (!segmentsOut.empty()) {
        // 行尾紧跟一个 <mm:ss.xx> 且无后续文本，作为最后一个词的显式结束时间。
        explicitEndMsOut = currentStartMs;
    }

    // 去除首尾纯空白，保留词间空格以维持英文词距。
    while (!segmentsOut.empty()) {
        auto& first = segmentsOut.front().text;
        const size_t nonSpace = first.find_first_not_of(L" \t");
        if (nonSpace == std::wstring::npos) {
            segmentsOut.erase(segmentsOut.begin());
        } else {
            first.erase(0, nonSpace);
            break;
        }
    }
    while (!segmentsOut.empty()) {
        auto& last = segmentsOut.back().text;
        while (!last.empty() && (last.back() == L' ' || last.back() == L'\t'))
            last.pop_back();
        if (last.empty())
            segmentsOut.pop_back();
        else
            break;
    }
    return !segmentsOut.empty();
}

} // namespace

bool parseLrcTimestamp(const std::string& text, int64_t& milliseconds) {
    return parseTimeStamp(text, milliseconds);
}

std::string decodeLocalLyricBytesToUtf8(const std::string& rawBytes) {
    if (rawBytes.empty() || rawBytes.size() > kMaxLocalLyricBytes)
        return {};

    // UTF-8 BOM
    if (rawBytes.size() >= 3 && static_cast<unsigned char>(rawBytes[0]) == 0xEF &&
        static_cast<unsigned char>(rawBytes[1]) == 0xBB &&
        static_cast<unsigned char>(rawBytes[2]) == 0xBF) {
        const std::string body = rawBytes.substr(3);
        std::wstring check;
        if (decodeCodePageToWide(CP_UTF8, MB_ERR_INVALID_CHARS, body, check))
            return body;
        return wideToUtf8(toWide(body));
    }

    // UTF-16 LE BOM
    if (rawBytes.size() >= 2 && static_cast<unsigned char>(rawBytes[0]) == 0xFF &&
        static_cast<unsigned char>(rawBytes[1]) == 0xFE) {
        const size_t payloadBytes = rawBytes.size() - 2;
        const size_t wcharCount = payloadBytes / 2;
        std::wstring wide(wcharCount, L'\0');
        for (size_t i = 0; i < wcharCount; ++i) {
            const uint16_t lo = static_cast<unsigned char>(rawBytes[2 + i * 2]);
            const uint16_t hi = static_cast<unsigned char>(rawBytes[2 + i * 2 + 1]);
            wide[i] = static_cast<wchar_t>(lo | (hi << 8));
        }
        return wideToUtf8(wide);
    }

    // UTF-16 BE BOM
    if (rawBytes.size() >= 2 && static_cast<unsigned char>(rawBytes[0]) == 0xFE &&
        static_cast<unsigned char>(rawBytes[1]) == 0xFF) {
        const size_t payloadBytes = rawBytes.size() - 2;
        const size_t wcharCount = payloadBytes / 2;
        std::wstring wide(wcharCount, L'\0');
        for (size_t i = 0; i < wcharCount; ++i) {
            const uint16_t hi = static_cast<unsigned char>(rawBytes[2 + i * 2]);
            const uint16_t lo = static_cast<unsigned char>(rawBytes[2 + i * 2 + 1]);
            wide[i] = static_cast<wchar_t>(lo | (hi << 8));
        }
        return wideToUtf8(wide);
    }

    // 无 BOM：先严格检验 UTF-8，失败则依次回退 GB18030 与系统默认 ANSI。
    std::wstring wide;
    if (decodeCodePageToWide(CP_UTF8, MB_ERR_INVALID_CHARS, rawBytes, wide))
        return rawBytes;
    if (decodeCodePageToWide(54936, MB_ERR_INVALID_CHARS, rawBytes, wide))
        return wideToUtf8(wide);
    if (decodeCodePageToWide(CP_ACP, 0, rawBytes, wide))
        return wideToUtf8(wide);
    return {};
}

std::vector<LyricLine> parseLrcText(const std::string& content) {
    std::vector<LyricLine> lines;
    std::istringstream input(content);
    std::string raw;
    while (std::getline(input, raw)) {
        if (!raw.empty() && raw.back() == '\r')
            raw.pop_back();

        std::vector<int64_t> timestamps;
        size_t position = 0;
        while (position < raw.size() && raw[position] == '[') {
            const size_t close = raw.find(']', position);
            if (close == std::string::npos)
                break;
            int64_t milliseconds = 0;
            if (!parseTimeStamp(raw.substr(position + 1, close - position - 1), milliseconds)) {
                timestamps.clear();
                break; // [ti:] 等元数据行
            }
            timestamps.push_back(milliseconds);
            position = close + 1;
        }
        if (timestamps.empty())
            continue;

        std::wstring text = trimWide(toWide(raw.substr(position)));
        if (text.empty())
            continue;
        for (const int64_t milliseconds : timestamps)
            lines.push_back({milliseconds, text});
    }

    std::sort(lines.begin(), lines.end(),
              [](const LyricLine& left, const LyricLine& right) { return left.ms < right.ms; });
    return lines;
}

ParsedLrcDocument parseLocalLrcUtf8(const std::string& utf8Content) {
    ParsedLrcDocument doc;
    if (utf8Content.empty() || utf8Content.size() > kMaxLocalLyricBytes)
        return doc;

    std::string content = utf8Content;
    if (content.size() >= 3 && static_cast<unsigned char>(content[0]) == 0xEF &&
        static_cast<unsigned char>(content[1]) == 0xBB &&
        static_cast<unsigned char>(content[2]) == 0xBF) {
        content.erase(0, 3);
    }

    struct RawParsedLine {
        LyricLine line;
        bool needsTrailingCharEnd = false;
    };
    std::vector<RawParsedLine> rawLines;

    std::istringstream input(content);
    std::string raw;
    while (std::getline(input, raw)) {
        if (!raw.empty() && raw.back() == '\r')
            raw.pop_back();

        size_t position = 0;
        while (position < raw.size() && (raw[position] == ' ' || raw[position] == '\t'))
            ++position;

        std::vector<int64_t> timestamps;
        while (position < raw.size() && raw[position] == '[') {
            const size_t close = raw.find(']', position);
            if (close == std::string::npos)
                break;
            const std::string tag = raw.substr(position + 1, close - position - 1);
            int64_t milliseconds = 0;
            if (!parseFlexibleTimestamp(tag, milliseconds)) {
                if (timestamps.empty())
                    parseMetadataTag(tag, doc);
                // 已有合法行时间戳时，后续非时间方括号属于歌词正文（如 "[合唱]"），保留已解析时间戳。
                break;
            }
            timestamps.push_back(milliseconds);
            position = close + 1;
        }
        if (timestamps.empty())
            continue;

        const std::string body = raw.substr(position);
        std::vector<EnhancedSegment> enhanced;
        int64_t explicitEndMs = -1;
        const bool isEnhanced =
            parseEnhancedWordSegments(body, timestamps.front(), enhanced, explicitEndMs);

        if (isEnhanced) {
            const int64_t anchorMs = timestamps.front();
            for (const int64_t lineMs : timestamps) {
                const int64_t shift = lineMs - anchorMs;
                LyricLine entry;
                entry.ms = lineMs;
                entry.chars.reserve(enhanced.size());
                for (size_t i = 0; i < enhanced.size(); ++i) {
                    const int64_t startMs = std::max<int64_t>(0, enhanced[i].startMs + shift);
                    int64_t endMs = -1;
                    if (i + 1 < enhanced.size()) {
                        endMs = std::max<int64_t>(startMs + 1, enhanced[i + 1].startMs + shift);
                    } else if (explicitEndMs >= 0) {
                        endMs = std::max<int64_t>(startMs + 1, explicitEndMs + shift);
                    }
                    entry.text += enhanced[i].text;
                    entry.chars.push_back({startMs, endMs, enhanced[i].text});
                }
                if (!entry.text.empty())
                    rawLines.push_back({std::move(entry), explicitEndMs < 0});
            }
        } else {
            const std::wstring text = trimWide(toWide(body));
            if (text.empty())
                continue;
            for (const int64_t lineMs : timestamps)
                rawLines.push_back({LyricLine{lineMs, text}, false});
        }
    }

    if (doc.appliedOffsetMs != 0) {
        for (auto& item : rawLines) {
            item.line.ms = std::max<int64_t>(0, item.line.ms + doc.appliedOffsetMs);
            for (auto& ch : item.line.chars) {
                ch.startMs = std::max<int64_t>(0, ch.startMs + doc.appliedOffsetMs);
                if (ch.endMs >= 0)
                    ch.endMs = std::max<int64_t>(ch.startMs + 1, ch.endMs + doc.appliedOffsetMs);
            }
        }
    }

    // 必须使用 stable_sort 保持同时间戳双语行在原文件中的先后顺序。
    std::stable_sort(rawLines.begin(), rawLines.end(),
                     [](const RawParsedLine& a, const RawParsedLine& b) {
                         return a.line.ms < b.line.ms;
                     });

    // 合并相同时间戳的相邻行：首行作原文，次行作翻译，第三行作罗马音。
    std::vector<bool> mergedNeedsEnd;
    doc.lines.reserve(rawLines.size());
    mergedNeedsEnd.reserve(rawLines.size());
    for (auto& item : rawLines) {
        if (!doc.lines.empty() && doc.lines.back().ms == item.line.ms) {
            auto& target = doc.lines.back();
            if (item.line.text == target.text)
                continue;
            if (target.translation.empty()) {
                target.translation = std::move(item.line.text);
            } else if (target.romanization.empty() && item.line.text != target.translation) {
                target.romanization = std::move(item.line.text);
            }
            continue;
        }
        doc.lines.push_back(std::move(item.line));
        mergedNeedsEnd.push_back(item.needsTrailingCharEnd);
    }

    // 为缺少行尾结束时间戳的增强 LRC 补全末词结束时间。
    for (size_t i = 0; i < doc.lines.size(); ++i) {
        if (!mergedNeedsEnd[i] || doc.lines[i].chars.empty())
            continue;
        auto& lastChar = doc.lines[i].chars.back();
        if (lastChar.endMs < 0) {
            const int64_t nextMs =
                i + 1 < doc.lines.size() ? doc.lines[i + 1].ms : lastChar.startMs + 500;
            lastChar.endMs = std::clamp<int64_t>(nextMs, lastChar.startMs + 1,
                                                 lastChar.startMs + 5000);
        }
    }

    return doc;
}

ParsedLrcDocument parseLocalLrcBytes(const std::string& rawBytes) {
    const std::string utf8 = decodeLocalLyricBytesToUtf8(rawBytes);
    if (utf8.empty())
        return {};
    return parseLocalLrcUtf8(utf8);
}
