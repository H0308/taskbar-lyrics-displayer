#include "media/smtc/sampled_smtc_adapter.h"
#include "media/player_source.h"

namespace smtc {

SmtcPlayerType SampledSmtcAdapter::playerType() const noexcept {
    return player_;
}

SmtcSessionIdentity SampledSmtcAdapter::identifySession(const Session& session) const {
    SmtcSessionIdentity identity;
    if (!session)
        return identity;
    try {
        identity.sourceAppUserModelId = session.SourceAppUserModelId().c_str();
        const auto* source = player_source::find(identity.sourceAppUserModelId);
        if (source && source->player == playerType()) {
            // 系统可能暂留已关闭的来源标识，不能将其重新绑定为有效会话。
            using Status =
                winrt::Windows::Media::Control::GlobalSystemMediaTransportControlsSessionPlaybackStatus;
            auto info = session.GetPlaybackInfo();
            if (info && info.PlaybackStatus() != Status::Closed)
                identity.player = player_;
        }
    } catch (...) {
    }
    return identity;
}

void SampledSmtcAdapter::prepareInitialSnapshot(SmtcSnapshot& snapshot) const {
    timeline_.initialize(snapshot, nowUtcMs());
}

void SampledSmtcAdapter::refreshTimeline(const PlaybackSample& playback,
                                         const TimelineSample& timeline,
                                         SmtcSnapshot& snapshot,
                                         int64_t eventNowMs) {
    if (!playback.valid)
        return;

    timeline_.updatePlayback(snapshot, playback.status, eventNowMs);
    applyPlaybackControls(playback, snapshot);

    if (!timeline.valid)
        return;
    timeline_.updateSample(snapshot, timeline.positionMs, timeline.anchorUtcMs,
                           timeline.durationMs, eventNowMs);
}

void SampledSmtcAdapter::refreshPlayback(const PlaybackSample& playback,
                                         const TimelineSample& timeline,
                                         SmtcSnapshot& snapshot,
                                         int64_t eventNowMs) {
    // 与时间线更新共用一次状态读取，保留旧状态供暂停／恢复的锚点换算使用。
    refreshTimeline(playback, timeline, snapshot, eventNowMs);
}

SmtcSnapshot SampledSmtcAdapter::snapshot(const SmtcSnapshot& source, int64_t nowMs) const {
    return timeline_.snapshot(source, nowMs);
}

void SampledSmtcAdapter::reset() {
    timeline_.reset();
}

} // namespace smtc
