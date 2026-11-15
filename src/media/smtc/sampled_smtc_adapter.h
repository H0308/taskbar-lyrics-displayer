#pragma once

#include "media/smtc/smtc_player_adapter.h"
#include "media/smtc/sampled_timeline.h"

namespace smtc {

class SampledSmtcAdapter final : public SmtcPlayerAdapter {
public:
    explicit SampledSmtcAdapter(SmtcPlayerType player) : player_(player) {}
    SmtcPlayerType playerType() const noexcept override;
    SmtcSessionIdentity identifySession(const Session& session) const override;
    void prepareInitialSnapshot(SmtcSnapshot& snapshot) const override;
    void refreshTimeline(const Session& session, SmtcSnapshot& snapshot,
                         int64_t eventNowMs) override;
    void refreshPlayback(const Session& session, SmtcSnapshot& snapshot,
                         int64_t eventNowMs) override;
    SmtcSnapshot snapshot(const SmtcSnapshot& source, int64_t nowUtcMs) const override;
    void reset() override;

private:
    const SmtcPlayerType player_;
    mutable SampledTimeline timeline_;
};

} // namespace smtc
