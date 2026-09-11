#pragma once

#include "vitadaw/timeline/Timeline.h"

namespace vitadaw::transport {

enum class PlaybackState {
    stopped,
    playing,
};

// Application-thread state. The audio thread will expose a separate atomic view.
struct TransportState {
    PlaybackState playback{PlaybackState::stopped};
    timeline::ProjectFramePosition position;
    timeline::ProjectFrameCount duration;

    void markPlaying() noexcept {
        playback = PlaybackState::playing;
    }

    void setDuration(timeline::ProjectFrameCount newDuration) noexcept {
        duration = newDuration;
        position = {0};
        playback = PlaybackState::stopped;
    }

    void stopAndRewind() noexcept {
        playback = PlaybackState::stopped;
        position = {0};
    }

    void synchronise(bool isPlaying,
                     timeline::ProjectFramePosition newPosition,
                     timeline::ProjectFrameCount newDuration) noexcept {
        duration = newDuration;
        position = newPosition.value >= newDuration.value
                       ? timeline::ProjectFramePosition{newDuration.value}
                       : newPosition;
        playback = isPlaying ? PlaybackState::playing : PlaybackState::stopped;
    }
};

} // namespace vitadaw::transport
