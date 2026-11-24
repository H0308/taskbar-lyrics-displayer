#pragma once

#include "lyric/lyric_provider.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace lyric_detail {

enum class CustomLocalKind {
    LrcFile,
    AudioEmbedded,
};

struct CustomLocalEntry {
    std::filesystem::path path;
    CustomLocalKind kind = CustomLocalKind::LrcFile;
    std::wstring stem;
    std::wstring cleanedStem;
    std::wstring normalizedStem;
    std::wstring normalizedCleaned;
    // 文件名按常见分隔符拆出的候选 (A, B) 对，供双向匹配（歌手 - 歌名 / 歌名 - 歌手）。
    std::vector<std::pair<std::wstring, std::wstring>> splitPairs;
};

struct CustomLocalLyricIndex {
    std::filesystem::path rootDir;
    std::vector<CustomLocalEntry> entries;
};

// 判断扩展名是否为支持内嵌歌词提取的音频格式（小写含点，如 L".flac"）。
bool isSupportedLocalAudioExtension(const std::wstring& lowerExt);

// 仅扫描目标目录的直接子文件（不递归深层目录），建索引阶段不打开音频文件。
std::shared_ptr<CustomLocalLyricIndex> buildCustomLocalLyricIndex(const std::wstring& root);

// 按需加载单个 .lrc 文件或音频内嵌同步歌词；纯静态无时间轴歌词返回 false。
bool loadCustomLocalLyricFile(const std::filesystem::path& path, CustomLocalKind kind,
                              std::vector<LyricLine>& out, SongInfo& info,
                              std::wstring* fileMetadataArtist = nullptr);

// 自动匹配当前曲目：精确优先、保留版本标注、外挂 LRC 优先于音频内嵌，并做元数据防串词校验。
bool lookupCustomLocalLyric(const CustomLocalLyricIndex& index, const std::wstring& title,
                            const std::wstring& artist, int64_t durationMs,
                            std::vector<LyricLine>& out, SongInfo& info);

// 供手动搜索弹窗展示本地目录中的候选文件（同时包含 .lrc 与音频文件）。
std::vector<SearchCandidate> searchCustomLocalCandidates(const CustomLocalLyricIndex& index,
                                                         const std::wstring& title,
                                                         const std::wstring& artist);

// 单曲本地/在线顺序标记读写（按平台与曲目键哈希分文件持久化，与 QQ 顺序文件隔离）。
std::filesystem::path customLocalOrderPath(const std::wstring& orderDir, const std::wstring& key,
                                           const std::wstring& title, const std::wstring& artist,
                                           SmtcPlayerType player);
bool readCustomLocalOrderOverride(const std::filesystem::path& orderPath);
bool writeCustomLocalOrderOverride(const std::filesystem::path& orderPath, bool onlineFirst);

// 内置播放器规则标识（跨桌面版与商店版统一匹配）及指定播放器生效范围判定。
std::wstring builtinPlayerRuleSourceId(SmtcPlayerType player);
SmtcPlayerType parseBuiltinPlayerRuleSourceId(const std::wstring& sourceId);
GenericSmtcAppRule makeBuiltinPlayerRule(SmtcPlayerType player);
bool matchesCustomLocalPlayerRules(SmtcPlayerType player, const std::wstring& sourceId,
                                   const AudioSessionTarget& target,
                                   const std::vector<GenericSmtcAppRule>& rules);

} // namespace lyric_detail
