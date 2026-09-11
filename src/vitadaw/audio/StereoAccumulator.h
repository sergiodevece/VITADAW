#pragma once

#include "vitadaw/audio/TrackRenderer.h"

namespace vitadaw::audio {

inline void accumulateTrackContribution(
    StereoSample& destination,
    StereoSample contribution) noexcept {
    destination.left += contribution.left;
    destination.right += contribution.right;
}

} // namespace vitadaw::audio
