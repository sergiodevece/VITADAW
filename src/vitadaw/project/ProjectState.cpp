#include "vitadaw/project/ProjectState.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <type_traits>

namespace vitadaw::project {
namespace {

std::vector<clips::ClipId> canonicalClipIds(
    std::span<const clips::ClipId> input) {
    std::vector<clips::ClipId> result{input.begin(), input.end()};
    std::sort(result.begin(), result.end());
    result.erase(std::unique(result.begin(), result.end()), result.end());
    return result;
}

std::optional<timeline::ProjectFramePosition> shiftedPosition(
    timeline::ProjectFramePosition position, std::int64_t delta) noexcept {
    if ((delta > 0 && position.value >
                          std::numeric_limits<std::int64_t>::max() - delta) ||
        (delta < 0 && position.value <
                          std::numeric_limits<std::int64_t>::min() - delta)) {
        return std::nullopt;
    }
    return timeline::ProjectFramePosition{position.value + delta};
}

} // namespace

ProjectState::ProjectState(timeline::SampleRate projectSampleRate,
                           std::string name)
    : settings_{std::move(name), projectSampleRate} {
    if (!settings_.sampleRate.isValid()) {
        throw std::invalid_argument{"Project sample rate must be positive"};
    }
}

const ProjectState::ProjectSettings& ProjectState::settings() const noexcept {
    return settings_;
}

timeline::SampleRate ProjectState::sampleRate() const noexcept {
    return settings_.sampleRate;
}

const std::vector<tracks::AudioTrack>& ProjectState::tracks() const noexcept {
    return tracks_;
}

const std::vector<media::AudioSource>& ProjectState::sources() const noexcept {
    return sources_;
}

const routing::RoutingState& ProjectState::routing() const noexcept {
    return routing_;
}

tracks::TrackId ProjectState::addAudioTrack(
    std::string name, media::AudioChannelLayout layout) {
    if (!nextTrackId_.isValid() ||
        nextTrackId_.value == std::numeric_limits<std::uint64_t>::max() ||
        tracks_.size() >= maximumTracks ||
        (layout != media::AudioChannelLayout::mono &&
         layout != media::AudioChannelLayout::stereo)) {
        throw std::overflow_error{"Audio track identity space exhausted"};
    }
    const auto id = nextTrackId_;
    if (name.empty()) name = "Audio " + std::to_string(id.value);
    tracks_.push_back({id, std::move(name), layout, {}, {}, {}});
    try {
        routing_.addTrack(id);
    } catch (...) {
        tracks_.pop_back();
        throw;
    }
    ++nextTrackId_.value;
    return id;
}

std::optional<ProjectState::TrackHistoryState>
ProjectState::captureTrackHistoryState(tracks::TrackId track) const {
    const auto trackFound = std::find_if(
        tracks_.begin(), tracks_.end(),
        [track](const auto& candidate) { return candidate.id == track; });
    const auto routeFound = std::find_if(
        routing_.trackRoutes_.begin(), routing_.trackRoutes_.end(),
        [track](const auto& candidate) { return candidate.track == track; });
    if (trackFound == tracks_.end() || routeFound == routing_.trackRoutes_.end()) {
        return std::nullopt;
    }
    TrackHistoryState result{
        static_cast<std::size_t>(trackFound - tracks_.begin()), *trackFound,
        static_cast<std::size_t>(routeFound - routing_.trackRoutes_.begin()),
        *routeFound, {}};
    for (std::size_t index = 0; index < routing_.sends_.size(); ++index) {
        const auto* source =
            std::get_if<tracks::TrackId>(&routing_.sends_[index].source);
        if (source != nullptr && *source == track) {
            result.sends.push_back({index, routing_.sends_[index]});
        }
    }
    return result;
}

std::optional<ProjectState::TrackHistoryState>
ProjectState::removeAudioTrack(tracks::TrackId track) {
    auto snapshot = captureTrackHistoryState(track);
    if (!snapshot) return std::nullopt;
    routing_.sends_.erase(
        std::remove_if(routing_.sends_.begin(), routing_.sends_.end(),
            [track](const auto& send) {
                const auto* source = std::get_if<tracks::TrackId>(&send.source);
                return source != nullptr && *source == track;
            }),
        routing_.sends_.end());
    routing_.trackRoutes_.erase(
        routing_.trackRoutes_.begin() +
        static_cast<std::ptrdiff_t>(snapshot->routeIndex));
    tracks_.erase(tracks_.begin() +
                  static_cast<std::ptrdiff_t>(snapshot->trackIndex));
    return snapshot;
}

ProjectState::TrackReorderResult ProjectState::reorderAudioTrack(
    tracks::TrackId track, tracks::TrackId anchor, bool placeAfter) noexcept {
    const auto moved = std::find_if(
        tracks_.begin(), tracks_.end(),
        [track](const auto& candidate) { return candidate.id == track; });
    const auto anchored = std::find_if(
        tracks_.begin(), tracks_.end(),
        [anchor](const auto& candidate) { return candidate.id == anchor; });
    if (moved == tracks_.end() || anchored == tracks_.end()) {
        return {ClipEditStatus::trackNotFound};
    }
    const auto beforeIndex = static_cast<std::size_t>(moved - tracks_.begin());
    if (track == anchor) {
        return {ClipEditStatus::success, beforeIndex, beforeIndex, false};
    }
    const auto anchorIndex =
        static_cast<std::size_t>(anchored - tracks_.begin());
    const auto anchorAfterRemoval =
        anchorIndex - (beforeIndex < anchorIndex ? 1U : 0U);
    const auto afterIndex = anchorAfterRemoval + (placeAfter ? 1U : 0U);
    if (afterIndex == beforeIndex) {
        return {ClipEditStatus::success, beforeIndex, afterIndex, false};
    }
    if (beforeIndex < afterIndex) {
        std::rotate(tracks_.begin() + static_cast<std::ptrdiff_t>(beforeIndex),
                    tracks_.begin() + static_cast<std::ptrdiff_t>(beforeIndex + 1),
                    tracks_.begin() + static_cast<std::ptrdiff_t>(afterIndex + 1));
    } else {
        std::rotate(tracks_.begin() + static_cast<std::ptrdiff_t>(afterIndex),
                    tracks_.begin() + static_cast<std::ptrdiff_t>(beforeIndex),
                    tracks_.begin() + static_cast<std::ptrdiff_t>(beforeIndex + 1));
    }
    return {ClipEditStatus::success, beforeIndex, afterIndex, true};
}

routing::BusId ProjectState::addBus(std::string name) {
    return routing_.addBus(std::move(name));
}

bool ProjectState::setTrackOutputDestination(
    tracks::TrackId track,
    routing::OutputDestination destination) noexcept {
    return findTrack(track) != nullptr &&
           routing_.setTrackDestination(track, destination);
}

const routing::AudioBus* ProjectState::findBus(
    routing::BusId bus) const noexcept {
    return routing_.findBus(bus);
}

bool ProjectState::setBusMix(routing::BusId bus,
                             mixer::BusMixState state) noexcept {
    return routing_.setBusMix(bus, state);
}

bool ProjectState::setBusOutputDestination(
    routing::BusId bus, routing::OutputDestination destination) noexcept {
    return routing_.setBusDestination(bus, destination);
}

routing::SendId ProjectState::addSend(
    routing::SendSource source, routing::BusId destination,
    routing::SendTapPoint tapPoint, mixer::SendMixState mix) {
    return routing_.addSend(source, destination, tapPoint, mix);
}

bool ProjectState::setSendRoute(
    routing::SendId send, routing::BusId destination,
    routing::SendTapPoint tapPoint) noexcept {
    return routing_.setSendRoute(send, destination, tapPoint);
}

bool ProjectState::removeSend(routing::SendId send) noexcept {
    return routing_.removeSend(send);
}

bool ProjectState::setSendMix(routing::SendId send,
                              mixer::SendMixState mix) noexcept {
    return routing_.setSendMix(send, mix);
}

const routing::SendRoute* ProjectState::findSend(
    routing::SendId send) const noexcept {
    return routing_.findSend(send);
}

const tracks::AudioTrack* ProjectState::findTrack(
    tracks::TrackId track) const noexcept {
    const auto found = std::find_if(tracks_.begin(), tracks_.end(),
                                    [track](const auto& candidate) {
                                        return candidate.id == track;
                                    });
    return found == tracks_.end() ? nullptr : &*found;
}

tracks::AudioTrack* ProjectState::findTrackMutable(
    tracks::TrackId track) noexcept {
    const auto found = std::find_if(tracks_.begin(), tracks_.end(),
                                    [track](const auto& candidate) {
                                        return candidate.id == track;
                                    });
    return found == tracks_.end() ? nullptr : &*found;
}

const media::AudioSource* ProjectState::findSource(
    media::SourceId source) const noexcept {
    const auto found = std::find_if(sources_.begin(), sources_.end(),
                                    [source](const auto& candidate) {
                                        return candidate.id == source;
                                    });
    return found == sources_.end() ? nullptr : &*found;
}

const clips::AudioClip* ProjectState::findClip(
    clips::ClipId clip) const noexcept {
    for (const auto& track : tracks_) {
        const auto found = std::find_if(
            track.clips.begin(), track.clips.end(),
            [clip](const auto& candidate) { return candidate.id == clip; });
        if (found != track.clips.end()) {
            return &*found;
        }
    }
    return nullptr;
}

tracks::TrackId ProjectState::trackContainingClip(
    clips::ClipId clip) const noexcept {
    for (const auto& track : tracks_) {
        if (std::any_of(track.clips.begin(), track.clips.end(),
                        [clip](const auto& candidate) {
                            return candidate.id == clip;
                        })) {
            return track.id;
        }
    }
    return {};
}

std::span<const clips::AudioClip> ProjectState::clipsForTrack(
    tracks::TrackId track) const noexcept {
    const auto* found = findTrack(track);
    return found == nullptr ? std::span<const clips::AudioClip>{}
                            : std::span<const clips::AudioClip>{found->clips};
}

std::vector<clips::ClipId> ProjectState::clipsIntersectingRange(
    tracks::TrackId track, timeline::ProjectFramePosition rangeStart,
    timeline::ProjectFramePosition rangeEnd) const {
    std::vector<clips::ClipId> result;
    if (rangeStart.value < 0 || rangeEnd.value <= rangeStart.value) {
        return result;
    }
    const auto clips = clipsForTrack(track);
    result.reserve(clips.size());
    for (const auto& clip : clips) {
        const auto clipStart = static_cast<long double>(clip.projectStart.value);
        const auto clipEnd = clipStart +
                             static_cast<long double>(clip.duration.value);
        if (clipStart < static_cast<long double>(rangeEnd.value) &&
            clipEnd > static_cast<long double>(rangeStart.value)) {
            result.push_back(clip.id);
        }
    }
    return result;
}

const mixer::MasterMixState& ProjectState::masterMix() const noexcept {
    return masterMix_;
}

const processors::InsertChain& ProjectState::masterInserts() const noexcept {
    return masterInserts_;
}

processors::InsertChain* ProjectState::findProcessorChain(
    processors::ProcessorInstanceId processor) noexcept {
    for (auto& track : tracks_) {
        if (std::any_of(track.inserts.processors.begin(),
                        track.inserts.processors.end(),
                        [processor](const auto& candidate) {
                            return candidate.id == processor;
                        })) {
            return &track.inserts;
        }
    }
    for (const auto& busView : routing_.buses()) {
        auto* bus = routing_.findBusMutable(busView.id);
        if (bus != nullptr &&
            std::any_of(bus->inserts.processors.begin(),
                        bus->inserts.processors.end(),
                        [processor](const auto& candidate) {
                            return candidate.id == processor;
                        })) {
            return &bus->inserts;
        }
    }
    if (std::any_of(masterInserts_.processors.begin(),
                    masterInserts_.processors.end(),
                    [processor](const auto& candidate) {
                        return candidate.id == processor;
                    })) {
        return &masterInserts_;
    }
    return nullptr;
}

const processors::InsertChain* ProjectState::findProcessorChain(
    processors::ProcessorInstanceId processor) const noexcept {
    for (const auto& track : tracks_) {
        if (std::any_of(track.inserts.processors.begin(),
                        track.inserts.processors.end(),
                        [processor](const auto& candidate) {
                            return candidate.id == processor;
                        })) {
            return &track.inserts;
        }
    }
    for (const auto& bus : routing_.buses()) {
        if (std::any_of(bus.inserts.processors.begin(),
                        bus.inserts.processors.end(),
                        [processor](const auto& candidate) {
                            return candidate.id == processor;
                        })) {
            return &bus.inserts;
        }
    }
    if (std::any_of(masterInserts_.processors.begin(),
                    masterInserts_.processors.end(),
                    [processor](const auto& candidate) {
                        return candidate.id == processor;
                    })) {
        return &masterInserts_;
    }
    return nullptr;
}

const processors::ProcessorState* ProjectState::findProcessor(
    processors::ProcessorInstanceId processor) const noexcept {
    const auto* chain = findProcessorChain(processor);
    if (chain == nullptr) {
        return nullptr;
    }
    const auto found = std::find_if(
        chain->processors.begin(), chain->processors.end(),
        [processor](const auto& candidate) {
            return candidate.id == processor;
        });
    return found == chain->processors.end() ? nullptr : &*found;
}

processors::ProcessorInstanceId ProjectState::addProcessor(
    const processors::InsertTarget& target, processors::ProcessorType type) {
    if (!type.isValid() || !nextProcessorId_.isValid() ||
        nextProcessorId_.value == std::numeric_limits<std::uint64_t>::max()) {
        throw std::invalid_argument{"Invalid processor identity or type"};
    }
    processors::InsertChain* chain{};
    std::visit(
        [this, &chain](const auto& value) {
            using T = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<T, tracks::TrackId>) {
                const auto found = std::find_if(
                    tracks_.begin(), tracks_.end(),
                    [value](const auto& candidate) {
                        return candidate.id == value;
                    });
                if (found != tracks_.end()) {
                    chain = &found->inserts;
                }
            } else if constexpr (std::is_same_v<T, routing::BusId>) {
                if (auto* bus = routing_.findBusMutable(value)) {
                    chain = &bus->inserts;
                }
            } else {
                chain = &masterInserts_;
            }
        },
        target);
    if (chain == nullptr) {
        throw std::out_of_range{"Insert target does not exist"};
    }
    const auto id = nextProcessorId_;
    processors::ProcessorState state;
    state.id = id;
    state.type = std::move(type);
    if (state.type.identifier == processors::internalGainProcessorType) {
        state.parameters.push_back({processors::gainParameterId, 0.0F});
    }
    chain->processors.push_back(std::move(state));
    ++nextProcessorId_.value;
    return id;
}

bool ProjectState::removeProcessor(
    processors::ProcessorInstanceId processor) noexcept {
    auto* chain = findProcessorChain(processor);
    if (chain == nullptr) {
        return false;
    }
    const auto found = std::find_if(
        chain->processors.begin(), chain->processors.end(),
        [processor](const auto& candidate) {
            return candidate.id == processor;
        });
    chain->processors.erase(found);
    return true;
}

bool ProjectState::moveProcessor(processors::ProcessorInstanceId processor,
                                 std::size_t newIndex) noexcept {
    auto* chain = findProcessorChain(processor);
    if (chain == nullptr || newIndex >= chain->processors.size()) {
        return false;
    }
    const auto found = std::find_if(
        chain->processors.begin(), chain->processors.end(),
        [processor](const auto& candidate) {
            return candidate.id == processor;
        });
    const auto oldIndex =
        static_cast<std::size_t>(found - chain->processors.begin());
    if (oldIndex == newIndex) {
        return true;
    }
    auto value = std::move(*found);
    chain->processors.erase(found);
    chain->processors.insert(
        chain->processors.begin() + static_cast<std::ptrdiff_t>(newIndex),
        std::move(value));
    return true;
}

bool ProjectState::setProcessorBypass(
    processors::ProcessorInstanceId processor, bool bypassed) noexcept {
    auto* chain = findProcessorChain(processor);
    if (chain == nullptr) {
        return false;
    }
    const auto found = std::find_if(
        chain->processors.begin(), chain->processors.end(),
        [processor](const auto& candidate) {
            return candidate.id == processor;
        });
    found->bypassed = bypassed;
    return true;
}

bool ProjectState::setProcessorParameter(
    processors::ProcessorInstanceId processor,
    processors::ParameterId parameter, float value) noexcept {
    auto* chain = findProcessorChain(processor);
    if (chain == nullptr || !parameter.isValid() || !std::isfinite(value)) {
        return false;
    }
    const auto found = std::find_if(
        chain->processors.begin(), chain->processors.end(),
        [processor](const auto& candidate) {
            return candidate.id == processor;
        });
    const auto parameterFound = std::find_if(
        found->parameters.begin(), found->parameters.end(),
        [parameter](const auto& candidate) {
            return candidate.id == parameter;
        });
    if (parameterFound == found->parameters.end()) {
        return false;
    }
    parameterFound->value = value;
    return true;
}

bool ProjectState::setTrackMix(tracks::TrackId track,
                               mixer::TrackMixState state) noexcept {
    if (!state.isValid()) {
        return false;
    }
    const auto found = std::find_if(tracks_.begin(), tracks_.end(),
                                    [track](const auto& candidate) {
                                        return candidate.id == track;
                                    });
    if (found == tracks_.end()) {
        return false;
    }
    found->mix = state;
    return true;
}

bool ProjectState::setMasterMix(mixer::MasterMixState state) noexcept {
    if (!state.isValid()) {
        return false;
    }
    masterMix_ = state;
    return true;
}

timeline::ProjectFrameCount ProjectState::duration() const noexcept {
    std::int64_t exclusiveEnd{};
    for (const auto& track : tracks_) {
        for (const auto& clip : track.clips) {
            const auto end = timeline::checkedExclusiveProjectEnd(clip.projectStart, clip.duration);
            if (!end) return {std::numeric_limits<std::int64_t>::max()};
            exclusiveEnd = std::max(exclusiveEnd, end->value);
        }
    }
    return {exclusiveEnd};
}

bool ProjectState::validateClip(
    const tracks::AudioTrack& track, const media::AudioSource& source,
    timeline::ProjectFramePosition projectStart,
    timeline::ProjectFrameDuration duration,
    timeline::SourceFramePosition sourceOffset) const noexcept {
    if (track.layout != source.layout ||
        !timeline::isSupportedProjectFramePosition(projectStart) ||
        !std::isfinite(duration.value) || duration.value <= 0.0 ||
        !std::isfinite(sourceOffset.value) || sourceOffset.value < 0.0 ||
        !settings_.sampleRate.isValid() || !source.sampleRate.isValid()) {
        return false;
    }
    const auto projectEnd = timeline::checkedExclusiveProjectEnd(
        projectStart, duration);
    if (!projectEnd ||
        !timeline::isSupportedProjectFramePosition(*projectEnd)) {
        return false;
    }
    const auto sourceEnd = static_cast<long double>(sourceOffset.value) +
        static_cast<long double>(duration.value) *
            static_cast<long double>(source.sampleRate.hertz()) /
            static_cast<long double>(settings_.sampleRate.hertz());
    const auto sourceLimit = static_cast<long double>(source.frameCount.value);
    const auto roundTripTolerance =
        std::numeric_limits<double>::epsilon() * 64.0L *
        std::max(1.0L, std::max(std::abs(sourceEnd), sourceLimit));
    return std::isfinite(sourceEnd) &&
           sourceEnd <= sourceLimit + roundTripTolerance;
}

void ProjectState::sortTrackClips(tracks::AudioTrack& track) noexcept {
    std::sort(track.clips.begin(), track.clips.end(),
              [](const auto& left, const auto& right) {
                  if (left.projectStart != right.projectStart) {
                      return left.projectStart.value < right.projectStart.value;
                  }
                  return left.id < right.id;
              });
}

ProjectState::ImportedAudio ProjectState::importAudioToTrack(
    tracks::TrackId track, media::MediaReference mediaReference,
    timeline::SourceFrameCount sourceFrameCount,
    timeline::SampleRate sourceSampleRate,
    media::AudioChannelLayout sourceLayout,
    timeline::ProjectFramePosition projectStart) {
    auto* destination = findTrackMutable(track);
    if (destination == nullptr) {
        throw std::out_of_range{"Audio track does not exist"};
    }
    if (!mediaReference.isValid() || sourceFrameCount.value == 0 ||
        !sourceSampleRate.isValid() || destination->layout != sourceLayout ||
        sources_.size() >= maximumSources ||
        destination->clips.size() >= maximumClipsPerTrack ||
        findClip({nextClipId_.value}) != nullptr ||
        nextSourceId_.value == std::numeric_limits<std::uint64_t>::max() ||
        nextClipId_.value == std::numeric_limits<std::uint64_t>::max()) {
        throw std::invalid_argument{"Invalid source or incompatible track layout"};
    }
    std::size_t clipCount{};
    for (const auto& candidate : tracks_) {
        clipCount += candidate.clips.size();
    }
    if (clipCount >= maximumClips) {
        throw std::length_error{"Project clip capacity exceeded"};
    }
    const auto sourceId = nextSourceId_;
    const media::AudioSource source{sourceId, std::move(mediaReference),
                                    sourceFrameCount, sourceSampleRate,
                                    sourceLayout};
    const auto duration = timeline::ProjectFrameDuration{
        static_cast<double>(sourceFrameCount.value) * sampleRate().hertz() /
        sourceSampleRate.hertz()};
    if (!validateClip(*destination, source, projectStart, duration, {0.0})) {
        throw std::invalid_argument{"Imported clip exceeds its source"};
    }
    const auto clipId = nextClipId_;
    sources_.push_back(source);
    try {
        destination = findTrackMutable(track);
        destination->clips.push_back(
            {clipId, sourceId, projectStart, duration, {0.0}});
    } catch (...) {
        sources_.pop_back();
        throw;
    }
    sortTrackClips(*destination);
    ++nextSourceId_.value;
    ++nextClipId_.value;
    return {sourceId, clipId};
}

clips::ClipId ProjectState::addClip(
    tracks::TrackId track, media::SourceId sourceId,
    timeline::ProjectFramePosition projectStart,
    timeline::ProjectFrameDuration duration,
    timeline::SourceFramePosition sourceOffset) {
    auto* destination = findTrackMutable(track);
    const auto* source = findSource(sourceId);
    if (destination == nullptr || source == nullptr ||
        destination->clips.size() >= maximumClipsPerTrack ||
        nextClipId_.value == std::numeric_limits<std::uint64_t>::max() ||
        !validateClip(*destination, *source, projectStart, duration,
                      sourceOffset)) {
        throw std::invalid_argument{"Invalid clip or incompatible source"};
    }
    std::size_t clipCount{};
    for (const auto& candidate : tracks_) {
        clipCount += candidate.clips.size();
    }
    if (clipCount >= maximumClips) {
        throw std::length_error{"Project clip capacity exceeded"};
    }
    const auto id = nextClipId_;
    destination->clips.push_back(
        {id, sourceId, projectStart, duration, sourceOffset});
    sortTrackClips(*destination);
    ++nextClipId_.value;
    return id;
}

tracks::AudioTrack* ProjectState::findTrackContainingClip(
    clips::ClipId clip) noexcept {
    for (auto& track : tracks_) {
        if (std::any_of(track.clips.begin(), track.clips.end(),
                        [clip](const auto& candidate) {
                            return candidate.id == clip;
                        })) {
            return &track;
        }
    }
    return nullptr;
}

ProjectState::ClipEditResult ProjectState::removeClip(
    clips::ClipId clip) noexcept {
    return deleteClip(clip);
}

ProjectState::ClipEditResult ProjectState::deleteClip(
    clips::ClipId clip) noexcept {
    auto* track = findTrackContainingClip(clip);
    if (track == nullptr) {
        return {ClipEditStatus::clipNotFound, clip, {}};
    }
    const auto found = std::find_if(
        track->clips.begin(), track->clips.end(),
        [clip](const auto& candidate) { return candidate.id == clip; });
    track->clips.erase(found);
    return {ClipEditStatus::success, clip, {}};
}

ProjectState::BatchClipEditResult ProjectState::moveClips(
    std::span<const clips::ClipId> input, std::int64_t deltaFrames) {
    BatchClipEditResult result;
    const auto ids = canonicalClipIds(input);
    if (ids.empty()) {
        result.status = ClipEditStatus::clipNotFound;
        return result;
    }
    result.before.reserve(ids.size());
    result.after.reserve(ids.size());
    for (const auto id : ids) {
        const auto* clip = findClip(id);
        if (clip == nullptr) {
            result.status = ClipEditStatus::clipNotFound;
            result.before.clear();
            return result;
        }
        const auto trackId = trackContainingClip(id);
        result.before.push_back({trackId, *clip});
    }
    for (const auto& before : result.before) {
        const auto* track = findTrack(before.track);
        const auto* source = findSource(before.clip.source);
        const auto target = shiftedPosition(before.clip.projectStart, deltaFrames);
        if (!target || track == nullptr || source == nullptr ||
            !validateClip(*track, *source, *target, before.clip.duration,
                          before.clip.sourceOffset)) {
            result.status = ClipEditStatus::invalidPosition;
            result.before.clear();
            return result;
        }
        auto after = before.clip;
        after.projectStart = *target;
        result.after.push_back({before.track, after});
    }
    if (deltaFrames == 0) {
        return result;
    }
    for (const auto& state : result.after) {
        auto* track = findTrackMutable(state.track);
        const auto found = std::find_if(
            track->clips.begin(), track->clips.end(),
            [&](const auto& clip) { return clip.id == state.clip.id; });
        *found = state.clip;
    }
    for (auto& track : tracks_) sortTrackClips(track);
    result.changed = true;
    return result;
}

ProjectState::BatchClipEditResult ProjectState::deleteClips(
    std::span<const clips::ClipId> input) {
    BatchClipEditResult result;
    const auto ids = canonicalClipIds(input);
    if (ids.empty()) {
        result.status = ClipEditStatus::clipNotFound;
        return result;
    }
    result.before.reserve(ids.size());
    for (const auto id : ids) {
        const auto* clip = findClip(id);
        if (clip == nullptr) {
            result.status = ClipEditStatus::clipNotFound;
            result.before.clear();
            return result;
        }
        result.before.push_back({trackContainingClip(id), *clip});
    }
    for (const auto id : ids) {
        const auto removed = deleteClip(id);
        if (!removed) throw std::logic_error{"Validated clip batch diverged"};
    }
    result.changed = true;
    return result;
}

ProjectState::BatchClipEditResult ProjectState::duplicateClips(
    std::span<const clips::ClipId> input, std::int64_t deltaFrames) {
    BatchClipEditResult result;
    const auto ids = canonicalClipIds(input);
    if (ids.empty()) {
        result.status = ClipEditStatus::clipNotFound;
        return result;
    }
    result.before.reserve(ids.size());
    result.after.reserve(ids.size());
    result.createdClips.reserve(ids.size());
    for (const auto id : ids) {
        const auto* clip = findClip(id);
        if (clip == nullptr) {
            result.status = ClipEditStatus::clipNotFound;
            result.before.clear();
            return result;
        }
        result.before.push_back({trackContainingClip(id), *clip});
    }
    std::size_t totalClips{};
    for (const auto& track : tracks_) totalClips += track.clips.size();
    if (ids.size() > maximumClips - totalClips ||
        ids.size() > std::numeric_limits<std::uint64_t>::max() -
                         nextClipId_.value) {
        result.status = ClipEditStatus::capacityExceeded;
        result.before.clear();
        return result;
    }
    std::vector<std::pair<tracks::TrackId, std::size_t>> additions;
    additions.reserve(ids.size());
    for (std::size_t index = 0; index < ids.size(); ++index) {
        const auto& original = result.before[index];
        const auto* clip = &original.clip;
        const auto trackId = original.track;
        const auto* track = findTrack(trackId);
        const auto* source = findSource(clip->source);
        const auto target = shiftedPosition(clip->projectStart, deltaFrames);
        if (!target || track == nullptr || source == nullptr ||
            !validateClip(*track, *source, *target, clip->duration,
                          clip->sourceOffset)) {
            result.status = ClipEditStatus::invalidPosition;
            result.before.clear();
            result.after.clear();
            result.createdClips.clear();
            return result;
        }
        auto addition = std::find_if(
            additions.begin(), additions.end(),
            [trackId](const auto& value) { return value.first == trackId; });
        std::size_t trackAdditionCount{};
        if (addition == additions.end()) {
            additions.push_back({trackId, 1});
            trackAdditionCount = 1;
        } else {
            trackAdditionCount = ++addition->second;
        }
        if (trackAdditionCount > maximumClipsPerTrack - track->clips.size()) {
            result.status = ClipEditStatus::capacityExceeded;
            result.before.clear();
            result.after.clear();
            result.createdClips.clear();
            return result;
        }
        const clips::ClipId created{nextClipId_.value + index};
        auto duplicate = *clip;
        duplicate.id = created;
        duplicate.projectStart = *target;
        result.after.push_back({trackId, duplicate});
        result.createdClips.push_back(created);
    }
    for (const auto& addition : additions) {
        auto* track = findTrackMutable(addition.first);
        track->clips.reserve(track->clips.size() + addition.second);
    }
    for (const auto& state : result.after) {
        findTrackMutable(state.track)->clips.push_back(state.clip);
    }
    for (const auto& addition : additions) {
        sortTrackClips(*findTrackMutable(addition.first));
    }
    nextClipId_.value += static_cast<std::uint64_t>(ids.size());
    result.changed = true;
    return result;
}

ProjectState::ClipEditResult ProjectState::moveClip(
    clips::ClipId clip,
    timeline::ProjectFramePosition projectStart) noexcept {
    if (projectStart.value < 0) {
        return {ClipEditStatus::invalidPosition, clip, {}};
    }
    auto* track = findTrackContainingClip(clip);
    if (track == nullptr) {
        return {ClipEditStatus::clipNotFound, clip, {}};
    }
    const auto found = std::find_if(
        track->clips.begin(), track->clips.end(),
        [clip](const auto& candidate) { return candidate.id == clip; });
    const auto* source = findSource(found->source);
    if (source == nullptr ||
        !validateClip(*track, *source, projectStart, found->duration,
                      found->sourceOffset)) {
        return {ClipEditStatus::invalidPosition, clip, {}};
    }
    found->projectStart = projectStart;
    sortTrackClips(*track);
    return {ClipEditStatus::success, clip, {}};
}

ProjectState::ClipEditResult ProjectState::moveClip(
    clips::ClipId clip, tracks::TrackId targetTrack,
    timeline::ProjectFramePosition projectStart) {
    if (projectStart.value < 0) {
        return {ClipEditStatus::invalidPosition, clip, {}};
    }
    auto* sourceTrack = findTrackContainingClip(clip);
    if (sourceTrack == nullptr) {
        return {ClipEditStatus::clipNotFound, clip, {}};
    }
    auto* destinationTrack = findTrackMutable(targetTrack);
    if (destinationTrack == nullptr) {
        return {ClipEditStatus::trackNotFound, clip, {}};
    }
    if (sourceTrack->id == destinationTrack->id) {
        return moveClip(clip, projectStart);
    }
    const auto found = std::find_if(
        sourceTrack->clips.begin(), sourceTrack->clips.end(),
        [clip](const auto& candidate) { return candidate.id == clip; });
    const auto* source = findSource(found->source);
    if (source == nullptr || destinationTrack->layout != source->layout) {
        return {ClipEditStatus::layoutMismatch, clip, {}};
    }
    if (destinationTrack->clips.size() >= maximumClipsPerTrack) {
        return {ClipEditStatus::capacityExceeded, clip, {}};
    }
    auto moved = *found;
    moved.projectStart = projectStart;
    if (!validateClip(*destinationTrack, *source, moved.projectStart,
                      moved.duration, moved.sourceOffset)) {
        return {ClipEditStatus::invalidPosition, clip, {}};
    }
    destinationTrack->clips.push_back(moved);
    sortTrackClips(*destinationTrack);
    sourceTrack = findTrackMutable(sourceTrack->id);
    const auto sourceFound = std::find_if(
        sourceTrack->clips.begin(), sourceTrack->clips.end(),
        [clip](const auto& candidate) { return candidate.id == clip; });
    sourceTrack->clips.erase(sourceFound);
    return {ClipEditStatus::success, clip, {}};
}

ProjectState::ClipEditResult ProjectState::duplicateClip(
    clips::ClipId clip, timeline::ProjectFramePosition projectStart) {
    if (projectStart.value < 0) {
        return {ClipEditStatus::invalidPosition, clip, {}};
    }
    auto* track = findTrackContainingClip(clip);
    if (track == nullptr) {
        return {ClipEditStatus::clipNotFound, clip, {}};
    }
    std::size_t totalClips{};
    for (const auto& candidate : tracks_) {
        totalClips += candidate.clips.size();
    }
    if (totalClips >= maximumClips ||
        track->clips.size() >= maximumClipsPerTrack ||
        !nextClipId_.isValid() ||
        nextClipId_.value == std::numeric_limits<std::uint64_t>::max()) {
        return {ClipEditStatus::capacityExceeded, clip, {}};
    }
    const auto found = std::find_if(
        track->clips.begin(), track->clips.end(),
        [clip](const auto& candidate) { return candidate.id == clip; });
    const auto* source = findSource(found->source);
    if (source == nullptr ||
        !validateClip(*track, *source, projectStart, found->duration,
                      found->sourceOffset)) {
        return {ClipEditStatus::invalidPosition, clip, {}};
    }
    const auto created = nextClipId_;
    const clips::AudioClip duplicate{created, found->source, projectStart,
                                     found->duration, found->sourceOffset};
    track->clips.push_back(duplicate);
    sortTrackClips(*track);
    ++nextClipId_.value;
    return {ClipEditStatus::success, clip, created};
}

ProjectState::ClipEditResult ProjectState::splitClip(
    clips::ClipId clip, timeline::ProjectFramePosition splitPosition) {
    auto* track = findTrackContainingClip(clip);
    if (track == nullptr) {
        return {ClipEditStatus::clipNotFound, clip, {}};
    }
    const auto found = std::find_if(
        track->clips.begin(), track->clips.end(),
        [clip](const auto& candidate) { return candidate.id == clip; });
    const auto original = *found;
    const auto oldStart = static_cast<long double>(original.projectStart.value);
    const auto oldEnd = oldStart + static_cast<long double>(original.duration.value);
    const auto split = static_cast<long double>(splitPosition.value);
    if (!(split > oldStart && split < oldEnd)) {
        return {ClipEditStatus::invalidPosition, clip, {}};
    }
    std::size_t totalClips{};
    for (const auto& candidate : tracks_) {
        totalClips += candidate.clips.size();
    }
    if (totalClips >= maximumClips ||
        track->clips.size() >= maximumClipsPerTrack ||
        !nextClipId_.isValid() ||
        nextClipId_.value == std::numeric_limits<std::uint64_t>::max()) {
        return {ClipEditStatus::capacityExceeded, clip, {}};
    }
    const auto* source = findSource(original.source);
    if (source == nullptr) {
        return {ClipEditStatus::sourceBoundsExceeded, clip, {}};
    }
    const auto leftDuration = static_cast<double>(split - oldStart);
    const auto rightDuration = static_cast<double>(oldEnd - split);
    const auto sourceAdvance =
        static_cast<double>(split - oldStart) * source->sampleRate.hertz() /
        settings_.sampleRate.hertz();
    const timeline::SourceFramePosition rightOffset{
        original.sourceOffset.value + sourceAdvance};
    if (leftDuration <= 0.0 || rightDuration <= 0.0) {
        return {ClipEditStatus::zeroLengthClip, clip, {}};
    }
    if (!validateClip(*track, *source, original.projectStart,
                      {leftDuration}, original.sourceOffset) ||
        !validateClip(*track, *source, splitPosition,
                      {rightDuration}, rightOffset)) {
        return {ClipEditStatus::sourceBoundsExceeded, clip, {}};
    }
    const auto created = nextClipId_;
    track->clips.push_back({created, original.source, splitPosition,
                            {rightDuration}, rightOffset});
    auto* originalTrack = findTrackContainingClip(clip);
    const auto originalAgain = std::find_if(
        originalTrack->clips.begin(), originalTrack->clips.end(),
        [clip](const auto& candidate) { return candidate.id == clip; });
    originalAgain->duration = {leftDuration};
    sortTrackClips(*originalTrack);
    ++nextClipId_.value;
    return {ClipEditStatus::success, clip, created};
}

ProjectState::ClipEditResult ProjectState::trimClipLeft(
    clips::ClipId clip,
    timeline::ProjectFramePosition projectStart) noexcept {
    auto* track = findTrackContainingClip(clip);
    if (track == nullptr) {
        return {ClipEditStatus::clipNotFound, clip, {}};
    }
    const auto found = std::find_if(
        track->clips.begin(), track->clips.end(),
        [clip](const auto& candidate) { return candidate.id == clip; });
    const auto oldStart = static_cast<long double>(found->projectStart.value);
    const auto oldEnd = oldStart + static_cast<long double>(found->duration.value);
    const auto nextStart = static_cast<long double>(projectStart.value);
    if (nextStart < oldStart || nextStart >= oldEnd) {
        return {nextStart == oldEnd ? ClipEditStatus::zeroLengthClip
                                    : ClipEditStatus::invalidPosition,
                clip, {}};
    }
    if (projectStart == found->projectStart) {
        return {ClipEditStatus::success, clip, {}};
    }
    const auto* source = findSource(found->source);
    if (source == nullptr) {
        return {ClipEditStatus::sourceBoundsExceeded, clip, {}};
    }
    const auto delta = static_cast<double>(nextStart - oldStart);
    const timeline::ProjectFrameDuration nextDuration{
        static_cast<double>(oldEnd - nextStart)};
    const timeline::SourceFramePosition nextOffset{
        found->sourceOffset.value +
        delta * source->sampleRate.hertz() / settings_.sampleRate.hertz()};
    if (!validateClip(*track, *source, projectStart, nextDuration,
                      nextOffset)) {
        return {ClipEditStatus::sourceBoundsExceeded, clip, {}};
    }
    found->projectStart = projectStart;
    found->sourceOffset = nextOffset;
    found->duration = nextDuration;
    sortTrackClips(*track);
    return {ClipEditStatus::success, clip, {}};
}

ProjectState::ClipEditResult ProjectState::trimClipRight(
    clips::ClipId clip,
    timeline::ProjectFramePosition projectEnd) noexcept {
    auto* track = findTrackContainingClip(clip);
    if (track == nullptr) {
        return {ClipEditStatus::clipNotFound, clip, {}};
    }
    const auto found = std::find_if(
        track->clips.begin(), track->clips.end(),
        [clip](const auto& candidate) { return candidate.id == clip; });
    const auto oldStart = static_cast<long double>(found->projectStart.value);
    const auto oldEnd = oldStart + static_cast<long double>(found->duration.value);
    const auto nextEnd = static_cast<long double>(projectEnd.value);
    if (nextEnd <= oldStart || nextEnd > oldEnd) {
        return {nextEnd == oldStart ? ClipEditStatus::zeroLengthClip
                                    : ClipEditStatus::invalidPosition,
                clip, {}};
    }
    if (nextEnd == oldEnd) {
        return {ClipEditStatus::success, clip, {}};
    }
    const auto* source = findSource(found->source);
    const timeline::ProjectFrameDuration nextDuration{
        static_cast<double>(nextEnd - oldStart)};
    if (source == nullptr ||
        !validateClip(*track, *source, found->projectStart, nextDuration,
                      found->sourceOffset)) {
        return {ClipEditStatus::sourceBoundsExceeded, clip, {}};
    }
    found->duration = nextDuration;
    return {ClipEditStatus::success, clip, {}};
}

bool ProjectState::restoreHistoryClip(tracks::TrackId id,
                                      const clips::AudioClip& clip) {
    auto* track = findTrackMutable(id);
    const auto* source = findSource(clip.source);
    std::size_t total{};
    for (const auto& entry : tracks_) total += entry.clips.size();
    if (!clip.id.isValid() || clip.id.value >= nextClipId_.value ||
        findClip(clip.id) || !track || !source || total >= maximumClips ||
        track->clips.size() >= maximumClipsPerTrack ||
        !validateClip(*track, *source, clip.projectStart, clip.duration, clip.sourceOffset))
        return false;
    track->clips.push_back(clip);
    sortTrackClips(*track);
    return true;
}

bool ProjectState::restoreHistoryTrack(const TrackHistoryState& state) {
    if (!state.track.id.isValid() || state.track.id.value >= nextTrackId_.value ||
        findTrack(state.track.id) != nullptr ||
        state.trackIndex > tracks_.size() ||
        state.routeIndex > routing_.trackRoutes_.size() ||
        state.route.track != state.track.id ||
        !state.route.destination.isValid() ||
        state.track.clips.size() > maximumClipsPerTrack ||
        (state.track.layout != media::AudioChannelLayout::mono &&
         state.track.layout != media::AudioChannelLayout::stereo)) {
        return false;
    }
    if (state.route.destination.kind == routing::DestinationKind::bus &&
        !routing_.containsBus(state.route.destination.bus)) {
        return false;
    }
    std::size_t totalClips{};
    for (const auto& track : tracks_) totalClips += track.clips.size();
    if (totalClips > maximumClips - state.track.clips.size()) return false;
    for (const auto& clip : state.track.clips) {
        const auto* source = findSource(clip.source);
        if (!clip.id.isValid() || clip.id.value >= nextClipId_.value ||
            findClip(clip.id) != nullptr || source == nullptr ||
            !validateClip(state.track, *source, clip.projectStart,
                          clip.duration, clip.sourceOffset)) {
            return false;
        }
    }
    for (const auto& processor : state.track.inserts.processors) {
        if (!processor.id.isValid() ||
            processor.id.value >= nextProcessorId_.value ||
            findProcessor(processor.id) != nullptr) return false;
    }
    for (const auto& indexed : state.sends) {
        const auto* source =
            std::get_if<tracks::TrackId>(&indexed.route.source);
        if (source == nullptr || *source != state.track.id ||
            !indexed.route.id.isValid() ||
            indexed.route.id.value >= routing_.nextSendId_.value ||
            findSend(indexed.route.id) != nullptr ||
            !routing_.containsBus(indexed.route.destination) ||
            !routing::isValid(indexed.route.tapPoint) ||
            !indexed.route.mix.isValid()) {
            return false;
        }
    }
    tracks_.insert(tracks_.begin() + static_cast<std::ptrdiff_t>(state.trackIndex),
                   state.track);
    routing_.trackRoutes_.insert(
        routing_.trackRoutes_.begin() +
            static_cast<std::ptrdiff_t>(state.routeIndex),
        state.route);
    for (const auto& indexed : state.sends) {
        const auto index = std::min(indexed.index, routing_.sends_.size());
        routing_.sends_.insert(
            routing_.sends_.begin() + static_cast<std::ptrdiff_t>(index),
            indexed.route);
    }
    return true;
}

bool ProjectState::transferHistoryClip(
    tracks::TrackId fromTrack, const clips::AudioClip& before,
    tracks::TrackId toTrack, const clips::AudioClip& after) {
    if (before.id != after.id || before.source != after.source ||
        before.duration != after.duration ||
        before.sourceOffset != after.sourceOffset) {
        return false;
    }
    auto* sourceTrack = findTrackMutable(fromTrack);
    auto* destinationTrack = findTrackMutable(toTrack);
    const auto* source = findSource(before.source);
    if (sourceTrack == nullptr || destinationTrack == nullptr || source == nullptr ||
        !validateClip(*destinationTrack, *source, after.projectStart,
                      after.duration, after.sourceOffset)) {
        return false;
    }
    const auto found = std::find_if(
        sourceTrack->clips.begin(), sourceTrack->clips.end(),
        [&](const auto& clip) { return clip == before; });
    if (found == sourceTrack->clips.end()) return false;
    if (fromTrack == toTrack) {
        *found = after;
        sortTrackClips(*sourceTrack);
        return true;
    }
    if (destinationTrack->layout != source->layout ||
        destinationTrack->clips.size() >= maximumClipsPerTrack ||
        std::any_of(destinationTrack->clips.begin(), destinationTrack->clips.end(),
                    [&](const auto& clip) { return clip.id == after.id; })) {
        return false;
    }
    destinationTrack->clips.push_back(after);
    sortTrackClips(*destinationTrack);
    sourceTrack = findTrackMutable(fromTrack);
    const auto foundAgain = std::find_if(
        sourceTrack->clips.begin(), sourceTrack->clips.end(),
        [&](const auto& clip) { return clip == before; });
    sourceTrack->clips.erase(foundAgain);
    return true;
}

bool ProjectState::replaceHistoryClip(tracks::TrackId id,
                                      const clips::AudioClip& clip) noexcept {
    auto* track = findTrackMutable(id);
    const auto* source = findSource(clip.source);
    if (!track || !source ||
        !validateClip(*track, *source, clip.projectStart, clip.duration, clip.sourceOffset))
        return false;
    const auto found = std::find_if(track->clips.begin(), track->clips.end(),
        [&](const auto& entry) { return entry.id == clip.id; });
    if (found == track->clips.end()) return false;
    *found = clip;
    sortTrackClips(*track);
    return true;
}

bool ProjectState::reorderHistoryTrack(
    tracks::TrackId track, std::size_t expectedIndex,
    std::size_t targetIndex) noexcept {
    if (expectedIndex >= tracks_.size() || targetIndex >= tracks_.size() ||
        tracks_[expectedIndex].id != track) {
        return false;
    }
    if (expectedIndex < targetIndex) {
        std::rotate(tracks_.begin() + static_cast<std::ptrdiff_t>(expectedIndex),
                    tracks_.begin() + static_cast<std::ptrdiff_t>(expectedIndex + 1),
                    tracks_.begin() + static_cast<std::ptrdiff_t>(targetIndex + 1));
    } else if (targetIndex < expectedIndex) {
        std::rotate(tracks_.begin() + static_cast<std::ptrdiff_t>(targetIndex),
                    tracks_.begin() + static_cast<std::ptrdiff_t>(expectedIndex),
                    tracks_.begin() + static_cast<std::ptrdiff_t>(expectedIndex + 1));
    }
    return true;
}

bool ProjectState::restoreHistoryClips(
    std::span<const ClipHistoryState> states) {
    std::size_t total{};
    for (const auto& track : tracks_) total += track.clips.size();
    if (states.size() > maximumClips - total) return false;
    std::vector<std::pair<tracks::TrackId, std::size_t>> additions;
    additions.reserve(states.size());
    for (std::size_t index = 0; index < states.size(); ++index) {
        const auto& state = states[index];
        for (std::size_t previous = 0; previous < index; ++previous) {
            if (states[previous].clip.id == state.clip.id) return false;
        }
        auto* track = findTrackMutable(state.track);
        const auto* source = findSource(state.clip.source);
        if (!state.clip.id.isValid() || state.clip.id.value >= nextClipId_.value ||
            findClip(state.clip.id) != nullptr || track == nullptr || source == nullptr ||
            !validateClip(*track, *source, state.clip.projectStart,
                          state.clip.duration, state.clip.sourceOffset)) {
            return false;
        }
        auto found = std::find_if(
            additions.begin(), additions.end(),
            [&](const auto& addition) { return addition.first == state.track; });
        if (found == additions.end()) {
            additions.push_back({state.track, 1});
        } else {
            ++found->second;
        }
    }
    for (const auto& addition : additions) {
        auto* track = findTrackMutable(addition.first);
        if (addition.second > maximumClipsPerTrack - track->clips.size()) {
            return false;
        }
    }
    for (const auto& addition : additions) {
        auto* track = findTrackMutable(addition.first);
        track->clips.reserve(track->clips.size() + addition.second);
    }
    for (const auto& state : states) {
        findTrackMutable(state.track)->clips.push_back(state.clip);
    }
    for (const auto& addition : additions) {
        sortTrackClips(*findTrackMutable(addition.first));
    }
    return true;
}

bool ProjectState::deleteHistoryClips(
    std::span<const ClipHistoryState> states) noexcept {
    for (std::size_t index = 0; index < states.size(); ++index) {
        const auto& state = states[index];
        for (std::size_t previous = 0; previous < index; ++previous) {
            if (states[previous].clip.id == state.clip.id) return false;
        }
        const auto* track = findTrack(state.track);
        if (track == nullptr || std::none_of(
                track->clips.begin(), track->clips.end(),
                [&](const auto& clip) { return clip == state.clip; })) {
            return false;
        }
    }
    for (const auto& state : states) {
        auto* track = findTrackMutable(state.track);
        const auto found = std::find_if(
            track->clips.begin(), track->clips.end(),
            [&](const auto& clip) { return clip == state.clip; });
        track->clips.erase(found);
    }
    return true;
}

bool ProjectState::replaceHistoryClips(
    std::span<const ClipHistoryState> expected,
    std::span<const ClipHistoryState> replacement) noexcept {
    if (expected.size() != replacement.size()) return false;
    for (std::size_t index = 0; index < expected.size(); ++index) {
        const auto& before = expected[index];
        const auto& after = replacement[index];
        for (std::size_t previous = 0; previous < index; ++previous) {
            if (expected[previous].clip.id == before.clip.id ||
                replacement[previous].clip.id == after.clip.id) return false;
        }
        const auto* track = findTrack(before.track);
        const auto* target = findTrack(after.track);
        const auto* source = findSource(after.clip.source);
        if (before.track != after.track || before.clip.id != after.clip.id ||
            before.clip.source != after.clip.source ||
            before.clip.duration != after.clip.duration ||
            before.clip.sourceOffset != after.clip.sourceOffset ||
            track == nullptr || target == nullptr || source == nullptr ||
            std::none_of(track->clips.begin(), track->clips.end(),
                         [&](const auto& clip) { return clip == before.clip; }) ||
            !validateClip(*target, *source, after.clip.projectStart,
                          after.clip.duration, after.clip.sourceOffset)) {
            return false;
        }
    }
    for (const auto& state : replacement) {
        auto* track = findTrackMutable(state.track);
        const auto found = std::find_if(
            track->clips.begin(), track->clips.end(),
            [&](const auto& clip) { return clip.id == state.clip.id; });
        *found = state.clip;
    }
    for (auto& track : tracks_) sortTrackClips(track);
    return true;
}

bool ProjectState::removeSource(media::SourceId source) noexcept {
    for (const auto& track : tracks_) {
        if (std::any_of(track.clips.begin(), track.clips.end(),
                        [source](const auto& clip) {
                            return clip.source == source;
                        })) {
            return false;
        }
    }
    const auto found = std::find_if(sources_.begin(), sources_.end(),
                                    [source](const auto& candidate) {
                                        return candidate.id == source;
                                    });
    if (found == sources_.end()) {
        return false;
    }
    sources_.erase(found);
    return true;
}

timeline::ProjectFrameCount ProjectState::projectContentDuration() const noexcept {
    return duration();
}

void ProjectState::swap(ProjectState& other) noexcept {
    using std::swap;
    swap(settings_, other.settings_);
    swap(musicalTime_, other.musicalTime_);
    swap(loopRange_, other.loopRange_);
    tracks_.swap(other.tracks_);
    sources_.swap(other.sources_);
    swap(routing_, other.routing_);
    swap(nextTrackId_, other.nextTrackId_);
    swap(nextSourceId_, other.nextSourceId_);
    swap(nextClipId_, other.nextClipId_);
    swap(masterMix_, other.masterMix_);
    swap(masterInserts_, other.masterInserts_);
    swap(nextProcessorId_, other.nextProcessorId_);
}

} // namespace vitadaw::project
