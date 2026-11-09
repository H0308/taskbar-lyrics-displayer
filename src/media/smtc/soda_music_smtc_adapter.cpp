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
    // 汽水播放期间保留同一个位置采样，旧的 LastUpdatedTime 仍是有效锚点。
    // 不能按锚点年龄重新锚定，否则中途接入会回到该采样的位置。
    if (snapshot.anchorUtcMs <= 0)
        snapshot.anchorUtcMs = nowUtcMs();
    snapshot.timelineStale = false;
}

void SodaMusicSmtcAdapter::refreshTimeline(const Session& session, SmtcSnapshot& snapshot,
                                       int64_t eventNowMs) {
    auto info = session.GetPlaybackInfo();
    auto timeline = session.GetTimelineProperties();
    if (!info || !timeline)
        return;

    snapshot.status = mapStatus(info.PlaybackStatus());
    snapshot.durationMs = timeSpanMs(timeline.EndTime());
    snapshot.positionMs = std::max<int64_t>(0, timeSpanMs(timeline.Position()));
    if (snapshot.durationMs > 0)
        snapshot.positionMs = std::min(snapshot.positionMs, snapshot.durationMs);
    const int64_t anchor = lastUpdatedMs(timeline.LastUpdatedTime());
    snapshot.anchorUtcMs = anchor > 0 ? anchor : eventNowMs;
    snapshot.timelineStale = false;
    applyPlaybackControls(info, snapshot);
}

void SodaMusicSmtcAdapter::refreshPlayback(const Session& session, SmtcSnapshot& snapshot,
                                       int64_t eventNowMs) {
    // 暂停、恢复和 seek 都重新读取时间线，接受向前及向后的真实位置变化。
    auto info = session.GetPlaybackInfo();
    if (!info)
        return;
    snapshot.status = mapStatus(info.PlaybackStatus());
    applyPlaybackControls(info, snapshot);
    refreshTimeline(session, snapshot, eventNowMs);
}

SmtcSnapshot SodaMusicSmtcAdapter::snapshot(const SmtcSnapshot& source, int64_t nowMs) const {
    SmtcSnapshot result = source;
    advancePlayingPosition(result, nowMs, true);
    return result;
}

void SodaMusicSmtcAdapter::reset() {
    // 位置和锚点只保存在监视器快照中，没有需要跨会话保留的内部状态。
}

} // namespace smtc
