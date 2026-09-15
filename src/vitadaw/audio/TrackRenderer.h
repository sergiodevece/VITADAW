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

// Offline compatibility entry points: explicit preparation of input binary64
// coordinates. processBlock calls only the prepared DSP overloads below.
[[nodiscard]] inline DspFramePosition prepareDspPosition(
    exact::Boundary point, exact::UInt128 denominator) noexcept {
    const auto divisor = exact::powerOfTwo(point.denominatorPower);
    const auto fraction = exact::divmod(exact::multiply(point.fraction, denominator), divisor);
    return {{static_cast<std::int64_t>(point.whole)}, {exact::low128(fraction.quotient), denominator, false}};
}
[[nodiscard]] inline StereoSample renderTrackAtDspPosition(
    const PreparedTrackView&, DspFramePosition, timeline::SampleRate) noexcept;
[[nodiscard]] inline StereoSample renderPreparedClipAtDspPosition(
    const PreparedProcessingPlan&, const PreparedClipView&, DspFramePosition) noexcept;

[[nodiscard]] inline float readSourceChannelLinear(const PreparedSourceView& source,
    timeline::SourceFramePosition position, std::uint32_t outputChannel) noexcept {
    const auto at = exact::boundaryForPreparation(position.value, 116);
    if (!at.valid || !source.isAvailable() || at.whole >= source.frameCount.value) return 0;
    const auto next = std::min(at.whole + 1, source.frameCount.value - 1);
    const auto fraction = static_cast<float>(exact::approximate(exact::wide(at.fraction)) /
                                             exact::approximate(exact::powerOfTwo(at.denominatorPower)));
    const auto* samples = source.channels[std::min(outputChannel, source.channelCount - 1)];
    return samples[at.whole] + fraction * (samples[next] - samples[at.whole]);
}
[[nodiscard]] inline float readTrackChannelLinear(const PreparedTrackView& track,
    timeline::SourceFramePosition position, std::uint32_t outputChannel) noexcept {
    return readSourceChannelLinear({{track.id.value}, track.channels, track.channelCount,
        track.frameCount, track.sourceSampleRate, track.channelCount == 1 ?
        media::AudioChannelLayout::mono : media::AudioChannelLayout::stereo}, position, outputChannel);
}
[[nodiscard]] inline StereoSample renderPreparedClipAtProjectPosition(
    const PreparedProcessingPlan& plan, const PreparedClipView& clip,
    timeline::PreciseProjectFramePosition position) noexcept {
    const auto point = exact::boundaryForPreparation(position.value, 72);
    if (!point.valid) return {};
    return renderPreparedClipAtDspPosition(plan, clip, prepareDspPosition(point, plan.exactClock.denominator));
}
[[nodiscard]] inline StereoSample renderTrackAtProjectPosition(const PreparedTrackView& track,
    timeline::PreciseProjectFramePosition position, timeline::SampleRate projectRate) noexcept {
    const auto point = exact::boundaryForPreparation(position.value, 72);
    const auto clock = exact::clockForPreparation(projectRate.hertz(), projectRate.hertz());
    if (!point.valid || !clock.valid) return {};
    auto prepared = track;
    prepared.exactSource = exact::sourceForPreparation(track.sourceSampleRate.hertz(), projectRate.hertz(),
        static_cast<double>(track.clipDuration.value), static_cast<double>(track.sourceOffset.value), clock);
    return renderTrackAtDspPosition(prepared, prepareDspPosition(point, clock.denominator), projectRate);
}

[[nodiscard]] inline StereoSample readExactSample(
    const PreparedSourceView& source, const exact::SourceMapping& mapping,
    exact::Position local) noexcept {
    const auto at = exact::sourceIndex(mapping, local, source.frameCount.value);
    if (!at.inside || source.channels[0] == nullptr || source.channelCount == 0) return {};
    const auto next = std::min(at.index + 1, source.frameCount.value - 1);
    const auto weight = static_cast<float>(at.interpolationWeight());
    const auto read = [&](std::uint32_t channel) noexcept {
        const auto* samples = source.channels[std::min(channel, source.channelCount - 1)];
        return samples[at.index] + weight * (samples[next] - samples[at.index]);
    };
    return {read(0), read(1)};
}

[[nodiscard]] inline StereoSample renderTrackAtDspPosition(
    const PreparedTrackView& track, DspFramePosition position,
    timeline::SampleRate) noexcept {
    auto local = position.exactPosition();
    local.frame -= track.clipStart.value;
    const PreparedSourceView source{{track.id.value}, track.channels, track.channelCount,
        track.frameCount, track.sourceSampleRate, track.channelCount == 1
            ? media::AudioChannelLayout::mono : media::AudioChannelLayout::stereo};
    return readExactSample(source, track.exactSource, local);
}

[[nodiscard]] inline StereoSample renderPreparedClipAtDspPosition(
    const PreparedProcessingPlan& plan, const PreparedClipView& clip,
    DspFramePosition position) noexcept {
    if (clip.sourceIndex >= plan.sources.size()) return {};
    auto local = position.exactPosition();
    local.frame -= clip.exactStart;
    return readExactSample(plan.sources[clip.sourceIndex], clip.exactSource, local);
}

// Integer conservative candidate bounds. Individual membership is exact;
// ceil(end) may admit extra candidates but never excludes a valid sample.
[[nodiscard]] inline PreparedClipCandidateRange findPreparedClipCandidates(
    const PreparedProcessingPlan& plan, const PreparedTrackRoute& track,
    DspFramePosition first, DspFramePosition last) noexcept {
    const auto begin = plan.clips.begin() + static_cast<std::ptrdiff_t>(track.clips.first);
    const auto end = begin + static_cast<std::ptrdiff_t>(track.clips.count);
    const auto upper = std::upper_bound(begin, end,
        exact::floorPosition(last.exactPosition()).frame,
        [](std::int64_t value, const PreparedClipView& clip) { return value < clip.exactStart; });
    const auto prefix = plan.clipPrefixMaximumEnd.begin() + static_cast<std::ptrdiff_t>(track.clips.first);
    const auto prefixEnd = prefix + (upper - begin);
    const auto lower = std::upper_bound(prefix, prefixEnd,
        exact::floorPosition(first.exactPosition()).frame);
    return {track.clips.first + static_cast<std::size_t>(lower - prefix),
            static_cast<std::size_t>(prefixEnd - lower)};
}
} // namespace vitadaw::audio
