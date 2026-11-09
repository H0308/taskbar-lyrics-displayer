#include "media/smtc/soda_music_smtc_adapter.h"
#include "media/player_source.h"

namespace smtc {

SmtcPlayerType SodaMusicSmtcAdapter::playerType() const noexcept {
    return SmtcPlayerType::Qishui;
}

SmtcSessionIdentity SodaMusicSmtcAdapter::identifySession(const Session& session) const {
    SmtcSessionIdentity identity;
    if (!session)
        return identity;
    try {
        identity.sourceAppUserModelId = session.SourceAppUserModelId().c_str();
        const auto* source = player_source::find(identity.sourceAppUserModelId);
        if (source && source->player == playerType())
            identity.player = SmtcPlayerType::Qishui;
    } catch (...) {
    }
    return identity;
}

void SodaMusicSmtcAdapter::prepareInitialSnapshot(SmtcSnapshot& snapshot) const {
    timelineInitialized_ = true;
    sampledPositionMs_ = snapshot.positionMs;
    sampledAnchorMs_ = snapshot.anchorUtcMs;
    resetPresentedPosition_ = true;
    // 汽水播放期间保留同一个位置采样，旧的 LastUpdatedTime 仍是有效锚点。
    // 不能按锚点年龄重新锚定，否则中途接入会回到该采样的位置。
    if (snapshot.anchorUtcMs <= 0)
        snapshot.anchorUtcMs = nowUtcMs();
    snapshot.timelineStale = false;
}

void SodaMusicSmtcAdapter::refreshTimeline(const Session& session, SmtcSnapshot& snapshot,
                                       int64_t eventNowMs) {
    auto info = session.GetPlaybackInfo();
    if (!info)
        return;

    const PlaybackStatus newStatus = mapStatus(info.PlaybackStatus());
    SmtcSnapshot currentAtEvent = snapshot;
    advancePlayingPosition(currentAtEvent, eventNowMs, true);
    const bool statusChanged = newStatus != snapshot.status;
    // 先按旧状态换算到切换时刻，再改变状态。暂停通知可能早于新的位置采样。
    if (statusChanged) {
        advancePlayingPosition(snapshot, eventNowMs, true);
        snapshot.anchorUtcMs = eventNowMs;
    }
    snapshot.status = newStatus;
    applyPlaybackControls(info, snapshot);

    auto timeline = session.GetTimelineProperties();
    if (!timeline)
        return;
    const int64_t position = timeSpanMs(timeline.Position());
    const int64_t anchor = lastUpdatedMs(timeline.LastUpdatedTime());
    snapshot.durationMs = timeSpanMs(timeline.EndTime());
    // 只用新的原始采样校准时间线。重复采样不能把暂停位置拉回播放起点，
    // 也不能让恢复播放继续沿用暂停前的锚点、把暂停时长计入进度。
    const bool sampleChanged = !timelineInitialized_ || position != sampledPositionMs_ ||
                               anchor != sampledAnchorMs_;
    const bool olderSample = timelineInitialized_ && anchor > 0 && sampledAnchorMs_ > 0 &&
                             anchor < sampledAnchorMs_;
    if (sampleChanged && !olderSample) {
        // 新时间戳也可能带来几十毫秒的校时，不能把它当成用户回拖。
        // 在同一事件时刻比较两条时间线；明显跳变或原始位置实际向后才重置显示。
        constexpr int64_t kPositionDiscontinuityMs = 250;
        int64_t incomingAtEventMs = std::max<int64_t>(0, position);
        if (newStatus == PlaybackStatus::Playing && anchor > 0)
            incomingAtEventMs += std::max<int64_t>(0, eventNowMs - anchor);
        const int64_t deltaMs = incomingAtEventMs - currentAtEvent.positionMs;
        if (!timelineInitialized_ || position < sampledPositionMs_ ||
            deltaMs < -kPositionDiscontinuityMs || deltaMs > kPositionDiscontinuityMs)
            resetPresentedPosition_ = true;
        sampledPositionMs_ = position;
        sampledAnchorMs_ = anchor;
        timelineInitialized_ = true;
        snapshot.positionMs = std::max<int64_t>(0, position);
        snapshot.anchorUtcMs = anchor > 0 ? anchor : eventNowMs;
    }
    if (snapshot.durationMs > 0)
        snapshot.positionMs = std::min(snapshot.positionMs, snapshot.durationMs);
    snapshot.timelineStale = false;
}

void SodaMusicSmtcAdapter::refreshPlayback(const Session& session, SmtcSnapshot& snapshot,
                                       int64_t eventNowMs) {
    // 与时间线更新共用一次状态读取，保留旧状态供暂停／恢复的锚点换算使用。
    refreshTimeline(session, snapshot, eventNowMs);
}

SmtcSnapshot SodaMusicSmtcAdapter::snapshot(const SmtcSnapshot& source, int64_t nowMs) const {
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

void SodaMusicSmtcAdapter::reset() {
    timelineInitialized_ = false;
    sampledPositionMs_ = 0;
    sampledAnchorMs_ = 0;
    presentedPositionMs_ = -1;
    resetPresentedPosition_ = true;
}

} // namespace smtc
