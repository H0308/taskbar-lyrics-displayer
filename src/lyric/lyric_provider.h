#pragma once

#include <cstdint>
#include "lyric/lyric_types.h"
#include "media/smtc_monitor.h"
#include <functional>
#include <memory>
#include <string>
#include <vector>


enum class LyricOrigin { Unknown, QqMusic, Kugou, Netease, Soda, Lrclib };

// 搜索匹配到的歌曲信息（用于歌词下载，也用于封面兜底）
struct SongInfo {
    std::wstring songmid;
    std::wstring albummid;
    std::wstring neteaseSongId;
    LyricOrigin lyricOrigin = LyricOrigin::Unknown;
};

// 每次请求独立交付的结果，不依赖 Provider 内部的可变“当前歌词”。
struct LyricResult {
    bool ok = false;
    std::vector<LyricLine> lines;
    SongInfo info;
    bool fromLocal = false;
    bool fromManual = false;
};

enum class LyricSource {
    Qrc, // QQ 音乐原生逐字歌词
    Krc, // 酷狗逐字歌词
    Lrc, // QQ 音乐整行歌词
    Yrc, // 网易云音乐逐字歌词
    Soda, // 汽水原生歌词（逐字或 LRC，由歌词接口确定）
    Lrclib // LRCLIB 词级／逐行歌词
};

// 手动搜索返回的候选歌曲
struct SearchCandidate {
    std::wstring songmid;   // QQ 歌曲 mid（LRC 候选使用）
    std::wstring songid;    // QQ 数字歌曲 ID（QRC 候选使用）
    std::wstring albummid;
    std::wstring kugouHash; // 酷狗歌曲 hash（KRC 候选使用）
    std::wstring name;
    std::wstring singer;
    int64_t durationMs = 0;
    LyricSource source = LyricSource::Lrc;
    std::wstring neteaseSongId; // 网易云歌曲 ID（YRC 候选使用）
    std::wstring sodaSongId;
    int64_t previewStartMs = 0;
    int64_t previewDurationMs = 0;
    std::wstring album;
    int64_t lrclibId = 0;
    bool lrclibWordSync = false;
};

// 自动匹配优先取原生歌词，随后按酷狗、QQ 音乐、LRCLIB 和剩余来源查找。
class LyricProvider {
public:
    // 所有异步接口均在工作线程回调，调用前已释放内部锁；UI 更新由调用方投递。
    using ReadyCallback = std::function<void(LyricResult)>;
    using SearchCallback = std::function<void(const std::vector<SearchCandidate>&)>;
    using FetchCallback = std::function<void(bool ok, const std::vector<LyricLine>& lines,
                                             const SongInfo& info)>;
    using SongInfoCallback = std::function<void(const SongInfo& info)>;

    LyricProvider();
    ~LyricProvider();

    LyricProvider(const LyricProvider&) = delete;
    LyricProvider& operator=(const LyricProvider&) = delete;

    enum class RequestSource { Default, PreferOnline, PreferLocal, Spotify };

    // 异步请求歌词；优先按版本标注匹配，无合适结果时按匹配分排序。
    // persistOrder 在后台保存单曲的本地/在线顺序标记，不阻塞调用线程。
    void requestAsync(const std::wstring& title, const std::wstring& artist, int64_t durationMs,
                      ReadyCallback cb, RequestSource source = RequestSource::Default,
                      bool persistOrder = false, const std::wstring& album = {});

    // 按网易云歌曲 ID 异步取歌词：优先 YRC，失败后回退网易云 LRC。
    // 曲目元数据用于手动覆盖和原生未命中后的通用匹配。
    void requestNeteaseAsync(const std::wstring& songId, const std::wstring& title,
                             const std::wstring& artist, const std::wstring& album,
                             int64_t durationMs,
                             ReadyCallback cb);

    // 汽水优先搜索原生逐字/LRC，试听片段按原生的试听起点对齐。
    // 联网前合并切歌元数据事件，新请求使旧请求取消；手动覆盖和缓存立即返回。
    // 手动覆盖独立保存，不读取 QQ 本地 QRC。
    void requestSodaAsync(const std::wstring& title, const std::wstring& artist,
                            const std::wstring& album, int64_t durationMs, ReadyCallback cb);

    // 仅按当前元数据查询 QQ songmid/albummid，不读取或替换当前歌词。
    void requestQqSongInfoAsync(const std::wstring& title, const std::wstring& artist,
                                int64_t durationMs, SongInfoCallback cb);

    // 手动搜索查询全部来源，原生候选优先显示。
    void searchCandidatesAsync(const std::wstring& title, const std::wstring& artist,
                               SearchCallback cb, SmtcPlayerType player);

    // 按候选歌曲异步取歌词，不修改当前播放内容。
    void fetchLyricAsync(const SearchCandidate& cand, FetchCallback cb,
                         int64_t playbackDurationMs = 0);

    // 手动选择按播放源和曲目标识保存。
    void setManualOverride(const std::wstring& title, const std::wstring& artist,
                           int64_t durationMs, std::vector<LyricLine> lines, SongInfo info,
                           SmtcPlayerType player, const std::wstring& album);

    // 设置手动歌词的持久化目录；每首歌保存为独立文件，请求歌词时按 key 查找对应文件
    void setManualOverrideDir(const std::wstring& dir);

    // 设置 QQ 音乐本地 QRC 来源；仅影响 QQ 请求，未启用或目录为空时保持原有在线链路。
    void setQqLocalLyricsConfig(bool enabled, const std::wstring& dir);

    // 设置 QQ 版本顺序标记目录；只保存 *_qm.order.json，不保存歌词内容。
    void setQqLyricOrderDir(const std::wstring& dir);

    // 缓存/覆盖键：标题 + 歌手 + 时长（5 秒分桶），区分同名不同版本
    static std::wstring makeKey(const std::wstring& title, const std::wstring& artist,
                                int64_t durationMs);
    static std::wstring makeSpotifyKey(const std::wstring& title, const std::wstring& artist,
                                       const std::wstring& album, int64_t durationMs);
    // 二分定位：最后一个 ms <= positionMs 的行号；无则 -1
    static int findLine(const std::vector<LyricLine>& lines, int64_t positionMs);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
