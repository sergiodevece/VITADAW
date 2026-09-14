#pragma once

#include "vitadaw/audio/TrackRenderer.h"
#include "vitadaw/mixer/MixerState.h"

namespace vitadaw::audio {

[[nodiscard]] inline StereoSample makeBusPreFaderPreBalanceTap(
    StereoSample input) noexcept {
    return input;
}

[[nodiscard]] inline StereoSample makeBusPostFaderPostBalanceTap(
    StereoSample input, const mixer::PreparedBusMixState& mix) noexcept {
    if (mix.muted) {
        return {};
    }
    return {input.left * mix.linearGain * mix.leftBalance,
            input.right * mix.linearGain * mix.rightBalance};
}

[[nodiscard]] inline StereoSample applyBusMix(
    StereoSample input, const mixer::PreparedBusMixState& mix,
    bool pathIsAudible) noexcept {
    if (!pathIsAudible) {
        return {};
    }
    return makeBusPostFaderPostBalanceTap(input, mix);
}

} // namespace vitadaw::audio
