#include "lyric/custom_local_lyric_index.h"

#include "logging/runtime_logger.h"
#include "lyric/lrc_parser.h"
#include "lyric/lyric_http.h"
#include "lyric/lyric_matching.h"
#include "media/player_source.h"

#include <nlohmann/json.hpp>
#include <taglib/audioproperties.h>
#include <taglib/fileref.h>
#include <taglib/flacfile.h>
#include <taglib/id3v2tag.h>
#include <taglib/mp4file.h>
#include <taglib/mpegfile.h>
#include <taglib/synchronizedlyricsframe.h>
#include <taglib/tag.h>
#include <taglib/tfilestream.h>
#include <taglib/tpropertymap.h>
#include <taglib/unsynchronizedlyricsframe.h>
#include <taglib/wavfile.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <cwctype>
#include <fstream>
#include <system_error>
#include <unordered_set>

namespace lyric_detail {
namespace {

constexpr uintmax_t kMaxLrcFileBytes = 2 * 1024 * 1024;
constexpr int kLocalFuzzyScoreThreshold = 85;
constexpr int64_t kMaxDurationMismatchMs = 15000;
constexpr size_t kMaxManualSearchHits = 30;

bool isFilenameSeparator(wchar_t ch) {
    if (std::iswspace(ch))
        return true;
    if (ch == L'\\' || ch == L'/' || ch == L':' || ch == L'*' || ch == L'?' || ch == L'"' ||
        ch == L'<' || ch == L'>' || ch == L'|' || ch == L'_')
        return true;
    if (ch < 0x80 && !std::iswalnum(ch))
        return true;
    switch (ch) {
    case L'，': case L'。': case L'、': case L'；': case L'：': case L'！':
    case L'？': case L'（': case L'）': case L'【': case L'】': case L'《':
    case L'》': case L'「': case L'」': case L'『': case L'』': case L'—':
    case L'–': case L'…': case L'·': case L'　':
        return true;
    default:
        return false;
    }
}

wchar_t foldFullWidth(wchar_t ch) {
    if (ch == 0x3000)
        return L' ';
    if (ch >= 0xFF01 && ch <= 0xFF5E)
        return static_cast<wchar_t>(ch - 0xFEE0);
    return ch;
}

// 规范化可比对字符串：等价处理 Windows 非法文件名字符、全半角与繁简，
// 但保留括号及版本词，避免 AI 翻唱或 Live 版被错误折叠成原版。
std::wstring normalizeFilenameComparable(const std::wstring& text) {
    const std::wstring simplified = simplifiedChinese(text);
    std::wstring out;
    out.reserve(simplified.size());
    bool prevSpace = true;
    for (wchar_t rawCh : simplified) {
        const wchar_t ch = foldFullWidth(rawCh);
        if (isFilenameSeparator(ch)) {
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

// 去除常见 1~2 位前导音轨编号（如 "01. 晴天"、"02 - 稻香"、"03_花海"），
// 保留 4 位年份或纯数字歌名。
std::wstring stripLeadingTrackNumber(const std::wstring& stem) {
    const std::wstring trimmed = trimW(stem);
    size_t digits = 0;
    while (digits < trimmed.size() && trimmed[digits] >= L'0' && trimmed[digits] <= L'9')
        ++digits;
    if (digits == 0 || digits > 2 || digits >= trimmed.size())
        return trimmed;

    size_t pos = digits;
    while (pos < trimmed.size() && (trimmed[pos] == L' ' || trimmed[pos] == L'\t'))
        ++pos;
    if (pos >= trimmed.size())
        return trimmed;

    const wchar_t sep = trimmed[pos];
    if (sep == L'.' || sep == L'-' || sep == L'–' || sep == L'—' || sep == L'_' || sep == L'、') {
        const std::wstring rest = trimW(trimmed.substr(pos + 1));
        if (!rest.empty())
            return rest;
    }
    return trimmed;
}

void addUniquePair(std::vector<std::pair<std::wstring, std::wstring>>& pairs,
                   const std::wstring& left, const std::wstring& right) {
    const std::wstring a = trimW(left);
    const std::wstring b = trimW(right);
    if (a.empty() || b.empty())
        return;
    for (const auto& existing : pairs) {
        if (existing.first == a && existing.second == b)
            return;
    }
    pairs.emplace_back(a, b);
}

void collectSplitPairs(const std::wstring& text,
                       std::vector<std::pair<std::wstring, std::wstring>>& pairs) {
    bool foundSpaced = false;
    for (const wchar_t* sep : {L" - ", L" – ", L" — "}) {
        const size_t sepLen = std::wcslen(sep);
        const size_t first = text.find(sep);
        if (first != std::wstring::npos) {
            foundSpaced = true;
            addUniquePair(pairs, text.substr(0, first), text.substr(first + sepLen));
            const size_t last = text.rfind(sep);
            if (last != first)
                addUniquePair(pairs, text.substr(0, last), text.substr(last + sepLen));
        }
    }
    if (!foundSpaced) {
        const size_t dash = text.find(L'-');
        if (dash != std::wstring::npos)
            addUniquePair(pairs, text.substr(0, dash), text.substr(dash + 1));
    }
}

bool sameVersionTags(std::vector<std::wstring> left, std::vector<std::wstring> right) {
    std::sort(left.begin(), left.end());
    std::sort(right.begin(), right.end());
    return left == right;
}

bool isUsefulArtistText(const std::wstring& value) {
    const std::wstring lower = lowerW(trimW(value));
    return !lower.empty() && lower != L"unknown artist" && lower != L"未知歌手";
}

bool artistMatches(const std::wstring& queryArtist, const std::wstring& candidateArtist) {
    if (!isUsefulArtistText(queryArtist) || !isUsefulArtistText(candidateArtist))
        return false;
    if (singerSimilarity(queryArtist, candidateArtist) >= 80)
        return true;
    // 文件名常把歌手分隔符写成下划线（如 "周杰伦_费玉清"），还原为空格或斜杠再做一次比对。
    std::wstring replaced = candidateArtist;
    replaceAllW(replaced, L"_", L"/");
    return singerSimilarity(queryArtist, replaced) >= 80;
}

bool readFileBytesLimited(const std::filesystem::path& path, std::string& out) {
    out.clear();
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    if (ec || size == 0 || size > kMaxLrcFileBytes)
        return false;
    std::ifstream file(path, std::ios::binary);
    if (!file)
        return false;
    out.resize(static_cast<size_t>(size));
    file.read(out.data(), static_cast<std::streamsize>(size));
    return file.good() || file.eof();
}

std::wstring sanitizeOrderName(const std::wstring& s) {
    std::wstring out;
    out.reserve(s.size());
    for (wchar_t ch : s) {
        if (ch == L'\\' || ch == L'/' || ch == L':' || ch == L'*' || ch == L'?' ||
            ch == L'"' || ch == L'<' || ch == L'>' || ch == L'|' || ch < 0x20)
            out.push_back(L'_');
        else
            out.push_back(ch);
    }
    while (!out.empty() && (out.back() == L' ' || out.back() == L'.'))
        out.pop_back();
    if (out.size() > 50)
        out.resize(50);
    return out.empty() ? L"lyric" : out;
}

uint64_t fnv1a64(const std::string& s) {
    uint64_t h = 14695981039346656037ull;
    for (unsigned char c : s) {
        h ^= c;
        h *= 1099511628211ull;
    }
    return h;
}

TagLib::ID3v2::Tag* findId3v2Tag(TagLib::File* file) {
    if (!file)
        return nullptr;
    if (auto* mpeg = dynamic_cast<TagLib::MPEG::File*>(file))
        return mpeg->ID3v2Tag(false);
    if (auto* flac = dynamic_cast<TagLib::FLAC::File*>(file))
        return flac->ID3v2Tag(false);
    if (auto* wav = dynamic_cast<TagLib::RIFF::WAV::File*>(file))
        return wav->hasID3v2Tag() ? wav->ID3v2Tag() : nullptr;
    return nullptr;
}

bool extractId3v2Sylt(TagLib::ID3v2::Tag* id3v2, std::vector<LyricLine>& out) {
    out.clear();
    if (!id3v2)
        return false;
    const auto frames = id3v2->frameList("SYLT");
    for (auto* rawFrame : frames) {
        const auto* sylt = dynamic_cast<const TagLib::ID3v2::SynchronizedLyricsFrame*>(rawFrame);
        if (!sylt ||
            sylt->timestampFormat() != TagLib::ID3v2::SynchronizedLyricsFrame::AbsoluteMilliseconds)
            continue;
        std::vector<LyricLine> parsed;
        for (const auto& entry : sylt->synchedText()) {
            std::wstring text = trimW(entry.text.toWString());
            if (text.empty())
                continue;
            parsed.push_back({static_cast<int64_t>(entry.time), std::move(text)});
        }
        if (parsed.empty())
            continue;
        std::stable_sort(parsed.begin(), parsed.end(),
                         [](const LyricLine& a, const LyricLine& b) { return a.ms < b.ms; });
        for (auto& line : parsed) {
            if (!out.empty() && out.back().ms == line.ms) {
                if (line.text != out.back().text && out.back().translation.empty())
                    out.back().translation = std::move(line.text);
                continue;
            }
            out.push_back(std::move(line));
        }
        if (!out.empty())
            return true;
    }
    return false;
}

bool tryParseTagLibLyricString(const TagLib::String& text, ParsedLrcDocument& docOut) {
    if (text.isEmpty())
        return false;
    docOut = parseLocalLrcUtf8(text.to8Bit(true));
    return !docOut.lines.empty();
}

bool extractAudioEmbeddedDocument(TagLib::File* file, ParsedLrcDocument& docOut) {
    docOut = {};
    if (!file)
        return false;

    const TagLib::PropertyMap props = file->properties();

    auto tryPropertyKey = [&](const char* keyName) -> bool {
        if (!props.contains(keyName))
            return false;
        for (const auto& val : props[keyName]) {
            if (tryParseTagLibLyricString(val, docOut))
                return true;
        }
        return false;
    };

    // 优先读取显式同步歌词字段。
    if (tryPropertyKey("SYNCEDLYRICS"))
        return true;

    // 次优先尝试 ID3v2 SYLT 二进制同步歌词帧。
    if (auto* id3v2 = findId3v2Tag(file)) {
        if (extractId3v2Sylt(id3v2, docOut.lines))
            return true;
    }

    // 读取通用 LYRICS / UNSYNCEDLYRICS / WM/LYRICS（内部携带 LRC 时间戳时视为有效同步歌词）。
    if (tryPropertyKey("LYRICS"))
        return true;
    for (const auto& entry : props) {
        const std::string keyUpper = entry.first.upper().to8Bit(true);
        if (keyUpper.rfind("LYRICS:", 0) == 0) {
            for (const auto& val : entry.second) {
                if (tryParseTagLibLyricString(val, docOut))
                    return true;
            }
        }
    }
    if (tryPropertyKey("UNSYNCEDLYRICS") || tryPropertyKey("WM/LYRICS"))
        return true;

    // 兜底直接检查 ID3v2 USLT 帧与 MP4 ©lyr 原子。
    if (auto* id3v2 = findId3v2Tag(file)) {
        for (auto* rawFrame : id3v2->frameList("USLT")) {
            const auto* uslt =
                dynamic_cast<const TagLib::ID3v2::UnsynchronizedLyricsFrame*>(rawFrame);
            if (uslt && tryParseTagLibLyricString(uslt->text(), docOut))
                return true;
        }
    }
    if (auto* mp4 = dynamic_cast<TagLib::MP4::File*>(file)) {
        if (const auto* tag = mp4->tag(); tag && tag->contains("\xa9lyr")) {
            for (const auto& val : tag->item("\xa9lyr").toStringList()) {
                if (tryParseTagLibLyricString(val, docOut))
                    return true;
            }
        }
    }
    return false;
}

struct RankedEntry {
    size_t index = 0;
    int score = 0;
    int tier = 0; // 1 精确双字段, 2 精确单标题/无歌手, 3 版本感知模糊
    std::wstring inferredArtist;
};

} // namespace

bool isSupportedLocalAudioExtension(const std::wstring& lowerExt) {
    static constexpr const wchar_t* kAudioExtensions[] = {
        L".mp3", L".flac", L".m4a", L".mp4", L".ogg", L".oga",
        L".opus", L".wma", L".wav", L".ape", L".wv",
    };
    for (const wchar_t* ext : kAudioExtensions) {
        if (lowerExt == ext)
            return true;
    }
    return false;
}

std::shared_ptr<CustomLocalLyricIndex> buildCustomLocalLyricIndex(const std::wstring& root) {
    auto index = std::make_shared<CustomLocalLyricIndex>();
    try {
        const std::filesystem::path rootPath(root);
        std::error_code ec;
        if (!std::filesystem::is_directory(rootPath, ec) || ec)
            return index;
        index->rootDir = rootPath;

        // 按设计约定仅扫描单层目录，不递归深层子目录。
        const auto options = std::filesystem::directory_options::skip_permission_denied;
        std::filesystem::directory_iterator it(rootPath, options, ec);
        const std::filesystem::directory_iterator end;
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

            const auto& path = it->path();
            const std::wstring ext = lowerW(path.extension().wstring());
            CustomLocalKind kind = CustomLocalKind::LrcFile;
            if (ext == L".lrc") {
                kind = CustomLocalKind::LrcFile;
            } else if (isSupportedLocalAudioExtension(ext)) {
                kind = CustomLocalKind::AudioEmbedded;
            } else {
                continue;
            }

            CustomLocalEntry entry;
            entry.path = path;
            entry.kind = kind;
            entry.stem = path.stem().wstring();
            entry.cleanedStem = stripLeadingTrackNumber(entry.stem);
            entry.normalizedStem = normalizeFilenameComparable(entry.stem);
            entry.normalizedCleaned = normalizeFilenameComparable(entry.cleanedStem);
            collectSplitPairs(entry.stem, entry.splitPairs);
            if (entry.cleanedStem != entry.stem)
                collectSplitPairs(entry.cleanedStem, entry.splitPairs);
            if (entry.normalizedStem.empty())
                continue;
            index->entries.push_back(std::move(entry));
        }

        // 同基名优先排外挂 .lrc，再排音频文件，保证查找顺序确定。
        std::sort(index->entries.begin(), index->entries.end(),
                  [](const CustomLocalEntry& a, const CustomLocalEntry& b) {
                      const std::wstring la = lowerW(a.stem);
                      const std::wstring lb = lowerW(b.stem);
                      if (la != lb)
                          return la < lb;
                      if (a.kind != b.kind)
                          return a.kind == CustomLocalKind::LrcFile;
                      return a.path.wstring() < b.path.wstring();
                  });
    } catch (...) {
        index->entries.clear();
    }
    return index;
}

bool loadCustomLocalLyricFile(const std::filesystem::path& path, CustomLocalKind kind,
                              std::vector<LyricLine>& out, SongInfo& info,
                              std::wstring* fileMetadataArtist) {
    out.clear();
    info = {};
    if (fileMetadataArtist)
        fileMetadataArtist->clear();

    if (kind == CustomLocalKind::LrcFile) {
        std::string rawBytes;
        if (!readFileBytesLimited(path, rawBytes))
            return false;
        ParsedLrcDocument doc = parseLocalLrcBytes(rawBytes);
        if (doc.lines.empty())
            return false;
        out = std::move(doc.lines);
        info.lyricOrigin = LyricOrigin::LocalLrc;
        info.matchedTitle = !doc.title.empty() ? doc.title : path.stem().wstring();
        info.matchedArtist = doc.artist;
        if (doc.hasOffset) {
            info.timingOffsetMs = doc.appliedOffsetMs;
            info.timingOffsetKnown = true;
        }
        if (fileMetadataArtist)
            *fileMetadataArtist = doc.artist;
        return true;
    }

    try {
        // 以只读模式打开文件流，避免与正在播放该音频的播放器冲突；stream 必须晚于 ref 析构。
        TagLib::FileStream stream(path.c_str(), true);
        if (!stream.isOpen())
            return false;
        TagLib::FileRef ref(&stream, true, TagLib::AudioProperties::Fast);
        if (ref.isNull() || !ref.file() || !ref.file()->isValid())
            return false;

        ParsedLrcDocument doc;
        if (!extractAudioEmbeddedDocument(ref.file(), doc) || doc.lines.empty())
            return false;

        std::wstring tagTitle;
        std::wstring tagArtist;
        if (const auto* tag = ref.tag()) {
            tagTitle = trimW(tag->title().toWString());
            tagArtist = trimW(tag->artist().toWString());
        }
        int64_t durationMs = 0;
        if (const auto* props = ref.audioProperties())
            durationMs = static_cast<int64_t>(props->lengthInMilliseconds());

        out = std::move(doc.lines);
        info.lyricOrigin = LyricOrigin::LocalAudio;
        info.matchedTitle =
            !tagTitle.empty() ? tagTitle : (!doc.title.empty() ? doc.title : path.stem().wstring());
        info.matchedArtist = !tagArtist.empty() ? tagArtist : doc.artist;
        info.matchedDurationMs = durationMs;
        if (doc.hasOffset) {
            info.timingOffsetMs = doc.appliedOffsetMs;
            info.timingOffsetKnown = true;
        }
        if (fileMetadataArtist)
            *fileMetadataArtist = info.matchedArtist;
        return true;
    } catch (...) {
        out.clear();
        info = {};
        return false;
    }
}

bool lookupCustomLocalLyric(const CustomLocalLyricIndex& index, const std::wstring& title,
                            const std::wstring& artist, int64_t durationMs,
                            std::vector<LyricLine>& out, SongInfo& info) {
    out.clear();
    info = {};
    const std::wstring strictQueryTitle = normalizeFilenameComparable(title);
    if (strictQueryTitle.empty() || index.entries.empty())
        return false;

    const bool hasArtist = isUsefulArtistText(artist);
    const std::vector<std::wstring> queryVersionTags = extractVersionTags(title);
    const MatchQuery fuzzyQuery{title, artist, 0, queryVersionTags};

    std::vector<RankedEntry> ranked;
    for (size_t i = 0; i < index.entries.size(); ++i) {
        const auto& entry = index.entries[i];
        const int lrcBonus = entry.kind == CustomLocalKind::LrcFile ? 10 : 0;

        int bestScore = 0;
        int bestTier = 0;
        std::wstring bestInferredArtist;

        auto consider = [&](int score, int tier, const std::wstring& inferredArtist) {
            if (score > bestScore) {
                bestScore = score;
                bestTier = tier;
                bestInferredArtist = inferredArtist;
            }
        };

        // 第一层：精确双字段（保留括号与版本词，兼容“歌手 - 歌名”与“歌名 - 歌手”）。
        for (const auto& [partA, partB] : entry.splitPairs) {
            const std::wstring normA = normalizeFilenameComparable(partA);
            const std::wstring normB = normalizeFilenameComparable(partB);
            if (hasArtist) {
                if (normB == strictQueryTitle && artistMatches(artist, partA))
                    consider(215 + lrcBonus, 1, partA);
                if (normA == strictQueryTitle && artistMatches(artist, partB))
                    consider(210 + lrcBonus, 1, partB);
            } else {
                if (normB == strictQueryTitle)
                    consider(165 + lrcBonus, 2, partA);
                if (normA == strictQueryTitle)
                    consider(160 + lrcBonus, 2, partB);
            }
        }

        // 第二层：纯歌名精确匹配（如 "晴天 (AI翻唱).lrc" 或 "01. 晴天.flac"）。
        if (bestTier == 0 && (entry.normalizedStem == strictQueryTitle ||
                              entry.normalizedCleaned == strictQueryTitle)) {
            consider(170 + lrcBonus, 2, {});
        }

        // 第三层：版本感知模糊匹配（复用 lyric_matching，强制版本标签一致）。
        if (bestTier == 0) {
            for (const auto& [partA, partB] : entry.splitPairs) {
                const int s1 = scoreCandidate(fuzzyQuery, partB, partA, 0);
                if (s1 >= kLocalFuzzyScoreThreshold)
                    consider(s1 + (entry.kind == CustomLocalKind::LrcFile ? 8 : 0), 3, partA);
                const int s2 = scoreCandidate(fuzzyQuery, partA, partB, 0);
                if (s2 >= kLocalFuzzyScoreThreshold)
                    consider(s2 + (entry.kind == CustomLocalKind::LrcFile ? 8 : 0), 3, partB);
            }
            if (!hasArtist && bestTier == 0 &&
                sameVersionTags(queryVersionTags, extractVersionTags(entry.cleanedStem))) {
                const int sim = titleSimilarity(title, entry.cleanedStem);
                if (sim >= 92)
                    consider(sim + (entry.kind == CustomLocalKind::LrcFile ? 8 : 0), 3, {});
            }
        }

        if (bestScore > 0)
            ranked.push_back({i, bestScore, bestTier, std::move(bestInferredArtist)});
    }

    std::sort(ranked.begin(), ranked.end(), [&index](const RankedEntry& a, const RankedEntry& b) {
        if (a.score != b.score)
            return a.score > b.score;
        const auto& ea = index.entries[a.index];
        const auto& eb = index.entries[b.index];
        if (ea.kind != eb.kind)
            return ea.kind == CustomLocalKind::LrcFile;
        return ea.path.wstring() < eb.path.wstring();
    });

    for (const auto& cand : ranked) {
        if (isLyricShuttingDown())
            return false;
        const auto& entry = index.entries[cand.index];
        std::vector<LyricLine> candidateLines;
        SongInfo candidateInfo;
        std::wstring fileArtist;
        if (!loadCustomLocalLyricFile(entry.path, entry.kind, candidateLines, candidateInfo,
                                      &fileArtist)) {
            continue;
        }

        // 二、三档命中后用文件内部标签二次把关，避免同名不同歌手或不同版本误匹配。
        if (cand.tier >= 2 && hasArtist && isUsefulArtistText(fileArtist) &&
            !artistMatches(artist, fileArtist) && singerSimilarity(artist, fileArtist) == 0) {
            continue;
        }
        if (durationMs > 0 && candidateInfo.matchedDurationMs > 0 &&
            std::llabs(durationMs - candidateInfo.matchedDurationMs) > kMaxDurationMismatchMs) {
            continue;
        }

        if (candidateInfo.matchedArtist.empty() && !cand.inferredArtist.empty())
            candidateInfo.matchedArtist = cand.inferredArtist;

        out = std::move(candidateLines);
        info = std::move(candidateInfo);
        return true;
    }

    return false;
}

std::vector<SearchCandidate> searchCustomLocalCandidates(const CustomLocalLyricIndex& index,
                                                         const std::wstring& title,
                                                         const std::wstring& artist) {
    std::vector<SearchCandidate> out;
    if (index.entries.empty())
        return out;

    const std::wstring normTitle = normalizeFilenameComparable(title);
    const std::wstring normArtist = normalizeFilenameComparable(artist);
    const bool emptyQuery = normTitle.empty() && normArtist.empty();

    struct ScoredHit {
        size_t index = 0;
        int score = 0;
        std::wstring name;
        std::wstring singer;
    };
    std::vector<ScoredHit> hits;

    for (size_t i = 0; i < index.entries.size(); ++i) {
        const auto& entry = index.entries[i];
        int score = 0;
        std::wstring displayName = entry.cleanedStem;
        std::wstring displaySinger;

        if (!entry.splitPairs.empty()) {
            // 默认按 “歌手 - 歌名” 展示；若方向相反且匹配度更高，下面会替换。
            displaySinger = entry.splitPairs.front().first;
            displayName = entry.splitPairs.front().second;
        }

        if (emptyQuery) {
            score = 50;
        } else {
            if (!normTitle.empty()) {
                if (entry.normalizedStem == normTitle || entry.normalizedCleaned == normTitle)
                    score = std::max(score, 120);
                else if (entry.normalizedStem.find(normTitle) != std::wstring::npos ||
                         entry.normalizedCleaned.find(normTitle) != std::wstring::npos)
                    score = std::max(score, 95);
                const int sim = titleSimilarity(title, entry.cleanedStem);
                if (sim >= 55)
                    score = std::max(score, sim);
            }
            if (!normArtist.empty() &&
                entry.normalizedStem.find(normArtist) != std::wstring::npos) {
                score = std::max(score, normTitle.empty() ? 85 : score + 15);
            }

            for (const auto& [partA, partB] : entry.splitPairs) {
                const std::wstring nA = normalizeFilenameComparable(partA);
                const std::wstring nB = normalizeFilenameComparable(partB);
                int sAB = 0;
                if (!normTitle.empty()) {
                    if (nB == normTitle)
                        sAB = 125;
                    else if (nB.find(normTitle) != std::wstring::npos)
                        sAB = 100;
                    else
                        sAB = titleSimilarity(title, partB);
                }
                if (!normArtist.empty() &&
                    (artistMatches(artist, partA) || nA.find(normArtist) != std::wstring::npos)) {
                    sAB += normTitle.empty() ? 90 : 30;
                }
                if (sAB > score && sAB >= 55) {
                    score = sAB;
                    displaySinger = partA;
                    displayName = partB;
                }

                int sBA = 0;
                if (!normTitle.empty()) {
                    if (nA == normTitle)
                        sBA = 122;
                    else if (nA.find(normTitle) != std::wstring::npos)
                        sBA = 98;
                    else
                        sBA = titleSimilarity(title, partA);
                }
                if (!normArtist.empty() &&
                    (artistMatches(artist, partB) || nB.find(normArtist) != std::wstring::npos)) {
                    sBA += normTitle.empty() ? 90 : 30;
                }
                if (sBA > score && sBA >= 55) {
                    score = sBA;
                    displaySinger = partB;
                    displayName = partA;
                }
            }
        }

        if (score >= 50) {
            if (displaySinger.empty()) {
                displaySinger = entry.kind == CustomLocalKind::LrcFile
                                    ? L"本地 LRC"
                                    : lowerW(entry.path.extension().wstring());
            }
            hits.push_back({i, score, std::move(displayName), std::move(displaySinger)});
        }
    }

    std::sort(hits.begin(), hits.end(), [&index](const ScoredHit& a, const ScoredHit& b) {
        if (a.score != b.score)
            return a.score > b.score;
        const auto& ea = index.entries[a.index];
        const auto& eb = index.entries[b.index];
        if (ea.kind != eb.kind)
            return ea.kind == CustomLocalKind::LrcFile;
        return ea.path.wstring() < eb.path.wstring();
    });

    const size_t limit = std::min(hits.size(), kMaxManualSearchHits);
    out.reserve(limit);
    for (size_t i = 0; i < limit; ++i) {
        const auto& hit = hits[i];
        const auto& entry = index.entries[hit.index];
        SearchCandidate cand;
        cand.name = hit.name;
        cand.singer = hit.singer;
        cand.source = entry.kind == CustomLocalKind::LrcFile ? LyricSource::LocalLrc
                                                             : LyricSource::LocalAudio;
        cand.localPath = entry.path.wstring();
        out.push_back(std::move(cand));
    }
    return out;
}

std::filesystem::path customLocalOrderPath(const std::wstring& orderDir, const std::wstring& key,
                                           const std::wstring& title, const std::wstring& artist,
                                           SmtcPlayerType player) {
    if (orderDir.empty() || key.empty())
        return {};
    const std::wstring readable = sanitizeOrderName(title + L" - " + artist);
    wchar_t hex[17];
    std::swprintf(hex, 17, L"%016llx",
                  static_cast<unsigned long long>(fnv1a64(toUtf8(key))));
    const wchar_t* prefix = player == SmtcPlayerType::Spotify      ? L"spotify-"
                            : player == SmtcPlayerType::SaltPlayer ? L"saltplayer-"
                            : player == SmtcPlayerType::Generic    ? L"generic-"
                            : player == SmtcPlayerType::Soda       ? L"soda-"
                            : player == SmtcPlayerType::NetEase    ? L"netease-"
                                                                   : L"qq-";
    return std::filesystem::path(orderDir) /
           (std::wstring(prefix) + readable + L"-" + hex + L".order.json");
}

bool readCustomLocalOrderOverride(const std::filesystem::path& orderPath) {
    if (orderPath.empty())
        return false;
    try {
        std::ifstream file(orderPath, std::ios::binary);
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

bool writeCustomLocalOrderOverride(const std::filesystem::path& orderPath, bool onlineFirst) {
    if (orderPath.empty())
        return false;
    if (!onlineFirst) {
        std::error_code ec;
        std::filesystem::remove(orderPath, ec);
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
        std::filesystem::create_directories(orderPath.parent_path(), ec);
        if (ec)
            return false;
        std::ofstream file(orderPath, std::ios::binary | std::ios::trunc);
        if (!file)
            return false;
        file << json.dump(2);
        return file.good();
    } catch (...) {
        return false;
    }
}

std::wstring builtinPlayerRuleSourceId(SmtcPlayerType player) {
    switch (player) {
    case SmtcPlayerType::QQMusic:
        return L"builtin:qqmusic";
    case SmtcPlayerType::NetEase:
        return L"builtin:netease";
    case SmtcPlayerType::Soda:
        return L"builtin:soda";
    case SmtcPlayerType::Spotify:
        return L"builtin:spotify";
    case SmtcPlayerType::SaltPlayer:
        return L"builtin:saltplayer";
    default:
        return {};
    }
}

SmtcPlayerType parseBuiltinPlayerRuleSourceId(const std::wstring& sourceId) {
    if (_wcsicmp(sourceId.c_str(), L"builtin:qqmusic") == 0)
        return SmtcPlayerType::QQMusic;
    if (_wcsicmp(sourceId.c_str(), L"builtin:netease") == 0)
        return SmtcPlayerType::NetEase;
    if (_wcsicmp(sourceId.c_str(), L"builtin:soda") == 0)
        return SmtcPlayerType::Soda;
    if (_wcsicmp(sourceId.c_str(), L"builtin:spotify") == 0)
        return SmtcPlayerType::Spotify;
    if (_wcsicmp(sourceId.c_str(), L"builtin:saltplayer") == 0)
        return SmtcPlayerType::SaltPlayer;
    return SmtcPlayerType::Unknown;
}

GenericSmtcAppRule makeBuiltinPlayerRule(SmtcPlayerType player) {
    GenericSmtcAppRule rule;
    rule.sourceId = builtinPlayerRuleSourceId(player);
    if (const auto* def = player_source::find(player)) {
        rule.displayName = def->displayName;
        if (!def->processNames.empty())
            rule.processName = def->processNames.front();
    }
    return rule;
}

bool matchesCustomLocalPlayerRules(SmtcPlayerType player, const std::wstring& sourceId,
                                   const AudioSessionTarget& target,
                                   const std::vector<GenericSmtcAppRule>& rules) {
    const auto* builtinDef = player_source::find(player);
    for (const auto& rule : rules) {
        const SmtcPlayerType ruleBuiltin = parseBuiltinPlayerRuleSourceId(rule.sourceId);
        if (ruleBuiltin != SmtcPlayerType::Unknown) {
            if (player == ruleBuiltin)
                return true;
            continue;
        }
        if (!rule.sourceId.empty() && !sourceId.empty() &&
            _wcsicmp(sourceId.c_str(), rule.sourceId.c_str()) == 0) {
            return true;
        }
        if (!rule.packageFamilyName.empty() && !target.packageFamilyName.empty() &&
            _wcsicmp(target.packageFamilyName.c_str(), rule.packageFamilyName.c_str()) == 0) {
            return true;
        }
        if (!rule.processName.empty()) {
            for (const auto& proc : target.processNames) {
                if (_wcsicmp(proc.c_str(), rule.processName.c_str()) == 0)
                    return true;
            }
            if (builtinDef) {
                for (const wchar_t* proc : builtinDef->processNames) {
                    if (_wcsicmp(proc, rule.processName.c_str()) == 0)
                        return true;
                }
            }
        }
    }
    return false;
}

} // namespace lyric_detail
