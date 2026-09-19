#include "vitadaw/audio/RecordingTypes.h"

#include <cmath>
#include <limits>

namespace vitadaw::audio {

std::optional<timeline::ProjectFrameCount>
convertRecordingLatencyToProjectFrames(timeline::DeviceFrameCount frames,
                                       timeline::SampleRate deviceRate,
                                       timeline::SampleRate projectRate) noexcept {
    if (!deviceRate.isValid() || !projectRate.isValid()) return std::nullopt;
    const auto converted = static_cast<long double>(frames.value) *
        static_cast<long double>(projectRate.hertz()) /
        static_cast<long double>(deviceRate.hertz());
    const auto maximum = static_cast<long double>(timeline::maximumSupportedProjectFrame().value);
    if (!std::isfinite(converted) || converted < 0.0L || converted > maximum) return std::nullopt;
    const auto rounded = std::round(converted); // Existing temporal policy: nearest, ties away from zero.
    if (!std::isfinite(rounded) || rounded < 0.0L || rounded > maximum) return std::nullopt;
    return timeline::ProjectFrameCount{static_cast<std::int64_t>(rounded)};
}

std::optional<timeline::ProjectFrameCount> computeEffectiveRecordingCompensation(
    RecordingLatencyStatus status, timeline::ProjectFrameCount automatic,
    timeline::ProjectFrameCount manual) noexcept {
    const auto maximum = timeline::maximumSupportedProjectFrame().value;
    auto reported = status == RecordingLatencyStatus::reported ? automatic.value : 0;
    if (reported < 0 || reported > maximum) reported = 0;
    if (manual.value < 0 && reported > std::numeric_limits<std::int64_t>::max() + manual.value)
        return std::nullopt;
    return timeline::ProjectFrameCount{reported - manual.value};
}

RecordingPlacementResult computeRecordingPlacement(
    timeline::ProjectFramePosition capturedStart,
    const RecordingPlacementSnapshot& snapshot) noexcept {
    RecordingPlacementResult result;
    if (!timeline::isSupportedProjectFramePosition(capturedStart)) {
        result.upperBoundExceeded = true;
        return result;
    }
    const auto maximum = timeline::maximumSupportedProjectFrame().value;
    const auto effective = computeEffectiveRecordingCompensation(
        snapshot.latencyStatus, snapshot.reportedInputLatencyProjectFrames,
        snapshot.manualOffsetProjectFrames);
    if (!effective) {
        result.upperBoundExceeded = true;
        return result;
    }
    const auto compensation = effective->value;
    if (compensation > 0 && capturedStart.value < compensation) {
        result.start = {0};
        result.unappliedEarlyFrames = static_cast<std::uint64_t>(compensation - capturedStart.value);
        return result;
    }
    const auto base = compensation > 0 ? capturedStart.value - compensation
                                       : capturedStart.value;
    const auto later = compensation < 0 ? -compensation : 0;
    if (later > maximum - base) {
        result.upperBoundExceeded = true;
        return result;
    }
    result.start = {base + later};
    return result;
}

} // namespace vitadaw::audio
