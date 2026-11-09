#pragma once

#include "media/smtc/smtc_common.h"

namespace smtc {

// 汽水时间线只处理已读取的采样；会话访问仍由适配器负责。
class SodaMusicTimeline {
public:
    void initialize(SmtcSnapshot& snapshot, int64_t nowMs) {
        initialized_ = true;
        sampledPositionMs_ = snapshot.positionMs;
        sampledAnchorMs_ = snapshot.anchorUtcMs;
        resetPresentedPosition_ = true;
        // 旧采样的更新时间仍是有效锚点，中途接入时不能按年龄重新锚定。
        if (snapshot.anchorUtcMs <= 0)
            snapshot.anchorUtcMs = nowMs;
        snapshot.timelineStale = false;
    }

    void updatePlayback(SmtcSnapshot& snapshot, PlaybackStatus status, int64_t nowMs) {
        if (status != snapshot.status) {
            // 暂停通知可能早于新采样，先按旧状态推进到事件时刻。
            advancePlayingPosition(snapshot, nowMs, true);
            snapshot.anchorUtcMs = nowMs;
        }
        snapshot.status = status;
    }

    void updateSample(SmtcSnapshot& snapshot, int64_t position, int64_t anchor,
                      int64_t duration, int64_t nowMs) {
        SmtcSnapshot currentAtEvent = snapshot;
        advancePlayingPosition(currentAtEvent, nowMs, true);
        snapshot.durationMs = duration;
        const bool changed = !initialized_ || position != sampledPositionMs_ ||
                             anchor != sampledAnchorMs_;
        const bool older = initialized_ && anchor > 0 && sampledAnchorMs_ > 0 &&
                           anchor < sampledAnchorMs_;
        if (changed && !older) {
            // 小幅校时保持展示连续，真实 seek 则允许进度跳变。
            constexpr int64_t kPositionDiscontinuityMs = 250;
            int64_t incomingAtEventMs = std::max<int64_t>(0, position);
            if (snapshot.status == PlaybackStatus::Playing && anchor > 0)
                incomingAtEventMs += std::max<int64_t>(0, nowMs - anchor);
            const int64_t deltaMs = incomingAtEventMs - currentAtEvent.positionMs;
            if (!initialized_ || position < sampledPositionMs_ ||
                deltaMs < -kPositionDiscontinuityMs || deltaMs > kPositionDiscontinuityMs)
                resetPresentedPosition_ = true;
            sampledPositionMs_ = position;
            sampledAnchorMs_ = anchor;
            initialized_ = true;
            snapshot.positionMs = std::max<int64_t>(0, position);
            snapshot.anchorUtcMs = anchor > 0 ? anchor : nowMs;
        }
        if (snapshot.durationMs > 0)
            snapshot.positionMs = std::min(snapshot.positionMs, snapshot.durationMs);
        snapshot.timelineStale = false;
    }

    SmtcSnapshot snapshot(const SmtcSnapshot& source, int64_t nowMs) {
        SmtcSnapshot result = source;
        advancePlayingPosition(result, nowMs, true);
        if (!resetPresentedPosition_ && presentedPositionMs_ >= 0)
            result.positionMs = std::max(result.positionMs, presentedPositionMs_);
        if (result.durationMs > 0)
            result.positionMs = std::min(result.positionMs, result.durationMs);
        presentedPositionMs_ = result.positionMs;
        resetPresentedPosition_ = false;
        return result;
    }

    void reset() { *this = SodaMusicTimeline{}; }

private:
    bool initialized_ = false;
    int64_t sampledPositionMs_ = 0;
    int64_t sampledAnchorMs_ = 0;
    int64_t presentedPositionMs_ = -1;
    bool resetPresentedPosition_ = true;
};

} // namespace smtc
