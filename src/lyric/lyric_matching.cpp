#include "lyric/lyric_matching.h"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cwchar>
#include <cwctype>
#include <initializer_list>
#include <limits>
#include <utility>

namespace lyric_detail {
namespace {

size_t matchTrailingOpen(const std::wstring& s, wchar_t open, wchar_t close) {
    int depth = 0;
    for (size_t i = s.size(); i-- > 0;) {
        if (s[i] == close) {
            ++depth;
        } else if (s[i] == open) {
            if (--depth == 0)
                return i;
        }
    }
    return std::wstring::npos;
}

bool containsVersionWord(const std::wstring& raw, const wchar_t* needle) {
    const std::wstring text = lowerW(simplifiedChinese(raw));
    const std::wstring word = lowerW(needle);
    if (text.empty() || word.empty())
        return false;

    size_t pos = 0;
    while ((pos = text.find(word, pos)) != std::wstring::npos) {
        const bool asciiWord = std::all_of(word.begin(), word.end(), [](wchar_t ch) {
            return ch < 0x80 && (std::iswalnum(ch) || ch == L'.');
        });
        const bool leftBoundary = pos == 0 || !asciiWord || !std::iswalnum(text[pos - 1]);
        const size_t end = pos + word.size();
        const bool rightBoundary = end >= text.size() || !asciiWord || !std::iswalnum(text[end]);
        if (leftBoundary && rightBoundary)
            return true;
        pos = end;
    }
    return false;
}

std::vector<std::wstring> versionTagsInText(const std::wstring& raw) {
    std::vector<std::wstring> tags;
    auto hasAny = [&](std::initializer_list<const wchar_t*> aliases) {
        return std::any_of(aliases.begin(), aliases.end(), [&](const wchar_t* alias) {
            return containsVersionWord(raw, alias);
        });
    };
    auto add = [&](const wchar_t* tag) {
        if (std::find(tags.begin(), tags.end(), tag) == tags.end())
            tags.emplace_back(tag);
    };

    bool specific = false;
    if (hasAny({L"live", L"现场", L"演唱会", L"concert"})) {
        add(L"live");
        specific = true;
    }
    if (hasAny({L"remix", L"混音"})) {
        add(L"remix");
        specific = true;
    }
    if (hasAny({L"acoustic", L"unplugged", L"不插电"})) {
        add(L"acoustic");
        specific = true;
    }
    if (hasAny({L"instrumental", L"karaoke", L"伴奏", L"纯音乐"})) {
        add(L"instrumental");
        specific = true;
    }
    if (hasAny({L"demo", L"小样"})) {
        add(L"demo");
        specific = true;
    }
    if (hasAny({L"simlish"})) {
        add(L"simlish");
        specific = true;
    }
    if (hasAny({L"japanese", L"japan", L"日文", L"日语", L"日本語"})) {
        add(L"japanese");
        specific = true;
    }
    if (hasAny({L"english", L"英文", L"英语"})) {
        add(L"english");
        specific = true;
    }
    if (hasAny({L"chinese", L"中文", L"中文版", L"国语", L"普通话"})) {
        add(L"chinese");
        specific = true;
    }
    if (hasAny({L"dj", L"dj版", L"剧情", L"剧情版"})) {
        add(L"special");
        specific = true;
    }
    if (hasAny({L"vma", L"award", L"颁奖", L"典礼"})) {
        add(L"award");
        specific = true;
    }
    if (hasAny({L"remastered", L"重制"})) {
        add(L"remastered");
        specific = true;
    }
    if (!specific && hasAny({L"version", L"ver.", L"edit", L"special", L"anniversary"}))
        add(L"version");
    return tags;
}

void appendUniqueVersionTags(std::vector<std::wstring>& out, const std::wstring& text) {
    for (const auto& tag : versionTagsInText(text)) {
        if (std::find(out.begin(), out.end(), tag) == out.end())
            out.push_back(tag);
    }
}

bool isBracketOpen(wchar_t ch, wchar_t& close) {
    switch (ch) {
    case L'(': close = L')'; return true;
    case L'[': close = L']'; return true;
    case L'（': close = L'）'; return true;
    case L'【': close = L'】'; return true;
    default: return false;
    }
}

bool isNoiseBracketContent(const std::wstring& content) {
    const std::wstring value = lowerW(trimW(content));
    for (const wchar_t* noise : {L"explicit", L"deluxe", L"digital", L"premium",
                                 L"album", L"edit", L"version", L"special",
                                 L"anniversary", L"studio", L"remastered"}) {
        if (value == noise)
            return true;
    }
    return false;
}

std::wstring stripFeatureSuffix(std::wstring s) {
    const std::wstring lower = lowerW(s);
    size_t cut = std::wstring::npos;
    for (const wchar_t* marker : {L" feat.", L" feat ", L" ft.", L" ft ", L" with "}) {
        size_t pos = 0;
        while ((pos = lower.find(marker, pos)) != std::wstring::npos) {
            if (pos > 0 && (pos + 1 < lower.size() || marker[0] == L' '))
                cut = cut == std::wstring::npos ? pos : std::min(cut, pos);
            pos += std::wcslen(marker);
        }
    }
    if (cut != std::wstring::npos)
        s.resize(cut);
    return trimW(s);
}

bool isMatchSeparator(wchar_t ch) {
    if (std::iswspace(ch))
        return true;
    if (ch < 0x80 && !std::isalnum(static_cast<unsigned char>(ch)))
        return true;
    switch (ch) {
    case L'，': case L'。': case L'、': case L'；': case L'：': case L'！':
    case L'？': case L'（': case L'）': case L'【': case L'】': case L'《':
    case L'》': case L'「': case L'」': case L'『': case L'』': case L'—':
    case L'–': case L'…': case L'·':
        return true;
    default:
        return false;
    }
}

std::vector<std::wstring> splitSingers(const std::wstring& s) {
    std::vector<std::wstring> parts;
    std::wstring current;
    for (wchar_t ch : s) {
        if (ch == L'/' || ch == L'、' || ch == L'&' || ch == L';' || ch == L'；' ||
            ch == L',' || ch == L'，' || ch == L'×') {
            auto t = trimW(current);
            if (!t.empty())
                parts.push_back(t);
            current.clear();
        } else {
            current.push_back(ch);
        }
    }
    auto t = trimW(current);
    if (!t.empty())
        parts.push_back(t);
    return parts;
}

std::vector<std::wstring> splitArtists(const std::wstring& s) {
    std::vector<std::wstring> out;
    for (const auto& part : splitSingers(s)) {
        std::wstring n = L' ' + normalizeTitle(part) + L' ';
        for (const wchar_t* kw : {L" feat. ", L" feat ", L" ft. ", L" ft ", L" featuring ",
                                  L" with ", L" vs ", L" vs. "})
            replaceAllW(n, kw, L" / ");
        for (const auto& p : splitSingers(n)) {
            auto t = trimW(p);
            if (!t.empty())
                out.push_back(t);
        }
    }
    return out;
}

bool sameVersionTags(std::vector<std::wstring> left, std::vector<std::wstring> right) {
    std::sort(left.begin(), left.end());
    std::sort(right.begin(), right.end());
    return left == right;
}

bool isUnknownTitle(const std::wstring& value) {
    const std::wstring normalized = lowerW(trimW(value));
    return normalized.empty() || normalized == L"unknown title";
}

bool isUsefulArtist(const std::wstring& value) {
    const std::wstring normalized = lowerW(trimW(value));
    return !normalized.empty() && normalized != L"unknown artist" && normalized != L"未知歌手";
}

} // namespace

std::wstring trimW(const std::wstring& s) {
    const size_t first = s.find_first_not_of(L" \t\r\n");
    if (first == std::wstring::npos)
        return {};
    const size_t last = s.find_last_not_of(L" \t\r\n");
    return s.substr(first, last - first + 1);
}

std::wstring lowerW(std::wstring value) {
    for (auto& ch : value)
        ch = static_cast<wchar_t>(std::towlower(ch));
    return value;
}

void replaceAllW(std::wstring& s, const std::wstring& from, const std::wstring& to) {
    size_t pos = 0;
    while ((pos = s.find(from, pos)) != std::wstring::npos) {
        s.replace(pos, from.size(), to);
        pos += to.size();
    }
}

std::wstring simplifiedChinese(const std::wstring& text) {
    if (text.empty() || text.size() > static_cast<size_t>((std::numeric_limits<int>::max)()))
        return text;
    const int length = static_cast<int>(text.size());
    const int size = LCMapStringEx(L"zh-CN", LCMAP_SIMPLIFIED_CHINESE, text.data(), length,
                                   nullptr, 0, nullptr, nullptr, 0);
    if (size <= 0)
        return text;
    std::wstring result(static_cast<size_t>(size), L'\0');
    if (LCMapStringEx(L"zh-CN", LCMAP_SIMPLIFIED_CHINESE, text.data(), length,
                      result.data(), size, nullptr, nullptr, 0) != size)
        return text;
    return result;
}

void stripTrailingBrackets(std::wstring& s, std::vector<std::wstring>* tagsOut) {
    while (!s.empty() && (s.back() == L' ' || s.back() == L'\t'))
        s.pop_back();
    while (!s.empty()) {
        wchar_t open = 0;
        const wchar_t close = s.back();
        if (close == L')')
            open = L'(';
        else if (close == L'）')
            open = L'（';
        else if (close == L']')
            open = L'[';
        else if (close == L'】')
            open = L'【';
        else
            break;
        const size_t pos = matchTrailingOpen(s, open, close);
        if (pos == std::wstring::npos || pos == 0)
            break;
        if (tagsOut) {
            std::wstring tag = trimW(s.substr(pos + 1, s.size() - pos - 2));
            if (!tag.empty())
                tagsOut->push_back(std::move(tag));
        }
        size_t end = pos;
        while (end > 0 && s[end - 1] == L' ')
            --end;
        s.resize(end);
    }
}

std::wstring normalizeTitle(const std::wstring& s) {
    const std::wstring source = stripFeatureSuffix(simplifiedChinese(s));
    std::wstring out;
    out.reserve(source.size());
    bool prevSpace = true;
    for (size_t i = 0; i < source.size(); ++i) {
        wchar_t close = 0;
        if (isBracketOpen(source[i], close)) {
            const size_t end = source.find(close, i + 1);
            if (end != std::wstring::npos) {
                const std::wstring content = source.substr(i + 1, end - i - 1);
                if (!isNoiseBracketContent(content)) {
                    if (!prevSpace) {
                        out.push_back(L' ');
                        prevSpace = true;
                    }
                    for (wchar_t ch : content) {
                        if (isMatchSeparator(ch)) {
                            if (!prevSpace) {
                                out.push_back(L' ');
                                prevSpace = true;
                            }
                        } else {
                            out.push_back(static_cast<wchar_t>(std::towlower(ch)));
                            prevSpace = false;
                        }
                    }
                    if (!prevSpace) {
                        out.push_back(L' ');
                        prevSpace = true;
                    }
                } else if (!prevSpace) {
                    out.push_back(L' ');
                    prevSpace = true;
                }
                i = end;
                continue;
            }
        }
        const wchar_t ch = source[i];
        if (isMatchSeparator(ch)) {
            if (!prevSpace) {
                out.push_back(L' ');
                prevSpace = true;
            }
        } else {
            out.push_back(static_cast<wchar_t>(std::towlower(ch)));
            prevSpace = false;
        }
    }
    while (!out.empty() && out.back() == L' ')
        out.pop_back();
    return out;
}

std::vector<std::wstring> extractVersionTags(const std::wstring& s) {
    std::vector<std::wstring> tags;
    auto collectBracket = [&](wchar_t open, wchar_t close) {
        size_t pos = 0;
        while ((pos = s.find(open, pos)) != std::wstring::npos) {
            const size_t end = s.find(close, pos + 1);
            if (end == std::wstring::npos)
                break;
            appendUniqueVersionTags(tags, s.substr(pos + 1, end - pos - 1));
            pos = end + 1;
        }
    };
    collectBracket(L'(', L')');
    collectBracket(L'[', L']');
    collectBracket(L'（', L'）');
    collectBracket(L'【', L'】');

    size_t delimiter = std::wstring::npos;
    size_t delimiterLength = 0;
    for (const wchar_t* sep : {L" - ", L" – ", L" — ", L"-", L"–", L"—"}) {
        const size_t pos = s.rfind(sep);
        if (pos != std::wstring::npos && (delimiter == std::wstring::npos || pos > delimiter)) {
            delimiter = pos;
            delimiterLength = std::wcslen(sep);
        }
    }
    if (delimiter != std::wstring::npos)
        appendUniqueVersionTags(tags, s.substr(delimiter + delimiterLength));

    const std::wstring trimmed = trimW(s);
    const bool endsWithBracket = !trimmed.empty() &&
                                 (trimmed.back() == L')' || trimmed.back() == L']' ||
                                  trimmed.back() == L'）' || trimmed.back() == L'】');
    if (!endsWithBracket) {
        const size_t lastSpace = trimmed.find_last_of(L" \t\r\n");
        const std::wstring lastToken = lastSpace == std::wstring::npos
                                           ? trimmed
                                           : trimmed.substr(lastSpace + 1);
        appendUniqueVersionTags(tags, lastToken);
        if (containsVersionWord(lastToken, L"version") ||
            containsVersionWord(lastToken, L"ver.")) {
            const size_t previousSpace = lastSpace == std::wstring::npos || lastSpace == 0
                                             ? std::wstring::npos
                                             : trimmed.find_last_of(L" \t\r\n", lastSpace - 1);
            const size_t start = previousSpace == std::wstring::npos ? 0 : previousSpace + 1;
            appendUniqueVersionTags(tags, trimmed.substr(start));
        }
    }
    return tags;
}

bool titleEndsWithArtist(const std::wstring& title, const std::wstring& artist) {
    if (artist.empty())
        return false;
    const std::wstring t = lowerW(title);
    const std::wstring a = lowerW(artist);
    const wchar_t* seps[] = {L" - ", L"-", L" – ", L"–", L" — ", L"—", L" / ", L"/"};
    for (const wchar_t* sep : seps) {
        const std::wstring suffix = sep + a;
        if (t.size() > suffix.size() &&
            t.compare(t.size() - suffix.size(), suffix.size(), suffix) == 0)
            return true;
    }
    return false;
}

std::wstring stripVersionSuffix(std::wstring s) {
    s = stripFeatureSuffix(std::move(s));
    stripTrailingBrackets(s, nullptr);
    return trimW(s);
}

std::wstring stripDelimitedVersionSuffix(const std::wstring& s) {
    size_t delimiter = std::wstring::npos;
    size_t delimiterLength = 0;
    for (const wchar_t* sep : {L" - ", L" – ", L" — "}) {
        const size_t pos = s.rfind(sep);
        if (pos != std::wstring::npos && (delimiter == std::wstring::npos || pos > delimiter)) {
            delimiter = pos;
            delimiterLength = std::wcslen(sep);
        }
    }
    if (delimiter == std::wstring::npos ||
        extractVersionTags(s.substr(delimiter + delimiterLength)).empty())
        return s;
    return trimW(s.substr(0, delimiter));
}

double jaroWinkler(const std::wstring& a, const std::wstring& b) {
    if (a.empty() || b.empty())
        return 0.0;
    if (a == b)
        return 1.0;

    const size_t maxLen = std::max(a.size(), b.size());
    const int matchDistance = std::max(0, static_cast<int>(maxLen / 2) - 1);
    std::vector<bool> aMatched(a.size(), false);
    std::vector<bool> bMatched(b.size(), false);
    size_t matches = 0;

    for (size_t i = 0; i < a.size(); ++i) {
        const size_t start = i > static_cast<size_t>(matchDistance)
                                 ? i - static_cast<size_t>(matchDistance)
                                 : 0;
        const size_t end = std::min(b.size(), i + static_cast<size_t>(matchDistance) + 1);
        for (size_t j = start; j < end; ++j) {
            if (bMatched[j] || a[i] != b[j])
                continue;
            aMatched[i] = true;
            bMatched[j] = true;
            ++matches;
            break;
        }
    }
    if (matches == 0)
        return 0.0;

    size_t transpositions = 0;
    size_t j = 0;
    for (size_t i = 0; i < a.size(); ++i) {
        if (!aMatched[i])
            continue;
        while (!bMatched[j])
            ++j;
        if (a[i] != b[j])
            ++transpositions;
        ++j;
    }

    const double matchCount = static_cast<double>(matches);
    double score = (matchCount / a.size() + matchCount / b.size() +
                    (matchCount - transpositions / 2.0) / matchCount) /
                   3.0;
    if (score > 0.7) {
        size_t prefix = 0;
        while (prefix < std::min({a.size(), b.size(), static_cast<size_t>(4)}) &&
               a[prefix] == b[prefix])
            ++prefix;
        score += static_cast<double>(prefix) * 0.1 * (1.0 - score);
    }
    return std::clamp(score, 0.0, 1.0);
}

int titleSimilarity(const std::wstring& a, const std::wstring& b) {
    const std::wstring na = normalizeTitle(a);
    const std::wstring nb = normalizeTitle(b);
    if (na.empty() || nb.empty())
        return 0;
    const double score = jaroWinkler(na, nb);
    if (score < 0.50)
        return 0;
    return static_cast<int>(std::lround(score * 100.0));
}

int singerSimilarity(const std::wstring& a, const std::wstring& b) {
    if (a.empty() || b.empty())
        return 0;
    std::wstring na = normalizeTitle(a);
    std::wstring nb = normalizeTitle(b);
    if (na == nb)
        return 100;
    for (std::wstring* n : {&na, &nb}) {
        *n = L' ' + *n + L' ';
        for (const wchar_t* kw : {L" feat. ", L" feat ", L" ft. ", L" ft ", L" featuring ",
                                  L" with ", L" vs ", L" vs. "})
            replaceAllW(*n, kw, L" / ");
        *n = trimW(*n);
    }
    if (na == nb)
        return 100;
    if (na.find(nb) != std::wstring::npos || nb.find(na) != std::wstring::npos)
        return 90;
    const auto pa = splitArtists(a);
    const auto pb = splitArtists(b);
    if (pa.empty() || pb.empty())
        return 0;
    int matches = 0;
    for (const auto& x : pa) {
        for (const auto& y : pb) {
            if (x == y) {
                ++matches;
                break;
            }
        }
    }
    if (matches == 0)
        return 0;
    const int tokenScore =
        std::min(100, matches * 100 / static_cast<int>(std::max(pa.size(), pb.size())));
    const int textScore = static_cast<int>(std::lround(jaroWinkler(na, nb) * 100.0));
    return std::max(tokenScore, textScore);
}

double durationSimilarity(int64_t queryMs, int64_t candMs) {
    if (queryMs <= 0 || candMs <= 0)
        return 0.0;
    const int64_t diff = std::llabs(static_cast<long long>(candMs - queryMs));
    if (diff <= 1000)
        return 1.0;
    if (diff >= 10000)
        return 0.0;
    return 1.0 - static_cast<double>(diff - 1000) / 9000.0;
}

void addSearchVariant(std::vector<SearchVariant>& variants, std::wstring title,
                      std::wstring artist) {
    title = trimW(title);
    artist = trimW(artist);
    if (title.empty())
        return;
    for (const auto& existing : variants) {
        if (existing.title == title && existing.artist == artist)
            return;
    }
    variants.push_back({std::move(title), std::move(artist)});
}

std::wstring primaryArtist(const std::wstring& artist) {
    std::wstring normalized = normalizeTitle(artist);
    for (const wchar_t* kw : {L" feat ", L" ft ", L" featuring ", L" with ", L" vs "})
        replaceAllW(normalized, kw, L"/");
    const auto collab = splitSingers(normalized);
    if (collab.size() > 1)
        return collab.front();
    const auto raw = splitSingers(artist);
    if (!raw.empty())
        return trimW(raw.front());
    return trimW(artist);
}

std::vector<SearchVariant> buildSearchVariants(const MatchQuery& q) {
    std::vector<SearchVariant> variants;
    addSearchVariant(variants, q.title, q.artist); // exact
    addSearchVariant(variants, simplifiedChinese(q.title), simplifiedChinese(q.artist));
    addSearchVariant(variants, normalizeTitle(q.title), normalizeTitle(q.artist));
    addSearchVariant(variants, q.title, primaryArtist(q.artist));
    addSearchVariant(variants, stripVersionSuffix(q.title), q.artist);
    addSearchVariant(variants, stripDelimitedVersionSuffix(q.title), q.artist);
    const size_t count = variants.size();
    for (size_t index = 0; index < count; ++index) {
        auto title = simplifiedChinese(variants[index].title);
        auto artist = simplifiedChinese(variants[index].artist);
        addSearchVariant(variants, std::move(title), std::move(artist));
    }
    return variants;
}

std::wstring makeSearchQuery(const SearchVariant& variant) {
    std::wstring query = variant.title;
    if (!variant.artist.empty() && !titleEndsWithArtist(variant.title, variant.artist)) {
        if (!query.empty())
            query += L' ';
        query += variant.artist;
    }
    return query;
}

int scoreCandidate(const MatchQuery& q, const std::wstring& name, const std::wstring& singer,
                   int64_t candDurationMs) {
    if (isUnknownTitle(q.title) || isUnknownTitle(name))
        return 0;

    const int t = titleSimilarity(q.title, name);
    if (t == 0)
        return 0;
    const bool hasArtist = isUsefulArtist(q.artist) && isUsefulArtist(singer);
    const int s = hasArtist ? singerSimilarity(q.artist, singer) : 0;
    if (hasArtist && s == 0)
        return 0;
    if (q.durationMs > 0 && candDurationMs > 0 &&
        std::llabs(static_cast<long long>(candDurationMs - q.durationMs)) > 15000)
        return 0;
    if (!sameVersionTags(q.versionTags, extractVersionTags(name)))
        return 0;

    const bool hasDuration = q.durationMs > 0 && candDurationMs > 0;
    const double titleScore = t / 100.0;
    const double artistScore = hasArtist ? s / 100.0 : 0.0;
    const double durationScore = hasDuration ? durationSimilarity(q.durationMs, candDurationMs)
                                             : 0.0;
    double total = titleScore;
    if (hasArtist && hasDuration)
        total = titleScore * 0.50 + artistScore * 0.30 + durationScore * 0.20;
    else if (hasArtist)
        total = titleScore * 0.60 + artistScore * 0.40;
    else if (hasDuration)
        total = titleScore * 0.75 + durationScore * 0.25;
    return static_cast<int>(std::lround(std::clamp(total, 0.0, 1.0) * 100.0));
}

} // namespace lyric_detail
