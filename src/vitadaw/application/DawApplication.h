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

class DawApplication final : public commands::ICommandHandler {
public:
    DawApplication(audio::IAudioEngineControl& audioEngine,
                   timeline::SampleRate projectSampleRate,
                   platform::files::IProjectFileIO& files = platform::files::nativeProjectFileIO());

    commands::CommandResult handle(const commands::Command& command) override;
    void synchroniseTransport() noexcept;

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
    [[nodiscard]] const musical::PreparedMusicalTimeMap& musicalTime() const noexcept { return *musicalTime_; }
    [[nodiscard]] std::uint64_t musicalRevision() const noexcept { return musicalRevision_; }

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
        int historyDirection = 0);
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
    std::uint64_t musicalRevision_{};
};

} // namespace vitadaw::application
