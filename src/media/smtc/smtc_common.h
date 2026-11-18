#pragma once

#include "media/smtc_monitor.h"

#include <algorithm>
#include <memory>
#include <vector>

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Media.Control.h>

namespace smtc {

using Session = winrt::Windows::Media::Control::GlobalSystemMediaTransportControlsSession;

int64_t nowUtcMs();
int64_t timeSpanMs(winrt::Windows::Foundation::TimeSpan value);
int64_t lastUpdatedMs(winrt::Windows::Foundation::DateTime value);

bool sameSession(const Session& left, const Session& right) noexcept;

PlaybackStatus mapStatus(
    winrt::Windows::Media::Control::GlobalSystemMediaTransportControlsSessionPlaybackStatus status);

std::shared_ptr<const std::vector<uint8_t>> readThumbnail(
    const winrt::Windows::Media::Control::GlobalSystemMediaTransportControlsSessionMediaProperties& props,
    uint64_t readSequence);

struct PlaybackSample {
    bool valid = false;
    PlaybackStatus status = PlaybackStatus::Other;
    bool hasControls = false;
    bool canPrev = false;
    bool canPlayPause = false;
    bool canNext = false;
    bool canRepeat = false;
};

struct TimelineSample {
    bool valid = false;
    int64_t positionMs = 0;
    int64_t durationMs = 0;
    int64_t anchorUtcMs = 0;
};

template <typename PlaybackInfo>
inline PlaybackSample readPlaybackSample(const PlaybackInfo& info) {
    PlaybackSample sample;
    if (!info)
        return sample;
    sample.valid = true;
    sample.status = mapStatus(info.PlaybackStatus());
    if (auto controls = info.Controls()) {
        sample.hasControls = true;
        sample.canPrev = controls.IsPreviousEnabled();
        sample.canPlayPause = controls.IsPlayEnabled() || controls.IsPauseEnabled();
        sample.canNext = controls.IsNextEnabled();
        sample.canRepeat = controls.IsRepeatEnabled() || controls.IsShuffleEnabled();
    }
    return sample;
}

template <typename TimelineProperties>
inline TimelineSample readTimelineSample(const TimelineProperties& timeline) {
    TimelineSample sample;
    if (!timeline)
        return sample;
    sample.valid = true;
    sample.positionMs = timeSpanMs(timeline.Position());
    sample.durationMs = timeSpanMs(timeline.EndTime());
    sample.anchorUtcMs = lastUpdatedMs(timeline.LastUpdatedTime());
    return sample;
}

inline void applyPlaybackControls(const PlaybackSample& sample, SmtcSnapshot& snapshot) {
    if (!sample.valid || !sample.hasControls)
        return;
    snapshot.canPrev = sample.canPrev;
    snapshot.canPlayPause = sample.canPlayPause;
    snapshot.canNext = sample.canNext;
    snapshot.canRepeat = sample.canRepeat && snapshot.player != SmtcPlayerType::QQMusic &&
                         snapshot.player != SmtcPlayerType::Soda;
}

inline void advancePlayingPosition(SmtcSnapshot& snapshot, int64_t nowMs,
                                   bool requireAnchor) {
    if (snapshot.status != PlaybackStatus::Playing ||
        (requireAnchor && snapshot.anchorUtcMs <= 0))
        return;

    const int64_t elapsed = nowMs - snapshot.anchorUtcMs;
    if (elapsed <= 0)
        return;

    snapshot.positionMs += elapsed;
    if (snapshot.durationMs > 0)
        snapshot.positionMs = std::min(snapshot.positionMs, snapshot.durationMs);
}

} // namespace smtc
