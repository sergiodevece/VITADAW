#pragma once

#include "vitadaw/audio/TrackRenderer.h"

namespace vitadaw::audio {

// Temporary fixed gain per track. It is independent of the number of loaded
// tracks and will be replaced by explicit gain staging in a later mixer.
inline constexpr float provisionalTrackGain = 0.125F;

inline void accumulateTrackContribution(
    StereoSample& destination,
    StereoSample contribution) noexcept {
    destination.left += provisionalTrackGain * contribution.left;
    destination.right += provisionalTrackGain * contribution.right;
}

} // namespace vitadaw::audio
