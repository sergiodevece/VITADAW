#include "vitadaw/timeline/Time.h"
#include "vitadaw/audio/TemporalInteger.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace vitadaw::timeline {
namespace {

std::int64_t roundedFrames(double frames) noexcept {
    return static_cast<std::int64_t>(std::llround(frames));
}

} // namespace

bool SampleRate::isValid() const noexcept {
    return hertz_ > 0.0 && std::isfinite(hertz_);
}

std::optional<ProjectFramePosition> checkedExclusiveProjectEnd(
    ProjectFramePosition start, ProjectFrameDuration duration) noexcept {
    using namespace audio::exact;
    const auto decoded = decodeForPreparation(duration.value);
    if (start.value < 0 || !decoded.valid || zero(wide(decoded.numerator))) return std::nullopt;
    auto length = divmod(wide(decoded.numerator), wide(decoded.denominator));
    if (!zero(length.remainder)) add(length.quotient, wide(1), length.quotient);
    if (!fits64(length.quotient) || length.quotient.words[0] > static_cast<std::uint64_t>(INT64_MAX)) return std::nullopt;
    const auto whole = static_cast<std::int64_t>(length.quotient.words[0]);
    if (whole > std::numeric_limits<std::int64_t>::max() - start.value)
        return std::nullopt;
    return ProjectFramePosition{start.value + whole};
}

Seconds sourceFramesToSeconds(SourceFrameCount frames,
                              SampleRate sourceSampleRate) noexcept {
    return {static_cast<double>(frames.value) / sourceSampleRate.hertz()};
}

Seconds sourcePositionToSeconds(SourceFramePosition position,
                                SampleRate sourceSampleRate) noexcept {
    return {position.value / sourceSampleRate.hertz()};
}

Seconds projectFramesToSeconds(ProjectFrameCount frames,
                               SampleRate projectSampleRate) noexcept {
    return {static_cast<double>(frames.value) / projectSampleRate.hertz()};
}

Seconds projectPositionToSeconds(ProjectFramePosition position,
                                 SampleRate projectSampleRate) noexcept {
    return {static_cast<double>(position.value) / projectSampleRate.hertz()};
}

std::optional<Seconds> checkedProjectPositionToSeconds(
    ProjectFramePosition position, SampleRate projectSampleRate) noexcept {
    if (!isSupportedProjectFramePosition(position) ||
        !projectSampleRate.isValid()) return std::nullopt;
    const auto seconds = projectPositionToSeconds(position, projectSampleRate);
    return std::isfinite(seconds.value)
        ? std::optional<Seconds>{seconds} : std::nullopt;
}

ProjectFrameCount secondsToProjectFrames(Seconds seconds,
                                         SampleRate projectSampleRate) noexcept {
    return {roundedFrames(seconds.value * projectSampleRate.hertz())};
}

ProjectFrameCount sourceFramesToProjectFrames(SourceFrameCount frames,
                                              SampleRate sourceSampleRate,
                                              SampleRate projectSampleRate) noexcept {
    return secondsToProjectFrames(sourceFramesToSeconds(frames, sourceSampleRate),
                                  projectSampleRate);
}

ProjectFrameCount sourceFramesToProjectDuration(SourceFrameCount frames,
                                                SampleRate sourceSampleRate,
                                                SampleRate projectSampleRate) noexcept {
    if (frames.value == 0 || !sourceSampleRate.isValid() ||
        !projectSampleRate.isValid()) {
        return {};
    }
    using namespace audio::exact;
    const auto ratio = rateRatioForPreparation(projectSampleRate.hertz(), sourceSampleRate.hertz());
    if (!ratio.valid) return {std::numeric_limits<std::int64_t>::max()};
    auto converted = divmod(multiply({frames.value, 0}, ratio.numerator), wide(ratio.denominator));
    if (!zero(converted.remainder)) add(converted.quotient, wide(1), converted.quotient);
    if (!fits64(converted.quotient) || converted.quotient.words[0] > static_cast<std::uint64_t>(INT64_MAX)) {
        return {std::numeric_limits<std::int64_t>::max()};
    }
    return {std::max<std::int64_t>(1, static_cast<std::int64_t>(converted.quotient.words[0]))};
}

ProjectFramePosition sourcePositionToProjectPosition(
    SourceFramePosition position,
    SampleRate sourceSampleRate,
    SampleRate projectSampleRate) noexcept {
    return {secondsToProjectFrames(
                sourcePositionToSeconds(position, sourceSampleRate), projectSampleRate)
                .value};
}

SourceFramePosition projectPositionToSourcePosition(
    PreciseProjectFramePosition position,
    SampleRate projectSampleRate,
    SampleRate sourceSampleRate) noexcept {
    return {position.value * sourceSampleRate.hertz() /
            projectSampleRate.hertz()};
}

SourceFramePosition advanceSourcePosition(SourceFramePosition position,
                                          DeviceFrameCount deviceFrames,
                                          SampleRate sourceSampleRate,
                                          SampleRate deviceSampleRate) noexcept {
    return {position.value +
            sourceFramesForDeviceFrames(
                deviceFrames, sourceSampleRate, deviceSampleRate)
                .value};
}

SourceFrameDuration sourceFramesForDeviceFrames(
    DeviceFrameCount deviceFrames,
    SampleRate sourceSampleRate,
    SampleRate deviceSampleRate) noexcept {
    return {static_cast<double>(deviceFrames.value) * sourceSampleRate.hertz() /
            deviceSampleRate.hertz()};
}

ProjectFrameDuration projectFramesForDeviceFrames(
    DeviceFrameCount deviceFrames,
    SampleRate projectSampleRate,
    SampleRate deviceSampleRate) noexcept {
    return {static_cast<double>(deviceFrames.value) * projectSampleRate.hertz() /
            deviceSampleRate.hertz()};
}

} // namespace vitadaw::timeline
