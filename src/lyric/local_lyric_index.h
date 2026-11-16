#pragma once

#include "lyric/lyric_provider.h"

#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace lyric_detail {

enum class LocalLyricVariant {
    Original,
    Translation,
    Romanization,
};

struct LocalLyricBundle {
    std::filesystem::path originalPath;
    std::filesystem::path translationPath;
    std::filesystem::path romanizationPath;
    std::filesystem::path orderPath;
    std::wstring artist;
    std::wstring artistKey;
    std::vector<std::wstring> titles;
};

struct LocalLyricIndex {
    std::vector<LocalLyricBundle> bundles;
    std::unordered_map<std::wstring, std::vector<size_t>> byArtist;
};

struct LocalLyricMetadata {
    std::wstring artist;
    std::wstring artistKey;
    std::vector<std::wstring> titles;
};

bool parseLocalLyricBaseName(const std::wstring& base, LocalLyricMetadata& out);
bool parseLocalQrcPath(const std::filesystem::path& path, std::wstring& base,
                       LocalLyricVariant& variant);

std::shared_ptr<LocalLyricIndex> buildLocalLyricIndex(const std::wstring& root,
                                                      const std::wstring& orderDir);

std::vector<size_t> findLocalLyricCandidates(const LocalLyricIndex& index,
                                             const std::wstring& title,
                                             const std::wstring& artist);

bool lookupLocalLyric(const LocalLyricIndex& index, const std::wstring& title,
                      const std::wstring& artist, std::vector<LyricLine>& out, SongInfo& info);

bool readLocalOrderOverride(const LocalLyricBundle& bundle);
bool writeLocalOrderOverride(const LocalLyricBundle& bundle, bool onlineFirst);

} // namespace lyric_detail
