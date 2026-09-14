#include "vitadaw/application/DawApplication.h"
#include "vitadaw/commands/CommandDispatcher.h"
#include "vitadaw/processors/GainProcessor.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <memory>
#include <string_view>

namespace {

using namespace vitadaw;

void check(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

class FakeControl final : public audio::IAudioEngineControl {
public:
    class PreparedPlan final : public audio::PreparedProcessingPlanChange {
    public:
        explicit PreparedPlan(audio::ProcessingPlanSpecification value)
            : specification(std::move(value)) {}
        audio::ProcessingPlanSpecification specification;
    };

    audio::AudioFilePreparationResult prepareWav(
        const std::filesystem::path&) override {
        return {nullptr, "not used"};
    }
    audio::StructuralPlanPreparationResult prepareProcessingPlan(
        const audio::ProcessingPlanSpecification& specification) override {
        ++prepareCalls;
        if (failNextPrepare) {
            failNextPrepare = false;
            return {nullptr, "Injected processor preparation failure"};
        }
        const auto validation = audio::prepareProcessingPlan(specification, {});
        if (!validation.success()) {
            return {nullptr, validation.errorMessage};
        }
        return {std::make_unique<PreparedPlan>(specification), {}};
    }
    bool commitPreparedProcessingPlan(
        audio::PreparedProcessingPlanChangePtr prepared,
        audio::AudioFileCommitAction modelCommit) noexcept override {
        auto* candidate = dynamic_cast<PreparedPlan*>(prepared.get());
        if (candidate == nullptr || !modelCommit.isValid() || failNextCommit) {
            failNextCommit = false;
            return false;
        }
        ++commitCalls;
        live = std::move(candidate->specification);
        snapshot.playing = false;
        snapshot.position = {};
        modelCommit.execute();
        return true;
    }
    bool tryUpdateTrackMix(tracks::TrackId,
                           mixer::PreparedTrackMixState,
                           audio::PreparedAudibilityState) noexcept override {
        return false;
    }
    bool tryUpdateBusMix(routing::BusId, mixer::PreparedBusMixState,
                         audio::PreparedAudibilityState) noexcept override {
        return false;
    }
    bool tryUpdateSendMix(routing::SendId,
                          mixer::PreparedSendMixState) noexcept override {
        return false;
    }
    bool tryUpdateMasterMix(
        mixer::PreparedMasterMixState) noexcept override {
        return false;
    }
    bool tryUpdateProcessorBypass(
        processors::ProcessorInstanceId processor,
        bool bypassed) noexcept override {
        ++bypassCalls;
        if (!acceptLightweight) {
            return false;
        }
        auto* state = find(processor);
        if (state == nullptr) {
            return false;
        }
        state->bypassed = bypassed;
        return true;
    }
    bool tryUpdateProcessorParameter(
        processors::ProcessorInstanceId processor,
        processors::ParameterId parameter,
        float desiredValue,
        float preparedValue) noexcept override {
        ++parameterCalls;
        lastPreparedParameterValue = preparedValue;
        if (!acceptLightweight) {
            return false;
        }
        auto* state = find(processor);
        if (state == nullptr) {
            return false;
        }
        const auto found = std::find_if(
            state->parameters.begin(), state->parameters.end(),
            [parameter](const auto& candidate) {
                return candidate.id == parameter;
            });
        if (found == state->parameters.end()) {
            return false;
        }
        found->value = desiredValue;
        return true;
    }
    audio::AudioControlRequestResult tryRequestPlay() noexcept override {
        const auto sequence = nextSequence++;
        snapshot.playing = true;
        snapshot.lastProcessedCommandSequence = sequence;
        return {true, sequence};
    }
    audio::AudioControlRequestResult tryRequestStop() noexcept override {
        const auto sequence = nextSequence++;
        snapshot.playing = false;
        snapshot.position = {};
        snapshot.lastProcessedCommandSequence = sequence;
        return {true, sequence};
    }
    audio::RealtimeTransportSnapshot transportSnapshot() const noexcept override {
        return snapshot;
    }
    mixer::MeterSnapshot meterSnapshot() const noexcept override { return {}; }

    processors::ProcessorState* find(
        processors::ProcessorInstanceId id) noexcept {
        const auto findIn = [id](processors::InsertChain& chain)
            -> processors::ProcessorState* {
            const auto found = std::find_if(
                chain.processors.begin(), chain.processors.end(),
                [id](const auto& candidate) { return candidate.id == id; });
            return found == chain.processors.end() ? nullptr : &*found;
        };
        for (auto& track : live.tracks) {
            if (auto* found = findIn(track.inserts)) {
                return found;
            }
        }
        for (auto& bus : live.buses) {
            if (auto* found = findIn(bus.inserts)) {
                return found;
            }
        }
        return findIn(live.masterInserts);
    }

    audio::ProcessingPlanSpecification live;
    audio::RealtimeTransportSnapshot snapshot;
    audio::AudioCommandSequence nextSequence{1};
    bool failNextPrepare{};
    bool failNextCommit{};
    bool acceptLightweight{true};
    int prepareCalls{};
    int commitCalls{};
    int parameterCalls{};
    int bypassCalls{};
    float lastPreparedParameterValue{};
};

bool accepted(const commands::CommandResult& result) {
    return result.status == commands::CommandStatus::accepted;
}

} // namespace

int main() {
    FakeControl engine;
    application::DawApplication application{
        engine, timeline::SampleRate{48000.0}};
    commands::CommandDispatcher dispatcher{application};

    check(accepted(dispatcher.dispatch(commands::AddAudioTrack{"Track"})) &&
              accepted(dispatcher.dispatch(commands::AddBus{"Bus"})),
          "processor targets require committed track and bus nodes");
    const auto track = application.project().tracks().front().id;
    const auto bus = application.project().routing().buses().front().id;

    check(accepted(dispatcher.dispatch(commands::AddProcessor{
              track, {processors::internalGainProcessorType}})) &&
              accepted(dispatcher.dispatch(commands::AddProcessor{
                  bus, {processors::internalGainProcessorType}})) &&
              accepted(dispatcher.dispatch(commands::AddProcessor{
                  processors::MasterTarget{},
                  {processors::internalGainProcessorType}})),
          "AddProcessor must target track, bus and master through commands");
    const auto& project = application.project();
    check(project.tracks()[0].inserts.processors[0].id ==
                  processors::ProcessorInstanceId{1} &&
              project.routing().buses()[0].inserts.processors[0].id ==
                  processors::ProcessorInstanceId{2} &&
              project.masterInserts().processors[0].id ==
                  processors::ProcessorInstanceId{3},
          "ProcessorInstanceId must be strong, global and monotonic");

    check(accepted(dispatcher.dispatch(commands::AddProcessor{
              track, {processors::internalGainProcessorType}})) &&
              accepted(dispatcher.dispatch(commands::MoveProcessor{{4}, 0})) &&
              application.project().tracks()[0].inserts.processors[0].id ==
                  processors::ProcessorInstanceId{4},
          "MoveProcessor must reorder only the owning insert chain");

    check(accepted(dispatcher.dispatch(commands::SetProcessorParameter{
              {4}, processors::gainParameterId, -6.0F})) &&
              application.project().findProcessor({4})
                      ->parameters.front().value == -6.0F &&
              engine.find({4})->parameters.front().value == -6.0F &&
              std::abs(engine.lastPreparedParameterValue -
                       processors::GainProcessor::gainDbToLinear(-6.0F)) <
                  1.0e-6F,
          "accepted lightweight parameter updates must commit model and engine together");
    const auto previousValue = application.project()
                                   .findProcessor({4})
                                   ->parameters.front()
                                   .value;
    engine.acceptLightweight = false;
    check(!accepted(dispatcher.dispatch(commands::SetProcessorParameter{
              {4}, processors::gainParameterId, -12.0F})) &&
              application.project().findProcessor({4})
                      ->parameters.front().value == previousValue,
          "rejected parameter publication must preserve editable state");
    check(!accepted(dispatcher.dispatch(commands::SetProcessorBypass{{4}, true})) &&
              !application.project().findProcessor({4})->bypassed,
          "rejected bypass publication must preserve editable state");
    engine.acceptLightweight = true;
    check(accepted(dispatcher.dispatch(commands::SetProcessorBypass{{4}, true})) &&
              application.project().findProcessor({4})->bypassed &&
              engine.find({4})->bypassed,
          "accepted bypass must update by stable ProcessorInstanceId");
    check(!accepted(dispatcher.dispatch(commands::SetProcessorParameter{
              {4}, processors::gainParameterId,
              std::numeric_limits<float>::quiet_NaN()})),
          "invalid GainProcessor values must fail before queue publication");

    const auto processorCountBeforeFailure =
        application.project().tracks()[0].inserts.processors.size();
    engine.failNextPrepare = true;
    check(!accepted(dispatcher.dispatch(commands::AddProcessor{
              track, {processors::internalGainProcessorType}})) &&
              application.project().tracks()[0].inserts.processors.size() ==
                  processorCountBeforeFailure,
          "processor prepare failure must preserve the prior project and engine plan");
    engine.failNextCommit = true;
    check(!accepted(dispatcher.dispatch(commands::RemoveProcessor{{4}})) &&
              application.project().findProcessor({4}) != nullptr &&
              engine.find({4}) != nullptr,
          "processor commit failure must preserve both editable and live states");

    check(accepted(dispatcher.dispatch(commands::RemoveProcessor{{2}})) &&
              application.project().findProcessor({2}) == nullptr &&
              accepted(dispatcher.dispatch(commands::AddProcessor{
                  bus, {processors::internalGainProcessorType}})) &&
              application.project().routing().buses()[0]
                      .inserts.processors[0].id ==
                  processors::ProcessorInstanceId{5},
          "removed processor identities must never be reused");

    const auto preparesBeforePlay = engine.prepareCalls;
    check(accepted(dispatcher.dispatch(commands::Play{})) &&
              !accepted(dispatcher.dispatch(commands::AddProcessor{
                  track, {processors::internalGainProcessorType}})) &&
              !accepted(dispatcher.dispatch(commands::RemoveProcessor{{1}})) &&
              !accepted(dispatcher.dispatch(commands::MoveProcessor{{1}, 1})) &&
              engine.prepareCalls == preparesBeforePlay,
          "structural insert commands must be rejected while transport plays");
    check(accepted(dispatcher.dispatch(commands::SetProcessorParameter{
              {1}, processors::gainParameterId, -3.0F})) &&
              accepted(dispatcher.dispatch(commands::SetProcessorBypass{{1},
                                                                         true})),
          "parameter and bypass commands must remain available during Play");
    check(accepted(dispatcher.dispatch(commands::Stop{})) &&
              accepted(dispatcher.dispatch(commands::RemoveProcessor{{1}})),
          "stopped transport must permit transactional structural removal");

    check(!accepted(dispatcher.dispatch(commands::AddProcessor{
              tracks::TrackId{999},
              {processors::internalGainProcessorType}})) &&
              !accepted(dispatcher.dispatch(commands::SetProcessorBypass{
                  {999}, true})),
          "unknown targets and processor identities must be rejected coherently");

    std::cout << "Processor command tests passed\n";
    return EXIT_SUCCESS;
}
