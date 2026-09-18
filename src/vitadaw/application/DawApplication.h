#pragma once

#include "vitadaw/audio/IAudioEngineControl.h"
#include "vitadaw/commands/Command.h"
#include "vitadaw/project/ProjectState.h"
#include "vitadaw/transport/TransportState.h"
#include "vitadaw/history/UndoManager.h"
#include "vitadaw/application/ProjectSession.h"
#include "vitadaw/platform/files/ProjectFileIO.h"
#include "vitadaw/ui/timeline/TimelineModel.h"

namespace vitadaw::application {

// Ephemeral UI read model. It is populated from bounded RT publications and
// deliberately has no ProjectState or history representation.
struct InputMonitoringReadModel {
    bool enabled{};
    audio::MonitorGainDb gain;
    bool routeSupported{true};
    bool lifecycleForcedOff{};
    bool inputAvailable{};
    mixer::StereoPeak inputPeak;
};

class DawApplication final : public commands::ICommandHandler {
public:
    DawApplication(audio::IAudioEngineControl& audioEngine,
                   timeline::SampleRate projectSampleRate,
                   platform::files::IProjectFileIO& files = platform::files::nativeProjectFileIO());

    commands::CommandResult handle(const commands::Command& command) override;
    void synchroniseTransport() noexcept;
    // Called by native shutdown before its audio adapter or this application is
    // destroyed. It is intentionally cancellation/recovery only: no model or
    // history commit may occur here.
    void shutdownRecording() noexcept;
    [[nodiscard]] commands::CommandResult recoverRecording(
        const std::filesystem::path&, tracks::TrackId,
        timeline::ProjectFramePosition = {});

    [[nodiscard]] const project::ProjectState& project() const noexcept;
    [[nodiscard]] const transport::TransportState& transport() const noexcept;
    [[nodiscard]] mixer::MeterSnapshot meterSnapshot() const noexcept;
    [[nodiscard]] const history::UndoManager& history() const noexcept { return session_.history; }
    [[nodiscard]] bool canUndo() const noexcept { return session_.history.canUndo(); }
    [[nodiscard]] bool canRedo() const noexcept { return session_.history.canRedo(); }
    [[nodiscard]] std::string_view undoLabel() const noexcept { return session_.history.undoLabel(); }
    [[nodiscard]] std::string_view redoLabel() const noexcept { return session_.history.redoLabel(); }
    void clearHistory() noexcept { session_.history.clearHistory(); }
    [[nodiscard]] const ProjectSession& session() const noexcept { return session_; }
    [[nodiscard]] std::uint64_t timelineRevision() const noexcept {
        return session_.history.revision();
    }
    [[nodiscard]] ui::timeline::TimelineSnapshot timelineSnapshot() const;
    [[nodiscard]] const waveform::WaveformCache& waveformCache() const noexcept {
        return session_.waveforms;
    }
    [[nodiscard]] const musical::PreparedMusicalTimeMap& musicalTime() const noexcept { return *musicalTime_; }
    [[nodiscard]] std::uint64_t musicalRevision() const noexcept { return musicalRevision_; }
    [[nodiscard]] bool loopEnabled() const noexcept { return session_.loopEnabled; }
    [[nodiscard]] bool metronomeEnabled() const noexcept { return session_.metronomeEnabled; }
    [[nodiscard]] audio::MetronomeLevelDb metronomeLevel() const noexcept { return session_.metronomeLevel; }
    [[nodiscard]] bool inputMonitoringEnabled() const noexcept {
        return inputMonitoringEnabled_;
    }
    [[nodiscard]] audio::MonitorGainDb monitorGain() const noexcept {
        return monitorGain_;
    }
    [[nodiscard]] InputMonitoringReadModel inputMonitoringReadModel() const noexcept;
    [[nodiscard]] std::optional<tracks::TrackId> armedTrack() const noexcept {
        return session_.armedTrack;
    }
    [[nodiscard]] audio::RecordingPhase recordingPhase() const noexcept {
        return recordingPhase_;
    }
    [[nodiscard]] const std::string& recordingError() const noexcept {
        return recordingError_;
    }
    [[nodiscard]] ui::timeline::MetronomeReadModel
        metronomeReadModel() const noexcept;

private:
    commands::CommandResult musicalCommand(const commands::Command&);
    commands::CommandResult commitMusicalProject(project::ProjectState,
        history::UndoManager::PendingAppend* pending = nullptr, int historyDirection = 0,
        std::unique_ptr<const musical::PreparedMusicalTimeMap> prepared = {});
    commands::CommandResult persistenceCommand(const commands::Command& command);
    persistence::PersistenceResult saveProject(std::filesystem::path path);
    persistence::PersistenceResult loadProject(std::filesystem::path path, bool discard);
    commands::CommandResult execute(const commands::Command& command);
    commands::CommandResult traverseHistory(bool forward);
    [[nodiscard]] audio::ProcessingPlanSpecification makePlanSpecification(
        const project::ProjectState& project) const;
    [[nodiscard]] commands::CommandResult commitStructuralProject(
        project::ProjectState candidate, std::string successMessage,
        history::UndoManager::PendingAppend* pending = nullptr,
        int historyDirection = 0,
        std::vector<clips::ClipId> createdClips = {});
    [[nodiscard]] commands::CommandResult commitRecordedAudio(
        audio::RecordingFinalizationResult);
    [[nodiscard]] commands::CommandResult redoRecordedAudio(
        const history::RecordAudio&);
    void synchroniseRecording() noexcept;
    [[nodiscard]] bool recordingBusy() const noexcept {
        return activeRecording_.session != 0;
    }
    [[nodiscard]] audio::PreparedAudibilityState resolveAudibility(
        const project::ProjectState& project,
        tracks::TrackId overriddenTrack = {},
        const mixer::TrackMixState* trackMix = nullptr,
        routing::BusId overriddenBus = {},
        const mixer::BusMixState* busMix = nullptr) const noexcept;

    audio::IAudioEngineControl& audioEngine_;
    ProjectSession session_;
    platform::files::IProjectFileIO& files_;
    transport::TransportState transport_;
    audio::PreparedAudibilityState audibility_;
    audio::AudioCommandSequence pendingAudioCommandSequence_{};
    std::unique_ptr<const musical::PreparedMusicalTimeMap> musicalTime_;
    std::optional<audio::PreparedLoopView> preparedLoopView_;
    std::uint64_t musicalRevision_{};
    audio::RecordingRequest activeRecording_;
    audio::RecordingPhase recordingPhase_{audio::RecordingPhase::idle};
    std::string recordingError_;
    // Application-only read/request model. It must never become a ProjectState
    // or ProjectSession field because Monitoring is not document state.
    bool inputMonitoringEnabled_{};
    bool requestedInputMonitoringEnabled_{};
    bool inputMonitoringRouteSupported_{true};
    bool inputMonitoringLifecycleForcedOff_{};
    bool inputMeterAvailable_{};
    mixer::StereoPeak inputMeterPeak_;
    audio::MonitorGainDb monitorGain_{};
    audio::MonitorGainDb requestedMonitorGain_{};
};

} // namespace vitadaw::application
