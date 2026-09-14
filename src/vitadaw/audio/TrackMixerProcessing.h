#pragma once

#include "vitadaw/audio/TrackRenderer.h"
#include "vitadaw/mixer/MixerState.h"

namespace vitadaw::audio {

inline constexpr float monoCentreCoefficient = 0.70710678F;

// This tap is deliberately derived from the original render. It must never be
// fed back into the mono post-fader path, which would apply centre attenuation
// twice.
[[nodiscard]] inline StereoSample makeTrackPreFaderPrePanTap(
    StereoSample input, std::uint32_t sourceChannelCount) noexcept {
    if (sourceChannelCount == 1) {
        return {input.left * monoCentreCoefficient,
                input.left * monoCentreCoefficient};
    }
    return input;
}

[[nodiscard]] inline StereoSample makeTrackPostFaderPostPanTap(
    StereoSample input, std::uint32_t sourceChannelCount,
    const mixer::PreparedTrackMixState& mix) noexcept {
    if (mix.muted) {
        return {};
    }
    if (sourceChannelCount == 1) {
        return {input.left * mix.linearGain * mix.monoLeft,
                input.left * mix.linearGain * mix.monoRight};
    }
    return {input.left * mix.linearGain * mix.stereoLeft,
            input.right * mix.linearGain * mix.stereoRight};
}

// Mono uses a -3 dB equal-power law at centre. Stereo uses an equal-power
// balance law whose centre preserves both original channels at unity.
[[nodiscard]] inline StereoSample applyTrackMix(
    StereoSample input, std::uint32_t sourceChannelCount,
    const mixer::PreparedTrackMixState& mix, bool anySolo) noexcept {
    if (anySolo && !mix.solo) {
        return {};
    }
    return makeTrackPostFaderPostPanTap(input, sourceChannelCount, mix);
}

[[nodiscard]] inline StereoSample applyTrackMixResolved(
    StereoSample input, std::uint32_t sourceChannelCount,
    const mixer::PreparedTrackMixState& mix, bool pathIsAudible) noexcept {
    if (!pathIsAudible) {
        return {};
    }
    return makeTrackPostFaderPostPanTap(input, sourceChannelCount, mix);
}

inline void applyMasterGain(StereoSample& sample,
                            mixer::PreparedMasterMixState master) noexcept {
    sample.left *= master.linearGain;
    sample.right *= master.linearGain;
}

} // namespace vitadaw::audio
