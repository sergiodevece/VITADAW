#include "vitadaw/application/DawApplication.h"
#include "vitadaw/commands/CommandDispatcher.h"
#include "vitadaw/audio/RealtimeAudioEngine.h"
#include "vitadaw/processors/GainProcessor.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <new>

namespace {
using namespace vitadaw;
std::atomic<long> failAllocation{-1};
std::atomic<bool> inRealtime{};
std::atomic<unsigned> rtAllocations{}, rtDestructions{};
void check(bool value, const char* message) {
    if (!value) { std::cerr << "FAILED: " << message << '\n'; std::exit(1); }
}

class CountingProcessor final : public processors::IAudioProcessor {
public:
    explicit CountingProcessor(unsigned& calls) : calls_(calls) {}
    bool prepare(const processors::ProcessingFormat& format) override {
        channels_ = format.channelCount(); return format.isValid();
    }
    void reset() noexcept override {}
    void applyParameter(processors::PreparedParameterEvent) noexcept override {}
    processors::ProcessStatus processBlock(const processors::ProcessorProcessContext& context,
        audio::ConstAudioBlockView input, audio::AudioBlockView output) noexcept override {
        ++calls_;
        for (std::size_t c = 0; c < channels_; ++c)
            for (std::size_t f = 0; f < context.frameCount; ++f)
                output.channels[c][f] = input.channels[c][f];
        return processors::ProcessStatus::processed;
    }
    processors::ProcessingFrameCount latency() const noexcept override { return {}; }
    processors::TailInfo tail() const noexcept override { return {}; }
    processors::ProcessorCapabilities capabilities() const noexcept override { return {false,true,true,true}; }
    std::size_t runtimeMemoryBytes() const noexcept override { return 0; }
private:
    unsigned& calls_;
    std::size_t channels_{};
};
class Factory final : public processors::IAudioProcessorFactory {
public:
    explicit Factory(unsigned& calls) : calls_(calls) {}
    std::unique_ptr<processors::IAudioProcessor> create(const processors::ProcessorState&) const override {
        return std::make_unique<CountingProcessor>(calls_);
    }
private: unsigned& calls_;
};

// Hardware-free platform double; uses the production plan compiler and processBlock.
class Engine final : public audio::IAudioEngineControl {
public:
    struct Plan final : audio::PreparedProcessingPlanChange {
        std::unique_ptr<audio::PreparedProcessingBundle> bundle;
        explicit Plan(std::unique_ptr<audio::PreparedProcessingBundle> value) : bundle(std::move(value)) {}
        ~Plan() override { if (inRealtime) ++rtDestructions; }
    };
    std::array<float, 441> pcm{};
    audio::PreparedSourceView source{{1}, {{pcm.data(), nullptr}}, 1, {441},
        timeline::SampleRate{44100}, media::AudioChannelLayout::mono};
    unsigned decodes{}, calls{};
    Factory factory{calls};
    std::unique_ptr<Plan> active;
    audio::RealtimeAudioEngine rt;
    bool failPrepare{}, failCommit{};
    std::size_t blockCapacity{128};
    Engine() { pcm.fill(0.25F); }
    ~Engine() override {
        rt.deviceUnavailable();
        rt.configure(audio::PreparedProjectView{});
        active.reset();
    }
    audio::AudioFilePreparationResult prepareWav(const std::filesystem::path&) override {
        ++decodes;
        return {std::make_unique<audio::PreparedAudioFile>(audio::AudioFileMetadata{
            timeline::SampleRate{44100}, 1, {441}, {0.01}}), {}};
    }
    audio::StructuralPlanPreparationResult prepareProcessingPlan(
        const audio::ProcessingPlanSpecification& specification) override {
        if (failPrepare) return {nullptr, "Injected failure"};
        auto spec = specification;
        spec.processingSampleRate = timeline::SampleRate{48000};
        auto prepared = audio::prepareProcessingPlanFromSources(spec,
            spec.sources.empty() ? std::span<const audio::PreparedSourceView>{} : std::span{&source, 1},
            blockCapacity, audio::defaultProcessingMemoryBudgetBytes, &factory);
        if (!prepared.success()) return {nullptr, std::move(prepared.errorMessage)};
        return {std::make_unique<Plan>(std::move(prepared.prepared)), {}};
    }
    audio::StructuralPlanPreparationResult prepareProcessingPlanWithAudio(
        const audio::ProcessingPlanSpecification& spec, media::SourceId id,
        audio::PreparedAudioFilePtr) override {
        source.id = id;
        return prepareProcessingPlan(spec);
    }
    bool commitPreparedProcessingPlan(audio::PreparedProcessingPlanChangePtr prepared,
                                      audio::AudioFileCommitAction commit) noexcept override {
        if (failCommit) return false;
        // No concurrent callback in this double; explicit lifecycle quiescence.
        rt.deviceStopped();
        auto next = std::unique_ptr<Plan>(static_cast<Plan*>(prepared.release()));
        rt.configure(next->bundle->plan, next->bundle->runtime);
        active.swap(next);
        commit.execute();
        rt.deviceInitialising();
        rt.processBlock({nullptr, 0, 0}, timeline::SampleRate{48000});
        return true;
    }
    bool tryUpdateTrackMix(tracks::TrackId id, mixer::PreparedTrackMixState mix,
        audio::PreparedAudibilityState audible) noexcept override { return rt.tryUpdateTrackMix(id,mix,audible); }
    bool tryUpdateBusMix(routing::BusId id, mixer::PreparedBusMixState mix,
        audio::PreparedAudibilityState audible) noexcept override { return rt.tryUpdateBusMix(id,mix,audible); }
    bool tryUpdateSendMix(routing::SendId id, mixer::PreparedSendMixState mix) noexcept override { return rt.tryUpdateSendMix(id,mix); }
    bool tryUpdateMasterMix(mixer::PreparedMasterMixState mix) noexcept override { return rt.tryUpdateMasterMix(mix); }
    audio::AudioControlRequestResult tryRequestPlay() noexcept override { return rt.tryRequestPlay(); }
    audio::AudioControlRequestResult tryRequestStop() noexcept override { return rt.tryRequestStop(); }
    audio::RealtimeTransportSnapshot transportSnapshot() const noexcept override { return rt.transportSnapshot(); }
    mixer::MeterSnapshot meterSnapshot() const noexcept override { return rt.meterSnapshot(); }
    std::vector<float> render(std::size_t frames) {
        check(rt.tryRequestPlay().accepted, "RT Play accepted");
        std::vector<float> left(frames), right(frames);
        std::array<float*, 2> output{left.data(), right.data()};
        inRealtime = true;
        rt.processBlock({output.data(), 2, frames}, timeline::SampleRate{48000});
        inRealtime = false;
        check(rtAllocations == 0 && rtDestructions == 0, "RT has no history allocation/destruction");
        return left;
    }
};

void run() {
    Engine engine;
    application::DawApplication app{engine, timeline::SampleRate{48000}};
    commands::CommandDispatcher dispatch{app};
    auto ok = [&](commands::Command command) {
        auto result = dispatch.dispatch(command);
        if (result.status != commands::CommandStatus::accepted)
            std::cerr << result.message << '\n';
        check(result.status == commands::CommandStatus::accepted, "command accepted");
    };
    check(dispatch.dispatch(commands::Undo{}).error == commands::CommandError::nothingToUndo,
          "empty Undo explicit");
    check(dispatch.dispatch(commands::Redo{}).error == commands::CommandError::nothingToRedo,
          "empty Redo explicit");
    ok(commands::AddAudioTrack{"Audio"});
    ok(commands::ImportAudioToTrack{"test.wav", {1}, {0}});
    const auto original = *app.project().findClip({1});
    const auto saved = app.history().currentStateToken();
    ok(commands::MoveClip{{1}, {100}});
    const auto moved = *app.project().findClip({1});
    const auto movedToken = app.history().currentStateToken();
    check(app.project().duration().value == 580 && app.undoLabel() == "history.moveClip", "move duration/label");
    {
        // Deliberately violate application ownership to exercise the divergence guard.
        auto& divergent = const_cast<project::ProjectState&>(app.project());
        check(bool(divergent.moveClip({1}, {101})), "inject unexpected model edit");
        const auto cursor = app.history().cursor();
        auto* previousPlan = engine.active.get();
        check(dispatch.dispatch(commands::Undo{}).error == commands::CommandError::historyInvalid &&
              divergent.findClip({1})->projectStart.value == 101 &&
              app.history().cursor() == cursor && engine.active.get() == previousPlan,
              "historyInvalid leaves divergent model, plan and cursor untouched");
        check(bool(divergent.moveClip({1}, {100})), "restore expected test state");
    }
    for (bool failCommit : {false, true}) {
        auto* previousPlan = engine.active.get();
        const auto cursor = app.history().cursor();
        engine.failPrepare = !failCommit; engine.failCommit = failCommit;
        check(dispatch.dispatch(commands::Undo{}).error == commands::CommandError::preparationFailed,
              "Undo preparation/commit failure");
        check(*app.project().findClip({1}) == moved && engine.active.get() == previousPlan &&
              app.history().cursor() == cursor && app.history().currentStateToken() == movedToken,
              "failed Undo preserves model RT cursor token");
        engine.failPrepare = engine.failCommit = false;
    }
    ok(commands::Undo{});
    check(*app.project().findClip({1}) == original && app.history().currentStateToken() == saved,
          "Undo restores exact clip and saved token");
    for (bool failCommit : {false, true}) {
        engine.failPrepare = !failCommit; engine.failCommit = failCommit;
        check(dispatch.dispatch(commands::Redo{}).error == commands::CommandError::preparationFailed,
              "Redo preparation/commit failure");
        check(app.canRedo() && *app.project().findClip({1}) == original &&
              app.history().currentStateToken() == saved, "failed Redo remains retryable");
        engine.failPrepare = engine.failCommit = false;
    }
    ok(commands::Redo{});
    check(*app.project().findClip({1}) == moved && app.history().currentStateToken() == movedToken,
          "Redo restores state token");
    ok(commands::Undo{});
    engine.failPrepare = true;
    check(dispatch.dispatch(commands::SplitClip{{1}, {200}}).status == commands::CommandStatus::rejected,
          "new branch preparation failure");
    check(app.canRedo(), "failed new branch preserves Redo");
    engine.failPrepare = false;
    ok(commands::SplitClip{{1}, {200}});
    check(!app.canRedo() && app.history().currentStateToken() != movedToken, "new branch replaces Redo/token");
    const auto left = *app.project().findClip({1});
    const auto right = *app.project().findClip({2});
    ok(commands::Undo{});
    check(*app.project().findClip({1}) == original && !app.project().findClip({2}), "Split Undo atomic");
    ok(commands::Redo{});
    check(*app.project().findClip({1}) == left && *app.project().findClip({2}) == right, "Split Redo same IDs");
    ok(commands::Undo{});
    ok(commands::DuplicateClip{{1}, {100}});
    const auto duplicate = *app.project().findClip({3});
    check(!app.project().findClip({2}), "abandoned split ID never reused");
    for (int repeat = 0; repeat < 5; ++repeat) {
        ok(commands::Undo{}); check(!app.project().findClip({3}), "Duplicate Undo deletes exact ID");
        ok(commands::Redo{}); check(*app.project().findClip({3}) == duplicate, "Duplicate Redo identical");
    }
    check(engine.decodes == 1 && app.project().sources().size() == 1 &&
          engine.active->bundle->plan.sources.size() == 1 &&
          engine.active->bundle->plan.sources[0].channels[0] == engine.pcm.data(), "source cache shared, no decode");
    app.clearHistory();
    const auto baseline = app.project().tracks()[0].clips;
    const auto baselineDuration = app.project().duration();
    const auto baselineAudio = engine.render(1100);
    app.synchroniseTransport();
    ok(commands::MoveClip{{1}, {30}});
    ok(commands::DuplicateClip{{1}, {80}});
    ok(commands::TrimClipLeft{{4}, {123}});
    ok(commands::DeleteClip{{3}});
    const auto final = app.project().tracks()[0].clips;
    const auto finalDuration = app.project().duration();
    const auto finalAudio = engine.render(1100);
    app.synchroniseTransport();
    for (int i=0; i<4; ++i) ok(commands::Undo{});
    check(app.project().tracks()[0].clips == baseline && app.project().duration() == baselineDuration,
          "chain Undo exact order/duration/IDs");
    check(engine.render(1100) == baselineAudio, "chain Undo restores identical prepared render");
    app.synchroniseTransport();
    for (int i=0; i<4; ++i) ok(commands::Redo{});
    check(app.project().tracks()[0].clips == final && app.project().duration() == finalDuration,
          "chain Redo exact order/duration/IDs");
    check(engine.render(1100) == finalAudio, "chain Redo restores identical prepared render");
    app.synchroniseTransport();
    const auto beforeTrim = *app.project().findClip({4});
    ok(commands::TrimClipRight{{4}, {300}});
    const auto afterTrim = *app.project().findClip({4});
    ok(commands::Undo{}); check(*app.project().findClip({4}) == beforeTrim, "TrimR Undo exact");
    ok(commands::Redo{}); check(*app.project().findClip({4}) == afterTrim, "TrimR Redo exact");
    const auto tokenBeforeNoop = app.history().currentStateToken();
    ok(commands::MoveClip{{4}, afterTrim.projectStart});
    check(app.history().currentStateToken() == tokenBeforeNoop, "no-op creates no history/token");

    // Every allocation position in a new edit must either fail before publication or succeed atomically.
    ok(commands::Undo{});
    bool reachedSuccess = false;
    for (long allocation = 0; allocation < 2000 && !reachedSuccess; ++allocation) {
        const auto clipsBefore = app.project().tracks()[0].clips;
        const auto token = app.history().currentStateToken();
        const auto cursor = app.history().cursor();
        auto* plan = engine.active.get();
        failAllocation = allocation;
        const auto result = dispatch.dispatch(commands::MoveClip{{1}, {77}});
        failAllocation = -1;
        reachedSuccess = result.status == commands::CommandStatus::accepted;
        if (!reachedSuccess)
            check(app.project().tracks()[0].clips == clipsBefore && engine.active.get() == plan &&
                  app.history().cursor() == cursor && app.history().currentStateToken() == token && app.canRedo(),
                  "allocation failure preserves active state and Redo at every allocation site");
    }
    check(reachedSuccess && !app.canRedo(), "allocation sweep reaches one complete commit");

    // Persistent barriers include structural and lightweight changes; failed changes preserve history.
    engine.failPrepare = true;
    check(dispatch.dispatch(commands::AddBus{"Fail"}).status == commands::CommandStatus::rejected && app.canUndo(),
          "failed barrier preserves history");
    engine.failPrepare = false;
    ok(commands::AddBus{"Aux"});
    check(!app.canUndo() && !app.canRedo(), "structural barrier clears history");
    ok(commands::MoveClip{{1}, {0}});
    ok(commands::SetTrackGain{{1}, mixer::GainDb{-3}});
    check(!app.canUndo(), "parameter barrier clears history");
    ok(commands::SetTrackGain{{1}, mixer::GainDb{0}});
    ok(commands::SetTrackOutputDestination{{1}, routing::OutputDestination::toBus({1})});
    ok(commands::AddTrackSend{{1}, {1}, routing::SendTapPoint::preFaderPrePan, mixer::GainDb{-6}});
    ok(commands::AddProcessor{processors::InsertTarget{tracks::TrackId{1}}, processors::ProcessorType{"test.count"}});
    ok(commands::AddProcessor{processors::InsertTarget{routing::BusId{1}}, processors::ProcessorType{"test.count"}});
    ok(commands::AddProcessor{processors::InsertTarget{processors::MasterTarget{}}, processors::ProcessorType{"test.count"}});
    ok(commands::MoveClip{{1}, {12}});
    ok(commands::Undo{}); ok(commands::Redo{});
    check(engine.active->bundle->plan.sources.size() == 1 && engine.active->bundle->plan.sends.size() == 1 &&
          engine.active->bundle->plan.buses.size() == 1 && engine.active->bundle->plan.processors.size() == 3,
          "rebuild preserves source routing sends track/bus/master inserts");
    const auto historySize = app.history().size();
    ok(commands::Play{});
    check(dispatch.dispatch(commands::Undo{}).error == commands::CommandError::transportMustBeStopped,
          "Undo during Play explicitly rejected");
    check(dispatch.dispatch(commands::Redo{}).error == commands::CommandError::transportMustBeStopped,
          "Redo during Play explicitly rejected");
    ok(commands::Stop{});
    engine.rt.processBlock({nullptr,0,0}, timeline::SampleRate{48000});
    app.synchroniseTransport();
    check(app.history().size() == historySize, "transport never clears history");
    for (auto capacity : {64u,128u,256u,512u,1024u}) {
        engine.blockCapacity = capacity;
        ok(commands::Undo{}); ok(commands::Redo{});
        engine.calls = 0;
        const auto rendered = engine.render(1100);
        app.synchroniseTransport();
        check(engine.calls == 3 * ((app.project().duration().value + capacity - 1) / capacity),
              "inserts once per track/bus/master active subblock");
        check(rendered[0] == 0 && rendered[150] > rendered[20] && rendered[1099] == 0,
              "actual processBlock restores gaps overlap sum and end");
        check(app.transport().playback == transport::PlaybackState::stopped &&
              engine.transportSnapshot().position.value == app.project().duration().value, "natural end after history rebuild");
    }
}

void managerTests() {
    using namespace vitadaw;
    project::ProjectState model{timeline::SampleRate{48000}};
    const auto track = model.addAudioTrack("Test");
    const auto imported = model.importAudioToTrack(track, {"source",{}}, {441},
        timeline::SampleRate{44100}, media::AudioChannelLayout::mono);
    const auto before = *model.findClip(imported.clip);
    auto after = before; after.projectStart = {10};
    history::UndoableOperation move{history::MoveClip{track,track,before,after}};
    auto diverged = model;
    check(bool(diverged.moveClip(imported.clip,{20})), "diverge test model");
    const auto expected = *diverged.findClip(imported.clip);
    check(!move.apply(diverged,true) && *diverged.findClip(imported.clip) == expected, "expected state validation");
    history::UndoableOperation restore{history::DeleteClip{track,before}};
    check(!restore.apply(model,false), "restore cannot overwrite existing ID");
    auto invalid = before; invalid.id = {9999};
    check(!history::UndoableOperation{history::DeleteClip{track,invalid}}.apply(model,false), "cannot restore unconsumed ID");
    invalid = before; invalid.source = {900};
    check(bool(model.deleteClip(before.id)), "delete for restoration tests");
    check(!history::UndoableOperation{history::DeleteClip{track,invalid}}.apply(model,false), "missing source rejected");
    invalid = before; invalid.duration = {999999};
    check(!history::UndoableOperation{history::DeleteClip{track,invalid}}.apply(model,false), "out of bounds restoration rejected");
    check(restore.apply(model,false) && *model.findClip(before.id) == before, "exact internal restore");
    history::UndoManager manager;
    for (int i=0; i<600; ++i) {
        auto staged = manager.stage(move); check(bool(staged), "history stage succeeds");
        manager.commit(std::move(*staged));
    }
    check(manager.size() == 512 && manager.cursor() == 512 && manager.memoryBytes() <= history::UndoManager::memoryBudget,
          "512 cap evicts oldest applied entries");
    check(manager.revision() == 600, "revision increments on every new commit");
    for (int i=0; i<512; ++i) manager.commitUndo();
    check(!manager.canUndo() && manager.canRedo(), "eviction leaves coherent oldest boundary");
    for (int i=0; i<512; ++i) manager.commitRedo();
    check(manager.revision() == 1624, "revision increments on Undo and Redo");
    history::UndoManager small{{512, 2*sizeof(history::HistoryEntry)}};
    for (int i=0; i<5; ++i) { auto p=small.stage(move); small.commit(std::move(*p)); }
    check(small.size()==2 && small.memoryBytes()<=2*sizeof(history::HistoryEntry), "byte budget evicts independently");
    history::UndoManager tooSmall{{512,sizeof(history::HistoryEntry)-1}};
    check(!tooSmall.stage(move) && !tooSmall.canUndo(), "oversize payload rejected before commit");
    const auto trackState = model.captureTrackHistoryState(track);
    check(trackState.has_value(), "track history snapshot exists");
    history::UndoableOperation addTrack{history::AddAudioTrack{
        std::make_shared<const project::ProjectState::TrackHistoryState>(*trackState)}};
    history::UndoManager payloadTooSmall{{512, sizeof(history::HistoryEntry)}};
    check(!payloadTooSmall.stage(std::move(addTrack)) &&
              !payloadTooSmall.canUndo(),
          "variable track payload is rejected before model/plan commit");
}
} // namespace

void* operator new(std::size_t size) {
    if (inRealtime) ++rtAllocations;
    const auto remaining = failAllocation.load();
    if (remaining >= 0 && failAllocation.fetch_sub(1) == 0) throw std::bad_alloc{};
    if (void* ptr = std::malloc(size ? size : 1)) return ptr;
    throw std::bad_alloc{};
}
void operator delete(void* ptr) noexcept {
    if (ptr && inRealtime) ++rtDestructions;
    std::free(ptr);
}
void operator delete(void* ptr, std::size_t) noexcept { ::operator delete(ptr); }

int main() {
    run(); managerTests();
    std::cout << "Undo/Redo transactions, IDs, history budgets and RT regression passed\n";
}
