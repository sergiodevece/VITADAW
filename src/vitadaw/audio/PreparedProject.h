#pragma once

#include "vitadaw/timeline/Timeline.h"
#include "vitadaw/tracks/AudioTrack.h"

#include <array>
#include <cstdint>
#include <span>

namespace vitadaw::audio {

// Immutable views prepared outside realtime. The owner of the sample buffers
// and the view collection must outlive every processBlock that can observe it.
struct PreparedTrackView {
    tracks::TrackId id;
    std::array<const float*, 2> channels{};
    std::uint32_t channelCount{};
    timeline::SourceFrameCount frameCount;
    timeline::SampleRate sourceSampleRate;
    timeline::ProjectFramePosition clipStart;
    timeline::ProjectFrameCount clipDuration;
    timeline::SourceFrameCount sourceOffset;

    [[nodiscard]] bool isAvailable() const noexcept {
        return id.isValid() && channelCount > 0 && channelCount <= channels.size() &&
               channels[0] != nullptr &&
               (channelCount == 1 || channels[1] != nullptr) &&
               frameCount.value > sourceOffset.value && sourceSampleRate.isValid() &&
               clipStart.value >= 0 && clipDuration.value > 0;
    }
};

struct PreparedProjectView {
    timeline::SampleRate projectSampleRate;
    timeline::ProjectFrameCount duration;
    std::span<const PreparedTrackView> tracks;
};

} // namespace vitadaw::audio
