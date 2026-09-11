#include "vitadaw/application/DawApplication.h"

#include <exception>
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
                std::is_same_v<T, commands::SetTrackOutputDestination>) {
                if (transport_.playback == transport::PlaybackState::playing) {
                    return {commands::CommandStatus::rejected,
                            "Routing cannot change during playback"};
                }
                try {
                    auto candidate = project_;
                    if (!candidate.setTrackOutputDestination(
                            value.track, value.destination)) {
                        return {commands::CommandStatus::rejected,
                                "Invalid track output destination"};
                    }
                    return commitStructuralProject(
                        std::move(candidate), "Track output routing updated");
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
        result.buses.push_back({bus.id, mixer::prepare(bus.mix)});
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
    const auto effectiveTrackMix = [&](const auto& track) -> const auto& {
        return trackMix != nullptr && track.id == overriddenTrack
                   ? *trackMix
                   : track.mix;
    };
    const auto effectiveBusMix = [&](const auto& bus) -> const auto& {
        return busMix != nullptr && bus.id == overriddenBus ? *busMix
                                                            : bus.mix;
    };
    std::array<audio::AudibilityTrackInput, audio::audibilityTrackCapacity>
        resolvedTracks{};
    std::array<bool, audio::audibilityBusCapacity> busSolos{};
    for (std::size_t trackIndex = 0; trackIndex < tracks.size(); ++trackIndex) {
        resolvedTracks[trackIndex].solo =
            effectiveTrackMix(tracks[trackIndex]).solo;
        const auto* route = project.routing().findTrackRoute(
            tracks[trackIndex].id);
        if (route != nullptr &&
            route->destination.kind == routing::DestinationKind::bus) {
            for (std::size_t busIndex = 0; busIndex < buses.size(); ++busIndex) {
                if (buses[busIndex].id == route->destination.bus) {
                    resolvedTracks[trackIndex].destinationBusIndex = busIndex;
                    break;
                }
            }
        }
    }
    for (std::size_t busIndex = 0; busIndex < buses.size(); ++busIndex) {
        busSolos[busIndex] = effectiveBusMix(buses[busIndex]).solo;
    }
    return audio::resolveAudibility(
        {resolvedTracks.data(), tracks.size()},
        {busSolos.data(), buses.size()});
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
