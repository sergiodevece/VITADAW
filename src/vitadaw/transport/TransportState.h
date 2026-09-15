#pragma once

#include "vitadaw/timeline/Timeline.h"
#include "vitadaw/transport/PlaybackState.h"

#include <algorithm>

namespace vitadaw::transport {

// Application-thread state. The audio thread will expose a separate atomic view.
struct TransportState {
    PlaybackState playback{PlaybackState::stopped};
    timeline::ProjectFramePosition position;
    timeline::ProjectFrameCount duration;

    void markPlaying() noexcept {
        playback = PlaybackState::playing;
    }

    void markPaused() noexcept { playback = PlaybackState::paused; }

    void setDuration(timeline::ProjectFrameCount newDuration) noexcept {
        duration = newDuration;
        position = {0};
        playback = PlaybackState::stopped;
    }

    void stopAndRewind() noexcept {
        playback = PlaybackState::stopped;
        position = {0};
    }

    void stop() noexcept {
        if (playback == PlaybackState::stopped) position = {0};
        playback = PlaybackState::stopped;
    }

    void seek(timeline::ProjectFramePosition target) noexcept {
        position = {std::clamp(target.value, std::int64_t{0}, duration.value)};
    }

    void synchronise(PlaybackState state,
                     timeline::ProjectFramePosition newPosition,
                     timeline::ProjectFrameCount newDuration) noexcept {
        duration = newDuration;
        position = newPosition.value <= 0
                       ? timeline::ProjectFramePosition{0}
                       : (newPosition.value >= newDuration.value
                              ? timeline::ProjectFramePosition{newDuration.value}
                              : newPosition);
        playback = state;
    }

    void synchronise(bool isPlaying,
                     timeline::ProjectFramePosition newPosition,
                     timeline::ProjectFrameCount newDuration) noexcept {
        synchronise(isPlaying ? PlaybackState::playing : PlaybackState::stopped,
                    newPosition, newDuration);
    }
};

} // namespace vitadaw::transport
