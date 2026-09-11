#include "vitadaw/timeline/Time.h"

#include <cmath>

namespace vitadaw::timeline {
namespace {

std::int64_t roundedFrames(double frames) noexcept {
    return static_cast<std::int64_t>(std::llround(frames));
}

} // namespace

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

ProjectFramePosition sourcePositionToProjectPosition(
    SourceFramePosition position,
    SampleRate sourceSampleRate,
    SampleRate projectSampleRate) noexcept {
    return {secondsToProjectFrames(
                sourcePositionToSeconds(position, sourceSampleRate), projectSampleRate)
                .value};
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

} // namespace vitadaw::timeline
