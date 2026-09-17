#pragma once

#include "vitadaw/commands/Command.h"
#include "vitadaw/project/ProjectState.h"
#include "vitadaw/transport/TransportState.h"

#include <optional>
#include <span>
#include <string>
#include <vector>

namespace vitadaw::ui::timeline {

struct ClipSnapshot {
    clips::ClipId id;
    media::SourceId source;
    ::vitadaw::timeline::ProjectFramePosition projectStart;
    ::vitadaw::timeline::ProjectFrameDuration duration;
    ::vitadaw::timeline::SourceFramePosition sourceOffset;
    double sourceFramesPerProjectFrame{1.0};
    std::string label;
    bool operator==(const ClipSnapshot&) const = default;
};

struct TrackSnapshot {
    tracks::TrackId id;
    std::string name;
    media::AudioChannelLayout layout{media::AudioChannelLayout::mono};
    std::vector<ClipSnapshot> clips;
    bool operator==(const TrackSnapshot&) const = default;
};

struct LoopReadModel {
    std::optional<musical::MusicalLoopRange> documentRange;
    bool enabled{};
    std::uint64_t temporalRevision{};
    std::optional<audio::PreparedLoopView> prepared;
    std::optional<musical::MusicalPosition> startPosition;
    std::optional<musical::MusicalPosition> endPosition;
    bool operator==(const LoopReadModel&) const = default;
};

struct MetronomeReadModel {
    bool enabled{};
    audio::MetronomeLevelDb level;
    std::uint64_t temporalRevision{};
    bool operator==(const MetronomeReadModel&) const = default;
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
    LoopReadModel loop;
    MetronomeReadModel metronome;
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

struct TimeSelection {
    ::vitadaw::timeline::ProjectFramePosition startFrame;
    ::vitadaw::timeline::ProjectFramePosition exclusiveEndFrame;

    [[nodiscard]] constexpr bool isValid() const noexcept {
        return ::vitadaw::timeline::isSupportedProjectFramePosition(startFrame) &&
               ::vitadaw::timeline::isSupportedProjectFramePosition(exclusiveEndFrame) &&
               startFrame.value < exclusiveEndFrame.value;
    }
    bool operator==(const TimeSelection&) const = default;
};

// The numeric values are the complete, stable tie-break order. Structural
// arrange anchors win ties before transport, origin and musical grid.
enum class SnapTargetKind : unsigned {
    clipEdge = 0,
    timeSelectionEdge = 1,
    playhead = 2,
    frameZero = 3,
    beatGrid = 4,
};

struct SnapTarget {
    ::vitadaw::timeline::ProjectFramePosition frame;
    SnapTargetKind kind{SnapTargetKind::beatGrid};
    bool operator==(const SnapTarget&) const = default;
};

struct SnapResult {
    ::vitadaw::timeline::ProjectFramePosition frame;
    std::optional<SnapTargetKind> target;
    bool operator==(const SnapResult&) const = default;
};

class SnapPolicy {
public:
    [[nodiscard]] static SnapResult resolve(
        ::vitadaw::timeline::ProjectFramePosition raw,
        std::int64_t toleranceFrames, bool enabled,
        std::span<const SnapTarget> structuralTargets,
        const musical::PreparedMusicalTimeMap* musicalTime = nullptr) noexcept;
};

enum class GestureKind { none, move, trimLeft, trimRight };

struct ClipPreview {
    clips::ClipId id;
    tracks::TrackId track;
    ::vitadaw::timeline::ProjectFramePosition projectStart;
    ::vitadaw::timeline::ProjectFrameDuration duration;
    ::vitadaw::timeline::SourceFramePosition sourceOffset;
    bool validTarget{true};
};

// Ephemeral presentation state only. It never owns or mutates ProjectState.
class TimelineInteraction {
public:
    [[nodiscard]] std::optional<clips::ClipId> selection() const noexcept {
        return selection_.empty()
            ? std::nullopt
            : std::optional<clips::ClipId>{selection_.front()};
    }
    [[nodiscard]] std::span<const clips::ClipId> selections() const noexcept {
        return selection_;
    }
    [[nodiscard]] bool isSelected(clips::ClipId clip) const noexcept;
    [[nodiscard]] std::optional<tracks::TrackId> selectedTrack() const noexcept {
        return selectedTrack_;
    }
    [[nodiscard]] const std::optional<TimeSelection>& timeSelection() const noexcept {
        return timeSelection_;
    }
    [[nodiscard]] bool timeSelectionGestureActive() const noexcept {
        return timeSelectionAnchor_.has_value();
    }
    [[nodiscard]] bool snapEnabled() const noexcept { return snapEnabled_; }
    void setSnapEnabled(bool enabled) noexcept { snapEnabled_ = enabled; }
    void clearTimeSelection() noexcept;
    void beginTimeSelection(
        ::vitadaw::timeline::ProjectFramePosition anchor,
        const TimelineSnapshot& snapshot,
        const musical::PreparedMusicalTimeMap& musicalTime,
        std::int64_t snapToleranceFrames);
    void updateTimeSelection(
        ::vitadaw::timeline::ProjectFramePosition current,
        const musical::PreparedMusicalTimeMap& musicalTime,
        std::int64_t snapToleranceFrames) noexcept;
    [[nodiscard]] bool endTimeSelection() noexcept;
    [[nodiscard]] std::optional<commands::Command>
        setLoopFromTimeSelectionCommand(
            const musical::PreparedMusicalTimeMap& musicalTime) const noexcept;
    [[nodiscard]] const ClipPreview* previewFor(clips::ClipId clip) const noexcept;
    [[nodiscard]] const ClipPreview* preview() const noexcept {
        return previews_.empty() ? nullptr : &previews_.front();
    }
    void select(std::optional<clips::ClipId> clip) noexcept;
    void selectClips(std::span<const clips::ClipId> clips);
    void toggleClip(tracks::TrackId track, clips::ClipId clip);
    void selectTrack(std::optional<tracks::TrackId> track) noexcept;
    void selectClip(tracks::TrackId track, clips::ClipId clip) noexcept;
    void reconcile(const TimelineSnapshot& snapshot) noexcept;
    [[nodiscard]] bool beginGesture(GestureKind kind, const ClipSnapshot& clip,
                                    double pointerX) noexcept;
    [[nodiscard]] bool beginGesture(GestureKind kind,
                                    const TrackSnapshot& track,
                                    const ClipSnapshot& clip,
                                    double pointerX) noexcept;
    [[nodiscard]] bool beginGesture(GestureKind kind,
                                    const TimelineSnapshot& snapshot,
                                    const TrackSnapshot& track,
                                    const ClipSnapshot& clip,
                                    double pointerX);
    [[nodiscard]] bool beginMoveGesture(
        const TimelineSnapshot& snapshot, const TrackSnapshot& track,
        const ClipSnapshot& clip, double pointerX);
    void updateGesture(double pointerX, const CoordinateTransform& transform) noexcept;
    void updateGesture(double pointerX, const CoordinateTransform& transform,
                       const TimelineSnapshot& snapshot,
                       std::optional<tracks::TrackId> targetTrack) noexcept;
    void updateGesture(double pointerX, const CoordinateTransform& transform,
                       const TimelineSnapshot& snapshot,
                       std::optional<tracks::TrackId> targetTrack,
                       const musical::PreparedMusicalTimeMap& musicalTime,
                       std::int64_t snapToleranceFrames) noexcept;
    [[nodiscard]] static std::optional<tracks::TrackId> trackAtVerticalPosition(
        const TimelineSnapshot& snapshot, double pointerY,
        double contentTop, double laneHeight, double verticalOffset) noexcept;
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
    void prepareSnapTargets(const TimelineSnapshot& snapshot,
                            bool excludeSelectedClips = true);
    [[nodiscard]] ::vitadaw::timeline::ProjectFramePosition snapped(
        ::vitadaw::timeline::ProjectFramePosition raw,
        const musical::PreparedMusicalTimeMap& musicalTime,
        std::int64_t toleranceFrames) const noexcept;
    std::vector<clips::ClipId> selection_;
    std::optional<tracks::TrackId> selectedTrack_;
    std::optional<TimeSelection> timeSelection_;
    std::optional<::vitadaw::timeline::ProjectFramePosition> timeSelectionAnchor_;
    std::vector<SnapTarget> snapTargets_;
    bool snapEnabled_{};
    GestureKind gesture_{GestureKind::none};
    ClipPreview original_{};
    std::vector<ClipPreview> originals_;
    media::AudioChannelLayout originalLayout_{media::AudioChannelLayout::mono};
    double originalSourceFramesPerProjectFrame_{1.0};
    std::vector<ClipPreview> previews_;
    double pointerOriginX_{};
};

} // namespace vitadaw::ui::timeline
