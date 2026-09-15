#include "vitadaw/project/ProjectState.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <type_traits>

namespace vitadaw::project {

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
    long double preciseEnd{};
    for (const auto& track : tracks_) {
        for (const auto& clip : track.clips) {
            preciseEnd = std::max(
                preciseEnd,
                static_cast<long double>(clip.projectStart.value) +
                    static_cast<long double>(clip.duration.value));
        }
    }
    if (!(preciseEnd > 0.0L)) {
        return {};
    }
    const auto limit = static_cast<long double>(
        std::numeric_limits<std::int64_t>::max());
    return {preciseEnd >= limit
                ? std::numeric_limits<std::int64_t>::max()
                : static_cast<std::int64_t>(std::ceil(preciseEnd))};
}

bool ProjectState::validateClip(
    const tracks::AudioTrack& track, const media::AudioSource& source,
    timeline::ProjectFramePosition projectStart,
    timeline::ProjectFrameDuration duration,
    timeline::SourceFramePosition sourceOffset) const noexcept {
    if (track.layout != source.layout || projectStart.value < 0 ||
        !std::isfinite(duration.value) || duration.value <= 0.0 ||
        !std::isfinite(sourceOffset.value) || sourceOffset.value < 0.0 ||
        !settings_.sampleRate.isValid() || !source.sampleRate.isValid()) {
        return false;
    }
    const auto projectEnd = static_cast<long double>(projectStart.value) +
                            static_cast<long double>(duration.value);
    const auto sourceEnd = static_cast<long double>(sourceOffset.value) +
        static_cast<long double>(duration.value) *
            static_cast<long double>(source.sampleRate.hertz()) /
            static_cast<long double>(settings_.sampleRate.hertz());
    const auto sourceLimit = static_cast<long double>(source.frameCount.value);
    const auto roundTripTolerance =
        std::numeric_limits<double>::epsilon() * 64.0L *
        std::max(1.0L, std::max(std::abs(sourceEnd), sourceLimit));
    return std::isfinite(projectEnd) && std::isfinite(sourceEnd) &&
           projectEnd <= static_cast<long double>(
                             std::numeric_limits<std::int64_t>::max()) &&
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
