#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace lyric_detail {

constexpr int kScoreThreshold = 80;

struct MatchQuery {
    std::wstring title;
    std::wstring artist;
    int64_t durationMs = 0;
    std::vector<std::wstring> versionTags; // 从原始（未净化）标题提取
};

struct SearchVariant {
    std::wstring title;
    std::wstring artist;
};

std::wstring trimW(const std::wstring& s);
std::wstring lowerW(std::wstring value);
void replaceAllW(std::wstring& s, const std::wstring& from, const std::wstring& to);
std::wstring simplifiedChinese(const std::wstring& text);

void stripTrailingBrackets(std::wstring& s, std::vector<std::wstring>* tagsOut);
std::wstring stripVersionSuffix(std::wstring s);
std::wstring stripDelimitedVersionSuffix(const std::wstring& s);
std::wstring normalizeTitle(const std::wstring& s);
std::vector<std::wstring> extractVersionTags(const std::wstring& s);
bool titleEndsWithArtist(const std::wstring& title, const std::wstring& artist);

double jaroWinkler(const std::wstring& a, const std::wstring& b);
int titleSimilarity(const std::wstring& a, const std::wstring& b);
int singerSimilarity(const std::wstring& a, const std::wstring& b);
double durationSimilarity(int64_t queryMs, int64_t candMs);

void addSearchVariant(std::vector<SearchVariant>& variants, std::wstring title,
                      std::wstring artist);
std::wstring primaryArtist(const std::wstring& artist);
std::vector<SearchVariant> buildSearchVariants(const MatchQuery& q);
std::wstring makeSearchQuery(const SearchVariant& variant);

int scoreCandidate(const MatchQuery& q, const std::wstring& name, const std::wstring& singer,
                   int64_t candDurationMs);

} // namespace lyric_detail
