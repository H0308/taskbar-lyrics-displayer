#include "lyric/simulated_karaoke.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace lyric_timing {
namespace {

struct WeightedToken {
    std::wstring text;
    double weight = 1.0;
};

constexpr double kWhitespaceWeight = 0.08;
constexpr double kOtherPunctuationWeight = 0.08;
constexpr double kPausePunctuationWeight = 0.40;
constexpr double kPauseVocalUnits = 0.30;

constexpr double kMaxMsPerUnit = 650.0;
constexpr double kMaxLineBaseMs = 1200.0;
constexpr int64_t kMinMaxLineMs = 2500;
constexpr int64_t kHardMaxLineMs = 18000;

constexpr double kFallbackMsPerUnit = 380.0;
constexpr double kFallbackBaseMs = 600.0;
constexpr int64_t kMinFallbackLineMs = 1800;
constexpr int64_t kMaxFallbackLineMs = 6000;

bool isHighSurrogate(wchar_t c) {
    return c >= 0xD800 && c <= 0xDBFF;
}

bool isLowSurrogate(wchar_t c) {
    return c >= 0xDC00 && c <= 0xDFFF;
}

bool isCombiningMark(wchar_t c) {
    return (c >= 0x0300 && c <= 0x036F) || (c >= 0x1AB0 && c <= 0x1AFF) ||
           (c >= 0x1DC0 && c <= 0x1DFF) || (c >= 0x20D0 && c <= 0x20FF) ||
           (c >= 0xFE00 && c <= 0xFE0F) || (c >= 0xFE20 && c <= 0xFE2F);
}

bool isWhitespace(wchar_t c) {
    return c == L' ' || c == L'\t' || c == L'\r' || c == L'\n' || c == 0x3000;
}

bool isWordCoreChar(wchar_t c) {
    return (c >= L'0' && c <= L'9') || (c >= L'A' && c <= L'Z') || (c >= L'a' && c <= L'z') ||
           (c >= 0x00C0 && c <= 0x024F) || (c >= 0x0370 && c <= 0x03FF) ||
           (c >= 0x0400 && c <= 0x04FF);
}

bool isWordJoiner(wchar_t c) {
    return c == L'\'' || c == 0x2019 || c == L'-';
}

bool isPausePunctuation(wchar_t c) {
    return c == L',' || c == 0xFF0C || c == 0x3001 || c == L';' || c == 0xFF1B || c == L':' ||
           c == 0xFF1A || c == 0x2014 || c == 0x2026 || c == L'~' || c == 0xFF5E;
}

bool isGeneralPunctuation(wchar_t c) {
    if (c < 0x80)
        return !isWordCoreChar(c) && !isWhitespace(c);
    return (c >= 0x2000 && c <= 0x206F) || (c >= 0x3001 && c <= 0x303F) ||
           (c >= 0xFF01 && c <= 0xFF0F) || (c >= 0xFF1A && c <= 0xFF20) ||
           (c >= 0xFF3B && c <= 0xFF40) || (c >= 0xFF5B && c <= 0xFF65);
}

size_t nextClusterEnd(const std::wstring& text, size_t start) {
    if (start >= text.size())
        return start;
    size_t end = start + 1;
    if (end < text.size() && isHighSurrogate(text[start]) && isLowSurrogate(text[end]))
        ++end;
    while (end < text.size() && isCombiningMark(text[end]))
        ++end;
    return end;
}

std::vector<WeightedToken> tokenizeLine(const std::wstring& text, double& vocalUnitsOut) {
    vocalUnitsOut = 0.0;
    std::vector<WeightedToken> tokens;
    if (text.empty())
        return tokens;

    bool hasVisible = false;
    for (wchar_t c : text) {
        if (!isWhitespace(c)) {
            hasVisible = true;
            break;
        }
    }
    if (!hasVisible)
        return tokens;

    tokens.reserve(text.size());
    for (size_t i = 0; i < text.size();) {
        const wchar_t ch = text[i];
        if (isWhitespace(ch)) {
            size_t end = i + 1;
            while (end < text.size() && isWhitespace(text[end]))
                ++end;
            tokens.push_back({text.substr(i, end - i), kWhitespaceWeight});
            i = end;
            continue;
        }

        if (isWordCoreChar(ch)) {
            size_t end = nextClusterEnd(text, i);
            size_t coreCount = 1;
            while (end < text.size()) {
                if (isWordCoreChar(text[end])) {
                    end = nextClusterEnd(text, end);
                    ++coreCount;
                } else if (isWordJoiner(text[end]) && end + 1 < text.size() &&
                           isWordCoreChar(text[end + 1])) {
                    end = nextClusterEnd(text, end);
                } else {
                    break;
                }
            }
            const double wordUnits = std::max(1.0, static_cast<double>(coreCount) * 0.35);
            const double weight = std::clamp(wordUnits, 1.0, 3.2);
            vocalUnitsOut += wordUnits;
            tokens.push_back({text.substr(i, end - i), weight});
            i = end;
            continue;
        }

        const size_t end = nextClusterEnd(text, i);
        if (isPausePunctuation(ch)) {
            vocalUnitsOut += kPauseVocalUnits;
            tokens.push_back({text.substr(i, end - i), kPausePunctuationWeight});
        } else if (isGeneralPunctuation(ch)) {
            tokens.push_back({text.substr(i, end - i), kOtherPunctuationWeight});
        } else {
            vocalUnitsOut += 1.0;
            tokens.push_back({text.substr(i, end - i), 1.0});
        }
        i = end;
    }

    return tokens;
}

int64_t resolveEffectiveLineDurationMs(const std::vector<LyricLine>& lines, size_t lineIndex,
                                       int64_t trackDurationMs, double vocalUnits) {
    const int64_t startMs = lines[lineIndex].ms;
    int64_t rawGapMs = -1;
    for (size_t next = lineIndex + 1; next < lines.size(); ++next) {
        if (lines[next].ms > startMs) {
            rawGapMs = lines[next].ms - startMs;
            break;
        }
    }
    if (rawGapMs <= 0 && trackDurationMs > startMs)
        rawGapMs = trackDurationMs - startMs;

    const double units = std::max(1.0, vocalUnits);
    const int64_t maxDurationMs = std::clamp<int64_t>(
        static_cast<int64_t>(std::llround(units * kMaxMsPerUnit + kMaxLineBaseMs)),
        kMinMaxLineMs, kHardMaxLineMs);
    const int64_t fallbackDurationMs = std::clamp<int64_t>(
        static_cast<int64_t>(std::llround(units * kFallbackMsPerUnit + kFallbackBaseMs)),
        kMinFallbackLineMs, kMaxFallbackLineMs);

    if (rawGapMs <= 0)
        return fallbackDurationMs;
    if (rawGapMs > maxDurationMs)
        return maxDurationMs;
    if (rawGapMs > 1200) {
        const int64_t tailBufferMs = std::clamp<int64_t>(rawGapMs / 16, 80, 180);
        return std::max<int64_t>(1, rawGapMs - tailBufferMs);
    }
    return rawGapMs;
}

} // namespace

std::vector<LyricChar> synthesizeSimulatedKaraokeChars(const std::vector<LyricLine>& lines,
                                                       size_t lineIndex,
                                                       int64_t trackDurationMs) {
    if (lineIndex >= lines.size())
        return {};
    const LyricLine& line = lines[lineIndex];
    double vocalUnits = 0.0;
    std::vector<WeightedToken> tokens = tokenizeLine(line.text, vocalUnits);
    if (tokens.empty())
        return {};

    double totalWeight = 0.0;
    for (const auto& token : tokens)
        totalWeight += token.weight;
    if (totalWeight <= 0.0)
        return {};

    const int64_t durationMs =
        resolveEffectiveLineDurationMs(lines, lineIndex, trackDurationMs, vocalUnits);
    const int64_t lineEndMs = line.ms + durationMs;

    std::vector<LyricChar> result;
    result.reserve(tokens.size());
    double accumulatedWeight = 0.0;
    int64_t currentStartMs = line.ms;
    for (size_t i = 0; i < tokens.size(); ++i) {
        accumulatedWeight += tokens[i].weight;
        int64_t endMs = lineEndMs;
        if (i + 1 < tokens.size()) {
            const double ratio = accumulatedWeight / totalWeight;
            endMs = line.ms + static_cast<int64_t>(std::llround(durationMs * ratio));
            endMs = std::clamp<int64_t>(endMs, currentStartMs, lineEndMs);
        }
        result.push_back(LyricChar{currentStartMs, endMs, std::move(tokens[i].text)});
        currentStartMs = endMs;
    }
    return result;
}

std::vector<LyricLine> buildSimulatedKaraokeLines(const std::vector<LyricLine>& lines,
                                                  int64_t trackDurationMs) {
    std::vector<LyricLine> result = lines;
    for (size_t i = 0; i < result.size(); ++i) {
        if (result[i].chars.empty())
            result[i].chars = synthesizeSimulatedKaraokeChars(lines, i, trackDurationMs);
    }
    return result;
}

} // namespace lyric_timing
