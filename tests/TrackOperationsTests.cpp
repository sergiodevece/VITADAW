#include "vitadaw/application/DawApplication.h"
#include "vitadaw/audio/RealtimeAudioEngine.h"
#include "vitadaw/commands/CommandDispatcher.h"
#include "vitadaw/persistence/ProjectPersistence.h"
#include "vitadaw/processors/GainProcessor.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <new>
#include <vector>

namespace {
using namespace vitadaw;
std::atomic<bool> inRealtime{};
std::atomic<unsigned> realtimeAllocations{};

void check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

class TestEngine final : public audio::IAudioEngineControl {
public:
    struct Plan final : audio::PreparedProcessingPlanChange {
        explicit Plan(audio::ProcessingPlanSpecification value,
                      unsigned* destroyed) noexcept
            : specification(std::move(value)), destroyed(destroyed) {}
        ~Plan() override { if (destroyed != nullptr) ++*destroyed; }
        audio::ProcessingPlanSpecification specification;
        unsigned* destroyed{};
    };

    audio::AudioFilePreparationResult prepareWav(
        const std::filesystem::path& path) override {
        const auto stereo = path.string().find("stereo") != std::string::npos;
        const auto channels = stereo ? 2U : 1U;
        auto result = std::make_unique<audio::PreparedAudioFile>(
            audio::AudioFileMetadata{timeline::SampleRate{48000.0}, channels,
                                     {48000}, {1.0}});
        result->media = {
            std::filesystem::path{"/tmp"} / path.filename(), {},
            media::MediaFingerprint{std::string(64, 'a'), 96000}};
        return {std::move(result), {}};
    }
    audio::StructuralPlanPreparationResult prepareProcessingPlan(
        const audio::ProcessingPlanSpecification& specification) override {
        ++preparations;
        if (failPreparation) {
            failPreparation = false;
            return {nullptr, "Injected structural preparation failure"};
        }
        return {std::make_unique<Plan>(specification, &destroyedPlans), {}};
    }
    audio::StructuralPlanPreparationResult prepareProcessingPlanWithAudio(
        const audio::ProcessingPlanSpecification& specification,
        media::SourceId, audio::PreparedAudioFilePtr) override {
        return prepareProcessingPlan(specification);
    }
    bool commitPreparedProcessingPlan(
        audio::PreparedProcessingPlanChangePtr prepared,
        audio::AudioFileCommitAction commit) noexcept override {
        if (failCommit) {
            failCommit = false;
            return false;
        }
        auto next = std::unique_ptr<Plan>(
            static_cast<Plan*>(prepared.release()));
        active.swap(next);
        commit.execute();
        ++commits;
        return true;
    }
    bool tryUpdateTrackMix(tracks::TrackId, mixer::PreparedTrackMixState,
                           audio::PreparedAudibilityState) noexcept override {
        return true;
    }
    bool tryUpdateBusMix(routing::BusId, mixer::PreparedBusMixState,
                         audio::PreparedAudibilityState) noexcept override {
        return true;
    }
    bool tryUpdateSendMix(routing::SendId,
                          mixer::PreparedSendMixState) noexcept override {
        return true;
    }
    bool tryUpdateMasterMix(mixer::PreparedMasterMixState) noexcept override {
        return true;
    }
    bool tryUpdateProcessorBypass(processors::ProcessorInstanceId,
                                  bool) noexcept override { return true; }
    bool tryUpdateProcessorParameter(processors::ProcessorInstanceId,
        processors::ParameterId, float, float) noexcept override { return true; }
    audio::AudioControlRequestResult tryRequestPlay() noexcept override {
        snapshot.playback = transport::PlaybackState::playing;
        snapshot.lastProcessedCommandSequence = ++sequence;
        return {true, sequence, audio::AudioControlRejection::none,
                snapshot.playback, snapshot.position, true};
    }
    audio::AudioControlRequestResult tryRequestPause() noexcept override {
        snapshot.playback = transport::PlaybackState::paused;
        snapshot.lastProcessedCommandSequence = ++sequence;
        return {true, sequence, audio::AudioControlRejection::none,
                snapshot.playback, snapshot.position, true};
    }
    audio::AudioControlRequestResult tryRequestStop() noexcept override {
        snapshot.playback = transport::PlaybackState::stopped;
        snapshot.lastProcessedCommandSequence = ++sequence;
        return {true, sequence, audio::AudioControlRejection::none,
                snapshot.playback, snapshot.position, true};
    }
    audio::AudioControlRequestResult tryRequestSeek(
        timeline::ProjectFramePosition position) noexcept override {
        snapshot.position = position;
        snapshot.lastProcessedCommandSequence = ++sequence;
        return {true, sequence, audio::AudioControlRejection::none,
                snapshot.playback, snapshot.position, true};
    }
    audio::AudioControlRequestResult trySetLoopEnabled(bool enabled) noexcept override {
        snapshot.loopEnabled = enabled;
        snapshot.lastProcessedCommandSequence = ++sequence;
        return {true, sequence};
    }
    audio::AudioControlRequestResult trySetMetronomeEnabled(bool enabled) noexcept override {
        snapshot.metronomeEnabled = enabled;
        snapshot.lastProcessedCommandSequence = ++sequence;
        return {true, sequence};
    }
    audio::AudioControlRequestResult trySetMetronomeLevel(
        audio::MetronomeLevelDb level) noexcept override {
        snapshot.metronomeLevelDb = level.value;
        snapshot.lastProcessedCommandSequence = ++sequence;
        return {true, sequence};
    }
    audio::RealtimeTransportSnapshot transportSnapshot() const noexcept override {
        return snapshot;
    }
    mixer::MeterSnapshot meterSnapshot() const noexcept override { return {}; }

    bool failPreparation{};
    bool failCommit{};
    unsigned preparations{};
    unsigned commits{};
    unsigned destroyedPlans{};
    std::unique_ptr<Plan> active;
    audio::RealtimeTransportSnapshot snapshot;
    audio::AudioCommandSequence sequence{};
};

audio::ProcessingPlanSpecification specification(
    const project::ProjectState& project) {
    audio::ProcessingPlanSpecification result;
    result.projectSampleRate = project.sampleRate();
    result.processingSampleRate = project.sampleRate();
    for (const auto& source : project.sources())
        result.sources.push_back({source.id, source.frameCount,
                                  source.sampleRate, source.layout});
    for (const auto& track : project.tracks()) {
        const auto* route = project.routing().findTrackRoute(track.id);
        result.tracks.push_back({track.id, mixer::prepare(track.mix),
            route->destination, track.inserts, track.layout, track.clips});
    }
    for (const auto& bus : project.routing().buses())
        result.buses.push_back({bus.id, mixer::prepare(bus.mix),
            bus.outputDestination, bus.inserts});
    for (const auto& send : project.routing().sends())
        result.sends.push_back({send.id, send.source, send.destination,
            send.tapPoint, mixer::prepare(send.mix)});
    result.masterMix = mixer::prepare(project.masterMix());
    result.masterInserts = project.masterInserts();
    return result;
}

float renderFirstSample(const project::ProjectState& project,
                        audio::PreparedSourceView source) {
    auto prepared = audio::prepareProcessingPlanFromSources(
        specification(project), std::span{&source, 1}, 64);
    check(prepared.success(), "portable plan prepares");
    audio::RealtimeAudioEngine engine;
    engine.configure(prepared.prepared->plan, prepared.prepared->runtime);
    engine.deviceInitialising();
    engine.processBlock({nullptr, 0, 0}, project.sampleRate());
    check(engine.tryRequestPlay().accepted, "portable Play accepted");
    std::array<float, 64> left{}, right{};
    std::array<float*, 2> channels{left.data(), right.data()};
    realtimeAllocations = 0;
    inRealtime = true;
    engine.processBlock({channels.data(), channels.size(), left.size()},
                        project.sampleRate());
    inRealtime = false;
    check(realtimeAllocations == 0, "track rebuild remains allocation-free in RT");
    engine.deviceUnavailable();
    engine.configure(audio::PreparedProjectView{});
    return left.front();
}

void applicationOperations() {
    TestEngine engine;
    application::DawApplication app{engine, timeline::SampleRate{48000.0}};
    commands::CommandDispatcher dispatch{app};
    const auto initialMap = app.project().musicalTime();

    auto result = dispatch.dispatch(commands::AddAudioTrack{
        {}, media::AudioChannelLayout::mono});
    check(result.status == commands::CommandStatus::accepted &&
          app.project().tracks().size() == 1 &&
          app.project().tracks()[0].id == tracks::TrackId{1} &&
          app.project().tracks()[0].name == "Audio 1" &&
          app.project().tracks()[0].layout == media::AudioChannelLayout::mono &&
          app.project().tracks()[0].clips.empty() &&
          app.project().tracks()[0].inserts.processors.empty() &&
          app.project().routing().sends().empty() &&
          app.project().routing().findTrackRoute({1})->destination ==
              routing::OutputDestination::master() &&
          app.project().duration().value == 0 && app.canUndo(),
          "Add Track creates a default Master-routed undoable track");
    const auto first = app.project().tracks()[0];
    check(dispatch.dispatch(commands::Undo{}).status == commands::CommandStatus::accepted &&
          app.project().tracks().empty(), "Add Track Undo removes exact track");
    check(dispatch.dispatch(commands::Redo{}).status == commands::CommandStatus::accepted &&
          app.project().tracks()[0] == first,
          "Add Track Redo restores the same TrackId and defaults");
    check(dispatch.dispatch(commands::AddAudioTrack{
              {}, media::AudioChannelLayout::mono}).status ==
              commands::CommandStatus::accepted &&
          app.project().tracks()[1].id == tracks::TrackId{2} &&
          app.project().tracks()[1].name == "Audio 2",
          "TrackId and default naming remain monotonic after Undo/Redo");

    check(dispatch.dispatch(commands::ImportAudioToTrack{
              "mono.wav", {1}, {100}}).status == commands::CommandStatus::accepted,
          "targeted import succeeds");
    const auto source = app.project().sources()[0].id;
    const auto clip = app.project().tracks()[0].clips[0].id;
    check(dispatch.dispatch(commands::AddClip{{2}, source, {300}, {1000}, {0}}).status ==
              commands::CommandStatus::accepted,
          "shared Source setup succeeds");
    check(dispatch.dispatch(commands::SetLoopRangeMusical{
              {4 * musical::ppq}, {12 * musical::ppq}}).status ==
              commands::CommandStatus::accepted &&
          dispatch.dispatch(commands::SetLoopEnabled{true}).status ==
              commands::CommandStatus::accepted &&
          dispatch.dispatch(commands::SetMetronomeEnabled{true}).status ==
              commands::CommandStatus::accepted,
          "loop and metronome regression fixture succeeds");
    app.synchroniseTransport();
    check(dispatch.dispatch(commands::AddBus{"Aux"}).status ==
              commands::CommandStatus::accepted &&
          dispatch.dispatch(commands::AddTrackSend{
              {1}, {1}, routing::SendTapPoint::postFaderPostPan,
              mixer::GainDb{-6.0F}}).status == commands::CommandStatus::accepted &&
          dispatch.dispatch(commands::AddTrackSend{
              {1}, {1}, routing::SendTapPoint::preFaderPrePan,
              mixer::GainDb{-12.0F}}).status == commands::CommandStatus::accepted &&
          dispatch.dispatch(commands::SetTrackOutputDestination{
              {1}, routing::OutputDestination::toBus({1})}).status ==
              commands::CommandStatus::accepted &&
          dispatch.dispatch(commands::SetTrackGain{{1}, mixer::GainDb{-3.0F}}).status ==
              commands::CommandStatus::accepted &&
          dispatch.dispatch(commands::AddProcessor{
              processors::InsertTarget{tracks::TrackId{1}},
              processors::ProcessorType{processors::internalGainProcessorType}}).status ==
              commands::CommandStatus::accepted,
          "track delete fixture has mixer, send and insert state");
    const auto beforeDelete = app.project().captureTrackHistoryState({1});
    const auto deleteToken = app.history().currentStateToken();
    check(beforeDelete && dispatch.dispatch(commands::DeleteAudioTrack{{1}}).status ==
              commands::CommandStatus::accepted &&
          app.project().findTrack({1}) == nullptr &&
          app.project().findSource(source) != nullptr &&
          app.project().findClip(clip) == nullptr &&
          app.project().findClip({2}) != nullptr &&
          app.project().duration().value == 1300 &&
          app.project().routing().sends().empty() &&
          app.project().findBus({1}) != nullptr,
          "Delete removes owned track state but preserves shared Source and unrelated bus");
    check(dispatch.dispatch(commands::Undo{}).status == commands::CommandStatus::accepted &&
          app.project().captureTrackHistoryState({1}) == beforeDelete &&
          app.project().findClip(clip) != nullptr &&
          engine.active->specification.tracks[0].inserts.processors.size() == 1,
          "Delete Undo restores exact IDs, clips, mixer, route, sends and inserts");
    check(dispatch.dispatch(commands::Redo{}).status == commands::CommandStatus::accepted &&
          app.project().findTrack({1}) == nullptr,
          "Delete Redo removes the same TrackId");
    check(deleteToken != app.history().currentStateToken(),
          "Delete advances dirty history state");
    check(app.project().musicalTime() == initialMap &&
          app.project().loopRange().has_value() && app.loopEnabled() &&
          app.metronomeEnabled(),
          "track lifecycle preserves musical maps, loop and metronome state");
    check(dispatch.dispatch(commands::DeleteAudioTrack{{2}}).status ==
              commands::CommandStatus::accepted &&
          app.project().tracks().empty() && app.project().sources().size() == 1,
          "deleting the last track leaves a valid source-preserving project");
    check(dispatch.dispatch(commands::ImportAudioFile{"again.wav", {0}}).status ==
              commands::CommandStatus::accepted &&
          app.project().tracks().size() == 1 &&
          app.project().tracks()[0].id == tracks::TrackId{3},
          "import after deleting the last track recreates one monotonic TrackId");
}

void moveAndFailureOperations() {
    TestEngine engine;
    application::DawApplication app{engine, timeline::SampleRate{48000.0}};
    commands::CommandDispatcher dispatch{app};
    auto ok = [&](commands::Command command) {
        const auto result = dispatch.dispatch(command);
        check(result.status == commands::CommandStatus::accepted,
              "fixture command accepted");
    };
    ok(commands::AddAudioTrack{"A", media::AudioChannelLayout::mono});
    ok(commands::AddAudioTrack{"B", media::AudioChannelLayout::mono});
    ok(commands::AddAudioTrack{"Stereo", media::AudioChannelLayout::stereo});
    ok(commands::ImportAudioToTrack{"mono.wav", {1}, {48000}});
    const auto clip = app.project().tracks()[0].clips[0];
    ok(commands::MoveClip{clip.id, tracks::TrackId{2}, clip.projectStart});
    check(app.project().trackContainingClip(clip.id) == tracks::TrackId{2} &&
          app.project().findClip(clip.id)->projectStart == clip.projectStart,
          "vertical-only Move commits even when clip fields are unchanged");
    ok(commands::Undo{});
    const auto token = app.history().currentStateToken();
    const auto plan = engine.active.get();
    engine.failPreparation = true;
    check(dispatch.dispatch(commands::MoveClip{
              clip.id, tracks::TrackId{2}, {144000}}).error ==
              commands::CommandError::preparationFailed &&
          app.project().trackContainingClip(clip.id) == tracks::TrackId{1} &&
          app.history().currentStateToken() == token && engine.active.get() == plan,
          "failed Move preparation preserves model, history and active plan");
    ok(commands::MoveClip{clip.id, tracks::TrackId{2}, {144000}});
    const auto moved = *app.project().findClip(clip.id);
    check(app.project().trackContainingClip(clip.id) == tracks::TrackId{2} &&
          moved.id == clip.id && moved.source == clip.source &&
          moved.sourceOffset == clip.sourceOffset && moved.duration == clip.duration &&
          moved.projectStart.value == 144000 &&
          app.project().duration().value == 192000 &&
          engine.active->specification.tracks[1].clips[0].id == clip.id,
          "one 2D Move changes owner/time and prepared renderer assignment only");
    ok(commands::Undo{});
    check(app.project().trackContainingClip(clip.id) == tracks::TrackId{1} &&
          *app.project().findClip(clip.id) == clip &&
          app.project().duration().value == 96000,
          "2D Move Undo restores both dimensions and identity");
    ok(commands::Redo{});
    check(app.project().trackContainingClip(clip.id) == tracks::TrackId{2} &&
          *app.project().findClip(clip.id) == moved,
          "2D Move Redo restores exact destination");
    const auto mismatchToken = app.history().currentStateToken();
    const auto mismatchPlan = engine.active.get();
    check(dispatch.dispatch(commands::MoveClip{
              clip.id, tracks::TrackId{3}, {0}}).error ==
              commands::CommandError::layoutMismatch &&
          app.history().currentStateToken() == mismatchToken &&
          engine.active.get() == mismatchPlan &&
          app.project().trackContainingClip(clip.id) == tracks::TrackId{2},
          "mono to stereo Move is explicitly rejected before publication");
    check(dispatch.dispatch(commands::MoveClip{
              clip.id, tracks::TrackId{999}, {0}}).error ==
              commands::CommandError::trackNotFound &&
          dispatch.dispatch(commands::DeleteAudioTrack{{999}}).error ==
              commands::CommandError::trackNotFound,
          "missing target and delete TrackIds are typed errors");

    engine.failPreparation = true;
    const auto addToken = app.history().currentStateToken();
    check(dispatch.dispatch(commands::AddAudioTrack{}).error ==
              commands::CommandError::preparationFailed &&
          app.project().tracks().size() == 3 &&
          app.history().currentStateToken() == addToken,
          "failed Add preparation rolls back model and history");
    engine.failCommit = true;
    check(dispatch.dispatch(commands::DeleteAudioTrack{{2}}).error ==
              commands::CommandError::preparationFailed &&
          app.project().findTrack({2}) != nullptr &&
          app.history().currentStateToken() == addToken,
          "failed Delete commit rolls back model and history");

    ok(commands::SeekToProjectFrame{{120000}});
    app.synchroniseTransport();
    ok(commands::AddAudioTrack{});
    check(app.transport().position.value == 120000,
          "Stopped structural track operation preserves transport position");
    ok(commands::Play{});
    app.synchroniseTransport();
    check(dispatch.dispatch(commands::AddAudioTrack{}).error ==
              commands::CommandError::transportMustBeStopped &&
          dispatch.dispatch(commands::DeleteAudioTrack{{2}}).error ==
              commands::CommandError::transportMustBeStopped,
          "Add and Delete Track are rejected while transport is active");
    ok(commands::Pause{});
    app.synchroniseTransport();
    check(dispatch.dispatch(commands::MoveClip{
              clip.id, tracks::TrackId{1}, {0}}).error ==
              commands::CommandError::transportMustBeStopped,
          "cross-track Move requires fully Stopped transport");
}

void certifiedTemporalBoundaryOperations() {
    TestEngine engine;
    application::DawApplication app{engine, timeline::SampleRate{48000.0}};
    commands::CommandDispatcher dispatch{app};
    auto ok = [&](commands::Command command) {
        const auto result = dispatch.dispatch(command);
        check(result.status == commands::CommandStatus::accepted,
              "certified-boundary fixture command accepted");
    };

    ok(commands::AddAudioTrack{"Boundary", media::AudioChannelLayout::mono});
    ok(commands::ImportAudioToTrack{"mono.wav", {1}, {0}});
    const auto clip = app.project().tracks()[0].clips[0];
    const auto maximum = timeline::maximumSupportedProjectFrame().value;
    const auto lastValidStart = maximum - 48000;

    ok(commands::MoveClip{clip.id, {lastValidStart}});
    check(app.project().findClip(clip.id)->projectStart.value == lastValidStart &&
              app.project().duration().value == maximum,
          "clip whose exclusive end equals the certified maximum is valid");

    const auto preparationsBeforeNoop = engine.preparations;
    const auto tokenBeforeNoop = app.history().currentStateToken();
    const auto* planBeforeNoop = engine.active.get();
    ok(commands::MoveClip{clip.id, {lastValidStart}});
    check(engine.preparations == preparationsBeforeNoop &&
              app.history().currentStateToken() == tokenBeforeNoop &&
              engine.active.get() == planBeforeNoop,
          "same-track same-position Move is a no-op without history or rebuild");

    const auto checkRejectedBeforePreparation = [&](std::int64_t start,
                                                     const char* message) {
        const auto preparations = engine.preparations;
        const auto token = app.history().currentStateToken();
        const auto* plan = engine.active.get();
        const auto before = *app.project().findClip(clip.id);
        const auto result = dispatch.dispatch(commands::MoveClip{clip.id, {start}});
        check(result.status == commands::CommandStatus::rejected &&
                  result.error == commands::CommandError::invalidPosition &&
                  engine.preparations == preparations &&
                  app.history().currentStateToken() == token &&
                  engine.active.get() == plan &&
                  *app.project().findClip(clip.id) == before,
              message);
    };

    checkRejectedBeforePreparation(
        lastValidStart + 1,
        "exclusive clip end beyond certified time is rejected before preparation");
    checkRejectedBeforePreparation(
        maximum + 1,
        "clip start beyond certified time is rejected before preparation");
    checkRejectedBeforePreparation(
        -1,
        "negative clip start is rejected before preparation");
}

void arrangeEditingIIBatchOperations() {
    TestEngine engine;
    application::DawApplication app{engine, timeline::SampleRate{48000.0}};
    commands::CommandDispatcher dispatch{app};
    const auto accepted = [&](commands::Command command) {
        const auto result = dispatch.dispatch(command);
        check(result.status == commands::CommandStatus::accepted,
              "Arrange Editing II fixture command accepted");
        return result;
    };

    accepted(commands::AddAudioTrack{"One"});
    accepted(commands::AddAudioTrack{"Two"});
    accepted(commands::AddAudioTrack{"Three"});
    accepted(commands::ImportAudioToTrack{"one.wav", {1}, {100}});
    accepted(commands::ImportAudioToTrack{"two.wav", {2}, {500}});
    const auto one = app.project().tracks()[0].clips[0];
    const auto two = app.project().tracks()[1].clips[0];

    const auto preparationsBeforeReorder = engine.preparations;
    const auto commitsBeforeReorder = engine.commits;
    accepted(commands::ReorderAudioTrack{
        {1}, {3}, commands::TrackPlacement::after});
    check(app.project().tracks()[0].id == tracks::TrackId{2} &&
              app.project().tracks()[1].id == tracks::TrackId{3} &&
              app.project().tracks()[2].id == tracks::TrackId{1} &&
              app.project().findClip(one.id) != nullptr &&
              app.project().routing().findTrackRoute({1}) != nullptr &&
              engine.preparations == preparationsBeforeReorder + 1 &&
              engine.commits == commitsBeforeReorder + 1,
          "reorder changes only documentary order with one preparation");
    accepted(commands::Undo{});
    check(app.project().tracks()[0].id == tracks::TrackId{1},
          "reorder Undo restores exact order");
    accepted(commands::Redo{});
    check(app.project().tracks()[2].id == tracks::TrackId{1},
          "reorder Redo restores exact order");
    const auto reorderNoopPreparations = engine.preparations;
    const auto reorderNoopHistory = app.history().size();
    accepted(commands::ReorderAudioTrack{
        {1}, {1}, commands::TrackPlacement::before});
    check(engine.preparations == reorderNoopPreparations &&
              app.history().size() == reorderNoopHistory &&
              dispatch.dispatch(commands::ReorderAudioTrack{
                  {999}, {1}, commands::TrackPlacement::before}).error ==
                  commands::CommandError::trackNotFound,
          "reorder self is a no-op and missing TrackId is rejected");
    for (int cycle = 0; cycle < 4; ++cycle) {
        accepted(commands::ReorderAudioTrack{
            {2}, {3}, commands::TrackPlacement::after});
        accepted(commands::ReorderAudioTrack{
            {2}, {3}, commands::TrackPlacement::before});
    }
    check(app.project().tracks()[0].id == tracks::TrackId{2} &&
              app.project().tracks()[1].id == tracks::TrackId{3} &&
              app.project().tracks()[2].id == tracks::TrackId{1} &&
              app.project().routing().findTrackRoute({1}) != nullptr &&
              app.project().routing().findTrackRoute({2}) != nullptr,
          "repeated reorder returns to exact documentary order and preserves routing IDs");

    const auto preparationsBeforeMove = engine.preparations;
    const auto commitsBeforeMove = engine.commits;
    const auto historyBeforeMove = app.history().size();
    accepted(commands::MoveClips{{two.id, one.id, two.id}, 1000});
    check(app.project().findClip(one.id)->projectStart.value == 1100 &&
              app.project().findClip(two.id)->projectStart.value == 1500 &&
              app.project().trackContainingClip(one.id) == tracks::TrackId{1} &&
              app.project().trackContainingClip(two.id) == tracks::TrackId{2} &&
              engine.preparations == preparationsBeforeMove + 1 &&
              engine.commits == commitsBeforeMove + 1 &&
              app.history().size() == historyBeforeMove + 1,
          "MoveClips preserves owners and relative offsets in one transaction");
    const auto noopPreparations = engine.preparations;
    const auto noopHistory = app.history().size();
    accepted(commands::MoveClips{{one.id, two.id}, 0});
    check(engine.preparations == noopPreparations &&
              app.history().size() == noopHistory,
          "zero-delta MoveClips is a no-op without preparation or history");
    accepted(commands::Undo{});
    check(*app.project().findClip(one.id) == one &&
              *app.project().findClip(two.id) == two,
          "MoveClips Undo restores all clips exactly");
    accepted(commands::Redo{});

    const auto duplicatePreparations = engine.preparations;
    const auto duplicateCommits = engine.commits;
    const auto duplicateResult = accepted(
        commands::DuplicateClips{{two.id, one.id, one.id}, 2000});
    check(duplicateResult.createdClips ==
              std::vector<clips::ClipId>{{3}, {4}} &&
              app.project().findClip({3})->source == one.source &&
              app.project().findClip({3})->projectStart.value == 3100 &&
              app.project().findClip({4})->source == two.source &&
              app.project().findClip({4})->projectStart.value == 3500 &&
              engine.preparations == duplicatePreparations + 1 &&
              engine.commits == duplicateCommits + 1,
          "DuplicateClips assigns deterministic IDs and returns created selection");
    accepted(commands::Undo{});
    check(app.project().findClip({3}) == nullptr &&
              app.project().findClip({4}) == nullptr,
          "DuplicateClips Undo removes the whole batch");
    accepted(commands::Redo{});
    check(app.project().findClip({3}) != nullptr &&
              app.project().findClip({4}) != nullptr,
          "DuplicateClips Redo restores the same IDs");

    const auto deletePreparations = engine.preparations;
    const auto deleteCommits = engine.commits;
    accepted(commands::DeleteClips{{{4}, {3}, {4}}});
    check(app.project().findClip({3}) == nullptr &&
              app.project().findClip({4}) == nullptr &&
              app.project().sources().size() == 2 &&
              engine.preparations == deletePreparations + 1 &&
              engine.commits == deleteCommits + 1,
          "DeleteClips deduplicates IDs and preserves Sources");
    accepted(commands::Undo{});
    check(app.project().findClip({3}) != nullptr &&
              app.project().findClip({4}) != nullptr,
          "DeleteClips Undo restores the entire batch");
    const auto encoded = persistence::serializeProject(
        app.project(), "/tmp/vitadaw-arrange-editing-ii.vitadaw");
    const auto loaded = persistence::deserializeProject(encoded.bytes);
    check(encoded.result.success(), "Arrange Editing II project serializes");
    check(loaded.result.success(), "Arrange Editing II project deserializes");
    check(loaded.project->documentData().tracks ==
              app.project().documentData().tracks,
          "schema v3 round-trip preserves reorder and batch ownership");
    check(loaded.project->documentData().nextClipId ==
              app.project().documentData().nextClipId,
          "schema v3 round-trip preserves batch nextClipId");

    const auto rejectedPreparations = engine.preparations;
    const auto rejectedToken = app.history().currentStateToken();
    const auto rejectedOne = *app.project().findClip(one.id);
    check(dispatch.dispatch(commands::MoveClips{{one.id, {9999}}, -5000}).error ==
              commands::CommandError::clipNotFound &&
              dispatch.dispatch(commands::DeleteClips{{one.id, {9999}}}).error ==
              commands::CommandError::clipNotFound &&
              dispatch.dispatch(commands::DuplicateClips{{one.id, {9999}}, 1}).error ==
              commands::CommandError::clipNotFound &&
              engine.preparations == rejectedPreparations &&
              app.history().currentStateToken() == rejectedToken &&
              *app.project().findClip(one.id) == rejectedOne,
          "a missing batch member rejects every operation before preparation");
    check(dispatch.dispatch(commands::MoveClips{{}, 1}).error ==
              commands::CommandError::validationFailed &&
              dispatch.dispatch(commands::DeleteClips{{}}).error ==
              commands::CommandError::validationFailed &&
              dispatch.dispatch(commands::DuplicateClips{{}, 1}).error ==
              commands::CommandError::validationFailed &&
              engine.preparations == rejectedPreparations,
          "empty batches are explicit validation failures without preparation");

    const auto boundaryDelta =
        timeline::maximumSupportedProjectFrame().value -
        timeline::checkedExclusiveProjectEnd(
            rejectedOne.projectStart, rejectedOne.duration)->value + 1;
    check(dispatch.dispatch(commands::MoveClips{{one.id, two.id}, boundaryDelta}).error ==
              commands::CommandError::invalidPosition &&
              engine.preparations == rejectedPreparations &&
              app.history().currentStateToken() == rejectedToken,
          "one out-of-domain member rejects the complete MoveClips batch");

    engine.failPreparation = true;
    const auto rollbackToken = app.history().currentStateToken();
    const auto rollbackOne = *app.project().findClip(one.id);
    check(dispatch.dispatch(commands::MoveClips{{one.id, two.id}, 1}).error ==
              commands::CommandError::preparationFailed &&
              app.history().currentStateToken() == rollbackToken &&
              *app.project().findClip(one.id) == rollbackOne,
          "batch preparation failure rolls back model and history");

    accepted(commands::Play{});
    app.synchroniseTransport();
    check(dispatch.dispatch(commands::MoveClips{{one.id}, 1}).error ==
              commands::CommandError::transportMustBeStopped &&
              dispatch.dispatch(commands::ReorderAudioTrack{
                  {1}, {2}, commands::TrackPlacement::before}).error ==
              commands::CommandError::transportMustBeStopped,
          "batch edits reject Playing and reorder requires Stopped");
    accepted(commands::Pause{});
    app.synchroniseTransport();
    accepted(commands::MoveClips{{one.id, two.id}, 1});
    check(app.transport().playback == transport::PlaybackState::paused &&
              !app.canRedo(),
          "horizontal MoveClips preserves Paused transport and branches history");
}

void dspAndPersistenceOperations() {
    project::ProjectState project{timeline::SampleRate{48000.0}, "Tracks"};
    const auto a = project.addAudioTrack("A", media::AudioChannelLayout::mono);
    const auto b = project.addAudioTrack("B", media::AudioChannelLayout::mono);
    const auto empty = project.addAudioTrack("Empty", media::AudioChannelLayout::stereo);
    const auto imported = project.importAudioToTrack(
        a, {"/tmp/shared.wav", {},
            media::MediaFingerprint{std::string(64, 'a'), 512}},
        {256}, timeline::SampleRate{48000.0},
        media::AudioChannelLayout::mono, {0});
    const auto processor = project.addProcessor(
        processors::InsertTarget{b},
        processors::ProcessorType{processors::internalGainProcessorType});
    check(project.setProcessorParameter(processor, processors::gainParameterId,
                                        6.0205999F),
          "destination insert configured");
    std::array<float, 256> pcm;
    pcm.fill(0.25F);
    const audio::PreparedSourceView source{imported.source,
        {{pcm.data(), nullptr}}, 1, {pcm.size()},
        timeline::SampleRate{48000.0},
        media::AudioChannelLayout::mono};
    const auto throughA = renderFirstSample(project, source);
    check(project.moveClip(imported.clip, b, {0}).succeeded(),
          "portable cross-track move succeeds");
    const auto throughB = renderFirstSample(project, source);
    check(throughB > throughA * 1.9F,
          "moved clip leaves Track A processing and enters Track B inserts");
    static_cast<void>(project.addClip(b, imported.source, {0}, {256.0}, {0.0}));
    const auto overlap = renderFirstSample(project, source);
    check(std::abs(overlap - throughB * 2.0F) < 1.0e-4F,
          "overlapping clips sum before the destination track insert");
    const auto beforeReorder = renderFirstSample(project, source);
    check(project.reorderAudioTrack(a, empty, true).succeeded() &&
              project.tracks()[0].id == b && project.tracks()[1].id == empty &&
              project.tracks()[2].id == a &&
              renderFirstSample(project, source) == beforeReorder,
          "documentary reorder preserves prepared DSP behaviour");

    project::ProjectState stereoProject{timeline::SampleRate{48000.0}};
    const auto stereoA = stereoProject.addAudioTrack(
        "Stereo A", media::AudioChannelLayout::stereo);
    const auto stereoB = stereoProject.addAudioTrack(
        "Stereo B", media::AudioChannelLayout::stereo);
    const auto stereoImport = stereoProject.importAudioToTrack(
        stereoA, {"stereo.wav", {}}, {128}, timeline::SampleRate{48000.0},
        media::AudioChannelLayout::stereo);
    check(stereoProject.moveClip(stereoImport.clip, stereoB, {64}).succeeded() &&
          stereoProject.trackContainingClip(stereoImport.clip) == stereoB,
          "stereo clip moves only to a stereo track");

    const auto encoded = persistence::serializeProject(
        project, "/tmp/vitadaw-track-operations.vitadaw");
    const auto loaded = persistence::deserializeProject(encoded.bytes);
    check(encoded.result.success() && loaded.result.success() &&
          loaded.project->documentData().tracks == project.documentData().tracks &&
          loaded.project->documentData().routing.trackRoutes ==
              project.documentData().routing.trackRoutes &&
          loaded.project->tracks()[1].id == empty &&
          loaded.project->tracks()[1].clips.empty(),
          "schema v3 round-trip preserves order, IDs, ownership and empty tracks");

    project::ProjectState large{timeline::SampleRate{48000.0}};
    for (std::size_t index = 0; index < 64; ++index)
        static_cast<void>(large.addAudioTrack("Track " + std::to_string(index + 1)));
    const auto largeImport = large.importAudioToTrack(
        large.tracks()[0].id, {"large.wav", {}}, {4096},
        timeline::SampleRate{48000.0},
        media::AudioChannelLayout::mono);
    for (std::size_t index = 1; index < 1000; ++index)
        static_cast<void>(large.addClip(
            large.tracks()[index % large.tracks().size()].id,
            largeImport.source, {static_cast<std::int64_t>(index * 8)},
            {32.0}, {0.0}));
    for (std::size_t index = 0; index < 32; ++index) {
        const auto id = large.tracks()[index].clips.front().id;
        check(large.moveClip(id, large.tracks()[63 - index].id,
                             {static_cast<std::int64_t>(index * 16)}).succeeded(),
              "large-session cross-track move succeeds");
    }
    for (std::size_t index = 0; index < 8; ++index)
        check(large.removeAudioTrack(large.tracks().front().id).has_value(),
              "large-session track delete succeeds");
    check(large.tracks().size() == 56 && large.sources().size() == 1,
          "64-track/1000-clip operations retain source ownership coherently");

    project::ProjectState capacity{timeline::SampleRate{48000.0}};
    for (std::size_t index = 0;
         index < project::ProjectState::maximumTracks; ++index)
        static_cast<void>(capacity.addAudioTrack({}));
    bool rejected{};
    try { static_cast<void>(capacity.addAudioTrack({})); }
    catch (const std::overflow_error&) { rejected = true; }
    check(rejected && capacity.tracks().size() ==
              project::ProjectState::maximumTracks,
          "track capacity rejects 257 without partial mutation");
}

void reorderRoutingPersistenceContract() {
    project::ProjectState project{timeline::SampleRate{48000.0},
                                  "Reorder routing contract"};
    const auto t1 = project.addAudioTrack("T1");
    const auto t2 = project.addAudioTrack("T2");
    const auto t3 = project.addAudioTrack("T3");
    const auto music = project.addBus("Music");
    const auto aux = project.addBus("Aux");
    check(project.setTrackOutputDestination(
              t1, routing::OutputDestination::toBus(music)),
          "reorder contract routes T1 to Music by TrackId");
    const auto trackSend = project.addSend(
        t1, aux, routing::SendTapPoint::preFaderPrePan, {{-12.0F}, false});
    const auto busSend = project.addSend(
        music, aux, routing::SendTapPoint::postFaderPostPan,
        {{-6.0F}, false});
    check(trackSend.isValid() && busSend.isValid(),
          "reorder contract creates track and bus sends");
    const auto imported = project.importAudioToTrack(
        t1, {"/tmp/reorder-routing.wav", {},
             media::MediaFingerprint{std::string(64, 'b'), 512}},
        {256}, timeline::SampleRate{48000.0},
        media::AudioChannelLayout::mono, {0});
    check(project.addClip(t2, imported.source, {0}, {256.0}, {0.0}).isValid(),
          "reorder contract contains clips on multiple tracks");

    std::array<float, 256> pcm;
    pcm.fill(0.125F);
    const audio::PreparedSourceView source{
        imported.source, {{pcm.data(), nullptr}}, 1, {pcm.size()},
        timeline::SampleRate{48000.0}, media::AudioChannelLayout::mono};
    const auto before = renderFirstSample(project, source);
    const auto routesBefore = project.routing().trackRoutes();
    const auto sendsBefore = project.routing().sends();
    check(project.reorderAudioTrack(t3, t1, false).succeeded() &&
              project.tracks()[0].id == t3 &&
              project.tracks()[1].id == t1 &&
              project.tracks()[2].id == t2,
          "reorder contract establishes non-trivial documentary order");
    check(project.routing().trackRoutes() == routesBefore &&
              project.routing().trackRoutes()[0].track == t1 &&
              project.routing().trackRoutes()[1].track == t2 &&
              project.routing().trackRoutes()[2].track == t3 &&
              project.tracks()[0].id !=
                  project.routing().trackRoutes()[0].track,
          "track routes are an independent TrackId collection, not a positional parallel vector");
    check(project.routing().findTrackRoute(t1)->destination ==
              routing::OutputDestination::toBus(music) &&
              project.findSend(trackSend)->source == routing::SendSource{t1} &&
              project.findSend(trackSend)->destination == aux &&
              project.findSend(busSend)->source == routing::SendSource{music} &&
              project.findSend(busSend)->destination == aux &&
              project.routing().sends() == sendsBefore,
          "reorder preserves routing and send identities");
    check(renderFirstSample(project, source) == before,
          "reorder preserves playback with routed tracks and sends");

    const auto firstSave = persistence::serializeProject(
        project, "/tmp/vitadaw-reorder-routing.vitadaw");
    const auto loaded = persistence::deserializeProject(firstSave.bytes);
    check(firstSave.result.success() && loaded.result.success() &&
              loaded.project->tracks()[0].id == t3 &&
              loaded.project->tracks()[1].id == t1 &&
              loaded.project->tracks()[2].id == t2,
          "Save and Load preserve documentary track order");
    check(loaded.project->routing().trackRoutes()[0].track == t1 &&
              loaded.project->routing().trackRoutes()[1].track == t2 &&
              loaded.project->routing().trackRoutes()[2].track == t3 &&
              loaded.project->routing().findTrackRoute(t1)->destination ==
                  routing::OutputDestination::toBus(music) &&
              loaded.project->routing().sends() == sendsBefore,
          "Load preserves TrackId routing and canonical TrackId route order");
    check(renderFirstSample(*loaded.project, source) == before,
          "playback after reordered Save and Load is bit-identical");
    const auto secondSave = persistence::serializeProject(
        *loaded.project, "/tmp/vitadaw-reorder-routing.vitadaw");
    check(secondSave.result.success() && secondSave.bytes == firstSave.bytes,
          "second Save preserves the canonical TrackId route representation");
}
} // namespace

void* operator new(std::size_t size) {
    if (inRealtime) realtimeAllocations.fetch_add(1, std::memory_order_relaxed);
    if (auto* memory = std::malloc(size == 0 ? 1 : size)) return memory;
    throw std::bad_alloc{};
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete[](void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }
void operator delete[](void* memory, std::size_t) noexcept { std::free(memory); }

int main() {
    applicationOperations();
    moveAndFailureOperations();
    certifiedTemporalBoundaryOperations();
    arrangeEditingIIBatchOperations();
    dspAndPersistenceOperations();
    reorderRoutingPersistenceContract();
    std::cout << "Track operations and cross-track editing tests passed\n";
}
