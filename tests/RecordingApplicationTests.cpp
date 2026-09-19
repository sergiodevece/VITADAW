#include "vitadaw/application/DawApplication.h"
#include "vitadaw/commands/CommandDispatcher.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <map>
#include <optional>
#include <span>

namespace {
using namespace vitadaw;

void check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

class MemoryFiles final : public platform::files::IProjectFileIO {
public:
    std::map<std::filesystem::path, std::string> content;

    platform::files::ReadResult read(const std::filesystem::path& path,
                                     std::size_t maximum) override {
        const auto found = content.find(path);
        if (found == content.end())
            return {{persistence::PersistenceCode::fileNotFound,
                     persistence::PersistencePhase::read}, {}};
        if (found->second.size() > maximum)
            return {{persistence::PersistenceCode::fileTooLarge,
                     persistence::PersistencePhase::read}, {}};
        const auto bytes = std::as_bytes(
            std::span{found->second.data(), found->second.size()});
        return {{}, {bytes.begin(), bytes.end()}};
    }

    persistence::PersistenceResult replace(
        const std::filesystem::path& path, std::string_view value) override {
        content[path] = value;
        return {};
    }
};

class RecordingEngine final : public audio::IAudioEngineControl {
public:
    struct Plan final : audio::PreparedProcessingPlanChange {};
    struct File final : audio::PreparedAudioFile {
        File(media::AudioChannelLayout layout,
             const std::filesystem::path& path)
            : PreparedAudioFile({timeline::SampleRate{44100.0},
                  media::channelCount(layout), {441}, {0.01}}) {
            media = {path, {}, media::MediaFingerprint{std::string(64, 'a'), 4096}};
            std::array<float, 441> samples{};
            waveform::PcmView view;
            view.channels = {samples.data(), samples.data()};
            view.channelCount = media::channelCount(layout);
            view.frameCount = {samples.size()};
            view.sampleRate = timeline::SampleRate{44100.0};
            waveform = waveform::prepareWaveform(view).prepared;
        }
    };

    audio::RealtimeTransportSnapshot transport;
    audio::RecordingSnapshot capture;
    audio::RecordingRequest preparedRequest;
    media::AudioChannelLayout preparedLayout{media::AudioChannelLayout::mono};
    std::filesystem::path mediaPath{"/virtual/Song Audio/Recording 000001.wav"};
    std::uint64_t nextSession{1};
    audio::AudioCommandSequence nextSequence{1};
    bool failPlan{};
    bool failFinalize{};
    bool failWaveform{};
    bool missingMedia{};
    bool cleanupFails{};
    bool deferFirstCallback{};
    bool recoveryMetadataAvailable{true};
    std::string recoveryWarning;
    std::optional<std::uint64_t> reportedInputLatency{0};
    timeline::SampleRate recordingDeviceRate{44100.0};
    std::uint32_t recordingBufferFrames{256};
    audio::DeviceLatencyReadModel latencyReadModel;
    unsigned commits{};
    unsigned discards{};

    audio::AudioFilePreparationResult prepareWav(
        const std::filesystem::path& path) override {
        return {std::make_unique<File>(preparedLayout, path), {}};
    }

    audio::StructuralPlanPreparationResult prepareProcessingPlan(
        const audio::ProcessingPlanSpecification&) override {
        if (failPlan) return {nullptr, "Injected plan failure"};
        return {std::make_unique<Plan>(), {}};
    }
    audio::StructuralPlanPreparationResult prepareProcessingPlanWithAudio(
        const audio::ProcessingPlanSpecification&, media::SourceId,
        audio::PreparedAudioFilePtr) override {
        if (failPlan) return {nullptr, "Injected plan failure"};
        return {std::make_unique<Plan>(), {}};
    }
    audio::StructuralPlanPreparationResult prepareProjectReplacement(
        const audio::ProcessingPlanSpecification&,
        std::vector<audio::PreparedSourceAudio>) override {
        if (failPlan) return {nullptr, "Injected plan failure"};
        return {std::make_unique<Plan>(), {}};
    }
    bool commitPreparedProcessingPlan(
        audio::PreparedProcessingPlanChangePtr plan,
        audio::AudioFileCommitAction action) noexcept override {
        if (!plan || !action.isValid()) return false;
        action.execute();
        ++commits;
        return true;
    }
    bool tryUpdateTrackMix(tracks::TrackId, mixer::PreparedTrackMixState,
                           audio::PreparedAudibilityState) noexcept override { return true; }
    bool tryUpdateBusMix(routing::BusId, mixer::PreparedBusMixState,
                         audio::PreparedAudibilityState) noexcept override { return true; }
    bool tryUpdateSendMix(routing::SendId,
                          mixer::PreparedSendMixState) noexcept override { return true; }
    bool tryUpdateMasterMix(mixer::PreparedMasterMixState) noexcept override { return true; }

    audio::AudioControlRequestResult result(
        transport::PlaybackState state,
        timeline::ProjectFramePosition position) noexcept {
        transport.playback = state;
        transport.playing = state == transport::PlaybackState::playing;
        transport.position = position;
        transport.lastProcessedCommandSequence = nextSequence;
        transport.projectedThroughTicket = nextSequence;
        return {true, nextSequence++, audio::AudioControlRejection::none,
                state, position, true};
    }
    audio::AudioControlRequestResult tryRequestPlay() noexcept override {
        return result(transport::PlaybackState::playing, transport.position);
    }
    audio::AudioControlRequestResult tryRequestPause() noexcept override {
        return result(transport::PlaybackState::paused, transport.position);
    }
    audio::AudioControlRequestResult tryRequestStop() noexcept override {
        if (capture.phase == audio::RecordingPhase::capturing)
            capture.phase = audio::RecordingPhase::complete;
        return result(transport::PlaybackState::stopped, transport.position);
    }
    audio::AudioControlRequestResult tryRequestSeek(
        timeline::ProjectFramePosition position) noexcept override {
        return result(transport::PlaybackState::stopped, position);
    }
    audio::AudioControlRequestResult trySetInputMonitoringEnabled(
        bool enabled) noexcept override {
        transport.monitoringEnabled = enabled;
        return result(transport.playback, transport.position);
    }
    bool trySetMonitorGain(audio::MonitorGainDb gain) noexcept override {
        if (!gain.isValid()) return false;
        transport.monitorGainDb = gain.value;
        return true;
    }
    audio::RealtimeTransportSnapshot transportSnapshot() const noexcept override {
        return transport;
    }
    audio::RealtimeTransportSnapshot projectedTransportSnapshot() noexcept override {
        return transport;
    }
    audio::DeviceLatencyReadModel deviceLatencyReadModel() const override {
        return latencyReadModel;
    }
    mixer::MeterSnapshot meterSnapshot() const noexcept override { return {}; }

    audio::RecordingPreflightResult prepareRecording(
        const audio::RecordingPreflightRequest& request) override {
        preparedLayout = request.layout;
        preparedRequest = {nextSession++, request.track, request.layout, {}};
        preparedRequest.placement.projectSampleRate = request.projectSampleRate;
        preparedRequest.placement.deviceSampleRate = recordingDeviceRate;
        preparedRequest.placement.deviceBufferFrames = recordingBufferFrames;
        preparedRequest.placement.manualOffsetProjectFrames = request.manualOffsetProjectFrames;
        if (reportedInputLatency) {
            preparedRequest.placement.reportedInputLatencyDeviceFrames = {*reportedInputLatency};
            const auto converted = audio::convertRecordingLatencyToProjectFrames(
                *preparedRequest.placement.reportedInputLatencyDeviceFrames,
                recordingDeviceRate, request.projectSampleRate);
            if (converted) {
                preparedRequest.placement.latencyStatus = audio::RecordingLatencyStatus::reported;
                preparedRequest.placement.reportedInputLatencyProjectFrames = *converted;
            } else {
                preparedRequest.placement.latencyStatus = audio::RecordingLatencyStatus::invalid;
            }
        }
        preparedRequest.placement.effectiveCompensationProjectFrames =
            audio::computeEffectiveRecordingCompensation(
                preparedRequest.placement.latencyStatus,
                preparedRequest.placement.reportedInputLatencyProjectFrames,
                request.manualOffsetProjectFrames).value_or(timeline::ProjectFrameCount{});
        return {preparedRequest, {}, recoveryWarning};
    }
    audio::AudioControlRequestResult tryRequestRecord(
        audio::RecordingRequest request) noexcept override {
        if (request.session != preparedRequest.session) return {};
        capture = {};
        capture.phase = audio::RecordingPhase::capturing;
        capture.failure = audio::RecordingFailure::none;
        capture.session = request.session;
        capture.track = request.track;
        capture.layout = request.layout;
        capture.projectStart = transport.position;
        capture.acceptedDeviceFrames = {441};
        capture.deviceSampleRate = timeline::SampleRate{44100.0};
        capture.placement = request.placement;
        if (deferFirstCallback) {
            capture.phase = audio::RecordingPhase::prepared;
            capture.acceptedDeviceFrames = {};
        }
        return result(transport::PlaybackState::playing, transport.position);
    }
    bool tryCancelRecording() noexcept override {
        if (capture.phase != audio::RecordingPhase::capturing) return false;
        capture.phase = audio::RecordingPhase::failed;
        capture.failure = audio::RecordingFailure::cancelled;
        return true;
    }
    audio::RecordingSnapshot recordingSnapshot() const noexcept override {
        return capture;
    }
    audio::RecordingFinalizationResult finalizeRecording() override {
        if (failFinalize)
            return {{}, capture, mediaPath, "Injected writer/decode failure", recoveryWarning};
        auto file = std::make_unique<File>(preparedLayout, mediaPath);
        if (failWaveform) {
            file->waveform.reset();
            file->waveformDiagnostic = "Injected waveform failure";
        }
        return {std::move(file), capture, mediaPath, {}, recoveryWarning};
    }
    audio::AudioFilePreparationResult prepareVerifiedWav(
        const std::filesystem::path& path,
        const media::MediaFingerprint& expected, std::size_t) override {
        if (missingMedia ||
            expected != media::MediaFingerprint{std::string(64, 'a'), 4096})
            return {nullptr, "Recorded media is missing or changed",
                    {persistence::PersistenceCode::mediaChanged,
                     persistence::PersistencePhase::media}};
        return {std::make_unique<File>(preparedLayout, path), {}};
    }
    bool discardRecording(bool) noexcept override {
        ++discards;
        capture = {};
        return !cleanupFails;
    }
    audio::RecordingCleanupResult discardRecordingWithDiagnostics(
        bool removePublished, std::string primary = {}) noexcept override {
        const auto clean = discardRecording(removePublished);
        audio::RecordingCleanupResult result;
        result.primaryError = std::move(primary);
        if (!recoveryMetadataAvailable && !recoveryWarning.empty()) {
            if (!result.primaryError.empty()) result.primaryError += "; ";
            result.primaryError += recoveryWarning;
        }
        if (!clean) result.retainedArtifacts.push_back(
            {"test-session", mediaPath, audio::RecordingRecoveryClass::publishedFinal, true});
        return result;
    }
    audio::RecordingCleanupResult shutdownRecording() noexcept override {
        return discardRecordingWithDiagnostics(true, "Recording cancelled during application shutdown");
    }
    void confirmRecordingCommit() noexcept override { capture = {}; }
};

commands::CommandResult send(application::DawApplication& app,
                             commands::Command command) {
    return app.handle(command);
}

void recoveryMetadataWarningDoesNotBlockRecording() {
    MemoryFiles files;
    RecordingEngine engine;
    engine.recoveryMetadataAvailable = false;
    engine.recoveryWarning =
        "Recording started, but crash-recovery metadata could not be persisted: "
        "create recovery marker '/virtual/Marker Audio/.vitadaw-recording-test.recovery' "
        "failed (13): Permission denied";
    application::DawApplication app{engine, timeline::SampleRate{48000.0}, files};
    check(send(app, commands::AddAudioTrack{"Marker warning"}).status ==
              commands::CommandStatus::accepted &&
              send(app, commands::SaveProjectAs{"/virtual/Marker.vitadaw"}).status ==
              commands::CommandStatus::accepted &&
              send(app, commands::SetTrackRecordArmed{{1}, true}).status ==
              commands::CommandStatus::accepted,
          "marker-warning fixture established");

    const auto started = send(app, commands::Record{});
    check(started.status == commands::CommandStatus::accepted &&
              engine.capture.phase == audio::RecordingPhase::capturing &&
              app.recordingPhase() == audio::RecordingPhase::capturing &&
              started.message.find("crash-recovery metadata") != std::string::npos,
          "non-fatal initial marker failure still creates writer/capture and accepts Record");
    check(send(app, commands::Stop{}).status == commands::CommandStatus::accepted,
          "markerless recording can stop normally");
    app.synchroniseTransport();
    check(app.recordingPhase() == audio::RecordingPhase::complete &&
              app.project().sources().size() == 1 &&
              app.project().tracks()[0].clips.size() == 1 &&
              app.recordingError().find("crash-recovery metadata") != std::string::npos,
          "markerless recording finalizes, commits, and retains its warning");
    check(send(app, commands::SaveProject{}).status == commands::CommandStatus::accepted,
          "markerless committed recording can be saved before failure atomicity checks");

    engine.failFinalize = true;
    check(send(app, commands::Record{}).status == commands::CommandStatus::accepted &&
              engine.capture.phase == audio::RecordingPhase::capturing,
          "subsequent Record remains possible without recovery metadata");
    check(send(app, commands::Stop{}).status == commands::CommandStatus::accepted,
          "markerless failure-cleanup recording can stop");
    app.synchroniseTransport();
    check(app.recordingPhase() == audio::RecordingPhase::failed &&
              app.project().sources().size() == 1 && !app.session().dirty() &&
              app.recordingError().find("crash-recovery metadata") != std::string::npos,
          "markerless failure cleanup remains atomic and reports the degraded recovery state");

    engine.failFinalize = false;
    check(send(app, commands::Record{}).status == commands::CommandStatus::accepted,
          "failure cleanup releases markerless session for another Record");
    app.shutdownRecording();
    check(app.recordingPhase() == audio::RecordingPhase::failed &&
              app.project().sources().size() == 1 && !app.session().dirty() &&
              app.recordingError().find("crash-recovery metadata") != std::string::npos,
          "graceful markerless shutdown retains media safely and reports unavailable metadata");
}

void recordingTransactionAndHistory() {
    MemoryFiles files;
    RecordingEngine engine;
    application::DawApplication app{engine, timeline::SampleRate{48000.0}, files};
    check(send(app, commands::AddAudioTrack{"Mono"}).status ==
              commands::CommandStatus::accepted,
          "track created");
    check(send(app, commands::Record{}).error ==
              commands::CommandError::selectTargetTrack,
          "unarmed Record rejected");
    check(send(app, commands::SetTrackRecordArmed{{1}, true}).status ==
              commands::CommandStatus::accepted &&
              send(app, commands::Record{}).error == commands::CommandError::invalidState,
          "unsaved-project Record restriction is enforced");
    check(send(app, commands::SetTrackRecordArmed{{1}, false}).status ==
              commands::CommandStatus::accepted,
          "unsaved fixture disarmed");
    check(send(app, commands::SaveProjectAs{"/virtual/Song.vitadaw"}).status ==
              commands::CommandStatus::accepted,
          "saved-project recording prerequisite");
    const auto cleanToken = app.history().currentStateToken();
    check(send(app, commands::SetTrackRecordArmed{{1}, true}).status ==
              commands::CommandStatus::accepted &&
              app.history().currentStateToken() == cleanToken,
          "arming is ephemeral and not undoable");
    check(send(app, commands::SeekToProjectFrame{{240}}).status ==
              commands::CommandStatus::accepted,
          "non-zero record locator prepared");
    check(send(app, commands::Record{}).status == commands::CommandStatus::accepted,
          "armed Record accepted");
    check(app.project().sources().empty() && app.project().tracks()[0].clips.empty(),
          "capture publishes no project state before finalization");
    for (const auto& forbidden : std::vector<commands::Command>{
             commands::Pause{}, commands::SeekToProjectFrame{{1}},
             commands::AddAudioTrack{"Blocked"}, commands::Undo{},
             commands::SaveProject{}}) {
        check(send(app, forbidden).error == commands::CommandError::invalidState,
              "forbidden capture operation rejected");
    }
    check(send(app, commands::Stop{}).status == commands::CommandStatus::accepted,
          "Stop completes capture");
    app.synchroniseTransport();
    check(app.recordingPhase() == audio::RecordingPhase::complete &&
              app.project().sources().size() == 1 &&
              app.project().tracks()[0].clips.size() == 1,
          "finalization atomically publishes one source and clip");
    const auto source = app.project().sources()[0];
    const auto clip = app.project().tracks()[0].clips[0];
    check(source.id == media::SourceId{1} && clip.id == clips::ClipId{1} &&
              source.sampleRate == timeline::SampleRate{44100.0} &&
              source.frameCount == timeline::SourceFrameCount{441} &&
              clip.projectStart == timeline::ProjectFramePosition{240} &&
              app.project().duration() == timeline::ProjectFrameCount{720},
          "TrackId, device metadata, exact start and converted duration retained");

    check(send(app, commands::Undo{}).status == commands::CommandStatus::accepted &&
              app.project().sources().empty() &&
              app.project().tracks()[0].clips.empty(),
          "Undo removes recorded source and clip without rewinding IDs");
    const auto redoCursor = app.history().cursor();
    engine.missingMedia = true;
    check(send(app, commands::Redo{}).status == commands::CommandStatus::rejected &&
              app.history().cursor() == redoCursor && app.project().sources().empty(),
          "Redo missing/changed WAV is atomic and preserves cursor");
    engine.missingMedia = false;
    check(send(app, commands::Redo{}).status == commands::CommandStatus::accepted &&
              app.project().sources()[0].id == source.id &&
              app.project().tracks()[0].clips[0].id == clip.id,
          "Redo restores exact recorded identities");
    const auto savedRecording = send(app, commands::SaveProject{});
    if (savedRecording.status != commands::CommandStatus::accepted)
        std::cerr << "save failure: " << savedRecording.message << " code="
                  << persistence::codeName(savedRecording.persistence.code) << '\n';
    check(savedRecording.status == commands::CommandStatus::accepted,
          "recorded project saves");
    check(send(app, commands::LoadProject{"/virtual/Song.vitadaw", true}).status ==
              commands::CommandStatus::accepted &&
              app.project().sources().size() == 1 &&
              app.project().tracks()[0].clips.size() == 1,
          "recorded source survives Save/Load round trip");
}

void recordToggleAndExplicitStopNeverLeavePlaybackRunning() {
    MemoryFiles files;
    RecordingEngine engine;
    application::DawApplication app{engine, timeline::SampleRate{48000.0}, files};
    check(send(app, commands::AddAudioTrack{"Record transport"}).status ==
              commands::CommandStatus::accepted &&
              send(app, commands::SaveProjectAs{"/virtual/Record transport.vitadaw"}).status ==
              commands::CommandStatus::accepted &&
              send(app, commands::SetTrackRecordArmed{{1}, true}).status ==
              commands::CommandStatus::accepted,
          "record-transport fixture established");

    check(send(app, commands::Record{}).status == commands::CommandStatus::accepted &&
              app.recordingPhase() == audio::RecordingPhase::capturing,
          "Stopped to Record starts the first take");
    check(send(app, commands::Record{}).status == commands::CommandStatus::accepted &&
              app.transport().playback == transport::PlaybackState::stopped &&
              engine.transport.playback == transport::PlaybackState::stopped,
          "second Record is a Command-System Stop and never an implicit Play");
    app.synchroniseTransport();
    check(app.recordingPhase() == audio::RecordingPhase::complete &&
              app.transport().playback == transport::PlaybackState::stopped &&
              app.project().tracks()[0].clips.size() == 1,
          "record-toggle finalizes WAV/commit with transport stopped");

    check(send(app, commands::Play{}).status == commands::CommandStatus::accepted &&
              send(app, commands::Record{}).error ==
                  commands::CommandError::transportMustBeStopped,
          "Playing to Record remains rejected by the existing stopped-only policy");
    check(send(app, commands::Stop{}).status == commands::CommandStatus::accepted,
          "playing-policy fixture returns to Stopped");

    check(send(app, commands::EnableInputMonitoring{}).status ==
              commands::CommandStatus::accepted,
          "Monitoring ON fixture enabled manually");
    app.synchroniseTransport();
    check(send(app, commands::Record{}).status == commands::CommandStatus::accepted &&
              send(app, commands::Record{}).status == commands::CommandStatus::accepted,
          "Record toggle remains available with Monitoring ON");
    app.synchroniseTransport();
    check(app.recordingPhase() == audio::RecordingPhase::complete &&
              app.transport().playback == transport::PlaybackState::stopped &&
              app.inputMonitoringEnabled() &&
              app.project().tracks()[0].clips.size() == 2,
          "Monitoring ON remains independent and second recording commits stopped");

    check(send(app, commands::DisableInputMonitoring{}).status ==
              commands::CommandStatus::accepted,
          "Monitoring OFF fixture disabled manually");
    app.synchroniseTransport();
    check(send(app, commands::Record{}).status == commands::CommandStatus::accepted &&
              send(app, commands::Stop{}).status == commands::CommandStatus::accepted,
          "explicit Stop remains the authoritative recording terminal command");
    app.synchroniseTransport();
    check(app.recordingPhase() == audio::RecordingPhase::complete &&
              app.transport().playback == transport::PlaybackState::stopped &&
              !app.inputMonitoringEnabled() &&
              app.project().tracks()[0].clips.size() == 3,
          "Monitoring OFF and explicit Stop finalize without implicit Play");
}

void failureAtomicityAndArmDeletion() {
    MemoryFiles files;
    RecordingEngine engine;
    application::DawApplication app{engine, timeline::SampleRate{48000.0}, files};
    check(send(app, commands::AddAudioTrack{"A"}).status == commands::CommandStatus::accepted,
          "failure track created");
    check(send(app, commands::SaveProjectAs{"/virtual/Failure.vitadaw"}).status ==
              commands::CommandStatus::accepted,
          "failure project saved");
    check(send(app, commands::SetTrackRecordArmed{{1}, true}).status ==
              commands::CommandStatus::accepted,
          "failure track armed");
    check(send(app, commands::Record{}).status == commands::CommandStatus::accepted,
          "failure recording started");
    engine.failFinalize = true;
    check(send(app, commands::Stop{}).status == commands::CommandStatus::accepted,
          "writer/decode failure recording stopped");
    app.synchroniseTransport();
    check(app.recordingPhase() == audio::RecordingPhase::failed &&
              app.project().sources().empty() && !app.session().dirty() &&
              engine.discards == 1,
          "writer/decode failure leaves project and history unchanged");
    engine.failFinalize = false;
    check(send(app, commands::Record{}).status == commands::CommandStatus::accepted,
          "waveform-failure recording restarted while arm remains set");
    engine.failWaveform = true;
    check(send(app, commands::Stop{}).status == commands::CommandStatus::accepted,
          "waveform-failure recording stopped");
    app.synchroniseTransport();
    check(app.recordingPhase() == audio::RecordingPhase::failed &&
              app.project().sources().empty() && !app.session().dirty() &&
              engine.discards == 2,
          "waveform failure leaves project and history unchanged");
    engine.failWaveform = false;
    check(send(app, commands::Record{}).status == commands::CommandStatus::accepted,
          "plan-failure recording restarted while arm remains set");
    engine.failPlan = true;
    engine.cleanupFails = true;
    check(send(app, commands::Stop{}).status == commands::CommandStatus::accepted,
          "failure recording stopped");
    app.synchroniseTransport();
    check(app.recordingPhase() == audio::RecordingPhase::failed &&
              app.project().sources().empty() && !app.session().dirty() &&
              engine.discards == 3 &&
              app.recordingError().find("retained for recovery") != std::string::npos,
          "plan failure is atomic and reports failed published-media cleanup");
    engine.failPlan = false;
    engine.cleanupFails = false;
    check(send(app, commands::DeleteAudioTrack{{1}}).status ==
              commands::CommandStatus::accepted && !app.armedTrack(),
          "deleting armed track clears ephemeral arm safely");
    check(send(app, commands::Undo{}).status == commands::CommandStatus::accepted &&
              send(app, commands::SetTrackRecordArmed{{1}, true}).status ==
                  commands::CommandStatus::accepted &&
              send(app, commands::Redo{}).status == commands::CommandStatus::accepted &&
              !app.armedTrack(),
          "history-driven track deletion also clears record arm");
}

void preparedDeviceLossCleansApplicationSession() {
    MemoryFiles files;
    RecordingEngine engine;
    application::DawApplication app{engine, timeline::SampleRate{48000.0}, files};
    check(send(app, commands::AddAudioTrack{"Prepared"}).status == commands::CommandStatus::accepted &&
              send(app, commands::SaveProjectAs{"/virtual/Prepared.vitadaw"}).status ==
                  commands::CommandStatus::accepted &&
              send(app, commands::SetTrackRecordArmed{{1}, true}).status ==
                  commands::CommandStatus::accepted,
          "prepared-loss fixture established");
    const auto historyToken = app.history().currentStateToken();
    engine.deferFirstCallback = true;
    check(send(app, commands::Record{}).status == commands::CommandStatus::accepted &&
              engine.capture.phase == audio::RecordingPhase::prepared,
          "Record remains prepared before the first callback");
    engine.capture.phase = audio::RecordingPhase::failed;
    engine.capture.failure = audio::RecordingFailure::deviceLost;
    app.synchroniseTransport();
    check(app.recordingPhase() == audio::RecordingPhase::failed && engine.discards == 1 &&
              app.project().sources().empty() && app.project().tracks()[0].clips.empty() &&
              app.history().currentStateToken() == historyToken && !app.session().dirty() &&
              app.armedTrack() == tracks::TrackId{1},
          "prepared device loss releases resources without model, history, dirty or arm mutation");
    engine.deferFirstCallback = false;
    check(send(app, commands::Record{}).status == commands::CommandStatus::accepted,
          "cleanup clears the active recording session for a subsequent Record");
}

void monitoringCommandsRemainAvailableDuringRecording() {
    MemoryFiles files;
    RecordingEngine engine;
    application::DawApplication app{engine, timeline::SampleRate{48000.0}, files};
    check(send(app, commands::AddAudioTrack{"Monitoring"}).status ==
              commands::CommandStatus::accepted &&
              send(app, commands::SaveProjectAs{"/virtual/Monitoring.vitadaw"}).status ==
                  commands::CommandStatus::accepted &&
              send(app, commands::SetTrackRecordArmed{{1}, true}).status ==
                  commands::CommandStatus::accepted &&
              send(app, commands::Record{}).status == commands::CommandStatus::accepted,
          "monitoring-during-recording fixture starts capture");
    const auto token = app.history().currentStateToken();
    const auto revision = app.timelineRevision();
    check(send(app, commands::SetMonitorGain{{-6.0F}}).status ==
              commands::CommandStatus::accepted &&
              send(app, commands::EnableInputMonitoring{}).status ==
                  commands::CommandStatus::accepted &&
              send(app, commands::DisableInputMonitoring{}).status ==
                  commands::CommandStatus::accepted,
          "monitoring commands remain admitted while recording is busy");
    check(engine.capture.phase == audio::RecordingPhase::capturing &&
              !engine.transport.monitoringEnabled && engine.transport.monitorGainDb == -6.0F &&
              app.history().currentStateToken() == token &&
              app.timelineRevision() == revision && !app.session().dirty(),
          "monitoring control leaves recording lifecycle and document history unchanged");
}

void placementIsFrozenAndRedoDoesNotRecalculate() {
    MemoryFiles files;
    RecordingEngine engine;
    engine.reportedInputLatency = 256;
    engine.recordingDeviceRate = timeline::SampleRate{44100.0};
    engine.recordingBufferFrames = 256;
    application::DawApplication app{engine, timeline::SampleRate{48000.0}, files};
    check(send(app, commands::AddAudioTrack{"Placement"}).status == commands::CommandStatus::accepted &&
              send(app, commands::SaveProjectAs{"/virtual/Placement.vitadaw"}).status == commands::CommandStatus::accepted &&
              send(app, commands::SetTrackRecordArmed{{1}, true}).status == commands::CommandStatus::accepted &&
              send(app, commands::SeekToProjectFrame{{1000}}).status == commands::CommandStatus::accepted &&
              send(app, commands::SetRecordingOffset{-16}).status == commands::CommandStatus::accepted,
          "placement fixture is established");
    check(send(app, commands::SetRecordingOffset{-96001}).status == commands::CommandStatus::rejected &&
              app.recordingPlacementReadModel().manualOffsetProjectFrames.value == -16,
          "out-of-range manual offset is rejected without changing the prior value");
    check(send(app, commands::Record{}).status == commands::CommandStatus::accepted &&
              send(app, commands::Stop{}).status == commands::CommandStatus::accepted,
          "placement recording completes");
    app.synchroniseTransport();
    const auto first = app.project().tracks()[0].clips[0];
    check(first.projectStart.value == 705,
          "44.1 device input latency converts to 279 project frames and combines with manual offset");
    check(send(app, commands::Undo{}).status == commands::CommandStatus::accepted,
          "placement recording undo succeeds");
    engine.reportedInputLatency = 0;
    engine.recordingDeviceRate = timeline::SampleRate{48000.0};
    check(send(app, commands::SetRecordingOffset{96}).status == commands::CommandStatus::accepted &&
              send(app, commands::Redo{}).status == commands::CommandStatus::accepted &&
              app.project().tracks()[0].clips[0].projectStart == first.projectStart,
          "redo restores the committed placement after latency and offset changes");
    check(send(app, commands::SaveProject{}).status == commands::CommandStatus::accepted &&
              send(app, commands::LoadProject{"/virtual/Placement.vitadaw", true}).status == commands::CommandStatus::accepted &&
              app.project().tracks()[0].clips[0].projectStart == first.projectStart,
          "save/load preserves compensated clip start without recalculation");
    // A later device/buffer configuration is captured only by its own preflight;
    // the already-published take remains documentary state, not a live latency view.
    engine.reportedInputLatency = 512;
    engine.recordingDeviceRate = timeline::SampleRate{44100.0};
    engine.recordingBufferFrames = 512;
    check(send(app, commands::SeekToProjectFrame{{1000}}).status == commands::CommandStatus::accepted &&
              send(app, commands::SetRecordingOffset{-16}).status == commands::CommandStatus::accepted &&
              send(app, commands::SetTrackRecordArmed{{1}, true}).status == commands::CommandStatus::accepted &&
              send(app, commands::Record{}).status == commands::CommandStatus::accepted &&
              send(app, commands::Stop{}).status == commands::CommandStatus::accepted,
          "second take accepts the later input-latency/buffer snapshot");
    app.synchroniseTransport();
    const auto second = app.project().tracks()[0].clips.front();
    check(first.projectStart.value == 705 && second.projectStart.value == 427 &&
              engine.preparedRequest.placement.deviceBufferFrames == 512,
          "new input latency affects only the later take while the first remains fixed");
    check(send(app, commands::SeekToProjectFrame{{1000}}).status == commands::CommandStatus::accepted &&
              send(app, commands::SetMonitorGain{{-3.0F}}).status == commands::CommandStatus::accepted &&
              send(app, commands::EnableInputMonitoring{}).status == commands::CommandStatus::accepted &&
              send(app, commands::SetTrackRecordArmed{{1}, true}).status == commands::CommandStatus::accepted &&
              send(app, commands::Record{}).status == commands::CommandStatus::accepted &&
              send(app, commands::Stop{}).status == commands::CommandStatus::accepted,
          "monitoring-on comparison take completes");
    app.synchroniseTransport();
    const auto& clips = app.project().tracks()[0].clips;
    const auto monitored = std::find_if(clips.begin(), clips.end(),
        [second](const auto& clip) {
            return clip.id != second.id && clip.projectStart == second.projectStart;
        });
    check(monitored != clips.end() && monitored->duration == second.duration &&
              app.project().findSource(monitored->source)->media ==
                  app.project().findSource(second.source)->media,
          "Monitoring and monitor gain do not alter documentary placement or clip end");
}

void recordingPlacementReadModelTracksUnavailableManualOffset() {
    MemoryFiles files;
    RecordingEngine engine;
    application::DawApplication app{engine, timeline::SampleRate{48000.0}, files};
    const auto verify = [&](std::int64_t manual, std::int64_t effective) {
        check(send(app, commands::SetRecordingOffset{manual}).status ==
                  commands::CommandStatus::accepted &&
                  app.recordingPlacementReadModel().latencyStatus ==
                      audio::RecordingLatencyStatus::unavailable &&
                  app.recordingPlacementReadModel().effectiveCompensationProjectFrames.value ==
                      effective,
              "unavailable latency read model retains the manual placement contribution");
    };
    verify(0, 0);
    verify(-16, 16);
    verify(16, -16);
    engine.latencyReadModel.configurationAvailable = true;
    engine.latencyReadModel.sampleRateHz = 48000.0;
    engine.latencyReadModel.inputLatencyFrames = 256;
    check(app.recordingPlacementReadModel().latencyStatus ==
              audio::RecordingLatencyStatus::reported &&
              app.recordingPlacementReadModel().effectiveCompensationProjectFrames.value == 240,
          "reported latency combines with the current manual offset through the shared formula");
}

void recordingOffsetBoundsAndClampDiagnosticRemainEphemeral() {
    const auto verifyBounds = [](timeline::SampleRate rate) {
        MemoryFiles files;
        RecordingEngine engine;
        application::DawApplication app{engine, rate, files};
        const auto limit = static_cast<std::int64_t>(std::llround(rate.hertz() * 2.0));
        const auto token = app.history().currentStateToken();
        check(send(app, commands::SetRecordingOffset{-limit}).status ==
                  commands::CommandStatus::accepted &&
                  send(app, commands::SetRecordingOffset{limit}).status ==
                  commands::CommandStatus::accepted &&
                  send(app, commands::SetRecordingOffset{limit + 1}).status ==
                  commands::CommandStatus::rejected &&
                  send(app, commands::SetRecordingOffset{-limit - 1}).status ==
                  commands::CommandStatus::rejected &&
                  app.recordingPlacementReadModel().manualOffsetProjectFrames.value == limit &&
                  app.history().currentStateToken() == token && !app.session().dirty(),
              "exact project-rate-derived manual bounds are accepted and out-of-range values are ephemeral rejects");
    };
    verifyBounds(timeline::SampleRate{44100.0});
    verifyBounds(timeline::SampleRate{96000.0});

    MemoryFiles files;
    RecordingEngine engine;
    engine.reportedInputLatency = 256;
    engine.recordingDeviceRate = timeline::SampleRate{48000.0};
    application::DawApplication app{engine, timeline::SampleRate{48000.0}, files};
    check(send(app, commands::AddAudioTrack{"Clamp"}).status == commands::CommandStatus::accepted &&
              send(app, commands::SaveProjectAs{"/virtual/Clamp.vitadaw"}).status == commands::CommandStatus::accepted &&
              send(app, commands::SetTrackRecordArmed{{1}, true}).status == commands::CommandStatus::accepted &&
              send(app, commands::SeekToProjectFrame{{255}}).status == commands::CommandStatus::accepted &&
              send(app, commands::Record{}).status == commands::CommandStatus::accepted &&
              send(app, commands::Stop{}).status == commands::CommandStatus::accepted,
          "one-frame clamp fixture records");
    app.synchroniseTransport();
    check(app.project().tracks()[0].clips[0].projectStart.value == 0 &&
              app.recordingPlacementReadModel().lastCommittedUnappliedEarlyFrames == 1,
          "one-frame clamp is committed at frame zero with its diagnostic");
    check(send(app, commands::Undo{}).status == commands::CommandStatus::accepted &&
              send(app, commands::Redo{}).status == commands::CommandStatus::accepted &&
              app.recordingPlacementReadModel().lastCommittedUnappliedEarlyFrames == 1,
          "undo/redo restores the committed clip without recalculating or altering clamp diagnostics");
}

void exclusiveEndPlacementFailureRetainsValidMediaAtomically() {
    MemoryFiles files;
    RecordingEngine engine;
    engine.cleanupFails = true;
    application::DawApplication app{engine, timeline::SampleRate{48000.0}, files};
    check(send(app, commands::AddAudioTrack{"Upper bound"}).status == commands::CommandStatus::accepted &&
              send(app, commands::SaveProjectAs{"/virtual/Upper.vitadaw"}).status == commands::CommandStatus::accepted &&
              send(app, commands::SetTrackRecordArmed{{1}, true}).status == commands::CommandStatus::accepted &&
              send(app, commands::SeekToProjectFrame{timeline::maximumSupportedProjectFrame()}).status ==
                  commands::CommandStatus::accepted,
          "exclusive-end overflow fixture is established");
    const auto token = app.history().currentStateToken();
    check(send(app, commands::Record{}).status == commands::CommandStatus::accepted &&
              send(app, commands::Stop{}).status == commands::CommandStatus::accepted,
          "exclusive-end overflow reaches finalization with valid media");
    app.synchroniseTransport();
    check(app.recordingPhase() == audio::RecordingPhase::failed && app.project().sources().empty() &&
              app.project().tracks()[0].clips.empty() && app.history().currentStateToken() == token &&
              !app.session().dirty() && engine.discards == 1 &&
              app.recordingError().find("retained for recovery") != std::string::npos,
          "exclusive-end overflow aborts only the documentary commit and retains valid media safely");
}

void shutdownCancelsWithoutCommitting() {
    MemoryFiles files;
    RecordingEngine engine;
    application::DawApplication app{engine, timeline::SampleRate{48000.0}, files};
    check(send(app, commands::AddAudioTrack{"Shutdown"}).status == commands::CommandStatus::accepted &&
              send(app, commands::SaveProjectAs{"/virtual/Shutdown.vitadaw"}).status == commands::CommandStatus::accepted &&
              send(app, commands::SetTrackRecordArmed{{1}, true}).status == commands::CommandStatus::accepted &&
              send(app, commands::Record{}).status == commands::CommandStatus::accepted,
          "shutdown fixture prepared");
    const auto token = app.history().currentStateToken();
    engine.cleanupFails = true;
    app.shutdownRecording();
    check(app.recordingPhase() == audio::RecordingPhase::failed && engine.discards == 1 &&
              app.project().sources().empty() && app.history().currentStateToken() == token &&
              !app.session().dirty() && app.armedTrack() == tracks::TrackId{1} &&
              app.recordingError().find("retained for recovery") != std::string::npos,
          "shutdown never commits and reports retained recovery media");
    app.shutdownRecording();
    check(engine.discards == 1, "repeated application shutdown is idempotent");

    RecordingEngine preparedEngine;
    application::DawApplication preparedApp{preparedEngine, timeline::SampleRate{48000.0}, files};
    check(send(preparedApp, commands::AddAudioTrack{"Prepared shutdown"}).status ==
              commands::CommandStatus::accepted &&
              send(preparedApp, commands::SaveProjectAs{"/virtual/PreparedShutdown.vitadaw"}).status ==
                  commands::CommandStatus::accepted &&
              send(preparedApp, commands::SetTrackRecordArmed{{1}, true}).status ==
                  commands::CommandStatus::accepted,
          "prepared shutdown fixture established");
    preparedEngine.deferFirstCallback = true;
    check(send(preparedApp, commands::Record{}).status == commands::CommandStatus::accepted,
          "prepared recording begins before shutdown");
    preparedApp.shutdownRecording();
    check(preparedEngine.discards == 1 && preparedApp.project().sources().empty() &&
              !preparedApp.session().dirty(),
          "shutdown also tears down a prepared session without committing");
}

void explicitRecoveryUsesTransactionalImport() {
    MemoryFiles files;
    RecordingEngine engine;
    application::DawApplication app{engine, timeline::SampleRate{48000.0}, files};
    check(send(app, commands::AddAudioTrack{"Recovery"}).status == commands::CommandStatus::accepted,
          "recovery target track created");
    const auto before = app.history().currentStateToken();
    const auto dirtyBefore = app.session().dirty();
    engine.failPlan = true;
    check(app.recoverRecording("/recovery/failed.wav", {1}).status ==
              commands::CommandStatus::rejected && app.project().sources().empty() &&
              app.history().currentStateToken() == before && app.session().dirty() == dirtyBefore,
          "failed explicit recovery leaves model, history and dirty state unchanged");
    engine.failPlan = false;
    check(app.recoverRecording("/recovery/salvaged.wav", {1}, {17}).status ==
              commands::CommandStatus::accepted && app.project().sources().size() == 1 &&
              app.project().tracks()[0].clips.size() == 1,
          "valid explicit recovery reuses normal transactional import");
}

} // namespace

int main() {
    recordingTransactionAndHistory();
    recordToggleAndExplicitStopNeverLeavePlaybackRunning();
    recoveryMetadataWarningDoesNotBlockRecording();
    failureAtomicityAndArmDeletion();
    preparedDeviceLossCleansApplicationSession();
    monitoringCommandsRemainAvailableDuringRecording();
    placementIsFrozenAndRedoDoesNotRecalculate();
    recordingPlacementReadModelTracksUnavailableManualOffset();
    recordingOffsetBoundsAndClampDiagnosticRemainEphemeral();
    exclusiveEndPlacementFailureRetainsValidMediaAtomically();
    shutdownCancelsWithoutCommitting();
    explicitRecoveryUsesTransactionalImport();
    std::cout << "Recording application tests passed\n";
}
