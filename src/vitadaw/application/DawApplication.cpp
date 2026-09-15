#include "vitadaw/application/DawApplication.h"
#include "vitadaw/processors/GainProcessor.h"

#include <exception>
#include <algorithm>
#include <array>
#include <iomanip>
#include <new>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace vitadaw::application {

namespace {

const char* clipEditError(project::ProjectState::ClipEditStatus status) noexcept {
    using Status = project::ProjectState::ClipEditStatus;
    switch (status) {
    case Status::clipNotFound:
        return "Clip not found";
    case Status::invalidPosition:
        return "Invalid timeline position";
    case Status::zeroLengthClip:
        return "Edit would create a zero-length clip";
    case Status::sourceBoundsExceeded:
        return "Edit exceeds source bounds";
    case Status::capacityExceeded:
        return "Clip capacity exceeded";
    case Status::success:
        return "";
    }
    return "Clip edit failed";
}

commands::CommandError clipCommandError(
    project::ProjectState::ClipEditStatus status) noexcept {
    using Edit = project::ProjectState::ClipEditStatus;
    using Error = commands::CommandError;
    switch (status) {
    case Edit::clipNotFound:
        return Error::clipNotFound;
    case Edit::invalidPosition:
        return Error::invalidPosition;
    case Edit::zeroLengthClip:
        return Error::zeroLengthClip;
    case Edit::sourceBoundsExceeded:
        return Error::sourceBoundsExceeded;
    case Edit::capacityExceeded:
        return Error::capacityExceeded;
    case Edit::success:
        return Error::none;
    }
    return Error::preparationFailed;
}

} // namespace

DawApplication::DawApplication(audio::IAudioEngineControl& audioEngine,
                               timeline::SampleRate projectSampleRate,
                               platform::files::IProjectFileIO& files)
    : audioEngine_(audioEngine), session_(projectSampleRate), files_(files) {
    auto map = musical::PreparedMusicalTimeMap::compile(session_.project.musicalTime(), projectSampleRate);
    if (!map) throw std::invalid_argument("Invalid musical time context");
    musicalTime_ = std::move(map.value);
}

commands::CommandResult DawApplication::handle(const commands::Command& command) {
    using namespace commands;
    // Serialized application-thread entry point; RT never reads the history.
    try {
        return std::visit([&](const auto& value) -> CommandResult {
            using T = std::decay_t<decltype(value)>;
            if constexpr (commands::isMusicalCommand<T>) {
                return musicalCommand(command);
            } else if constexpr (std::is_same_v<T, SaveProject> || std::is_same_v<T, SaveProjectAs> ||
                          std::is_same_v<T, LoadProject>) {
                return persistenceCommand(command);
            } else if constexpr (std::is_same_v<T, Undo> || std::is_same_v<T, Redo>) {
                return traverseHistory(std::is_same_v<T, Redo>);
            } else if constexpr (std::is_same_v<T, MoveClip> ||
                                 std::is_same_v<T, DuplicateClip> ||
                                 std::is_same_v<T, SplitClip> ||
                                 std::is_same_v<T, TrimClipLeft> ||
                                 std::is_same_v<T, TrimClipRight> ||
                                 std::is_same_v<T, DeleteClip>) {
                if (transport_.playback == transport::PlaybackState::playing)
                    return {CommandStatus::rejected, "Stop before editing history",
                            CommandError::transportMustBeStopped};
                const auto* original = session_.project.findClip(value.clip);
                if (!original)
                    return {CommandStatus::rejected, "Clip not found", CommandError::clipNotFound};
                const auto before = *original;
                tracks::TrackId trackId;
                for (const auto& track : session_.project.tracks())
                    for (const auto& clip : track.clips)
                        if (clip.id == value.clip) trackId = track.id;
                auto candidate = session_.project;
                project::ProjectState::ClipEditResult edit;
                history::UndoableOperation operation;
                if constexpr (std::is_same_v<T, MoveClip>)
                    edit = candidate.moveClip(value.clip, value.projectStart);
                else if constexpr (std::is_same_v<T, DuplicateClip>)
                    edit = candidate.duplicateClip(value.clip, value.projectStart);
                else if constexpr (std::is_same_v<T, SplitClip>)
                    edit = candidate.splitClip(value.clip, value.splitPosition);
                else if constexpr (std::is_same_v<T, TrimClipLeft>)
                    edit = candidate.trimClipLeft(value.clip, value.projectStart);
                else if constexpr (std::is_same_v<T, TrimClipRight>)
                    edit = candidate.trimClipRight(value.clip, value.projectEnd);
                else edit = candidate.deleteClip(value.clip);
                if (!edit)
                    return {CommandStatus::rejected, clipEditError(edit.status),
                            clipCommandError(edit.status)};
                if constexpr (std::is_same_v<T, DuplicateClip>)
                    operation.payload = history::DuplicateClip{trackId, *candidate.findClip(edit.createdClip)};
                else if constexpr (std::is_same_v<T, SplitClip>)
                    operation.payload = history::SplitClip{trackId, before,
                        *candidate.findClip(value.clip), *candidate.findClip(edit.createdClip)};
                else if constexpr (std::is_same_v<T, DeleteClip>)
                    operation.payload = history::DeleteClip{trackId, before};
                else {
                    const auto after = *candidate.findClip(value.clip);
                    if (before == after) return {CommandStatus::accepted, "Clip unchanged"};
                    if constexpr (std::is_same_v<T, MoveClip>)
                        operation.payload = history::MoveClip{trackId, before, after};
                    else if constexpr (std::is_same_v<T, TrimClipLeft>)
                        operation.payload = history::TrimClipLeft{trackId, before, after};
                    else operation.payload = history::TrimClipRight{trackId, before, after};
                }
                auto pending = session_.history.stage(std::move(operation));
                if (!pending)
                    return {CommandStatus::rejected, "History capacity exceeded",
                            CommandError::historyCapacityExceeded};
                return commitStructuralProject(std::move(candidate), "Clip edit committed",
                                               &*pending);
            } else {
                constexpr bool persistent = !std::is_same_v<T, Play> && !std::is_same_v<T, Stop>;
                if constexpr (persistent) {
                    if (!session_.history.canCreateState())
                        return {CommandStatus::rejected, "History token capacity exceeded",
                                CommandError::historyCapacityExceeded};
                }
                return execute(command);
            }
        }, command);
    } catch (const std::bad_alloc&) {
        return {CommandStatus::rejected, {}, CommandError::historyCapacityExceeded};
    } catch (...) {
        return {CommandStatus::rejected, {}, CommandError::preparationFailed};
    }
}

commands::CommandResult DawApplication::traverseHistory(bool forward) {
    using namespace commands;
    if (transport_.playback == transport::PlaybackState::playing)
        return {CommandStatus::rejected, "Stop before Undo/Redo", CommandError::transportMustBeStopped};
    const auto* entry = forward ? session_.history.redoEntry() : session_.history.undoEntry();
    if (!entry) return {CommandStatus::rejected, "No history entry",
        forward ? CommandError::nothingToRedo : CommandError::nothingToUndo};
    if (!session_.history.canCreateState())
        return {CommandStatus::rejected, "Revision capacity exceeded", CommandError::historyCapacityExceeded};
    const auto expectedToken = forward ? entry->beforeStateToken : entry->afterStateToken;
    if (session_.history.currentStateToken() != expectedToken)
        return {CommandStatus::rejected, "History state diverged", CommandError::historyInvalid};
    auto candidate = session_.project;
    if (!entry->operation.apply(candidate, forward))
        return {CommandStatus::rejected, "History entities diverged", CommandError::historyInvalid};
    if (entry->operation.isMusical())
        return commitMusicalProject(std::move(candidate), nullptr, forward ? 1 : -1);
    return commitStructuralProject(std::move(candidate), forward ? "Redo committed" : "Undo committed",
                                   nullptr, forward ? 1 : -1);
}

commands::CommandResult DawApplication::execute(const commands::Command& command) {
    // Allocate diagnostics before any lightweight parameter publication.
    commands::CommandResult parameterSuccess{commands::CommandStatus::accepted,
                                               "Parameter updated"};
    return std::visit(
        [this, &parameterSuccess](const auto& value) -> commands::CommandResult {
            using T = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<T, commands::AddAudioTrack>) {
                try {
                    auto candidate = session_.project;
                    const auto id = candidate.addAudioTrack(value.name,
                                                             value.layout);
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
                    auto candidate = session_.project;
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
                    auto candidate = session_.project;
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
            } else if constexpr (std::is_same_v<T, commands::AddBusSend>) {
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
                    auto candidate = session_.project;
                    const auto id = candidate.addSend(
                        value.bus, value.destination, value.tapPoint,
                        mixer::SendMixState{value.level, false});
                    return commitStructuralProject(
                        std::move(candidate),
                        "Added bus send " + std::to_string(id.value));
                } catch (const std::bad_alloc&) {
                    return {commands::CommandStatus::rejected,
                            "Not enough memory to add send"};
                } catch (const std::exception&) {
                    return {commands::CommandStatus::rejected,
                            "Bus send could not be added"};
                }
            } else if constexpr (std::is_same_v<T, commands::SetSendRoute>) {
                if (transport_.playback == transport::PlaybackState::playing) {
                    return {commands::CommandStatus::rejected,
                            "Routing cannot change during playback"};
                }
                try {
                    auto candidate = session_.project;
                    if (!candidate.setSendRoute(
                            value.send, value.destination, value.tapPoint)) {
                        return {commands::CommandStatus::rejected,
                                "Invalid send route"};
                    }
                    return commitStructuralProject(std::move(candidate),
                                                   "Updated send route");
                } catch (const std::bad_alloc&) {
                    return {commands::CommandStatus::rejected,
                            "Not enough memory to update send route"};
                } catch (...) {
                    return {commands::CommandStatus::rejected,
                            "Send route could not be updated"};
                }
            } else if constexpr (std::is_same_v<T, commands::RemoveSend>) {
                if (transport_.playback == transport::PlaybackState::playing) {
                    return {commands::CommandStatus::rejected,
                            "Routing cannot change during playback"};
                }
                try {
                    auto candidate = session_.project;
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
            } else if constexpr (std::is_same_v<T, commands::LoadAudioFile> ||
                                 std::is_same_v<T, commands::ImportAudioToTrack>) {
                if (transport_.playback == transport::PlaybackState::playing) {
                    return {commands::CommandStatus::rejected,
                            "Clips cannot change during playback",
                            commands::CommandError::transportMustBeStopped};
                }
                const auto* destination = session_.project.findTrack(value.track);
                if (destination == nullptr) {
                    return {commands::CommandStatus::rejected,
                            "Audio track does not exist"};
                }
                audio::AudioFilePreparationResult preparation;
                try {
                    preparation = audioEngine_.prepareWav(value.file);
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

                std::optional<project::ProjectState> candidate;
                project::ProjectState::ImportedAudio imported;
                std::string successMessage;
                try {
                    const auto metadata = preparation.prepared->metadata;
                    const auto layout = metadata.channelCount == 1
                                            ? media::AudioChannelLayout::mono
                                            : media::AudioChannelLayout::stereo;
                    candidate.emplace(session_.project);
                    imported = candidate->importAudioToTrack(
                        value.track, preparation.prepared->media.isValid() ? preparation.prepared->media
                            : media::MediaReference{value.file, {}, {}},
                        metadata.sourceFrameCount, metadata.sourceSampleRate,
                        layout,
                        [&]() {
                            if constexpr (std::is_same_v<
                                              T, commands::ImportAudioToTrack>) {
                                return value.projectStart;
                            }
                            return timeline::ProjectFramePosition{0};
                        }());

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

                audio::StructuralPlanPreparationResult planPreparation;
                try {
                    planPreparation = audioEngine_.prepareProcessingPlanWithAudio(
                        makePlanSpecification(*candidate), imported.source,
                        std::move(preparation.prepared));
                } catch (const std::bad_alloc&) {
                    return {commands::CommandStatus::rejected,
                            "Not enough memory to prepare imported project"};
                } catch (...) {
                    return {commands::CommandStatus::rejected,
                            "Imported project could not be prepared"};
                }
                if (!planPreparation.success()) {
                    return {commands::CommandStatus::rejected,
                            std::move(planPreparation.errorMessage)};
                }
                struct CommitContext {
                    DawApplication* application;
                    project::ProjectState* candidate;
                    audio::PreparedAudibilityState audibility;
                } commitContext{this, &*candidate,
                                resolveAudibility(*candidate)};
                const audio::AudioFileCommitAction modelCommit{
                    &commitContext,
                    [](void* rawContext) noexcept {
                        auto& context = *static_cast<CommitContext*>(rawContext);
                        context.application->session_.project.swap(*context.candidate);
                        context.application->audibility_ = context.audibility;
                        context.application->transport_.stopAndRewind();
                        context.application->transport_.setDuration(
                            context.application->session_.project.duration());
                        context.application->session_.history.commitBarrier();
                    }};

                if (!audioEngine_.commitPreparedProcessingPlan(
                        std::move(planPreparation.prepared), modelCommit)) {
                    return {commands::CommandStatus::rejected,
                            "Prepared WAV could not be committed"};
                }

                pendingAudioCommandSequence_ =
                    audioEngine_.transportSnapshot().lastProcessedCommandSequence;
                return {commands::CommandStatus::accepted,
                        std::move(successMessage)};
            } else if constexpr (std::is_same_v<T, commands::AddClip> ||
                                 std::is_same_v<T, commands::RemoveClip> ||
                                 std::is_same_v<T, commands::RemoveSource>) {
                if (transport_.playback == transport::PlaybackState::playing) {
                    return {commands::CommandStatus::rejected,
                            "Clips cannot change during playback",
                            commands::CommandError::transportMustBeStopped};
                }
                try {
                    auto candidate = session_.project;
                    if constexpr (std::is_same_v<T, commands::AddClip>) {
                        static_cast<void>(candidate.addClip(
                            value.track, value.source, value.projectStart,
                            value.duration, value.sourceOffset));
                    } else if constexpr (std::is_same_v<
                                             T, commands::RemoveSource>) {
                        if (!candidate.removeSource(value.source)) {
                            return {commands::CommandStatus::rejected,
                                    "Source does not exist or remains referenced"};
                        }
                    } else {
                        const auto edit = candidate.deleteClip(value.clip);
                        if (!edit) {
                            return {commands::CommandStatus::rejected,
                                    clipEditError(edit.status),
                                    clipCommandError(edit.status)};
                        }
                    }
                    return commitStructuralProject(
                        std::move(candidate), "Clip structure updated");
                } catch (const std::bad_alloc&) {
                    return {commands::CommandStatus::rejected,
                            "Not enough memory to prepare clips"};
                } catch (const std::exception& error) {
                    return {commands::CommandStatus::rejected, error.what()};
                } catch (...) {
                    return {commands::CommandStatus::rejected,
                            "Clip update could not be prepared"};
                }
            } else if constexpr (std::is_same_v<T, commands::Play>) {
                if (transport_.playback == transport::PlaybackState::playing) {
                    return {commands::CommandStatus::accepted, "Already playing"};
                }
                const auto request = audioEngine_.tryRequestPlay();
                if (!request.accepted) {
                    return {commands::CommandStatus::rejected,
                            "Play requires at least one valid prepared WAV"};
                }
                pendingAudioCommandSequence_ = request.sequence;
                transport_.markPlaying();
                return {commands::CommandStatus::accepted, "Playing"};
            } else if constexpr (std::is_same_v<T, commands::Pause>) {
                if (transport_.playback != transport::PlaybackState::playing) {
                    return {commands::CommandStatus::accepted,
                            transport_.playback == transport::PlaybackState::paused
                                ? "Already paused" : "Already stopped"};
                }
                const auto request = audioEngine_.tryRequestPause();
                if (!request.accepted) {
                    return {commands::CommandStatus::rejected,
                            "Transport is unavailable",
                            commands::CommandError::transportUnavailable};
                }
                pendingAudioCommandSequence_ = request.sequence;
                transport_.markPaused();
                return {commands::CommandStatus::accepted, "Paused"};
            } else if constexpr (std::is_same_v<T, commands::Stop>) {
                const auto request = audioEngine_.tryRequestStop();
                if (!request.accepted) {
                    return {commands::CommandStatus::rejected, "Audio command queue is full"};
                }
                pendingAudioCommandSequence_ = request.sequence;
                const auto rewinding =
                    transport_.playback == transport::PlaybackState::stopped;
                transport_.stop();
                return {commands::CommandStatus::accepted,
                        rewinding ? "Stopped at start" : "Stopped"};
            } else if constexpr (
                std::is_same_v<T, commands::SeekToProjectFrame> ||
                std::is_same_v<T, commands::GoToStart> ||
                std::is_same_v<T, commands::GoToEnd>) {
                if (transport_.playback == transport::PlaybackState::playing) {
                    return {commands::CommandStatus::rejected,
                            "Seek while Playing is not supported in 0.5.1",
                            commands::CommandError::seekRejectedWhilePlaying};
                }
                const auto target = [&] {
                    if constexpr (std::is_same_v<T, commands::GoToStart>)
                        return timeline::ProjectFramePosition{0};
                    else if constexpr (std::is_same_v<T, commands::GoToEnd>)
                        return timeline::ProjectFramePosition{
                            session_.project.projectContentDuration().value};
                    else return value.position;
                }();
                const auto duration = session_.project.projectContentDuration();
                if (target.value < 0 || target.value > duration.value) {
                    return {commands::CommandStatus::rejected,
                            "Seek position is outside project content",
                            commands::CommandError::invalidPosition};
                }
                const auto request = audioEngine_.tryRequestSeek(target);
                if (!request.accepted) {
                    return {commands::CommandStatus::rejected,
                            "Transport is unavailable or its queue is full",
                            commands::CommandError::transportUnavailable};
                }
                pendingAudioCommandSequence_ = request.sequence;
                transport_.seek(target);
                return {commands::CommandStatus::accepted, "Playhead moved"};
            } else if constexpr (
                std::is_same_v<T, commands::SetTrackGain> ||
                std::is_same_v<T, commands::SetTrackPan> ||
                std::is_same_v<T, commands::SetTrackMute> ||
                std::is_same_v<T, commands::SetTrackSolo>) {
                const auto* track = session_.project.findTrack(value.track);
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
                    session_.project, value.track, &updated);
                const auto published = audioEngine_.tryUpdateTrackMix(
                    value.track, mixer::prepare(updated), audibility);
                if (!published) {
                    return {commands::CommandStatus::rejected,
                            "Mixer parameter queue is full"};
                }
                static_cast<void>(session_.project.setTrackMix(value.track, updated));
                audibility_ = audibility;
                session_.history.commitBarrier();
                return std::move(parameterSuccess);
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
                static_cast<void>(session_.project.setMasterMix(updated));
                session_.history.commitBarrier();
                return std::move(parameterSuccess);
            } else if constexpr (
                std::is_same_v<T, commands::SetSendLevel> ||
                std::is_same_v<T, commands::SetSendMute>) {
                const auto* send = session_.project.findSend(value.send);
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
                static_cast<void>(session_.project.setSendMix(value.send, updated));
                session_.history.commitBarrier();
                return std::move(parameterSuccess);
            } else if constexpr (
                std::is_same_v<T, commands::SetBusGain> ||
                std::is_same_v<T, commands::SetBusPan> ||
                std::is_same_v<T, commands::SetBusMute> ||
                std::is_same_v<T, commands::SetBusSolo>) {
                const auto* bus = session_.project.findBus(value.bus);
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
                    session_.project, {}, nullptr, value.bus, &updated);
                if (!audioEngine_.tryUpdateBusMix(
                        value.bus, mixer::prepare(updated), audibility)) {
                    return {commands::CommandStatus::rejected,
                            "Mixer parameter queue is full"};
                }
                static_cast<void>(session_.project.setBusMix(value.bus, updated));
                audibility_ = audibility;
                session_.history.commitBarrier();
                return std::move(parameterSuccess);
            } else if constexpr (
                std::is_same_v<T, commands::AddProcessor> ||
                std::is_same_v<T, commands::RemoveProcessor> ||
                std::is_same_v<T, commands::MoveProcessor>) {
                if (transport_.playback == transport::PlaybackState::playing) {
                    return {commands::CommandStatus::rejected,
                            "Insert structure cannot change during playback"};
                }
                try {
                    auto candidate = session_.project;
                    std::string message;
                    if constexpr (std::is_same_v<T,
                                                 commands::AddProcessor>) {
                        const auto id = candidate.addProcessor(value.target,
                                                               value.type);
                        message = "Added processor " +
                                  std::to_string(id.value);
                    } else if constexpr (std::is_same_v<
                                             T, commands::RemoveProcessor>) {
                        if (!candidate.removeProcessor(value.processor)) {
                            return {commands::CommandStatus::rejected,
                                    "Processor does not exist"};
                        }
                        message = "Removed processor";
                    } else {
                        if (!candidate.moveProcessor(value.processor,
                                                     value.newIndex)) {
                            return {commands::CommandStatus::rejected,
                                    "Processor or destination position is invalid"};
                        }
                        message = "Moved processor";
                    }
                    return commitStructuralProject(std::move(candidate),
                                                   std::move(message));
                } catch (const std::bad_alloc&) {
                    return {commands::CommandStatus::rejected,
                            "Not enough memory to prepare insert structure"};
                } catch (const std::exception&) {
                    return {commands::CommandStatus::rejected,
                            "Insert structure could not be prepared"};
                } catch (...) {
                    return {commands::CommandStatus::rejected,
                            "Unknown error while preparing insert structure"};
                }
            } else if constexpr (
                std::is_same_v<T, commands::SetProcessorBypass>) {
                if (session_.project.findProcessor(value.processor) == nullptr) {
                    return {commands::CommandStatus::rejected,
                            "Processor does not exist"};
                }
                if (!audioEngine_.tryUpdateProcessorBypass(
                        value.processor, value.bypassed)) {
                    return {commands::CommandStatus::rejected,
                            "Processor parameter queue is full"};
                }
                static_cast<void>(session_.project.setProcessorBypass(
                    value.processor, value.bypassed));
                session_.history.commitBarrier();
                return std::move(parameterSuccess);
            } else if constexpr (
                std::is_same_v<T, commands::SetProcessorParameter>) {
                const auto* processor = session_.project.findProcessor(value.processor);
                if (processor == nullptr) {
                    return {commands::CommandStatus::rejected,
                            "Processor does not exist"};
                }
                if (processor->type.identifier ==
                        processors::internalGainProcessorType &&
                    (value.parameter != processors::gainParameterId ||
                     !processors::GainProcessor::isValidGainDb(value.value))) {
                    return {commands::CommandStatus::rejected,
                            "Gain parameter must be finite and between -100 and +12 dB"};
                }
                const auto preparedValue =
                    processor->type.identifier ==
                            processors::internalGainProcessorType
                        ? processors::GainProcessor::gainDbToLinear(value.value)
                        : value.value;
                if (!audioEngine_.tryUpdateProcessorParameter(
                        value.processor, value.parameter, value.value,
                        preparedValue)) {
                    return {commands::CommandStatus::rejected,
                            "Processor parameter queue is full"};
                }
                static_cast<void>(session_.project.setProcessorParameter(
                    value.processor, value.parameter, value.value));
                session_.history.commitBarrier();
                return std::move(parameterSuccess);
            } else if constexpr (
                std::is_same_v<T, commands::SetTrackOutputDestination> ||
                std::is_same_v<T, commands::SetBusOutputDestination>) {
                if (transport_.playback == transport::PlaybackState::playing) {
                    return {commands::CommandStatus::rejected,
                            "Routing cannot change during playback"};
                }
                try {
                    auto candidate = session_.project;
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
    result.sources.reserve(project.sources().size());
    for (const auto& source : project.sources()) {
        result.sources.push_back({source.id, source.frameCount,
                                  source.sampleRate, source.layout});
    }
    result.buses.reserve(project.routing().buses().size());
    for (const auto& bus : project.routing().buses()) {
        result.buses.push_back(
            {bus.id, mixer::prepare(bus.mix), bus.outputDestination,
             bus.inserts});
    }
    result.masterMix = mixer::prepare(project.masterMix());
    result.masterInserts = project.masterInserts();
    result.tracks.reserve(project.tracks().size());
    for (const auto& track : project.tracks()) {
        const auto* route = project.routing().findTrackRoute(track.id);
        if (route == nullptr) {
            throw std::logic_error{"Audio track has no output routing"};
        }
        audio::ProcessingPlanTrackSpecification specification;
        specification.id = track.id;
        specification.mix = mixer::prepare(track.mix);
        specification.destination = route->destination;
        specification.inserts = track.inserts;
        specification.layout = track.layout;
        specification.clips = track.clips;
        result.tracks.push_back(std::move(specification));
    }
    result.sends.reserve(project.routing().sends().size());
    for (const auto& send : project.routing().sends()) {
        result.sends.push_back({send.id, send.source, send.destination,
                                send.tapPoint, mixer::prepare(send.mix)});
    }
    return result;
}

commands::CommandResult DawApplication::commitStructuralProject(
    project::ProjectState candidate, std::string successMessage,
    history::UndoManager::PendingAppend* pending, int historyDirection) {
    if (transport_.playback == transport::PlaybackState::playing) {
        return {commands::CommandStatus::rejected,
                "Routing cannot change during playback",
                commands::CommandError::transportMustBeStopped};
    }
    auto preparation = audioEngine_.prepareProcessingPlan(
        makePlanSpecification(candidate));
    if (!preparation.success()) {
        return {commands::CommandStatus::rejected,
                std::move(preparation.errorMessage),
                commands::CommandError::preparationFailed};
    }
    struct CommitContext {
        DawApplication* application;
        project::ProjectState* candidate;
        audio::PreparedAudibilityState audibility;
        history::UndoManager::PendingAppend* pending;
        int historyDirection;
    } context{this, &candidate, resolveAudibility(candidate), pending, historyDirection};
    const audio::AudioFileCommitAction commit{
        &context,
        [](void* raw) noexcept {
            auto& value = *static_cast<CommitContext*>(raw);
            value.application->session_.project.swap(*value.candidate);
            value.application->audibility_ = value.audibility;
            value.application->transport_.stopAndRewind();
            value.application->transport_.setDuration(
                value.application->session_.project.duration());
            if (value.pending)
                value.application->session_.history.commit(std::move(*value.pending));
            else if (value.historyDirection > 0)
                value.application->session_.history.commitRedo();
            else if (value.historyDirection < 0)
                value.application->session_.history.commitUndo();
            else value.application->session_.history.commitBarrier();
        }};
    if (!audioEngine_.commitPreparedProcessingPlan(
            std::move(preparation.prepared), commit)) {
        return {commands::CommandStatus::rejected,
                "Prepared routing could not be committed",
                commands::CommandError::preparationFailed};
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
    const auto sourceKind = [](const routing::SendSource& source) noexcept {
        return std::holds_alternative<tracks::TrackId>(source)
                   ? audio::AudibilitySendSourceKind::track
                   : audio::AudibilitySendSourceKind::bus;
    };
    const auto sourceIndex = [&trackIndex, &busIndex](
                                 const routing::SendSource& source) noexcept {
        if (const auto* track = std::get_if<tracks::TrackId>(&source)) {
            return trackIndex(*track);
        }
        return busIndex(std::get<routing::BusId>(source));
    };
    std::sort(orderedSends.begin(), orderedSends.begin() + sends.size(),
              [&sourceKind, &sourceIndex](const auto* left,
                                          const auto* right) {
                  const auto leftKind = sourceKind(left->source);
                  const auto rightKind = sourceKind(right->source);
                  if (leftKind != rightKind) {
                      return leftKind < rightKind;
                  }
                  const auto leftIndex = sourceIndex(left->source);
                  const auto rightIndex = sourceIndex(right->source);
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
        resolvedSends[index] = {
            sourceKind(send.source), sourceIndex(send.source),
            busIndex(send.destination),
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

    const auto playback = snapshot.playback == transport::PlaybackState::stopped &&
                                  snapshot.playing
                              ? transport::PlaybackState::playing
                              : snapshot.playback;
    transport_.synchronise(playback, snapshot.position, snapshot.duration);
}

const project::ProjectState& DawApplication::project() const noexcept {
    return session_.project;
}

const transport::TransportState& DawApplication::transport() const noexcept {
    return transport_;
}

mixer::MeterSnapshot DawApplication::meterSnapshot() const noexcept {
    return audioEngine_.meterSnapshot();
}

ui::timeline::TimelineSnapshot DawApplication::timelineSnapshot() const {
    auto result = ui::timeline::makeTimelineSnapshot(session_.project,
                                               session_.history.revision(),
                                               transport_);
    result.musicalRevision = musicalRevision_;
    const auto position = musicalTime_->musicalPositionAt(transport_.position);
    if (position) result.musicalPosition = position.value;
    return result;
}

} // namespace vitadaw::application
