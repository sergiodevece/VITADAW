#include "vitadaw/project/ProjectState.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <type_traits>

namespace vitadaw::project {

ProjectState::ProjectState(timeline::SampleRate projectSampleRate)
    : projectSampleRate_(projectSampleRate) {
    if (!projectSampleRate_.isValid()) {
        throw std::invalid_argument{"Project sample rate must be positive"};
    }
}

timeline::SampleRate ProjectState::sampleRate() const noexcept {
    return projectSampleRate_;
}

const std::vector<tracks::AudioTrack>& ProjectState::tracks() const noexcept {
    return tracks_;
}

const routing::RoutingState& ProjectState::routing() const noexcept {
    return routing_;
}

tracks::TrackId ProjectState::addAudioTrack(std::string name) {
    if (!nextTrackId_.isValid() ||
        nextTrackId_.value == std::numeric_limits<std::uint64_t>::max()) {
        throw std::overflow_error{"Audio track identity space exhausted"};
    }
    const auto id = nextTrackId_;
    tracks_.push_back({id, std::move(name), std::nullopt, {}, {}});
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
    timeline::ProjectFrameCount result;
    for (const auto& track : tracks_) {
        if (track.clip.has_value()) {
            result.value = std::max(result.value, track.clip->timelineRange.end().value);
        }
    }
    return result;
}

ProjectState::PreparedAudioClipUpdate ProjectState::prepareAudioClipUpdate(
    tracks::TrackId track,
    const std::filesystem::path& sourceFile,
    timeline::SourceFrameCount sourceFrameCount,
    timeline::SampleRate sourceSampleRate) const {
    const auto found = std::find_if(tracks_.begin(), tracks_.end(),
                                    [track](const auto& candidate) {
                                        return candidate.id == track;
                                    });
    if (found == tracks_.end()) {
        throw std::out_of_range{"Audio track does not exist"};
    }
    const auto projectFrameCount = timeline::sourceFramesToProjectDuration(
        sourceFrameCount, sourceSampleRate, projectSampleRate_);
    PreparedAudioClipUpdate update;
    update.track = track;
    update.trackIndex = static_cast<std::size_t>(found - tracks_.begin());
    update.replacement.emplace(clips::AudioClip{
        nextClipId_, sourceFile, {{0}, projectFrameCount}, {0},
        sourceFrameCount, sourceSampleRate});
    update.nextClipId = nextClipId_ + 1;
    return update;
}

void ProjectState::commitAudioClipUpdate(
    PreparedAudioClipUpdate& update) noexcept {
    static_assert(std::is_nothrow_swappable_v<
                  std::optional<clips::AudioClip>>);
    tracks_[update.trackIndex].clip.swap(update.replacement);
    nextClipId_ = update.nextClipId;
}

void ProjectState::swap(ProjectState& other) noexcept {
    using std::swap;
    swap(projectSampleRate_, other.projectSampleRate_);
    tracks_.swap(other.tracks_);
    swap(routing_, other.routing_);
    swap(nextTrackId_, other.nextTrackId_);
    swap(nextClipId_, other.nextClipId_);
    swap(masterMix_, other.masterMix_);
    swap(masterInserts_, other.masterInserts_);
    swap(nextProcessorId_, other.nextProcessorId_);
}

} // namespace vitadaw::project
