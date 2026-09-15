#pragma once

#include <cstdint>
#include <optional>
#include <cmath>

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

// Compatibility ceiling retained by 0.6.0. Exact DSP preparation certifies its
// arithmetic independently; this is not the conceptual end of the timeline.
[[nodiscard]] constexpr ProjectFramePosition maximumSupportedProjectFrame()
    noexcept {
    return {(std::int64_t{1} << 53) - 1};
}
[[nodiscard]] constexpr bool isSupportedProjectFramePosition(
    ProjectFramePosition position) noexcept {
    return position.value >= 0 &&
           position.value <= maximumSupportedProjectFrame().value;
}

// Compare without adding a small DSP residue to an absolute double locator.
// Near a boundary the whole-part difference is small and preserves the residue;
// far away its rounding cannot change the sign of the comparison.
[[nodiscard]] inline double distanceToProjectBoundary(
    ProjectFramePosition position, double phase, double boundary) noexcept {
    if (!std::isfinite(boundary) || boundary < 0.0 ||
        boundary > static_cast<double>(maximumSupportedProjectFrame().value))
        return boundary - static_cast<double>(position.value) - phase;
    const auto whole = static_cast<std::int64_t>(std::floor(boundary));
    return static_cast<double>(whole - position.value) +
           ((boundary - static_cast<double>(whole)) - phase);
}

[[nodiscard]] Seconds sourceFramesToSeconds(SourceFrameCount frames,
                                            SampleRate sourceSampleRate) noexcept;
// ceil(integer start + local duration), without rounding an absolute sum.
[[nodiscard]] std::optional<ProjectFramePosition> checkedExclusiveProjectEnd(
    ProjectFramePosition start, ProjectFrameDuration duration) noexcept;
[[nodiscard]] Seconds sourcePositionToSeconds(SourceFramePosition position,
                                              SampleRate sourceSampleRate) noexcept;
[[nodiscard]] Seconds projectFramesToSeconds(ProjectFrameCount frames,
                                             SampleRate projectSampleRate) noexcept;
[[nodiscard]] Seconds projectPositionToSeconds(ProjectFramePosition position,
                                               SampleRate projectSampleRate) noexcept;
[[nodiscard]] std::optional<Seconds> checkedProjectPositionToSeconds(
    ProjectFramePosition position, SampleRate projectSampleRate) noexcept;
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
