#pragma once
#include "vitadaw/timeline/Time.h"
#include "vitadaw/audio/ExactTemporal.h"

namespace vitadaw::audio {
// Ephemeral copy. Only exactPhase participates in discrete DSP decisions.
struct DspFramePosition {
    timeline::ProjectFramePosition frame;
    exact::Phase exactPhase;
    double phase{}; // presentation compatibility; not an authority
    DspFramePosition() = default;
    DspFramePosition(timeline::ProjectFramePosition p, exact::Phase residue) noexcept
        : frame(p), exactPhase(residue), phase(exact::phaseForPresentation(residue)) {}
    [[nodiscard]] exact::Position exactPosition() const noexcept { return {frame.value, exactPhase}; }
    [[nodiscard]] double approximate() const noexcept {
        return static_cast<double>(frame.value) + phase;
    }
};
} // namespace vitadaw::audio
