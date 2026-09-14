#include "vitadaw/application/DawApplication.h"

#include <exception>
#include <algorithm>
#include <array>
#include <iomanip>
#include <new>
#include <optional>
#include <sstream>
#include <type_traits>
#include <utility>

namespace vitadaw::application {

DawApplication::DawApplication(audio::IAudioEngineControl& audioEngine,
                               timeline::SampleRate projectSampleRate)
    : audioEngine_(audioEngine), project_(projectSampleRate) {}

commands::CommandResult DawApplication::handle(const commands::Command& command) {
    return std::visit(
        [this](const auto& value) -> commands::CommandResult {
            using T = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<T, commands::AddAudioTrack>) {
                try {
                    auto candidate = project_;
                    const auto id = candidate.addAudioTrack(value.name);
                    return commitStructuralProject(
                        std::move(candidate),
                        "Added audio track " + std::to_string(id.value));
                } catch (const std::bad_alloc&) {
                    return {commands::CommandStatus::rejected,
                            "Not enough memory to add audio track"};
                } catch (const std::exception&) {
                    return {commands::CommandStatus::rejected,
                            "Audio track could not be added"};
                }
            } else if constexpr (std::is_same_v<T, commands::AddBus>) {
                if (transport_.playback == transport::PlaybackState::playing) {
                    return {commands::CommandStatus::rejected,
                            "Routing cannot change during playback"};
                }
                try {
                    auto candidate = project_;
                    const auto id = candidate.addBus(value.name);
                    return commitStructuralProject(
                        std::move(candidate),
                        "Added stereo bus " + std::to_string(id.value));
                } catch (const std::bad_alloc&) {
                    return {commands::CommandStatus::rejected,
                            "Not enough memory to add bus"};
                } catch (const std::exception&) {
                    return {commands::CommandStatus::rejected,
                            "Stereo bus could not be added"};
                }
            } else if constexpr (std::is_same_v<T, commands::AddTrackSend>) {
                if (transport_.playback == transport::PlaybackState::playing) {
                    return {commands::CommandStatus::rejected,
                            "Routing cannot change during playback"};
                }
                if (!value.level.isValid() ||
                    !routing::isValid(value.tapPoint)) {
                    return {commands::CommandStatus::rejected,
                            "Invalid send level or tap point"};
                }
                try {
                    auto candidate = project_;
                    const auto id = candidate.addSend(
                        value.track, value.destination, value.tapPoint,
                        mixer::SendMixState{value.level, false});
                    return commitStructuralProject(
                        std::move(candidate),
                        "Added track send " + std::to_string(id.value));
                } catch (const std::bad_alloc&) {
                    return {commands::CommandStatus::rejected,
                            "Not enough memory to add send"};
                } catch (const std::exception&) {
                    return {commands::CommandStatus::rejected,
                            "Track send could not be added"};
                }
            } else if constexpr (std::is_same_v<T, commands::RemoveSend>) {
                if (transport_.playback == transport::PlaybackState::playing) {
                    return {commands::CommandStatus::rejected,
                            "Routing cannot change during playback"};
                }
                try {
                    auto candidate = project_;
                    if (!candidate.removeSend(value.send)) {
                        return {commands::CommandStatus::rejected,
                                "Send does not exist"};
                    }
                    return commitStructuralProject(std::move(candidate),
                                                   "Removed send");
                } catch (const std::bad_alloc&) {
                    return {commands::CommandStatus::rejected,
                            "Not enough memory to remove send"};
                } catch (...) {
                    return {commands::CommandStatus::rejected,
                            "Send could not be removed"};
                }
            } else if constexpr (std::is_same_v<T, commands::LoadAudioFile>) {
                const auto* destination = project_.findTrack(value.track);
                if (destination == nullptr) {
                    return {commands::CommandStatus::rejected,
                            "Audio track does not exist"};
                }
                audio::AudioFilePreparationResult preparation;
                try {
                    preparation = audioEngine_.prepareWav(
                        value.file, value.track, project_.sampleRate(),
                        mixer::prepare(destination->mix));
                } catch (const std::bad_alloc&) {
                    return {commands::CommandStatus::rejected,
                            "Not enough memory to prepare WAV"};
                } catch (const std::exception&) {
                    return {commands::CommandStatus::rejected,
                            "Unexpected error while preparing WAV"};
                } catch (...) {
                    return {commands::CommandStatus::rejected,
                            "Unknown error while preparing WAV"};
                }
                if (!preparation.success()) {
                    return {commands::CommandStatus::rejected,
                            std::move(preparation.errorMessage)};
                }

                std::optional<project::ProjectState::PreparedAudioClipUpdate>
                    projectUpdate;
                std::string successMessage;
                try {
                    const auto metadata = preparation.prepared->metadata;
                    projectUpdate.emplace(project_.prepareAudioClipUpdate(
                        value.track, value.file, metadata.sourceFrameCount,
                        metadata.sourceSampleRate));

                    std::ostringstream message;
                    message << "Loaded track " << value.track.value
                            << ": " << value.file.filename().string() << " | "
                            << std::fixed << std::setprecision(0)
                            << metadata.sourceSampleRate.hertz() << " Hz | "
                            << metadata.channelCount << " ch | "
                            << std::setprecision(3) << metadata.duration.value
                            << " s";
                    successMessage = message.str();
                } catch (const std::bad_alloc&) {
                    return {commands::CommandStatus::rejected,
                            "Not enough memory to prepare project state"};
                } catch (const std::exception&) {
                    return {commands::CommandStatus::rejected,
                            "Project update could not be prepared"};
                } catch (...) {
                    return {commands::CommandStatus::rejected,
                            "Unknown error while preparing project state"};
                }

                struct CommitContext {
                    DawApplication* application;
                    project::ProjectState::PreparedAudioClipUpdate* update;
                } commitContext{this, &*projectUpdate};
                const audio::AudioFileCommitAction modelCommit{
                    &commitContext,
                    [](void* rawContext) noexcept {
                        auto& context = *static_cast<CommitContext*>(rawContext);
                        context.application->project_.commitAudioClipUpdate(
                            *context.update);
                        context.application->transport_.setDuration(
                            context.application->project_.duration());
                    }};

                if (!audioEngine_.commitPreparedWav(
                        std::move(preparation.prepared), modelCommit)) {
                    return {commands::CommandStatus::rejected,
                            "Prepared WAV could not be committed"};
                }

                pendingAudioCommandSequence_ =
                    audioEngine_.transportSnapshot().lastProcessedCommandSequence;
                return {commands::CommandStatus::accepted,
                        std::move(successMessage)};
            } else if constexpr (std::is_same_v<T, commands::Play>) {
                const auto request = audioEngine_.tryRequestPlay();
                if (!request.accepted) {
                    return {commands::CommandStatus::rejected,
                            "Play requires at least one valid prepared WAV"};
                }
                pendingAudioCommandSequence_ = request.sequence;
                transport_.markPlaying();
                return {commands::CommandStatus::accepted, "Playing"};
            } else if constexpr (std::is_same_v<T, commands::Stop>) {
                const auto request = audioEngine_.tryRequestStop();
                if (!request.accepted) {
                    return {commands::CommandStatus::rejected, "Audio command queue is full"};
                }
                pendingAudioCommandSequence_ = request.sequence;
                transport_.stopAndRewind();
                return {commands::CommandStatus::accepted, "Stopped at start"};
            } else if constexpr (
                std::is_same_v<T, commands::SetTrackGain> ||
                std::is_same_v<T, commands::SetTrackPan> ||
                std::is_same_v<T, commands::SetTrackMute> ||
                std::is_same_v<T, commands::SetTrackSolo>) {
                const auto* track = project_.findTrack(value.track);
                if (track == nullptr) {
                    return {commands::CommandStatus::rejected,
                            "Audio track does not exist"};
                }
                auto updated = track->mix;
                if constexpr (std::is_same_v<T, commands::SetTrackGain>) {
                    if (!value.gain.isValid()) {
                        return {commands::CommandStatus::rejected,
                                "Track gain must be finite and between -100 and +12 dB"};
                    }
                    updated.gain = value.gain;
                } else if constexpr (std::is_same_v<T, commands::SetTrackPan>) {
                    if (!value.pan.isValid()) {
                        return {commands::CommandStatus::rejected,
                                "Track pan must be finite and between -1 and +1"};
                    }
                    updated.pan = value.pan;
                } else if constexpr (std::is_same_v<T, commands::SetTrackMute>) {
                    updated.muted = value.muted;
                } else {
                    updated.solo = value.solo;
                }
                const auto audibility = resolveAudibility(
                    project_, value.track, &updated);
                const auto published = audioEngine_.tryUpdateTrackMix(
                    value.track, mixer::prepare(updated), audibility);
                if (!published) {
                    return {commands::CommandStatus::rejected,
                            "Mixer parameter queue is full"};
                }
                static_cast<void>(project_.setTrackMix(value.track, updated));
                audibility_ = audibility;
                return {commands::CommandStatus::accepted,
                        "Track mixer state updated"};
            } else if constexpr (std::is_same_v<T, commands::SetMasterGain>) {
                if (!value.gain.isValid()) {
                    return {commands::CommandStatus::rejected,
                            "Master gain must be finite and between -100 and +12 dB"};
                }
                const mixer::MasterMixState updated{value.gain};
                if (!audioEngine_.tryUpdateMasterMix(mixer::prepare(updated))) {
                    return {commands::CommandStatus::rejected,
                            "Mixer parameter queue is full"};
                }
                static_cast<void>(project_.setMasterMix(updated));
                return {commands::CommandStatus::accepted,
                        "Master mixer state updated"};
            } else if constexpr (
                std::is_same_v<T, commands::SetSendLevel> ||
                std::is_same_v<T, commands::SetSendMute>) {
                const auto* send = project_.findSend(value.send);
                if (send == nullptr) {
                    return {commands::CommandStatus::rejected,
                            "Send does not exist"};
                }
                auto updated = send->mix;
                if constexpr (std::is_same_v<T, commands::SetSendLevel>) {
                    if (!value.level.isValid()) {
                        return {commands::CommandStatus::rejected,
                                "Send level must be finite and between -100 and +12 dB"};
                    }
                    updated.level = value.level;
                } else {
                    updated.muted = value.muted;
                }
                if (!audioEngine_.tryUpdateSendMix(
                        value.send, mixer::prepare(updated))) {
                    return {commands::CommandStatus::rejected,
                            "Mixer parameter queue is full"};
                }
                static_cast<void>(project_.setSendMix(value.send, updated));
                return {commands::CommandStatus::accepted,
                        "Send mixer state updated"};
            } else if constexpr (
                std::is_same_v<T, commands::SetBusGain> ||
                std::is_same_v<T, commands::SetBusPan> ||
                std::is_same_v<T, commands::SetBusMute> ||
                std::is_same_v<T, commands::SetBusSolo>) {
                const auto* bus = project_.findBus(value.bus);
                if (bus == nullptr) {
                    return {commands::CommandStatus::rejected,
                            "Audio bus does not exist"};
                }
                auto updated = bus->mix;
                if constexpr (std::is_same_v<T, commands::SetBusGain>) {
                    if (!value.gain.isValid()) {
                        return {commands::CommandStatus::rejected,
                                "Bus gain must be finite and between -100 and +12 dB"};
                    }
                    updated.gain = value.gain;
                } else if constexpr (std::is_same_v<T, commands::SetBusPan>) {
                    if (!value.pan.isValid()) {
                        return {commands::CommandStatus::rejected,
                                "Bus balance must be finite and between -1 and +1"};
                    }
                    updated.balance = value.pan;
                } else if constexpr (std::is_same_v<T, commands::SetBusMute>) {
                    updated.muted = value.muted;
                } else {
                    updated.solo = value.solo;
                }
                const auto audibility = resolveAudibility(
                    project_, {}, nullptr, value.bus, &updated);
                if (!audioEngine_.tryUpdateBusMix(
                        value.bus, mixer::prepare(updated), audibility)) {
                    return {commands::CommandStatus::rejected,
                            "Mixer parameter queue is full"};
                }
                static_cast<void>(project_.setBusMix(value.bus, updated));
                audibility_ = audibility;
                return {commands::CommandStatus::accepted,
                        "Bus mixer state updated"};
            } else if constexpr (
                std::is_same_v<T, commands::SetTrackOutputDestination> ||
                std::is_same_v<T, commands::SetBusOutputDestination>) {
                if (transport_.playback == transport::PlaybackState::playing) {
                    return {commands::CommandStatus::rejected,
                            "Routing cannot change during playback"};
                }
                try {
                    auto candidate = project_;
                    if constexpr (std::is_same_v<
                                      T, commands::SetTrackOutputDestination>) {
                        if (!candidate.setTrackOutputDestination(
                                value.track, value.destination)) {
                            return {commands::CommandStatus::rejected,
                                    "Invalid track output destination"};
                        }
                    } else {
                        if (!candidate.setBusOutputDestination(
                                value.bus, value.destination)) {
                            return {commands::CommandStatus::rejected,
                                    "Invalid bus output destination"};
                        }
                    }
                    return commitStructuralProject(
                        std::move(candidate), "Output routing updated");
                } catch (const std::bad_alloc&) {
                    return {commands::CommandStatus::rejected,
                            "Not enough memory to prepare routing"};
                } catch (const std::exception&) {
                    return {commands::CommandStatus::rejected,
                            "Routing update could not be prepared"};
                } catch (...) {
                    return {commands::CommandStatus::rejected,
                            "Unknown error while preparing routing"};
                }
            }
            return {commands::CommandStatus::accepted, {}};
        },
        command);
}

audio::ProcessingPlanSpecification DawApplication::makePlanSpecification(
    const project::ProjectState& project) const {
    audio::ProcessingPlanSpecification result;
    result.projectSampleRate = project.sampleRate();
    result.buses.reserve(project.routing().buses().size());
    for (const auto& bus : project.routing().buses()) {
        result.buses.push_back(
            {bus.id, mixer::prepare(bus.mix), bus.outputDestination});
    }
    result.masterMix = mixer::prepare(project.masterMix());
    result.tracks.reserve(project.tracks().size());
    for (const auto& track : project.tracks()) {
        const auto* route = project.routing().findTrackRoute(track.id);
        if (route == nullptr) {
            throw std::logic_error{"Audio track has no output routing"};
        }
        result.tracks.push_back(
            {track.id, mixer::prepare(track.mix), route->destination});
    }
    result.sends.reserve(project.routing().sends().size());
    for (const auto& send : project.routing().sends()) {
        result.sends.push_back({send.id, send.source, send.destination,
                                send.tapPoint, mixer::prepare(send.mix)});
    }
    return result;
}

commands::CommandResult DawApplication::commitStructuralProject(
    project::ProjectState candidate, std::string successMessage) {
    if (transport_.playback == transport::PlaybackState::playing) {
        return {commands::CommandStatus::rejected,
                "Routing cannot change during playback"};
    }
    auto preparation = audioEngine_.prepareProcessingPlan(
        makePlanSpecification(candidate));
    if (!preparation.success()) {
        return {commands::CommandStatus::rejected,
                std::move(preparation.errorMessage)};
    }
    struct CommitContext {
        DawApplication* application;
        project::ProjectState* candidate;
        audio::PreparedAudibilityState audibility;
    } context{this, &candidate, resolveAudibility(candidate)};
    const audio::AudioFileCommitAction commit{
        &context,
        [](void* raw) noexcept {
            auto& value = *static_cast<CommitContext*>(raw);
            value.application->project_.swap(*value.candidate);
            value.application->audibility_ = value.audibility;
            value.application->transport_.stopAndRewind();
            value.application->transport_.setDuration(
                value.application->project_.duration());
        }};
    if (!audioEngine_.commitPreparedProcessingPlan(
            std::move(preparation.prepared), commit)) {
        return {commands::CommandStatus::rejected,
                "Prepared routing could not be committed"};
    }
    pendingAudioCommandSequence_ =
        audioEngine_.transportSnapshot().lastProcessedCommandSequence;
    return {commands::CommandStatus::accepted, std::move(successMessage)};
}

audio::PreparedAudibilityState DawApplication::resolveAudibility(
    const project::ProjectState& project, tracks::TrackId overriddenTrack,
    const mixer::TrackMixState* trackMix,
    routing::BusId overriddenBus,
    const mixer::BusMixState* busMix) const noexcept {
    const auto& tracks = project.tracks();
    const auto& buses = project.routing().buses();
    const auto& sends = project.routing().sends();
    const auto effectiveTrackMix = [&](const auto& track) -> const auto& {
        return trackMix != nullptr && track.id == overriddenTrack
                   ? *trackMix
                   : track.mix;
    };
    const auto effectiveBusMix = [&](const auto& bus) -> const auto& {
        return busMix != nullptr && bus.id == overriddenBus ? *busMix
                                                            : bus.mix;
    };
    std::array<const tracks::AudioTrack*, audio::audibilityTrackCapacity>
        orderedTracks{};
    std::array<const routing::AudioBus*, audio::audibilityBusCapacity>
        orderedBuses{};
    std::array<const routing::SendRoute*, audio::audibilitySendCapacity>
        orderedSends{};
    for (std::size_t index = 0; index < tracks.size(); ++index) {
        orderedTracks[index] = &tracks[index];
    }
    for (std::size_t index = 0; index < buses.size(); ++index) {
        orderedBuses[index] = &buses[index];
    }
    for (std::size_t index = 0; index < sends.size(); ++index) {
        orderedSends[index] = &sends[index];
    }
    std::sort(orderedTracks.begin(), orderedTracks.begin() + tracks.size(),
              [](const auto* left, const auto* right) {
                  return left->id < right->id;
              });
    std::sort(orderedBuses.begin(), orderedBuses.begin() + buses.size(),
              [](const auto* left, const auto* right) {
                  return left->id < right->id;
              });
    const auto busIndex = [&](routing::BusId id) noexcept {
        for (std::size_t index = 0; index < buses.size(); ++index) {
            if (orderedBuses[index]->id == id) {
                return index;
            }
        }
        return audio::audibilityMasterDestination;
    };
    const auto trackIndex = [&](tracks::TrackId id) noexcept {
        for (std::size_t index = 0; index < tracks.size(); ++index) {
            if (orderedTracks[index]->id == id) {
                return index;
            }
        }
        return audio::audibilityTrackCapacity;
    };
    std::sort(orderedSends.begin(), orderedSends.begin() + sends.size(),
              [&trackIndex](const auto* left, const auto* right) {
                  const auto leftTrack = std::get<tracks::TrackId>(left->source);
                  const auto rightTrack = std::get<tracks::TrackId>(right->source);
                  const auto leftIndex = trackIndex(leftTrack);
                  const auto rightIndex = trackIndex(rightTrack);
                  if (leftIndex != rightIndex) {
                      return leftIndex < rightIndex;
                  }
                  if (left->tapPoint != right->tapPoint) {
                      return left->tapPoint ==
                             routing::SendTapPoint::preFaderPrePan;
                  }
                  return left->id < right->id;
              });
    std::array<audio::AudibilityTrackInput, audio::audibilityTrackCapacity>
        resolvedTracks{};
    std::array<audio::AudibilityBusInput, audio::audibilityBusCapacity>
        resolvedBuses{};
    std::array<audio::AudibilitySendInput, audio::audibilitySendCapacity>
        resolvedSends{};
    for (std::size_t trackIndex = 0; trackIndex < tracks.size(); ++trackIndex) {
        const auto& track = *orderedTracks[trackIndex];
        resolvedTracks[trackIndex].solo =
            effectiveTrackMix(track).solo;
        const auto* route = project.routing().findTrackRoute(
            track.id);
        if (route != nullptr &&
            route->destination.kind == routing::DestinationKind::bus) {
            resolvedTracks[trackIndex].destinationBusIndex =
                busIndex(route->destination.bus);
        }
    }
    for (std::size_t index = 0; index < buses.size(); ++index) {
        const auto& bus = *orderedBuses[index];
        resolvedBuses[index].solo = effectiveBusMix(bus).solo;
        if (bus.outputDestination.kind == routing::DestinationKind::bus) {
            resolvedBuses[index].destinationBusIndex =
                busIndex(bus.outputDestination.bus);
        }
    }
    for (std::size_t index = 0; index < sends.size(); ++index) {
        const auto& send = *orderedSends[index];
        const auto source = std::get<tracks::TrackId>(send.source);
        resolvedSends[index] = {
            audio::AudibilitySendSourceKind::track,
            trackIndex(source), busIndex(send.destination),
            send.tapPoint == routing::SendTapPoint::postFaderPostPan};
    }
    return audio::resolveAudibility(
        {resolvedTracks.data(), tracks.size()},
        {resolvedBuses.data(), buses.size()},
        {resolvedSends.data(), sends.size()});
}

void DawApplication::synchroniseTransport() noexcept {
    const auto snapshot = audioEngine_.transportSnapshot();
    if (snapshot.lastProcessedCommandSequence < pendingAudioCommandSequence_) {
        return;
    }

    transport_.synchronise(snapshot.playing, snapshot.position, snapshot.duration);
}

const project::ProjectState& DawApplication::project() const noexcept {
    return project_;
}

const transport::TransportState& DawApplication::transport() const noexcept {
    return transport_;
}

mixer::MeterSnapshot DawApplication::meterSnapshot() const noexcept {
    return audioEngine_.meterSnapshot();
}

} // namespace vitadaw::application
