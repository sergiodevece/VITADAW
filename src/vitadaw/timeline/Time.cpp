#include "vitadaw/timeline/Time.h"

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
    const auto converted = std::ceil(
        static_cast<long double>(frames.value) *
        static_cast<long double>(projectSampleRate.hertz()) /
        static_cast<long double>(sourceSampleRate.hertz()));
    if (!std::isfinite(converted) ||
        converted > static_cast<long double>(std::numeric_limits<std::int64_t>::max())) {
        return {std::numeric_limits<std::int64_t>::max()};
    }
    return {std::max<std::int64_t>(1, static_cast<std::int64_t>(converted))};
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
