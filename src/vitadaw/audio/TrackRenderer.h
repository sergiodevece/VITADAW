#pragma once

#include "vitadaw/audio/PreparedProject.h"

#include <algorithm>
#include <cstdint>

namespace vitadaw::audio {

struct StereoSample {
    float left{};
    float right{};

    bool operator==(const StereoSample&) const = default;
};

[[nodiscard]] inline float readTrackChannelLinear(
    const PreparedTrackView& track,
    timeline::SourceFramePosition position,
    std::uint32_t outputChannel) noexcept {
    if (!track.isAvailable() || position.value < 0.0 ||
        position.value >= static_cast<double>(track.frameCount.value)) {
        return 0.0F;
    }

    const auto frame = static_cast<std::uint64_t>(position.value);
    const auto next = std::min(frame + 1, track.frameCount.value - 1);
    const auto fraction = static_cast<float>(
        position.value - static_cast<double>(frame));
    const auto sourceChannel = std::min(outputChannel, track.channelCount - 1);
    const auto* samples = track.channels[sourceChannel];
    return samples[frame] + fraction * (samples[next] - samples[frame]);
}

[[nodiscard]] inline StereoSample renderTrackAtProjectPosition(
    const PreparedTrackView& track,
    timeline::PreciseProjectFramePosition projectPosition,
    timeline::SampleRate projectSampleRate) noexcept {
    if (!track.isAvailable()) {
        return {};
    }

    const auto clipStart = static_cast<double>(track.clipStart.value);
    const auto clipEnd = clipStart + static_cast<double>(track.clipDuration.value);
    if (projectPosition.value < clipStart || projectPosition.value >= clipEnd) {
        return {};
    }

    const timeline::PreciseProjectFramePosition localProjectPosition{
        projectPosition.value - clipStart};
    auto sourcePosition = timeline::projectPositionToSourcePosition(
        localProjectPosition, projectSampleRate, track.sourceSampleRate);
    sourcePosition.value += static_cast<double>(track.sourceOffset.value);
    return {readTrackChannelLinear(track, sourcePosition, 0),
            readTrackChannelLinear(track, sourcePosition, 1)};
}

} // namespace vitadaw::audio
