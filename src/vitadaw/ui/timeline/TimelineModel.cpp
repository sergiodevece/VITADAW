#include "vitadaw/ui/timeline/TimelineModel.h"

#include <algorithm>
#include <array>
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

::vitadaw::timeline::ProjectFramePosition supportedFrame(
    ::vitadaw::timeline::ProjectFramePosition frame) noexcept {
    return {std::clamp(frame.value, std::int64_t{},
        ::vitadaw::timeline::maximumSupportedProjectFrame().value)};
}

std::uint64_t frameDistance(
    ::vitadaw::timeline::ProjectFramePosition a,
    ::vitadaw::timeline::ProjectFramePosition b) noexcept {
    return a.value >= b.value
        ? static_cast<std::uint64_t>(a.value - b.value)
        : static_cast<std::uint64_t>(b.value - a.value);
}
}

TimelineSnapshot makeTimelineSnapshot(const project::ProjectState& project,
                                      std::uint64_t revision,
                                      const transport::TransportState& transport) {
    TimelineSnapshot result;
    result.projectSampleRate = project.sampleRate();
    result.contentDuration = project.projectContentDuration();
    result.transportPosition = transport.position;
    result.playback = transport.playback;
    result.revision = revision;
    result.tracks.reserve(project.tracks().size());
    for (const auto& track : project.tracks()) {
        TrackSnapshot lane{track.id, track.name, track.layout, {}};
        lane.clips.reserve(track.clips.size());
        for (const auto& clip : track.clips)
        {
            const auto* source = project.findSource(clip.source);
            const auto ratio = source == nullptr
                ? 1.0 : source->sampleRate.hertz() / project.sampleRate().hertz();
            lane.clips.push_back({clip.id, clip.source, clip.projectStart,
                                  clip.duration, clip.sourceOffset,
                                  ratio,
                                  clipLabel(project, clip.source)});
        }
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

SnapResult SnapPolicy::resolve(
    ::vitadaw::timeline::ProjectFramePosition raw,
    std::int64_t toleranceFrames, bool enabled,
    std::span<const SnapTarget> structuralTargets,
    const musical::PreparedMusicalTimeMap* musicalTime) noexcept {
    raw = supportedFrame(raw);
    if (!enabled || toleranceFrames < 0) return {raw, {}};

    std::optional<SnapTarget> best;
    auto consider = [&](SnapTarget candidate) noexcept {
        if (!::vitadaw::timeline::isSupportedProjectFramePosition(candidate.frame))
            return;
        const auto distance = frameDistance(raw, candidate.frame);
        if (distance > static_cast<std::uint64_t>(toleranceFrames)) return;
        if (!best) {
            best = candidate;
            return;
        }
        const auto bestDistance = frameDistance(raw, best->frame);
        if (distance < bestDistance ||
            (distance == bestDistance &&
             (candidate.kind < best->kind ||
              (candidate.kind == best->kind &&
               candidate.frame.value < best->frame.value)))) {
            best = candidate;
        }
    };
    for (const auto target : structuralTargets) consider(target);

    if (musicalTime != nullptr) {
        const auto maximum = ::vitadaw::timeline::maximumSupportedProjectFrame().value;
        const auto start = raw.value > toleranceFrames
            ? raw.value - toleranceFrames : std::int64_t{};
        const auto end = raw.value > maximum - toleranceFrames
            ? maximum : raw.value + toleranceFrames;
        std::array<musical::GridLine, 128> lines;
        auto next = ::vitadaw::timeline::PreciseProjectFramePosition{
            static_cast<double>(start)};
        const auto exclusiveEnd = ::vitadaw::timeline::PreciseProjectFramePosition{
            static_cast<double>(end) + 1.0};
        for (;;) {
            const auto grid = musicalTime->enumerateGridLines(
                next, exclusiveEnd,
                {musical::GridKind::beats, 1}, lines);
            if (grid.error != musical::Error::none) break;
            for (std::size_t index = 0; index < grid.count; ++index) {
                const auto tick = musicalTime->tickAt(lines[index].position);
                if (!tick) continue;
                const auto frame = musicalTime->projectFrameAt(
                    tick.value, musical::Rounding::nearest);
                if (frame) consider({frame.value, SnapTargetKind::beatGrid});
            }
            if (!grid.hasMore || grid.count == 0) break;
            next = grid.nextStart;
        }
    }
    return best ? SnapResult{best->frame, best->kind} : SnapResult{raw, {}};
}

const ClipSnapshot* TimelineInteraction::find(const TimelineSnapshot& snapshot,
                                               clips::ClipId id) noexcept {
    for (const auto& track : snapshot.tracks)
        for (const auto& clip : track.clips)
            if (clip.id == id) return &clip;
    return nullptr;
}
void TimelineInteraction::select(std::optional<clips::ClipId> clip) noexcept {
    selection_.clear();
    if (clip) selection_.push_back(*clip);
    if (!clip) selectedTrack_.reset();
    cancelGesture();
}
void TimelineInteraction::clearTimeSelection() noexcept {
    timeSelection_.reset();
    timeSelectionAnchor_.reset();
}
void TimelineInteraction::prepareSnapTargets(const TimelineSnapshot& snapshot,
                                             bool excludeSelectedClips) {
    snapTargets_.clear();
    snapTargets_.push_back({{}, SnapTargetKind::frameZero});
    if (::vitadaw::timeline::isSupportedProjectFramePosition(
            snapshot.transportPosition)) {
        snapTargets_.push_back(
            {snapshot.transportPosition, SnapTargetKind::playhead});
    }
    if (timeSelection_) {
        snapTargets_.push_back(
            {timeSelection_->startFrame, SnapTargetKind::timeSelectionEdge});
        snapTargets_.push_back(
            {timeSelection_->exclusiveEndFrame,
             SnapTargetKind::timeSelectionEdge});
    }
    for (const auto& track : snapshot.tracks) {
        for (const auto& clip : track.clips) {
            if (excludeSelectedClips && isSelected(clip.id)) continue;
            if (::vitadaw::timeline::isSupportedProjectFramePosition(
                    clip.projectStart)) {
                snapTargets_.push_back(
                    {clip.projectStart, SnapTargetKind::clipEdge});
            }
            const auto end = ::vitadaw::timeline::checkedExclusiveProjectEnd(
                clip.projectStart, clip.duration);
            if (end && ::vitadaw::timeline::isSupportedProjectFramePosition(*end))
                snapTargets_.push_back({*end, SnapTargetKind::clipEdge});
        }
    }
}
::vitadaw::timeline::ProjectFramePosition TimelineInteraction::snapped(
    ::vitadaw::timeline::ProjectFramePosition raw,
    const musical::PreparedMusicalTimeMap& musicalTime,
    std::int64_t toleranceFrames) const noexcept {
    return SnapPolicy::resolve(raw, toleranceFrames, snapEnabled_, snapTargets_,
                               &musicalTime).frame;
}
void TimelineInteraction::beginTimeSelection(
    ::vitadaw::timeline::ProjectFramePosition anchor,
    const TimelineSnapshot& snapshot,
    const musical::PreparedMusicalTimeMap& musicalTime,
    std::int64_t snapToleranceFrames) {
    cancelGesture();
    prepareSnapTargets(snapshot, false);
    timeSelectionAnchor_ = snapped(supportedFrame(anchor), musicalTime,
                                   snapToleranceFrames);
}
void TimelineInteraction::updateTimeSelection(
    ::vitadaw::timeline::ProjectFramePosition current,
    const musical::PreparedMusicalTimeMap& musicalTime,
    std::int64_t snapToleranceFrames) noexcept {
    if (!timeSelectionAnchor_) return;
    current = snapped(supportedFrame(current), musicalTime,
                      snapToleranceFrames);
    const auto start = std::min(timeSelectionAnchor_->value, current.value);
    const auto end = std::max(timeSelectionAnchor_->value, current.value);
    if (start == end) timeSelection_.reset();
    else timeSelection_ = TimeSelection{{start}, {end}};
}
bool TimelineInteraction::endTimeSelection() noexcept {
    const auto result = timeSelection_.has_value();
    timeSelectionAnchor_.reset();
    snapTargets_.clear();
    if (!result) timeSelection_.reset();
    return result;
}
std::optional<commands::Command>
TimelineInteraction::setLoopFromTimeSelectionCommand(
    const musical::PreparedMusicalTimeMap& musicalTime) const noexcept {
    if (!timeSelection_ || !timeSelection_->isValid()) return {};
    const auto start = musicalTime.absoluteTickAt(timeSelection_->startFrame);
    auto end = musicalTime.absoluteTickAt(timeSelection_->exclusiveEndFrame);
    if (!start || !end) return {};
    const auto exactEnd = musicalTime.exactProjectFrameAtTick(end.value);
    if (!exactEnd) return {};
    const audio::exact::Position selectionEnd{
        timeSelection_->exclusiveEndFrame.value, {}};
    if (audio::exact::comparePositions(exactEnd.value, selectionEnd) < 0) {
        if (end.value.value == musical::maximumCoordinate) return {};
        ++end.value.value;
    }
    const musical::MusicalLoopRange range{start.value, end.value};
    if (!range.isStructurallyValid()) return {};
    return commands::SetLoopRangeMusical{range.start, range.end};
}
void TimelineInteraction::selectClips(std::span<const clips::ClipId> clips) {
    selection_.assign(clips.begin(), clips.end());
    std::sort(selection_.begin(), selection_.end());
    selection_.erase(std::unique(selection_.begin(), selection_.end()),
                     selection_.end());
    if (selection_.empty()) selectedTrack_.reset();
    cancelGesture();
}
bool TimelineInteraction::isSelected(clips::ClipId clip) const noexcept {
    return std::binary_search(selection_.begin(), selection_.end(), clip);
}
void TimelineInteraction::toggleClip(tracks::TrackId track,
                                     clips::ClipId clip) {
    const auto found = std::lower_bound(selection_.begin(), selection_.end(), clip);
    if (found != selection_.end() && *found == clip) selection_.erase(found);
    else selection_.insert(found, clip);
    selectedTrack_ = track;
    cancelGesture();
}
void TimelineInteraction::selectTrack(
    std::optional<tracks::TrackId> track) noexcept {
    selectedTrack_ = track;
    selection_.clear();
    cancelGesture();
}
void TimelineInteraction::selectClip(tracks::TrackId track,
                                     clips::ClipId clip) noexcept {
    selectedTrack_ = track;
    if (!isSelected(clip)) {
        selection_.assign(1, clip);
    }
    cancelGesture();
}
void TimelineInteraction::reconcile(const TimelineSnapshot& snapshot) noexcept {
    selection_.erase(
        std::remove_if(selection_.begin(), selection_.end(),
                       [&](const auto id) { return find(snapshot, id) == nullptr; }),
        selection_.end());
    if (selectedTrack_ && std::none_of(
            snapshot.tracks.begin(), snapshot.tracks.end(),
            [&](const auto& track) { return track.id == *selectedTrack_; })) {
        selectedTrack_.reset();
    }
    cancelGesture();
}
bool TimelineInteraction::beginGesture(GestureKind kind, const ClipSnapshot& clip,
                                       double pointerX) noexcept {
    if (kind == GestureKind::none || !std::isfinite(pointerX) ||
        clip.projectStart.value < 0 || clip.duration.value <= 0.0) return false;
    selection_.assign(1, clip.id);
    gesture_ = kind;
    original_ = {clip.id, {}, clip.projectStart, clip.duration,
                 clip.sourceOffset, true};
    originalSourceFramesPerProjectFrame_ = clip.sourceFramesPerProjectFrame;
    originals_.assign(1, original_);
    previews_.assign(1, original_);
    pointerOriginX_ = pointerX;
    return true;
}
bool TimelineInteraction::beginGesture(GestureKind kind,
                                       const TrackSnapshot& track,
                                       const ClipSnapshot& clip,
                                       double pointerX) noexcept {
    if (!beginGesture(kind, clip, pointerX)) return false;
    selectedTrack_ = track.id;
    original_.track = track.id;
    originalLayout_ = track.layout;
    originals_.assign(1, original_);
    previews_.assign(1, original_);
    return true;
}
bool TimelineInteraction::beginGesture(GestureKind kind,
                                       const TimelineSnapshot& snapshot,
                                       const TrackSnapshot& track,
                                       const ClipSnapshot& clip,
                                       double pointerX) {
    if (!beginGesture(kind, track, clip, pointerX)) return false;
    prepareSnapTargets(snapshot);
    return true;
}
bool TimelineInteraction::beginMoveGesture(
    const TimelineSnapshot& snapshot, const TrackSnapshot& track,
    const ClipSnapshot& clip, double pointerX) {
    if (!isSelected(clip.id) || selection_.size() <= 1) {
        return beginGesture(GestureKind::move, snapshot, track, clip, pointerX);
    }
    if (!std::isfinite(pointerX)) return false;
    gesture_ = GestureKind::move;
    originals_.clear();
    previews_.clear();
    originals_.reserve(selection_.size());
    previews_.reserve(selection_.size());
    for (const auto id : selection_) {
        for (const auto& lane : snapshot.tracks) {
            const auto found = std::find_if(
                lane.clips.begin(), lane.clips.end(),
                [id](const auto& candidate) { return candidate.id == id; });
            if (found == lane.clips.end()) continue;
            ClipPreview value{id, lane.id, found->projectStart, found->duration,
                              found->sourceOffset, true};
            originals_.push_back(value);
            previews_.push_back(value);
            break;
        }
    }
    if (originals_.size() != selection_.size()) {
        cancelGesture();
        return false;
    }
    const auto leader = std::find_if(
        originals_.begin(), originals_.end(),
        [&](const auto& candidate) { return candidate.id == clip.id; });
    original_ = *leader;
    selectedTrack_ = track.id;
    pointerOriginX_ = pointerX;
    prepareSnapTargets(snapshot);
    return true;
}
void TimelineInteraction::updateGesture(double pointerX,
                                        const CoordinateTransform& transform) noexcept {
    if (previews_.empty() || gesture_ == GestureKind::none || !std::isfinite(pointerX)) return;
    const auto origin = transform.xToProjectFrame(pointerOriginX_);
    const auto current = transform.xToProjectFrame(pointerX);
    const auto delta = static_cast<long double>(current.value) -
                       static_cast<long double>(origin.value);
    constexpr double minimumDurationFrames = 1.0;
    if (gesture_ == GestureKind::move) {
        auto minimumDelta = static_cast<long double>(
            std::numeric_limits<std::int64_t>::min());
        auto maximumDelta = static_cast<long double>(
            std::numeric_limits<std::int64_t>::max());
        for (const auto& original : originals_) {
            minimumDelta = std::max(
                minimumDelta,
                -static_cast<long double>(original.projectStart.value));
            maximumDelta = std::min(
                maximumDelta,
                static_cast<long double>(std::numeric_limits<std::int64_t>::max()) -
                    static_cast<long double>(original.projectStart.value));
        }
        const auto appliedDelta = std::clamp(delta, minimumDelta, maximumDelta);
        for (std::size_t index = 0; index < previews_.size(); ++index) {
            const auto position =
                static_cast<long double>(originals_[index].projectStart.value) +
                appliedDelta;
            previews_[index].projectStart.value =
                static_cast<std::int64_t>(std::llround(position));
        }
    } else if (gesture_ == GestureKind::trimLeft) {
        const auto originalEnd = static_cast<double>(original_.projectStart.value) + original_.duration.value;
        const auto start = std::clamp<double>(
            static_cast<double>(static_cast<long double>(original_.projectStart.value) + delta),
            0.0, originalEnd - minimumDurationFrames);
        previews_.front().projectStart.value = static_cast<std::int64_t>(std::llround(start));
        previews_.front().duration.value = originalEnd - static_cast<double>(previews_.front().projectStart.value);
        const auto projectDelta = static_cast<double>(previews_.front().projectStart.value -
                                                      original_.projectStart.value);
        previews_.front().sourceOffset.value = original_.sourceOffset.value +
            projectDelta * originalSourceFramesPerProjectFrame_;
    } else {
        const auto maximumDuration = static_cast<double>(
            std::numeric_limits<std::int64_t>::max() - original_.projectStart.value);
        previews_.front().duration.value = std::clamp(
            original_.duration.value + static_cast<double>(delta),
            minimumDurationFrames, maximumDuration);
    }
}
void TimelineInteraction::updateGesture(
    double pointerX, const CoordinateTransform& transform,
    const TimelineSnapshot& snapshot,
    std::optional<tracks::TrackId> targetTrack) noexcept {
    updateGesture(pointerX, transform);
    if (previews_.empty() || gesture_ != GestureKind::move ||
        !original_.track.isValid()) return;
    if (previews_.size() > 1) return;
    auto& preview = previews_.front();
    preview.validTarget = false;
    preview.track = {};
    if (!targetTrack) return;
    const auto found = std::find_if(
        snapshot.tracks.begin(), snapshot.tracks.end(),
        [&](const auto& track) { return track.id == *targetTrack; });
    if (found == snapshot.tracks.end()) return;
    preview.track = found->id;
    preview.validTarget = found->layout == originalLayout_;
}
void TimelineInteraction::updateGesture(
    double pointerX, const CoordinateTransform& transform,
    const TimelineSnapshot& snapshot,
    std::optional<tracks::TrackId> targetTrack,
    const musical::PreparedMusicalTimeMap& musicalTime,
    std::int64_t snapToleranceFrames) noexcept {
    updateGesture(pointerX, transform, snapshot, targetTrack);
    if (!snapEnabled_ || previews_.empty() || gesture_ == GestureKind::none)
        return;
    const auto origin = transform.xToProjectFrame(pointerOriginX_);
    const auto current = transform.xToProjectFrame(pointerX);
    const auto delta = static_cast<long double>(current.value) -
                       static_cast<long double>(origin.value);
    if (gesture_ == GestureKind::move) {
        const auto raw = supportedFrame({static_cast<std::int64_t>(std::clamp(
            static_cast<long double>(original_.projectStart.value) + delta,
            static_cast<long double>(0),
            static_cast<long double>(
                ::vitadaw::timeline::maximumSupportedProjectFrame().value)))});
        const auto target = snapped(raw, musicalTime, snapToleranceFrames);
        auto minimumDelta = std::int64_t{std::numeric_limits<std::int64_t>::min()};
        auto maximumDelta = std::int64_t{std::numeric_limits<std::int64_t>::max()};
        for (const auto& original : originals_) {
            minimumDelta = std::max(minimumDelta, -original.projectStart.value);
            maximumDelta = std::min(maximumDelta,
                ::vitadaw::timeline::maximumSupportedProjectFrame().value -
                    original.projectStart.value);
        }
        const auto applied = std::clamp(
            target.value - original_.projectStart.value,
            minimumDelta, maximumDelta);
        for (std::size_t index = 0; index < previews_.size(); ++index) {
            previews_[index].projectStart = {
                originals_[index].projectStart.value + applied};
        }
    } else if (gesture_ == GestureKind::trimLeft) {
        const auto target = snapped(previews_.front().projectStart, musicalTime,
                                    snapToleranceFrames);
        const auto end = static_cast<double>(original_.projectStart.value) +
                         original_.duration.value;
        if (static_cast<double>(target.value) < end) {
            previews_.front().projectStart = target;
            previews_.front().duration.value = end - target.value;
            previews_.front().sourceOffset.value = original_.sourceOffset.value +
                static_cast<double>(target.value - original_.projectStart.value) *
                    originalSourceFramesPerProjectFrame_;
        }
    } else if (gesture_ == GestureKind::trimRight) {
        const auto rawEnd = ::vitadaw::timeline::checkedExclusiveProjectEnd(
            original_.projectStart, previews_.front().duration);
        if (rawEnd) {
            const auto target = snapped(*rawEnd, musicalTime, snapToleranceFrames);
            if (target.value > original_.projectStart.value)
                previews_.front().duration.value = static_cast<double>(
                    target.value - original_.projectStart.value);
        }
    }
}

std::optional<tracks::TrackId> TimelineInteraction::trackAtVerticalPosition(
    const TimelineSnapshot& snapshot, double pointerY,
    double contentTop, double laneHeight, double verticalOffset) noexcept {
    if (!std::isfinite(pointerY) || !std::isfinite(contentTop) ||
        !std::isfinite(laneHeight) || laneHeight <= 0.0 ||
        !std::isfinite(verticalOffset)) return {};
    const auto logical = pointerY - contentTop + verticalOffset;
    if (logical < 0.0) return {};
    const auto index = static_cast<std::size_t>(std::floor(logical / laneHeight));
    return index < snapshot.tracks.size()
        ? std::optional<tracks::TrackId>{snapshot.tracks[index].id}
        : std::nullopt;
}
std::optional<commands::Command> TimelineInteraction::endGesture() noexcept {
    if (previews_.empty()) return {};
    std::optional<commands::Command> result;
    const auto& preview = previews_.front();
    if (gesture_ == GestureKind::move && previews_.size() > 1) {
        const auto delta = preview.projectStart.value - originals_.front().projectStart.value;
        if (delta != 0) result = commands::MoveClips{selection_, delta};
    } else if (gesture_ == GestureKind::move && preview.validTarget &&
        (preview.projectStart != original_.projectStart ||
         preview.track != original_.track)) {
        result = original_.track.isValid()
            ? std::optional<commands::Command>{commands::MoveClip{
                  original_.id, preview.track, preview.projectStart}}
            : std::optional<commands::Command>{commands::MoveClip{
                  original_.id, preview.projectStart}};
    }
    else if (gesture_ == GestureKind::trimLeft && preview.projectStart != original_.projectStart)
        result = commands::TrimClipLeft{original_.id, preview.projectStart};
    else if (gesture_ == GestureKind::trimRight && preview.duration != original_.duration) {
        const auto end = static_cast<double>(original_.projectStart.value) + preview.duration.value;
        result = commands::TrimClipRight{original_.id,
            {static_cast<std::int64_t>(std::llround(end))}};
    }
    cancelGesture();
    return result;
}
void TimelineInteraction::cancelGesture() noexcept {
    gesture_ = GestureKind::none;
    originals_.clear();
    previews_.clear();
    snapTargets_.clear();
}
const ClipPreview* TimelineInteraction::previewFor(clips::ClipId clip) const noexcept {
    const auto found = std::find_if(
        previews_.begin(), previews_.end(),
        [clip](const auto& candidate) { return candidate.id == clip; });
    return found == previews_.end() ? nullptr : &*found;
}
std::optional<commands::Command> TimelineInteraction::duplicateCommand(
    const TimelineSnapshot& snapshot) const noexcept {
    if (selection_.empty()) return {};
    auto minimum = std::numeric_limits<std::int64_t>::max();
    std::int64_t maximum{};
    for (const auto id : selection_) {
        const auto* clip = find(snapshot, id);
        if (!clip) return {};
        const auto end = ::vitadaw::timeline::checkedExclusiveProjectEnd(
            clip->projectStart, clip->duration);
        if (!end) return {};
        minimum = std::min(minimum, clip->projectStart.value);
        maximum = std::max(maximum, end->value);
    }
    return commands::DuplicateClips{selection_, maximum - minimum};
}
std::optional<commands::Command> TimelineInteraction::splitCommand(
    const TimelineSnapshot& snapshot,
    ::vitadaw::timeline::ProjectFramePosition playhead) const noexcept {
    if (selection_.size() != 1) return {};
    const auto* clip = find(snapshot, selection_.front());
    if (!clip) return {};
    const auto end = static_cast<double>(clip->projectStart.value) + clip->duration.value;
    if (playhead.value <= clip->projectStart.value || static_cast<double>(playhead.value) >= end) return {};
    return commands::SplitClip{clip->id, playhead};
}
std::optional<commands::Command> TimelineInteraction::deleteCommand() const noexcept {
    return selection_.empty()
        ? std::nullopt
        : std::optional<commands::Command>{commands::DeleteClips{selection_}};
}
} // namespace vitadaw::ui::timeline
