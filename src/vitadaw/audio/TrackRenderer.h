#pragma once

#include "vitadaw/audio/PreparedProcessingPlan.h"

#include <algorithm>
#include <cstdint>

namespace vitadaw::audio {

struct StereoSample {
    float left{};
    float right{};

    bool operator==(const StereoSample&) const = default;
};

struct PreparedClipCandidateRange {
    std::size_t first{};
    std::size_t count{};
};

[[nodiscard]] inline PreparedClipCandidateRange findPreparedClipCandidates(
    const PreparedProcessingPlan& plan, const PreparedTrackRoute& track,
    double blockStart, double blockEnd) noexcept {
    if (track.clips.count == 0 || !(blockEnd > blockStart)) {
        return {};
    }
    const auto begin = plan.clips.begin() +
                       static_cast<std::ptrdiff_t>(track.clips.first);
    const auto end = begin + static_cast<std::ptrdiff_t>(track.clips.count);
    const auto upper = std::lower_bound(
        begin, end, blockEnd,
        [](const PreparedClipView& clip, double value) {
            return clip.projectStart < value;
        });
    const auto prefixBegin = plan.clipPrefixMaximumEnd.begin() +
        static_cast<std::ptrdiff_t>(track.clips.first);
    const auto prefixUpper = prefixBegin + (upper - begin);
    const auto lowerPrefix = std::upper_bound(prefixBegin, prefixUpper,
                                              blockStart);
    return {track.clips.first +
                static_cast<std::size_t>(lowerPrefix - prefixBegin),
            static_cast<std::size_t>(prefixUpper - lowerPrefix)};
}

[[nodiscard]] inline float readSourceChannelLinear(
    const PreparedSourceView& source,
    timeline::SourceFramePosition position,
    std::uint32_t outputChannel) noexcept {
    if (!source.isAvailable() || position.value < 0.0 ||
        position.value >= static_cast<double>(source.frameCount.value)) {
        return 0.0F;
    }
    const auto frame = static_cast<std::uint64_t>(position.value);
    const auto next = std::min(frame + 1, source.frameCount.value - 1);
    const auto fraction = static_cast<float>(
        position.value - static_cast<double>(frame));
    const auto sourceChannel = std::min(outputChannel, source.channelCount - 1);
    const auto* samples = source.channels[sourceChannel];
    return samples[frame] + fraction * (samples[next] - samples[frame]);
}

[[nodiscard]] inline StereoSample renderPreparedClipAtProjectPosition(
    const PreparedProcessingPlan& plan, const PreparedClipView& clip,
    timeline::PreciseProjectFramePosition projectPosition) noexcept {
    if (!clip.isValid() || clip.sourceIndex >= plan.sources.size() ||
        projectPosition.value < clip.projectStart ||
        projectPosition.value >= clip.projectEnd) {
        return {};
    }
    const auto& source = plan.sources[clip.sourceIndex];
    const auto sourcePosition = timeline::SourceFramePosition{
        clip.sourceOffset +
        (projectPosition.value - clip.projectStart) *
            clip.sourceFramesPerProjectFrame};
    return {readSourceChannelLinear(source, sourcePosition, 0),
            readSourceChannelLinear(source, sourcePosition, 1)};
}

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
