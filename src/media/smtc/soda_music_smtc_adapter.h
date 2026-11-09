#pragma once

#include "media/smtc/smtc_player_adapter.h"

namespace smtc {

class SodaMusicSmtcAdapter final : public SmtcPlayerAdapter {
public:
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
    // 只记录已接收的原始采样，不能把状态切换时重新锚定的显示位置当成新采样。
    mutable bool timelineInitialized_ = false;
    mutable int64_t sampledPositionMs_ = 0;
    mutable int64_t sampledAnchorMs_ = 0;
    // 仅在 Soda Music 的快照出口保持连续进度；切歌或真实 seek 后重置。
    mutable int64_t presentedPositionMs_ = -1;
    mutable bool resetPresentedPosition_ = true;
};

} // namespace smtc
