#pragma once

#include "vitadaw/audio/TrackRenderer.h"
#include "vitadaw/mixer/MixerState.h"

namespace vitadaw::audio {

[[nodiscard]] inline StereoSample applyBusMix(
    StereoSample input, const mixer::PreparedBusMixState& mix,
    bool pathIsAudible) noexcept {
    if (mix.muted || !pathIsAudible) {
        return {};
    }
    return {input.left * mix.linearGain * mix.leftBalance,
            input.right * mix.linearGain * mix.rightBalance};
}

} // namespace vitadaw::audio
