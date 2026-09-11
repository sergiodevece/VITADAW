#pragma once

#include "vitadaw/audio/TrackRenderer.h"
#include "vitadaw/mixer/MixerState.h"

namespace vitadaw::audio {

// Mono uses a -3 dB equal-power law at centre. Stereo uses an equal-power
// balance law whose centre preserves both original channels at unity.
[[nodiscard]] inline StereoSample applyTrackMix(
    StereoSample input, std::uint32_t sourceChannelCount,
    const mixer::PreparedTrackMixState& mix, bool anySolo) noexcept {
    if (mix.muted || (anySolo && !mix.solo)) {
        return {};
    }

    float leftPan;
    float rightPan;
    if (sourceChannelCount == 1) {
        leftPan = mix.monoLeft;
        rightPan = mix.monoRight;
        input.right = input.left;
    } else {
        leftPan = mix.stereoLeft;
        rightPan = mix.stereoRight;
    }
    return {input.left * mix.linearGain * leftPan,
            input.right * mix.linearGain * rightPan};
}

[[nodiscard]] inline StereoSample applyTrackMixResolved(
    StereoSample input, std::uint32_t sourceChannelCount,
    const mixer::PreparedTrackMixState& mix, bool pathIsAudible) noexcept {
    if (mix.muted || !pathIsAudible) {
        return {};
    }

    if (sourceChannelCount == 1) {
        input.right = input.left;
        return {input.left * mix.linearGain * mix.monoLeft,
                input.right * mix.linearGain * mix.monoRight};
    }
    return {input.left * mix.linearGain * mix.stereoLeft,
            input.right * mix.linearGain * mix.stereoRight};
}

inline void applyMasterGain(StereoSample& sample,
                            mixer::PreparedMasterMixState master) noexcept {
    sample.left *= master.linearGain;
    sample.right *= master.linearGain;
}

} // namespace vitadaw::audio
