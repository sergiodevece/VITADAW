#pragma once

#include "vitadaw/commands/Command.h"
#include "vitadaw/project/ProjectState.h"
#include "vitadaw/transport/TransportState.h"

#include <optional>
#include <string>
#include <vector>

namespace vitadaw::ui::timeline {

struct ClipSnapshot {
    clips::ClipId id;
    media::SourceId source;
    ::vitadaw::timeline::ProjectFramePosition projectStart;
    ::vitadaw::timeline::ProjectFrameDuration duration;
    std::string label;
    bool operator==(const ClipSnapshot&) const = default;
};

struct TrackSnapshot {
    tracks::TrackId id;
    std::string name;
    std::vector<ClipSnapshot> clips;
    bool operator==(const TrackSnapshot&) const = default;
};

struct TimelineSnapshot {
    ::vitadaw::timeline::SampleRate projectSampleRate;
    ::vitadaw::timeline::ProjectFrameCount contentDuration;
    ::vitadaw::timeline::ProjectFramePosition transportPosition;
    transport::PlaybackState playback{transport::PlaybackState::stopped};
    std::uint64_t revision{};
    std::vector<TrackSnapshot> tracks;
    std::uint64_t musicalRevision{};
    std::optional<musical::MusicalPosition> musicalPosition;
    bool operator==(const TimelineSnapshot&) const = default;
};

[[nodiscard]] TimelineSnapshot makeTimelineSnapshot(
    const project::ProjectState& project, std::uint64_t revision,
    const transport::TransportState& transport = {});

class CoordinateTransform {
public:
    static constexpr double minimumPixelsPerSecond = 20.0;
    static constexpr double maximumPixelsPerSecond = 600.0;

    CoordinateTransform(::vitadaw::timeline::SampleRate sampleRate = {},
                        double pixelsPerSecond = 100.0,
                        double visibleStartSeconds = 0.0) noexcept;

    [[nodiscard]] bool isValid() const noexcept;
    [[nodiscard]] double projectFrameToX(
        ::vitadaw::timeline::ProjectFramePosition frame) const noexcept;
    [[nodiscard]] double preciseProjectFrameToX(double frame) const noexcept;
    [[nodiscard]] ::vitadaw::timeline::ProjectFramePosition xToProjectFrame(
        double x) const noexcept;
    [[nodiscard]] double pixelsPerSecond() const noexcept { return pixelsPerSecond_; }
    [[nodiscard]] double visibleStartSeconds() const noexcept { return visibleStartSeconds_; }
    void setVisibleStartSeconds(double seconds) noexcept;
    void setPixelsPerSecond(double value) noexcept;
    void zoomAround(double newPixelsPerSecond, double anchorX) noexcept;

private:
    ::vitadaw::timeline::SampleRate sampleRate_;
    double pixelsPerSecond_{100.0};
    double visibleStartSeconds_{};
};

enum class GestureKind { none, move, trimLeft, trimRight };

struct ClipPreview {
    clips::ClipId id;
    ::vitadaw::timeline::ProjectFramePosition projectStart;
    ::vitadaw::timeline::ProjectFrameDuration duration;
};

// Ephemeral presentation state only. It never owns or mutates ProjectState.
class TimelineInteraction {
public:
    [[nodiscard]] std::optional<clips::ClipId> selection() const noexcept { return selection_; }
    [[nodiscard]] const std::optional<ClipPreview>& preview() const noexcept { return preview_; }
    void select(std::optional<clips::ClipId> clip) noexcept;
    void reconcile(const TimelineSnapshot& snapshot) noexcept;
    [[nodiscard]] bool beginGesture(GestureKind kind, const ClipSnapshot& clip,
                                    double pointerX) noexcept;
    void updateGesture(double pointerX, const CoordinateTransform& transform) noexcept;
    [[nodiscard]] std::optional<commands::Command> endGesture() noexcept;
    void cancelGesture() noexcept;
    [[nodiscard]] std::optional<commands::Command> duplicateCommand(
        const TimelineSnapshot& snapshot) const noexcept;
    [[nodiscard]] std::optional<commands::Command> splitCommand(
        const TimelineSnapshot& snapshot,
        ::vitadaw::timeline::ProjectFramePosition playhead) const noexcept;
    [[nodiscard]] std::optional<commands::Command> deleteCommand() const noexcept;

private:
    [[nodiscard]] static const ClipSnapshot* find(
        const TimelineSnapshot&, clips::ClipId) noexcept;
    std::optional<clips::ClipId> selection_;
    GestureKind gesture_{GestureKind::none};
    ClipPreview original_{};
    std::optional<ClipPreview> preview_;
    double pointerOriginX_{};
};

} // namespace vitadaw::ui::timeline
