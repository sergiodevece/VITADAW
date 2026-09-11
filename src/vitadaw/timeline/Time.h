#pragma once

#include <cstdint>

namespace vitadaw::timeline {

struct Seconds {
    double value{};
    bool operator==(const Seconds&) const = default;
};

struct SourceFrameCount {
    std::uint64_t value{};
    bool operator==(const SourceFrameCount&) const = default;
};

struct SourceFramePosition {
    double value{};
    bool operator==(const SourceFramePosition&) const = default;
};

struct SourceFrameDuration {
    double value{};
    bool operator==(const SourceFrameDuration&) const = default;
};

struct ProjectFrameCount {
    std::int64_t value{};
    bool operator==(const ProjectFrameCount&) const = default;
};

struct ProjectFramePosition {
    std::int64_t value{};
    bool operator==(const ProjectFramePosition&) const = default;
};

struct PreciseProjectFramePosition {
    double value{};
    bool operator==(const PreciseProjectFramePosition&) const = default;
};

struct ProjectFrameDuration {
    double value{};
    bool operator==(const ProjectFrameDuration&) const = default;
};

struct DeviceFrameCount {
    std::uint64_t value{};
    bool operator==(const DeviceFrameCount&) const = default;
};

class SampleRate {
public:
    constexpr SampleRate() = default;
    explicit constexpr SampleRate(double hertz) noexcept : hertz_(hertz) {}

    [[nodiscard]] constexpr double hertz() const noexcept { return hertz_; }
    [[nodiscard]] bool isValid() const noexcept;

    bool operator==(const SampleRate&) const = default;

private:
    double hertz_{};
};

[[nodiscard]] Seconds sourceFramesToSeconds(SourceFrameCount frames,
                                            SampleRate sourceSampleRate) noexcept;
[[nodiscard]] Seconds sourcePositionToSeconds(SourceFramePosition position,
                                              SampleRate sourceSampleRate) noexcept;
[[nodiscard]] Seconds projectFramesToSeconds(ProjectFrameCount frames,
                                             SampleRate projectSampleRate) noexcept;
[[nodiscard]] Seconds projectPositionToSeconds(ProjectFramePosition position,
                                               SampleRate projectSampleRate) noexcept;
[[nodiscard]] ProjectFrameCount secondsToProjectFrames(
    Seconds seconds,
    SampleRate projectSampleRate) noexcept;
[[nodiscard]] ProjectFrameCount sourceFramesToProjectFrames(
    SourceFrameCount frames,
    SampleRate sourceSampleRate,
    SampleRate projectSampleRate) noexcept;
// Converts an exclusive resource end. Unlike position conversion, duration is
// rounded up so every valid source frame is covered by the project timeline.
[[nodiscard]] ProjectFrameCount sourceFramesToProjectDuration(
    SourceFrameCount frames,
    SampleRate sourceSampleRate,
    SampleRate projectSampleRate) noexcept;
[[nodiscard]] ProjectFramePosition sourcePositionToProjectPosition(
    SourceFramePosition position,
    SampleRate sourceSampleRate,
    SampleRate projectSampleRate) noexcept;
[[nodiscard]] SourceFramePosition projectPositionToSourcePosition(
    PreciseProjectFramePosition position,
    SampleRate projectSampleRate,
    SampleRate sourceSampleRate) noexcept;
[[nodiscard]] SourceFramePosition advanceSourcePosition(
    SourceFramePosition position,
    DeviceFrameCount deviceFrames,
    SampleRate sourceSampleRate,
    SampleRate deviceSampleRate) noexcept;
[[nodiscard]] SourceFrameDuration sourceFramesForDeviceFrames(
    DeviceFrameCount deviceFrames,
    SampleRate sourceSampleRate,
    SampleRate deviceSampleRate) noexcept;
[[nodiscard]] ProjectFrameDuration projectFramesForDeviceFrames(
    DeviceFrameCount deviceFrames,
    SampleRate projectSampleRate,
    SampleRate deviceSampleRate) noexcept;

} // namespace vitadaw::timeline
