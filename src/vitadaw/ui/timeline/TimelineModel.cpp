#include "vitadaw/ui/timeline/TimelineModel.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace vitadaw::ui::timeline {
namespace {
std::string clipLabel(const project::ProjectState& project, media::SourceId id) {
    const auto* source = project.findSource(id);
    if (source == nullptr) return "Source " + std::to_string(id.value);
    const auto& path = source->media.originalPath.empty() && source->media.projectRelativePath
                           ? *source->media.projectRelativePath
                           : source->media.originalPath;
    const auto filename = path.filename().string();
    return filename.empty() ? "Source " + std::to_string(id.value) : filename;
}
}

TimelineSnapshot makeTimelineSnapshot(const project::ProjectState& project,
                                      std::uint64_t revision,
                                      const transport::TransportState& transport) {
    TimelineSnapshot result{project.sampleRate(), project.projectContentDuration(),
                            transport.position, transport.playback, revision, {}};
    result.tracks.reserve(project.tracks().size());
    for (const auto& track : project.tracks()) {
        TrackSnapshot lane{track.id, track.name, {}};
        lane.clips.reserve(track.clips.size());
        for (const auto& clip : track.clips)
            lane.clips.push_back({clip.id, clip.source, clip.projectStart,
                                  clip.duration, clipLabel(project, clip.source)});
        result.tracks.push_back(std::move(lane));
    }
    return result;
}

CoordinateTransform::CoordinateTransform(::vitadaw::timeline::SampleRate sampleRate,
    double pixelsPerSecond, double visibleStartSeconds) noexcept : sampleRate_(sampleRate) {
    setPixelsPerSecond(pixelsPerSecond);
    setVisibleStartSeconds(visibleStartSeconds);
}
bool CoordinateTransform::isValid() const noexcept {
    return sampleRate_.isValid() && std::isfinite(pixelsPerSecond_) &&
           pixelsPerSecond_ >= minimumPixelsPerSecond &&
           pixelsPerSecond_ <= maximumPixelsPerSecond &&
           std::isfinite(visibleStartSeconds_) && visibleStartSeconds_ >= 0.0;
}
double CoordinateTransform::preciseProjectFrameToX(double frame) const noexcept {
    if (!isValid() || !std::isfinite(frame)) return 0.0;
    return (frame / sampleRate_.hertz() - visibleStartSeconds_) * pixelsPerSecond_;
}
double CoordinateTransform::projectFrameToX(
    ::vitadaw::timeline::ProjectFramePosition frame) const noexcept {
    return preciseProjectFrameToX(static_cast<double>(frame.value));
}
::vitadaw::timeline::ProjectFramePosition CoordinateTransform::xToProjectFrame(double x) const noexcept {
    if (!isValid() || !std::isfinite(x)) return {};
    const auto seconds = std::max(0.0, visibleStartSeconds_ + x / pixelsPerSecond_);
    const auto frames = seconds * sampleRate_.hertz();
    if (frames >= static_cast<double>(std::numeric_limits<std::int64_t>::max()))
        return {std::numeric_limits<std::int64_t>::max()};
    return {static_cast<std::int64_t>(std::llround(frames))};
}
void CoordinateTransform::setVisibleStartSeconds(double seconds) noexcept {
    visibleStartSeconds_ = std::isfinite(seconds) ? std::max(0.0, seconds) : 0.0;
}
void CoordinateTransform::setPixelsPerSecond(double value) noexcept {
    pixelsPerSecond_ = std::clamp(std::isfinite(value) ? value : 100.0,
                                  minimumPixelsPerSecond, maximumPixelsPerSecond);
}
void CoordinateTransform::zoomAround(double newPixelsPerSecond, double anchorX) noexcept {
    if (!isValid() || !std::isfinite(anchorX)) return;
    const auto anchorSeconds = visibleStartSeconds_ + anchorX / pixelsPerSecond_;
    setPixelsPerSecond(newPixelsPerSecond);
    setVisibleStartSeconds(anchorSeconds - anchorX / pixelsPerSecond_);
}

const ClipSnapshot* TimelineInteraction::find(const TimelineSnapshot& snapshot,
                                               clips::ClipId id) noexcept {
    for (const auto& track : snapshot.tracks)
        for (const auto& clip : track.clips)
            if (clip.id == id) return &clip;
    return nullptr;
}
void TimelineInteraction::select(std::optional<clips::ClipId> clip) noexcept {
    selection_ = clip;
    cancelGesture();
}
void TimelineInteraction::reconcile(const TimelineSnapshot& snapshot) noexcept {
    if (selection_ && find(snapshot, *selection_) == nullptr) selection_.reset();
    cancelGesture();
}
bool TimelineInteraction::beginGesture(GestureKind kind, const ClipSnapshot& clip,
                                       double pointerX) noexcept {
    if (kind == GestureKind::none || !std::isfinite(pointerX) ||
        clip.projectStart.value < 0 || clip.duration.value <= 0.0) return false;
    selection_ = clip.id;
    gesture_ = kind;
    original_ = {clip.id, clip.projectStart, clip.duration};
    preview_ = original_;
    pointerOriginX_ = pointerX;
    return true;
}
void TimelineInteraction::updateGesture(double pointerX,
                                        const CoordinateTransform& transform) noexcept {
    if (!preview_ || gesture_ == GestureKind::none || !std::isfinite(pointerX)) return;
    const auto origin = transform.xToProjectFrame(pointerOriginX_);
    const auto current = transform.xToProjectFrame(pointerX);
    const auto delta = static_cast<long double>(current.value) -
                       static_cast<long double>(origin.value);
    constexpr double minimumDurationFrames = 1.0;
    if (gesture_ == GestureKind::move) {
        const auto position = std::clamp<long double>(
            static_cast<long double>(original_.projectStart.value) + delta, 0.0L,
            static_cast<long double>(std::numeric_limits<std::int64_t>::max()));
        preview_->projectStart.value = static_cast<std::int64_t>(std::llround(position));
    } else if (gesture_ == GestureKind::trimLeft) {
        const auto originalEnd = static_cast<double>(original_.projectStart.value) + original_.duration.value;
        const auto start = std::clamp<double>(
            static_cast<double>(static_cast<long double>(original_.projectStart.value) + delta),
            0.0, originalEnd - minimumDurationFrames);
        preview_->projectStart.value = static_cast<std::int64_t>(std::llround(start));
        preview_->duration.value = originalEnd - static_cast<double>(preview_->projectStart.value);
    } else {
        const auto maximumDuration = static_cast<double>(
            std::numeric_limits<std::int64_t>::max() - original_.projectStart.value);
        preview_->duration.value = std::clamp(
            original_.duration.value + static_cast<double>(delta),
            minimumDurationFrames, maximumDuration);
    }
}
std::optional<commands::Command> TimelineInteraction::endGesture() noexcept {
    if (!preview_) return {};
    std::optional<commands::Command> result;
    if (gesture_ == GestureKind::move && preview_->projectStart != original_.projectStart)
        result = commands::MoveClip{original_.id, preview_->projectStart};
    else if (gesture_ == GestureKind::trimLeft && preview_->projectStart != original_.projectStart)
        result = commands::TrimClipLeft{original_.id, preview_->projectStart};
    else if (gesture_ == GestureKind::trimRight && preview_->duration != original_.duration) {
        const auto end = static_cast<double>(original_.projectStart.value) + preview_->duration.value;
        result = commands::TrimClipRight{original_.id,
            {static_cast<std::int64_t>(std::llround(end))}};
    }
    cancelGesture();
    return result;
}
void TimelineInteraction::cancelGesture() noexcept {
    gesture_ = GestureKind::none;
    preview_.reset();
}
std::optional<commands::Command> TimelineInteraction::duplicateCommand(
    const TimelineSnapshot& snapshot) const noexcept {
    if (!selection_) return {};
    const auto* clip = find(snapshot, *selection_);
    if (!clip) return {};
    const auto end = static_cast<double>(clip->projectStart.value) + clip->duration.value;
    if (!std::isfinite(end) || end >= std::ldexp(1.0, 63)) return {};
    return commands::DuplicateClip{clip->id,
        {static_cast<std::int64_t>(std::llround(end))}};
}
std::optional<commands::Command> TimelineInteraction::splitCommand(
    const TimelineSnapshot& snapshot,
    ::vitadaw::timeline::ProjectFramePosition playhead) const noexcept {
    if (!selection_) return {};
    const auto* clip = find(snapshot, *selection_);
    if (!clip) return {};
    const auto end = static_cast<double>(clip->projectStart.value) + clip->duration.value;
    if (playhead.value <= clip->projectStart.value || static_cast<double>(playhead.value) >= end) return {};
    return commands::SplitClip{clip->id, playhead};
}
std::optional<commands::Command> TimelineInteraction::deleteCommand() const noexcept {
    return selection_ ? std::optional<commands::Command>{commands::DeleteClip{*selection_}}
                      : std::nullopt;
}
} // namespace vitadaw::ui::timeline
