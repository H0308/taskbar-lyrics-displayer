#pragma once

#include "media/audio_target.h"
#include "media/generic_smtc_filter.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

enum class PlaybackStatus { Stopped, Playing, Paused, Other };

// Generic：白名单外任意 SMTC 会话的兜底类型，走通用采样与元数据歌词路径。
enum class SmtcPlayerType { Unknown, QQMusic, NetEase, Soda, Spotify, SaltPlayer, Generic };

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
    bool canSwitchSource = false;
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
    static constexpr int nextPlayMode(int mode) noexcept {
        return mode == 2 ? 3 : (mode == 3 ? 0 : 2);
    }
};

struct SmtcPlaybackSource {
    std::wstring appUserModelId;
    SmtcPlayerType player;
};

struct SmtcSourceSelection {
    std::vector<SmtcPlaybackSource> available;
    std::wstring preferredAppUserModelId; // 空值表示自动选择
};

// 监听受支持的系统媒体会话。
class SmtcMonitor {
public:
    using ChangeCallback = std::function<void()>; // 快照变化后触发（后台刷新/封面线程）
    using SourceTargetResolver = std::function<AudioSessionTarget(const std::wstring&)>;

    SmtcMonitor();
    ~SmtcMonitor();

    SmtcMonitor(const SmtcMonitor&) = delete;
    SmtcMonitor& operator=(const SmtcMonitor&) = delete;

    void start(ChangeCallback onChange);
    // 注入来源标识到进程/包族的解析器，用于通用 SMTC 规则按进程名与包族跨 AUMID 匹配。
    void setSourceTargetResolver(SourceTargetResolver resolver);
    // 更新通用 SMTC 开关与白名单/黑名单过滤配置（运行期实时生效，无需重启）。
    void setGenericSmtcFilter(GenericSmtcFilterConfig config);
    // 仅开关白名单外通用 SMTC 会话识别的便捷入口。
    void setGenericSmtcEnabled(bool enabled);
    // 返回运行期间检测到的所有非内置 SMTC 会话来源标识（去重、按最近活跃优先）。
    std::vector<std::wstring> observedGenericSources() const;
    SmtcSourceSelection sourceSelection() const;
    void selectSource(std::wstring appUserModelId); // 传空值恢复自动选择
    void cycleSource(int steps);
    SmtcSnapshot snapshot() const; // 当前快照（Playing 时进度已插值）

    // 播放控制（无会话或调用失败时不改变 UI，并记录诊断日志）
    void playPause();
    void skipNext();
    void skipPrevious();
    // 切换播放模式：顺序播放 → 单曲循环 → 随机播放 → 顺序播放（由播放器通过 SMTC 执行）。
    void cycleRepeatMode();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
