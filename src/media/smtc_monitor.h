#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

enum class PlaybackStatus { Stopped, Playing, Paused, Other };

enum class SmtcPlayerType { Unknown, QQMusic, NetEase, Soda, Spotify };

// SMTC 统一快照：锚点进度 + 本机时间，渲染侧据此插值
struct SmtcSnapshot {
    SmtcPlayerType player = SmtcPlayerType::Unknown;
    std::wstring title;
    std::wstring artist;
    std::wstring album;
    std::wstring albumArtist;
    std::wstring neteaseSongId;          // 网易云增强会话通过 Genres 传递的歌曲 ID
    std::wstring sourceAppUserModelId;   // 会话来源标识
    bool enhancedSmtc = false;           // 是否为带歌曲 ID/进度的增强会话
    int64_t durationMs = 0;
    int64_t positionMs = 0;   // 锚点进度（snapshot() 返回时已按播放状态插值）
    int64_t anchorUtcMs = 0;  // 锚点对应的本机系统时间
    // QQ 切歌窗口内时间线仍是旧歌残留：durationMs/positionMs 不可信，
    // 此时不应按它们发起歌词请求（时长过滤会把正确候选全部淘汰）。
    // 仅 QQ 适配器会置位。
    bool timelineStale = false;
    PlaybackStatus status = PlaybackStatus::Stopped;
    bool sessionAlive = false; // 当前是否存在可消费的已适配播放器会话
    std::shared_ptr<const std::vector<uint8_t>> thumbnail; // 专辑封面图片字节（共享避免快照拷贝）
    bool canPrev = false;
    bool canPlayPause = false;
    bool canNext = false;
    bool canRepeat = false;
    // 原始 SMTC 循环维度：0 关闭、1 单曲循环、2 列表循环（AutoRepeatMode）。
    int repeatMode = 0;
    bool shuffle = false; // 随机播放是独立开关（IsShuffleActive），不在 AutoRepeatMode 里
    // 合并成用户视角的四种播放模式：0 顺序、1 列表循环、2 单曲循环、3 随机播放。
    int playMode() const {
        if (shuffle)
            return 3;
        if (repeatMode == 2)
            return 1;
        if (repeatMode == 1)
            return 2;
        return 0;
    }
};

// 监听 QQ、汽水和 Spotify 的系统媒体会话及 Genres 中带 NCM-{ID} 的网易云增强会话。
class SmtcMonitor {
public:
    using ChangeCallback = std::function<void()>; // 快照变化后触发（后台刷新/封面线程）

    SmtcMonitor();
    ~SmtcMonitor();

    SmtcMonitor(const SmtcMonitor&) = delete;
    SmtcMonitor& operator=(const SmtcMonitor&) = delete;

    void start(ChangeCallback onChange);
    SmtcSnapshot snapshot() const; // 当前快照（Playing 时进度已插值）

    // 播放控制（无会话或调用失败时不改变 UI，并记录诊断日志）
    void playPause();
    void skipNext();
    void skipPrevious();
    // 切换循环模式：关闭 → 列表 → 单曲 → 关闭（由播放器通过 SMTC 执行）。
    void cycleRepeatMode();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
