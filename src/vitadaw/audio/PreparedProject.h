#pragma once

#include "vitadaw/timeline/Timeline.h"
#include "vitadaw/tracks/AudioTrack.h"
#include "vitadaw/mixer/MixerState.h"
#include "vitadaw/media/AudioSource.h"
#include "vitadaw/audio/ExactTemporal.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <span>

namespace vitadaw::audio {

struct PreparedSourceView {
    media::SourceId id;
    std::array<const float*, 2> channels{};
    std::uint32_t channelCount{};
    timeline::SourceFrameCount frameCount;
    timeline::SampleRate sampleRate;
    media::AudioChannelLayout layout{media::AudioChannelLayout::mono};

    [[nodiscard]] bool isAvailable() const noexcept {
        return id.isValid() && channelCount == media::channelCount(layout) &&
               channelCount > 0 && channelCount <= channels.size() &&
               channels[0] != nullptr &&
               (channelCount == 1 || channels[1] != nullptr) &&
               frameCount.value > 0 && sampleRate.isValid();
    }
};

struct PreparedClipView {
    clips::ClipId id;
    std::size_t sourceIndex{};
    double projectStart{};
    double projectEnd{};
    double sourceOffset{};
    double sourceFramesPerProjectFrame{};
    // Exact prepared local duration; projectEnd is a conservative search bound.
    double projectLength{};
    exact::SourceMapping exactSource;
    std::int64_t exactStart{};

    [[nodiscard]] bool isValid() const noexcept {
        return id.isValid() && std::isfinite(projectStart) &&
               std::isfinite(projectEnd) && std::isfinite(sourceOffset) &&
               projectStart >= 0.0 && projectEnd > projectStart &&
               sourceOffset >= 0.0 &&
               std::isfinite(sourceFramesPerProjectFrame) &&
               sourceFramesPerProjectFrame > 0.0;
    }
};

struct PreparedClipRange {
    std::size_t first{};
    std::size_t count{};
};

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
    mixer::PreparedTrackMixState mix;
    exact::SourceMapping exactSource;

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
    mixer::PreparedMasterMixState masterMix;
    bool anySolo{};
};

} // namespace vitadaw::audio
