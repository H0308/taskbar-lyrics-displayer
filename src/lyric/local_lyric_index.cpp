#include "lyric/local_lyric_index.h"

#include "logging/runtime_logger.h"
#include "lyric/lrc_parser.h"
#include "lyric/lyric_http.h"
#include "lyric/lyric_matching.h"
#include "lyric/qrc_decoder.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <fstream>
#include <map>
#include <system_error>

namespace lyric_detail {
namespace {

bool hasLocalSuffix(const std::wstring& value, const std::wstring& suffix) {
    return value.size() >= suffix.size() &&
           value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0;
}

std::vector<std::wstring> splitLocalField(const std::wstring& value, wchar_t separator) {
    std::vector<std::wstring> parts;
    size_t begin = 0;
    while (begin <= value.size()) {
        const size_t end = value.find(separator, begin);
        const size_t length = end == std::wstring::npos ? value.size() - begin : end - begin;
        parts.push_back(trimW(value.substr(begin, length)));
        if (end == std::wstring::npos)
            break;
        begin = end + 1;
    }
    return parts;
}

std::wstring joinLocalParts(const std::vector<std::wstring>& parts, size_t begin, size_t end) {
    std::wstring result;
    for (size_t i = begin; i < end; ++i) {
        const std::wstring part = trimW(parts[i]);
        if (part.empty())
            continue;
        if (!result.empty())
            result += L" - ";
        result += part;
    }
    return result;
}

bool isLocalNumber(const std::wstring& value) {
    if (value.empty())
        return false;
    for (wchar_t ch : value) {
        if (ch < L'0' || ch > L'9')
            return false;
    }
    return true;
}

std::wstring normalizeLocalArtists(const std::wstring& value, wchar_t separator) {
    std::wstring result;
    for (const auto& part : splitLocalField(value, separator)) {
        // QQ 音乐本地文件可能把外文歌手写成“原名 (翻译名)”；播放元数据通常只有原名。
        // 这里只去掉歌手字段末尾的括号别名，保留歌曲标题中的普通括号语义。
        std::wstring artist = part;
        stripTrailingBrackets(artist, nullptr);
        const std::wstring normalized = normalizeTitle(artist);
        if (normalized.empty())
            continue;
        if (!result.empty())
            result.push_back(L'\x1f');
        result += normalized;
    }
    return result;
}

bool decodeLocalLyricFile(const std::filesystem::path& path, std::vector<LyricLine>& out) {
    out.clear();
    std::string content;
    if (!decodeLocalQrcText(path, content))
        return false;
    if (parseQrcLyricsText(content, out))
        return true;
    out = parseLrcText(content);
    return !out.empty();
}

void filterLocalSecondary(std::vector<LyricLine>& lines, bool romanization) {
    std::vector<LyricLine> filtered;
    filtered.reserve(lines.size());
    for (auto& line : lines) {
        line.text = trimW(line.text);
        if (line.text.empty() || line.text.rfind(L"//", 0) == 0)
            continue;
        if (romanization && normalizeTitle(line.text).find(L"以下音译标注由ai工具生产") !=
                                std::wstring::npos)
            continue;
        filtered.push_back(std::move(line));
    }
    lines = std::move(filtered);
}

bool loadLocalLyricBundle(const LocalLyricBundle& bundle, std::vector<LyricLine>& out) {
    if (!decodeLocalLyricFile(bundle.originalPath, out) || out.empty()) {
        const std::wstring path = bundle.originalPath.wstring();
        runtime_log::writef(L"[lyric][local-decode-failed][QQ] file=\"%ls\"", path.c_str());
        return false;
    }

    if (!bundle.translationPath.empty()) {
        std::vector<LyricLine> translation;
        if (decodeLocalLyricFile(bundle.translationPath, translation)) {
            filterLocalSecondary(translation, false);
            attachSecondary(out, translation, true);
        }
    }
    if (!bundle.romanizationPath.empty()) {
        std::vector<LyricLine> romanization;
        if (decodeLocalLyricFile(bundle.romanizationPath, romanization)) {
            filterLocalSecondary(romanization, true);
            attachSecondary(out, romanization, false);
        }
    }
    return !out.empty();
}

bool localLyricTitleMatches(const LocalLyricBundle& bundle, const std::wstring& title) {
    const std::wstring query = normalizeTitle(title);
    if (query.empty())
        return false;
    for (const auto& candidate : bundle.titles) {
        if (normalizeTitle(candidate) == query)
            return true;
    }
    return false;
}

std::wstring joinLocalArtists(const std::vector<std::wstring>& artists) {
    std::wstring result;
    for (const auto& artist : artists) {
        if (artist.empty())
            continue;
        if (!result.empty())
            result.push_back(L' ');
        result += artist;
    }
    return result;
}

bool localArtistSubsetMatches(const std::wstring& localKey, const std::wstring& queryKey) {
    const auto localArtists = splitLocalField(localKey, L'\x1f');
    const auto queryArtists = splitLocalField(queryKey, L'\x1f');
    if (localArtists.empty() || queryArtists.empty())
        return false;

    // 某些 QQ 文件会把歌手名中的句点/空格也编码成下划线，例如
    // “G.E.M. 邓紫棋”会保存为“G_E_M_邓紫棋”。先比较完整序列，
    // 同时不影响真正的多歌手“歌手A_歌手B”匹配。
    if (joinLocalArtists(localArtists) == joinLocalArtists(queryArtists))
        return true;

    if (localArtists.size() == queryArtists.size())
        return false;

    const auto& subset = localArtists.size() < queryArtists.size() ? localArtists : queryArtists;
    const auto& superset = localArtists.size() < queryArtists.size() ? queryArtists : localArtists;
    return std::all_of(subset.begin(), subset.end(), [&](const auto& artist) {
        return std::find(superset.begin(), superset.end(), artist) != superset.end();
    });
}

} // namespace

bool parseLocalLyricBaseName(const std::wstring& base, LocalLyricMetadata& out) {
    std::vector<std::wstring> fields;
    size_t begin = 0;
    while (begin <= base.size()) {
        const size_t end = base.find(L" - ", begin);
        const size_t length = end == std::wstring::npos ? base.size() - begin : end - begin;
        fields.push_back(trimW(base.substr(begin, length)));
        if (end == std::wstring::npos)
            break;
        begin = end + 3;
    }
    // 标准格式为“歌手 - 歌名 - 数字ID - 专辑名”；专辑名可能为空或整个字段缺省。
    if (fields.size() < 3 || fields[0].empty())
        return false;

    size_t idIndex = std::wstring::npos;
    for (size_t i = fields.size(); i-- > 2;) {
        if (isLocalNumber(fields[i])) {
            idIndex = i;
            break;
        }
    }
    if (idIndex == std::wstring::npos)
        return false;

    const std::wstring title = joinLocalParts(fields, 1, idIndex);
    if (title.empty())
        return false;

    out.artist = fields[0];
    out.artistKey = normalizeLocalArtists(out.artist, L'_');
    if (out.artistKey.empty())
        return false;
    out.titles.clear();
    out.titles.push_back(title);
    return true;
}

bool parseLocalQrcPath(const std::filesystem::path& path, std::wstring& base,
                       LocalLyricVariant& variant) {
    if (lowerW(path.extension().wstring()) != L".qrc")
        return false;

    const std::wstring stem = path.stem().wstring();
    const std::wstring lowerStem = lowerW(stem);
    struct VariantSuffix {
        const wchar_t* value;
        LocalLyricVariant variant;
    };
    for (const auto& suffix : {VariantSuffix{L"_qmroma", LocalLyricVariant::Romanization},
                               VariantSuffix{L"_qmts", LocalLyricVariant::Translation},
                               VariantSuffix{L"_qm", LocalLyricVariant::Original}}) {
        const std::wstring suffixText = suffix.value;
        if (hasLocalSuffix(lowerStem, suffixText) && stem.size() > suffixText.size()) {
            base = stem.substr(0, stem.size() - suffixText.size());
            variant = suffix.variant;
            return true;
        }
    }
    return false;
}

std::shared_ptr<LocalLyricIndex> buildLocalLyricIndex(const std::wstring& root,
                                                      const std::wstring& orderDir) {
    auto index = std::make_shared<LocalLyricIndex>();
    try {
        const std::filesystem::path rootPath(root);
        std::error_code ec;
        if (!std::filesystem::is_directory(rootPath, ec) || ec)
            return index;

        std::map<std::wstring, LocalLyricBundle> groups;
        const auto options = std::filesystem::directory_options::skip_permission_denied;
        std::filesystem::recursive_directory_iterator it(rootPath, options, ec);
        const std::filesystem::recursive_directory_iterator end;
        for (; it != end; it.increment(ec)) {
            if (isLyricShuttingDown())
                break;
            if (ec) {
                ec.clear();
                continue;
            }

            std::error_code fileEc;
            if (!it->is_regular_file(fileEc) || fileEc)
                continue;

            std::wstring base;
            LocalLyricVariant variant = LocalLyricVariant::Original;
            if (!parseLocalQrcPath(it->path(), base, variant))
                continue;

            LocalLyricMetadata metadata;
            if (!parseLocalLyricBaseName(base, metadata))
                continue;

            const std::wstring groupKey = it->path().parent_path().wstring() + L'\x1f' + base;
            auto [groupIt, inserted] = groups.try_emplace(groupKey);
            if (inserted) {
                groupIt->second.artist = metadata.artist;
                groupIt->second.artistKey = metadata.artistKey;
                groupIt->second.titles = metadata.titles;
            }

            switch (variant) {
            case LocalLyricVariant::Original:
                groupIt->second.originalPath = it->path();
                break;
            case LocalLyricVariant::Translation:
                groupIt->second.translationPath = it->path();
                break;
            case LocalLyricVariant::Romanization:
                groupIt->second.romanizationPath = it->path();
                break;
            }
        }

        const std::filesystem::path orderRoot(orderDir);
        for (auto& [key, bundle] : groups) {
            (void)key;
            if (bundle.originalPath.empty() || bundle.artistKey.empty() || bundle.titles.empty())
                continue;
            if (!orderRoot.empty()) {
                bundle.orderPath = orderRoot / bundle.originalPath.filename();
                bundle.orderPath.replace_extension(L".order.json");
            }
            index->bundles.push_back(std::move(bundle));
        }
        std::sort(index->bundles.begin(), index->bundles.end(), [](const auto& a, const auto& b) {
            return a.originalPath.wstring() < b.originalPath.wstring();
        });
        for (size_t i = 0; i < index->bundles.size(); ++i)
            index->byArtist[index->bundles[i].artistKey].push_back(i);
    } catch (...) {
        index->bundles.clear();
        index->byArtist.clear();
    }
    return index;
}

std::vector<size_t> findLocalLyricCandidates(const LocalLyricIndex& index,
                                             const std::wstring& title,
                                             const std::wstring& artist) {
    const std::wstring artistKey = normalizeLocalArtists(artist, L'/');
    if (artistKey.empty())
        return {};

    std::vector<size_t> candidates;
    const auto exact = index.byArtist.find(artistKey);
    if (exact != index.byArtist.end()) {
        for (size_t bundleIndex : exact->second) {
            if (localLyricTitleMatches(index.bundles[bundleIndex], title))
                candidates.push_back(bundleIndex);
        }
    }

    // QQ 文件和 SMTC 元数据有时各自只保存部分歌手，允许一方歌手列表作为另一方的子集。
    for (size_t bundleIndex = 0; bundleIndex < index.bundles.size(); ++bundleIndex) {
        const auto& bundle = index.bundles[bundleIndex];
        if (localArtistSubsetMatches(bundle.artistKey, artistKey) &&
            localLyricTitleMatches(bundle, title))
            candidates.push_back(bundleIndex);
    }

    std::sort(candidates.begin(), candidates.end(), [&index](const auto& a, const auto& b) {
        return index.bundles[a].originalPath.wstring() < index.bundles[b].originalPath.wstring();
    });
    return candidates;
}

bool lookupLocalLyric(const LocalLyricIndex& index, const std::wstring& title,
                      const std::wstring& artist, std::vector<LyricLine>& out, SongInfo& info) {
    for (size_t bundleIndex : findLocalLyricCandidates(index, title, artist)) {
        const auto& bundle = index.bundles[bundleIndex];
        if (loadLocalLyricBundle(bundle, out)) {
            info = {};
            info.lyricOrigin = LyricOrigin::QqMusic;
            info.matchedArtist = bundle.artist;
            const auto query = normalizeTitle(title);
            const auto matched = std::find_if(bundle.titles.begin(), bundle.titles.end(),
                [&](const std::wstring& candidate) { return normalizeTitle(candidate) == query; });
            if (matched != bundle.titles.end())
                info.matchedTitle = *matched;
            return true;
        }
    }
    out.clear();
    return false;
}

bool readLocalOrderOverride(const LocalLyricBundle& bundle) {
    if (bundle.orderPath.empty())
        return false;
    try {
        std::ifstream file(bundle.orderPath, std::ios::binary);
        if (!file)
            return false;
        const auto json = nlohmann::json::parse(file, nullptr, false);
        if (json.is_discarded())
            return false;
        const auto it = json.find("sourceOrder");
        if (it == json.end() || !it->is_array() || it->size() != 3)
            return false;
        return (*it)[0] == "manual" && (*it)[1] == "online" && (*it)[2] == "local";
    } catch (...) {
        return false;
    }
}

bool writeLocalOrderOverride(const LocalLyricBundle& bundle, bool onlineFirst) {
    if (bundle.orderPath.empty())
        return false;
    if (!onlineFirst) {
        std::error_code ec;
        std::filesystem::remove(bundle.orderPath, ec);
        return !ec;
    }
    try {
        nlohmann::json order = nlohmann::json::array();
        order.push_back("manual");
        order.push_back("online");
        order.push_back("local");
        nlohmann::json json;
        json["sourceOrder"] = std::move(order);
        std::error_code ec;
        std::filesystem::create_directories(bundle.orderPath.parent_path(), ec);
        if (ec)
            return false;
        std::ofstream file(bundle.orderPath, std::ios::binary | std::ios::trunc);
        if (!file)
            return false;
        file << json.dump(2);
        return file.good();
    } catch (...) {
        return false;
    }
}

} // namespace lyric_detail
