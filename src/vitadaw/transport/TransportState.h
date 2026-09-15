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

    [[nodiscard]] bool seek(timeline::ProjectFramePosition target) noexcept {
        if (!timeline::isSupportedProjectFramePosition(target)) return false;
        position = target;
        return true;
    }

    void synchronise(PlaybackState state,
                     timeline::ProjectFramePosition newPosition,
                     timeline::ProjectFrameCount newDuration) noexcept {
        duration = newDuration;
        // Content duration is descriptive; loop and run-until-stop playback may
        // legitimately place the transport beyond the last clip.
        position = {std::max<std::int64_t>(0, newPosition.value)};
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
