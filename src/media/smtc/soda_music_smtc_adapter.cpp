#include "media/smtc/soda_music_smtc_adapter.h"
#include "media/player_source.h"

namespace smtc {

SmtcPlayerType SodaMusicSmtcAdapter::playerType() const noexcept {
    return SmtcPlayerType::Soda;
}

SmtcSessionIdentity SodaMusicSmtcAdapter::identifySession(const Session& session) const {
    SmtcSessionIdentity identity;
    if (!session)
        return identity;
    try {
        identity.sourceAppUserModelId = session.SourceAppUserModelId().c_str();
        const auto* source = player_source::find(identity.sourceAppUserModelId);
        if (source && source->player == playerType()) {
            // 系统可能暂留已关闭的来源标识，不能将其重新绑定为有效汽水会话。
            using Status =
                winrt::Windows::Media::Control::GlobalSystemMediaTransportControlsSessionPlaybackStatus;
            auto info = session.GetPlaybackInfo();
            if (info && info.PlaybackStatus() != Status::Closed)
                identity.player = SmtcPlayerType::Soda;
        }
    } catch (...) {
    }
    return identity;
}

void SodaMusicSmtcAdapter::prepareInitialSnapshot(SmtcSnapshot& snapshot) const {
    timeline_.initialize(snapshot, nowUtcMs());
}

void SodaMusicSmtcAdapter::refreshTimeline(const Session& session, SmtcSnapshot& snapshot,
                                       int64_t eventNowMs) {
    auto info = session.GetPlaybackInfo();
    if (!info)
        return;

    timeline_.updatePlayback(snapshot, mapStatus(info.PlaybackStatus()), eventNowMs);
    applyPlaybackControls(info, snapshot);

    auto timeline = session.GetTimelineProperties();
    if (!timeline)
        return;
    timeline_.updateSample(snapshot, timeSpanMs(timeline.Position()),
                           lastUpdatedMs(timeline.LastUpdatedTime()),
                           timeSpanMs(timeline.EndTime()), eventNowMs);
}

void SodaMusicSmtcAdapter::refreshPlayback(const Session& session, SmtcSnapshot& snapshot,
                                       int64_t eventNowMs) {
    // 与时间线更新共用一次状态读取，保留旧状态供暂停／恢复的锚点换算使用。
    refreshTimeline(session, snapshot, eventNowMs);
}

SmtcSnapshot SodaMusicSmtcAdapter::snapshot(const SmtcSnapshot& source, int64_t nowMs) const {
    return timeline_.snapshot(source, nowMs);
}

void SodaMusicSmtcAdapter::reset() {
    timeline_.reset();
}

} // namespace smtc
