#pragma once

#include "vitadaw/timeline/Time.h"

#include <array>
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace vitadaw::audio {

struct PreparedTrackView {
    std::array<const float*, 2> channels{};
    std::uint32_t channelCount{};
    timeline::SourceFrameCount frameCount;
    timeline::SampleRate sourceSampleRate;

    [[nodiscard]] bool isAvailable() const noexcept {
        return channelCount > 0 && channelCount <= channels.size() &&
               channels[0] != nullptr &&
               (channelCount == 1 || channels[1] != nullptr) && frameCount.value > 0 &&
               sourceSampleRate.isValid();
    }
};

struct StereoSample {
    float left{};
    float right{};
};

inline float readTrackChannelLinear(const PreparedTrackView& track,
                                    timeline::SourceFramePosition position,
                                    std::uint32_t outputChannel) noexcept {
    if (!track.isAvailable() || position.value < 0.0 ||
        position.value >= static_cast<double>(track.frameCount.value)) {
        return 0.0F;
    }

    const auto frame = static_cast<std::uint64_t>(position.value);
    const auto next = std::min(frame + 1, track.frameCount.value - 1);
    const auto fraction = static_cast<float>(position.value - static_cast<double>(frame));
    const auto sourceChannel = std::min(outputChannel, track.channelCount - 1);
    const auto* samples = track.channels[sourceChannel];
    return samples[frame] + fraction * (samples[next] - samples[frame]);
}

inline StereoSample mixTwoTracksAtProjectPosition(
    const std::array<PreparedTrackView, 2>& tracks,
    timeline::PreciseProjectFramePosition projectPosition,
    timeline::SampleRate projectSampleRate) noexcept {
    StereoSample mixed;
    for (const auto& track : tracks) {
        const auto sourcePosition = timeline::projectPositionToSourcePosition(
            projectPosition, projectSampleRate, track.sourceSampleRate);
        mixed.left += 0.5F * readTrackChannelLinear(track, sourcePosition, 0);
        mixed.right += 0.5F * readTrackChannelLinear(track, sourcePosition, 1);
    }
    return mixed;
}

} // namespace vitadaw::audio
