#include "vitadaw/audio/PreparedProcessingPlan.h"
#include "vitadaw/audio/RealtimeAudioEngine.h"
#include "vitadaw/processors/GainProcessor.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <semaphore>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace vitadaw;

std::atomic<bool> observeRealtimeAllocations{};
std::atomic<std::size_t> realtimeAllocations{};

void check(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

bool close(float actual, float expected, float tolerance = 4.0e-5F) {
    return std::abs(actual - expected) <= tolerance;
}

struct ProcessorProbe {
    int prepareCalls{};
    int processCalls{};
    int resetCalls{};
    std::atomic<int> destructions{};
    std::atomic<int> activeRealtimeRegions{};
    std::atomic<int> destructionsWhileRealtimeActive{};
};

class TestProcessor final : public processors::IAudioProcessor {
public:
    enum class Operation {
        fixedDelay,
        addOne,
        inPlaceAddOne,
        multiplyTwo,
        pass,
        failPrepare
    };

    TestProcessor(Operation operation, std::size_t latency,
                  ProcessorProbe* probe) noexcept
        : operation_(operation), latency_(latency), probe_(probe) {}
    ~TestProcessor() override {
        if (probe_ != nullptr) {
            if (probe_->activeRealtimeRegions.load(std::memory_order_acquire) !=
                0) {
                probe_->destructionsWhileRealtimeActive.fetch_add(
                    1, std::memory_order_relaxed);
            }
            ++probe_->destructions;
        }
    }

    bool prepare(const processors::ProcessingFormat& format) override {
        if (probe_ != nullptr) {
            ++probe_->prepareCalls;
        }
        if (operation_ == Operation::failPrepare || !format.isValid()) {
            return false;
        }
        channels_ = format.channelCount();
        delay_.assign(channels_, std::vector<float>(latency_));
        writePosition_ = 0;
        return true;
    }

    void reset() noexcept override {
        if (probe_ != nullptr) {
            ++probe_->resetCalls;
        }
        for (auto& channel : delay_) {
            std::fill(channel.begin(), channel.end(), 0.0F);
        }
        writePosition_ = 0;
    }

    void applyParameter(processors::PreparedParameterEvent) noexcept override {}

    processors::ProcessStatus processBlock(
        const processors::ProcessorProcessContext& context,
        audio::ConstAudioBlockView input,
        audio::AudioBlockView output) noexcept override {
        if (probe_ != nullptr) {
            ++probe_->processCalls;
        }
        for (std::size_t frame = 0; frame < context.frameCount; ++frame) {
            for (std::size_t channel = 0; channel < input.channelCount;
                 ++channel) {
                const auto value = input.channels[channel][frame];
                if (operation_ == Operation::fixedDelay) {
                    const auto delayed = latency_ == 0
                                             ? value
                                             : delay_[channel][writePosition_];
                    if (latency_ != 0) {
                        delay_[channel][writePosition_] = value;
                    }
                    output.channels[channel][frame] = delayed;
                } else if (operation_ == Operation::addOne ||
                           operation_ == Operation::inPlaceAddOne) {
                    output.channels[channel][frame] = value + 1.0F;
                } else if (operation_ == Operation::multiplyTwo) {
                    output.channels[channel][frame] = value * 2.0F;
                } else {
                    output.channels[channel][frame] = value;
                }
            }
            if (operation_ == Operation::fixedDelay && latency_ != 0) {
                writePosition_ = (writePosition_ + 1) % latency_;
            }
        }
        return processors::ProcessStatus::processed;
    }

    processors::ProcessingFrameCount latency() const noexcept override {
        return {latency_};
    }
    processors::TailInfo tail() const noexcept override {
        return operation_ == Operation::fixedDelay
                   ? processors::TailInfo{processors::TailKind::finite,
                                          {latency_}}
                   : processors::TailInfo{};
    }
    processors::ProcessorCapabilities capabilities() const noexcept override {
        if (operation_ == Operation::inPlaceAddOne) {
            return {true, false, true, true};
        }
        return {false, true, true, true};
    }
    std::size_t runtimeMemoryBytes() const noexcept override {
        return latency_ * channels_ * sizeof(float);
    }

private:
    Operation operation_;
    std::size_t latency_{};
    std::size_t channels_{};
    std::size_t writePosition_{};
    ProcessorProbe* probe_{};
    std::vector<std::vector<float>> delay_;
};

class TestFactory final : public processors::IAudioProcessorFactory {
public:
    struct ProbeBinding {
        processors::ProcessorInstanceId id;
        ProcessorProbe* probe{};
    };
    std::vector<ProbeBinding> probes;

    std::unique_ptr<processors::IAudioProcessor> create(
        const processors::ProcessorState& state) const override {
        if (state.type.identifier == processors::internalGainProcessorType) {
            return processors::internalAudioProcessorFactory().create(state);
        }
        ProcessorProbe* probe{};
        const auto binding = std::find_if(
            probes.begin(), probes.end(), [&state](const auto& candidate) {
                return candidate.id == state.id;
            });
        if (binding != probes.end()) {
            probe = binding->probe;
        }
        if (state.type.identifier == "test.fixed-latency") {
            std::size_t latency{};
            if (!state.parameters.empty()) {
                latency = static_cast<std::size_t>(
                    std::max(0.0F, state.parameters.front().value));
            }
            return std::make_unique<TestProcessor>(
                TestProcessor::Operation::fixedDelay, latency, probe);
        }
        if (state.type.identifier == "test.add-one") {
            return std::make_unique<TestProcessor>(
                TestProcessor::Operation::addOne, 0, probe);
        }
        if (state.type.identifier == "test.in-place-add-one") {
            return std::make_unique<TestProcessor>(
                TestProcessor::Operation::inPlaceAddOne, 0, probe);
        }
        if (state.type.identifier == "test.multiply-two") {
            return std::make_unique<TestProcessor>(
                TestProcessor::Operation::multiplyTwo, 0, probe);
        }
        if (state.type.identifier == "test.counter") {
            return std::make_unique<TestProcessor>(
                TestProcessor::Operation::pass, 0, probe);
        }
        if (state.type.identifier == "test.fail-prepare") {
            return std::make_unique<TestProcessor>(
                TestProcessor::Operation::failPrepare, 0, probe);
        }
        return {};
    }
};

processors::ProcessorState processor(std::uint64_t id,
                                     std::string type,
                                     float value = 0.0F,
                                     bool bypassed = false) {
    processors::ProcessorState result;
    result.id = {id};
    result.type.identifier = std::move(type);
    result.bypassed = bypassed;
    if (result.type.identifier == processors::internalGainProcessorType) {
        result.parameters.push_back({processors::gainParameterId, value});
    } else if (result.type.identifier == "test.fixed-latency") {
        result.parameters.push_back({{2}, value});
    }
    return result;
}

audio::PreparedTrackView source(tracks::TrackId id,
                                const std::vector<float>& left,
                                const std::vector<float>* right = nullptr,
                                double rate = 1000.0) {
    return {id,
            {{left.data(), right != nullptr ? right->data() : nullptr}},
            right != nullptr ? 2U : 1U,
            {static_cast<std::uint64_t>(left.size())},
            timeline::SampleRate{rate},
            {0},
            {static_cast<std::int64_t>(left.size())},
            {0},
            {}};
}

audio::ProcessingPlanSpecification basic(double rate = 1000.0) {
    audio::ProcessingPlanSpecification result;
    result.projectSampleRate = timeline::SampleRate{rate};
    result.processingSampleRate = timeline::SampleRate{rate};
    return result;
}

routing::OutputDestination bus(std::uint64_t id) {
    return routing::OutputDestination::toBus({id});
}

struct Harness {
    std::unique_ptr<audio::PreparedProcessingBundle> prepared;
    audio::RealtimeAudioEngine engine;
    timeline::SampleRate rate;

    Harness(audio::ProcessingPlanSpecification specification,
            std::span<const audio::PreparedTrackView> sources,
            std::size_t capacity = 64,
            const processors::IAudioProcessorFactory* factory = nullptr)
        : rate(specification.processingSampleRate.isValid()
                   ? specification.processingSampleRate
                   : specification.projectSampleRate) {
        auto result = audio::prepareProcessingPlan(
            specification, sources, capacity,
            audio::defaultProcessingMemoryBudgetBytes, factory);
        check(result.success(), result.errorMessage);
        prepared = std::move(result.prepared);
        engine.configure(prepared->plan, prepared->runtime);
        engine.deviceInitialising();
        std::array<float*, 0> noChannels{};
        engine.processBlock({noChannels.data(), 0, 0}, rate);
        check(engine.tryRequestPlay().accepted,
              "prepared processor project should play");
    }

    std::pair<std::vector<float>, std::vector<float>> render(
        std::size_t frames, bool checkNoAllocation = true) {
        std::pair<std::vector<float>, std::vector<float>> output{
            std::vector<float>(frames), std::vector<float>(frames)};
        std::array<float*, 2> channels{output.first.data(),
                                       output.second.data()};
        realtimeAllocations.store(0, std::memory_order_relaxed);
        observeRealtimeAllocations.store(checkNoAllocation,
                                         std::memory_order_release);
        engine.processBlock({channels.data(), channels.size(), frames}, rate);
        observeRealtimeAllocations.store(false, std::memory_order_release);
        if (checkNoAllocation) {
            check(realtimeAllocations.load(std::memory_order_acquire) == 0,
                  "processor processBlock must not allocate");
        }
        return output;
    }
};

} // namespace

void* operator new(std::size_t size) {
    if (observeRealtimeAllocations.load(std::memory_order_acquire)) {
        realtimeAllocations.fetch_add(1, std::memory_order_relaxed);
    }
    if (auto* memory = std::malloc(size)) {
        return memory;
    }
    throw std::bad_alloc{};
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete[](void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }
void operator delete[](void* memory, std::size_t) noexcept { std::free(memory); }

int main() {
    const std::vector<float> ones(8192, 1.0F);
    const std::vector<float> zeros(8192, 0.0F);
    const std::array stereoSource{source({1}, ones, &ones)};
    const std::array monoSource{source({1}, ones)};
    const auto minusSix = processors::GainProcessor::gainDbToLinear(-6.0F);

    auto trackGain = basic();
    trackGain.tracks = {{{1}, {}, {},
                         {{processor(1, processors::internalGainProcessorType,
                                     -6.0F)}}}};
    Harness trackGainHarness{trackGain, stereoSource};
    const auto trackGainOutput = trackGainHarness.render(1);
    check(close(trackGainOutput.first[0], minusSix) &&
              close(trackGainOutput.second[0], minusSix) &&
              trackGainHarness.prepared->plan.processors.size() == 1 &&
              trackGainHarness.prepared->plan.tracks[0].inserts.count == 1 &&
              trackGainHarness.prepared->plan.processors[0].format.channelLayout ==
                  processors::ChannelLayout::stereo,
          "track GainProcessor must execute before track mixer in stereo");

    auto monoGain = trackGain;
    Harness monoHarness{monoGain, monoSource};
    const auto monoOutput = monoHarness.render(1);
    check(close(monoOutput.first[0], minusSix * audio::monoCentreCoefficient) &&
              close(monoOutput.second[0],
                    minusSix * audio::monoCentreCoefficient) &&
              monoHarness.prepared->plan.processors[0].format.channelLayout ==
                  processors::ChannelLayout::mono,
          "mono inserts must remain mono until the existing equal-power pan stage");

    mixer::BusMixState mutedBus;
    mutedBus.muted = true;
    auto signalFlow = basic();
    signalFlow.buses = {{{1}, {}, bus(2),
                          {{processor(2,
                                      processors::internalGainProcessorType,
                                      -6.0F)}}},
                         {{2}, mixer::prepare(mutedBus), {}, {}} ,
                         {{3}, {}, {}, {}}};
    signalFlow.tracks = {{{1}, {}, bus(1),
                          {{processor(1,
                                      processors::internalGainProcessorType,
                                      -6.0F)}}}};
    signalFlow.sends = {{{1}, routing::BusId{1}, {3},
                         routing::SendTapPoint::preFaderPrePan, {}}};
    signalFlow.masterInserts.processors.push_back(
        processor(3, processors::internalGainProcessorType, -6.0F));
    signalFlow.masterMix =
        mixer::prepare(mixer::MasterMixState{{-6.0F}});
    Harness signalFlowHarness{signalFlow, stereoSource};
    const auto flowOutput = signalFlowHarness.render(1);
    check(close(flowOutput.first[0],
                minusSix * minusSix * minusSix * minusSix) &&
              close(flowOutput.second[0],
                    minusSix * minusSix * minusSix * minusSix),
          "track, bus and master chains must run at their documented tap positions");

    auto trackPre = basic();
    trackPre.buses = {{{1}, {}, {}, {}},
                      {{2}, mixer::prepare(mutedBus), {}, {}}};
    trackPre.tracks = {{{1}, mixer::prepare(mixer::TrackMixState{{-100.0F}}),
                        bus(2),
                        {{processor(1,
                                    processors::internalGainProcessorType,
                                    -6.0F)}}}};
    trackPre.sends = {{{1}, tracks::TrackId{1}, {1},
                       routing::SendTapPoint::preFaderPrePan, {}}};
    Harness trackPreHarness{trackPre, monoSource};
    check(close(trackPreHarness.render(1).first[0],
                minusSix * audio::monoCentreCoefficient),
          "track pre-send must observe inserts but ignore channel gain and pan");
    trackPre.tracks[0].mix =
        mixer::prepare(mixer::TrackMixState{{-6.0F}});
    trackPre.sends[0].tapPoint =
        routing::SendTapPoint::postFaderPostPan;
    Harness trackPostHarness{trackPre, monoSource};
    check(close(trackPostHarness.render(1).first[0],
                minusSix * minusSix * audio::monoCentreCoefficient),
          "track post-send must observe inserts and the channel gain/pan stage");

    TestFactory factory;
    auto nonCommutative = basic();
    nonCommutative.tracks = {{{1}, {}, {},
                              {{processor(1, "test.add-one"),
                                processor(2, "test.multiply-two")}}}};
    Harness ordered{nonCommutative, stereoSource, 64, &factory};
    check(close(ordered.render(1).first[0], 4.0F),
          "insert order must be semantically observable");
    std::swap(nonCommutative.tracks[0].inserts.processors[0],
              nonCommutative.tracks[0].inserts.processors[1]);
    Harness reversed{nonCommutative, stereoSource, 64, &factory};
    check(close(reversed.render(1).first[0], 3.0F),
          "moving processors must change DSP order without hidden reordering");

    auto inPlaceOnly = basic();
    inPlaceOnly.tracks = {{{1}, {}, {},
                           {{processor(1, "test.in-place-add-one")}}}};
    Harness inPlaceOnlyHarness{inPlaceOnly, stereoSource, 64, &factory};
    check(close(inPlaceOnlyHarness.render(1).first[0], 2.0F),
          "prepared scratch must support processors that require in-place aliasing");

    ProcessorProbe latencyProbe;
    factory.probes = {{{1}, &latencyProbe}};
    std::vector<float> impulse(64);
    impulse[0] = 1.0F;
    const std::array impulseSource{source({1}, impulse, &zeros)};
    auto bypass = basic();
    bypass.tracks = {{{1}, {}, {},
                      {{processor(1, "test.fixed-latency", 2.0F, true)}}}};
    Harness bypassHarness{bypass, impulseSource, 16, &factory};
    const auto bypassOutput = bypassHarness.render(8);
    check(bypassOutput.first[0] == 0.0F &&
              bypassOutput.first[1] == 0.0F &&
              bypassOutput.first[2] == 1.0F &&
              latencyProbe.processCalls == 1,
          "host bypass must run the processor and delay dry by declared latency");

    check(bypassHarness.engine.tryRequestStop().accepted &&
              bypassHarness.engine.tryRequestStop().accepted &&
              bypassHarness.engine.tryRequestPlay().accepted,
          "rapid double Stop then Play must enqueue as an ordered reset barrier");
    const auto replayed = bypassHarness.render(4);
    check(replayed.first[0] == 0.0F && replayed.first[1] == 0.0F &&
              replayed.first[2] == 1.0F && latencyProbe.resetCalls > 0,
          "double Stop then Play must reset processor and bypass-delay state");
    bypassHarness.engine.deviceStopped();
    bypassHarness.engine.deviceInitialising();
    std::array<float*, 0> noChannels{};
    bypassHarness.engine.processBlock({noChannels.data(), 0, 0},
                                      bypassHarness.rate);
    check(bypassHarness.engine.tryRequestPlay().accepted,
          "device restart must leave a reset prepared chain playable");
    const auto restarted = bypassHarness.render(4);
    check(restarted.first[2] == 1.0F,
          "device restart must reset processor history before playback");

    ProcessorProbe trackProbe;
    ProcessorProbe busProbe;
    ProcessorProbe masterProbe;
    factory.probes = {{{1}, &trackProbe}, {{2}, &busProbe},
                      {{3}, &masterProbe}};
    auto once = basic();
    once.buses = {{{1}, mixer::prepare(mutedBus), {},
                    {{processor(2, "test.counter")}}},
                   {{2}, {}, {}, {}}};
    once.tracks = {{{1}, {}, bus(1),
                    {{processor(1, "test.counter")}}},
                   {{2}, mixer::prepare(mixer::TrackMixState{{}, {}, false,
                                                              true}),
                    {}, {}}};
    once.sends = {{{1}, routing::BusId{1}, {2},
                   routing::SendTapPoint::preFaderPrePan, {}},
                  {{2}, routing::BusId{1}, {2},
                   routing::SendTapPoint::postFaderPostPan, {}}};
    once.masterInserts.processors.push_back(processor(3, "test.counter"));
    Harness onceHarness{once, stereoSource, 128, &factory};
    static_cast<void>(onceHarness.render(513));
    check(trackProbe.processCalls == 5 && busProbe.processCalls == 5 &&
              masterProbe.processCalls == 5,
          "each processor must execute once per node and subblock regardless of fan-out");

    auto smoothGain = trackGain;
    Harness smoothGainHarness{smoothGain, stereoSource};
    check(smoothGainHarness.engine.tryUpdateProcessorParameter(
              {1}, processors::gainParameterId, 1.0F),
          "GainProcessor target must enqueue during Play");
    const auto smoothedGain = smoothGainHarness.render(5);
    const auto increment = (1.0F - minusSix) / 5.0F;
    check(close(smoothedGain.first[0], minusSix + increment) &&
              close(smoothedGain.first[4], 1.0F),
          "GainProcessor must own an exact 5 ms linear smoother at 1 kHz");

    auto silenceGain = trackGain;
    silenceGain.tracks[0].inserts.processors[0].parameters[0].value = -100.0F;
    Harness silenceGainHarness{silenceGain, stereoSource};
    check(silenceGainHarness.render(1).first[0] == 0.0F,
          "GainProcessor -100 dB must be exact DSP silence");
    auto boostGain = trackGain;
    boostGain.tracks[0].inserts.processors[0].parameters[0].value = 12.0F;
    Harness boostGainHarness{boostGain, stereoSource};
    check(close(boostGainHarness.render(1).first[0],
                processors::GainProcessor::gainDbToLinear(12.0F)),
          "GainProcessor must support the documented +12 dB boost");

    for (const auto capacity : std::array<std::size_t, 5>{64, 128, 256,
                                                          512, 1024}) {
        ProcessorProbe probe;
        factory.probes = {{{1}, &probe}};
        auto sized = basic();
        sized.tracks = {{{1}, {}, {},
                         {{processor(1, "test.counter")}}}};
        Harness sizedHarness{sized, stereoSource, capacity, &factory};
        static_cast<void>(sizedHarness.render(capacity + 17));
        check(probe.processCalls == 2,
              "callbacks larger than capacity must preserve one DSP call per subblock");
    }
    factory.probes.clear();

    auto latency = basic();
    latency.buses = {{{1}, {}, {},
                       {{processor(2, "test.fixed-latency", 3.0F)}}},
                      {{2}, {}, {}, {}}};
    latency.tracks = {{{1}, {}, bus(1),
                       {{processor(1, "test.fixed-latency", 2.0F)}}}};
    latency.sends = {{{1}, routing::BusId{1}, {2},
                      routing::SendTapPoint::postFaderPostPan, {}}};
    latency.masterInserts.processors.push_back(
        processor(3, "test.fixed-latency", 4.0F));
    auto latencyPrepared = audio::prepareProcessingPlan(
        latency, stereoSource, 64, audio::defaultProcessingMemoryBudgetBytes,
        &factory);
    check(latencyPrepared.success(), latencyPrepared.errorMessage);
    const auto& latencyPlan = latencyPrepared.prepared->plan;
    check(latencyPlan.tracks[0].mainOutputLatency.maximum.value == 2 &&
              latencyPlan.buses[0].inputLatency.minimum.value == 2 &&
              latencyPlan.buses[0].mainOutputLatency.maximum.value == 5 &&
              latencyPlan.sends[0].sourceTapLatency.maximum.value == 5 &&
              latencyPlan.buses[1].inputLatency.maximum.value == 5 &&
              latencyPlan.masterInputLatency.maximum.value == 5 &&
              latencyPlan.masterOutputLatency.maximum.value == 9 &&
              latencyPlan.processors[0].tail.kind ==
                  processors::TailKind::finite,
          "latency metadata must accumulate per chain and remain attached to each edge");

    auto uncompensated = basic();
    uncompensated.buses = {{{1}, {}, {},
                             {{processor(2, "test.fixed-latency", 2.0F)}}}};
    uncompensated.tracks = {{{1}, {}, {}, {}}};
    uncompensated.sends = {{{1}, tracks::TrackId{1}, {1},
                            routing::SendTapPoint::postFaderPostPan, {}}};
    Harness uncompensatedHarness{uncompensated, impulseSource, 16, &factory};
    const auto uncompensatedOutput = uncompensatedHarness.render(6);
    check(uncompensatedOutput.first[0] == 1.0F &&
              uncompensatedOutput.first[2] == 1.0F,
          "0.3.0 must report but deliberately not compensate parallel-path latency");

    auto invalid = basic();
    invalid.tracks = {{{1}, {}, {},
                       {{processor(1, processors::internalGainProcessorType,
                                   std::numeric_limits<float>::quiet_NaN())}}}};
    check(!audio::prepareProcessingPlan(invalid, stereoSource).success(),
          "non-finite processor parameters must fail preparation");
    invalid.tracks[0].inserts.processors[0].parameters[0].value = 13.0F;
    check(!audio::prepareProcessingPlan(invalid, stereoSource).success(),
          "out-of-range GainProcessor parameters must fail preparation");
    invalid.tracks[0].inserts.processors[0].parameters[0] = {{2}, 0.0F};
    check(!audio::prepareProcessingPlan(invalid, stereoSource).success(),
          "GainProcessor must reject an unknown prepared parameter schema");
    invalid.tracks[0].inserts.processors = {
        processor(1, "test.fail-prepare")};
    check(!audio::prepareProcessingPlan(
               invalid, stereoSource, 64,
               audio::defaultProcessingMemoryBudgetBytes, &factory).success(),
          "processor prepare failure must reject the candidate plan");
    invalid.tracks[0].inserts.processors.clear();
    for (std::size_t index = 0;
         index <= audio::maximumPreparedProcessorsPerChain; ++index) {
        invalid.tracks[0].inserts.processors.push_back(
            processor(index + 1, processors::internalGainProcessorType));
    }
    check(!audio::prepareProcessingPlan(invalid, stereoSource).success(),
          "per-chain processor capacity must fail before RT publication");
    auto tooManyProcessors = basic();
    std::uint64_t nextProcessorId = 1;
    for (std::uint64_t trackId = 1; trackId <= 33; ++trackId) {
        audio::ProcessingPlanTrackSpecification track{{trackId}, {}, {}, {}};
        for (std::size_t index = 0;
             index < audio::maximumPreparedProcessorsPerChain; ++index) {
            track.inserts.processors.push_back(processor(
                nextProcessorId++, processors::internalGainProcessorType));
        }
        tooManyProcessors.tracks.push_back(std::move(track));
    }
    check(!audio::prepareProcessingPlan(tooManyProcessors, {}).success(),
          "global prepared processor capacity must be enforced before creation");
    auto duplicate = trackGain;
    duplicate.masterInserts.processors.push_back(
        processor(1, processors::internalGainProcessorType));
    check(!audio::prepareProcessingPlan(duplicate, stereoSource).success(),
          "ProcessorInstanceId must be unique across all insert targets");
    check(!audio::prepareProcessingPlan(
               trackGain, stereoSource, 64, 1).success(),
          "prepared scratch and runtime memory must obey a checked budget");
    check(!audio::prepareProcessingPlan(
               trackGain, stereoSource,
               std::numeric_limits<std::size_t>::max()).success(),
          "processing buffer arithmetic must reject overflow before allocation");

    auto realtimeSpec = trackGain;
    realtimeSpec.processingMode = processors::ProcessingMode::realtime;
    auto offlineSpec = trackGain;
    offlineSpec.processingMode = processors::ProcessingMode::offline;
    Harness realtimeHarness{realtimeSpec, stereoSource};
    Harness offlineHarness{offlineSpec, stereoSource};
    static_cast<void>(realtimeHarness.render(16));
    static_cast<void>(offlineHarness.render(16));
    check(realtimeHarness.engine.tryUpdateProcessorParameter(
              {1}, processors::gainParameterId, 1.0F) &&
              offlineHarness.engine.tryUpdateProcessorParameter(
                  {1}, processors::gainParameterId, 1.0F),
          "processor parameter events must be accepted by stable identity");
    const auto realtimeOutput = realtimeHarness.render(32);
    const auto offlineOutput = offlineHarness.render(32);
    check(realtimeOutput == offlineOutput,
          "realtime and offline modes must use the same engine and event timing");

    auto queueSpec = trackGain;
    Harness queueHarness{queueSpec, stereoSource};
    for (std::size_t index = 0;
         index < audio::RealtimeAudioEngine::parameterCommandCapacity - 1;
         ++index) {
        check(queueHarness.engine.tryUpdateProcessorBypass({1}, index % 2),
              "processor parameter ring must accept its usable capacity");
    }
    check(!queueHarness.engine.tryUpdateProcessorBypass({1}, true),
          "full processor parameter ring must reject without overwriting");

    auto generationOld = audio::prepareProcessingPlan(trackGain, stereoSource,
                                                       64);
    auto generationNewSpec = trackGain;
    generationNewSpec.tracks[0].inserts.processors[0].parameters[0].value =
        -12.0F;
    auto generationNew = audio::prepareProcessingPlan(generationNewSpec,
                                                       stereoSource, 64);
    check(generationOld.success() && generationNew.success(),
          "generation test plans must prepare");
    audio::RealtimeAudioEngine generationEngine;
    generationEngine.configure(generationOld.prepared->plan,
                               generationOld.prepared->runtime);
    generationEngine.deviceInitialising();
    std::array<float*, 0> generationNoChannels{};
    generationEngine.processBlock({generationNoChannels.data(), 0, 0},
                                  timeline::SampleRate{1000.0});
    check(generationEngine.tryUpdateProcessorParameter(
              {1}, processors::gainParameterId,
              processors::GainProcessor::gainDbToLinear(12.0F)),
          "old-plan parameter event must enqueue before replacement");
    generationEngine.configure(generationNew.prepared->plan,
                               generationNew.prepared->runtime);
    generationEngine.deviceInitialising();
    generationEngine.processBlock({generationNoChannels.data(), 0, 0},
                                  timeline::SampleRate{1000.0});
    check(generationEngine.tryRequestPlay().accepted,
          "replacement generation should play");
    std::array<float, 1> generationLeft{}, generationRight{};
    std::array<float*, 2> generationChannels{generationLeft.data(),
                                             generationRight.data()};
    generationEngine.processBlock(
        {generationChannels.data(), generationChannels.size(), 1},
        timeline::SampleRate{1000.0});
    check(close(generationLeft[0],
                processors::GainProcessor::gainDbToLinear(-12.0F)),
          "queued events from a retired plan generation must not reach its replacement");

    ProcessorProbe lifetimeProbe;
    factory.probes = {{{1}, &lifetimeProbe}};
    auto lifetimeSpec = basic();
    lifetimeSpec.tracks = {{{1}, {}, {},
                            {{processor(1, "test.counter")}}}};
    auto old = audio::prepareProcessingPlan(
        lifetimeSpec, stereoSource, 64,
        audio::defaultProcessingMemoryBudgetBytes, &factory);
    check(old.success(), old.errorMessage);
    audio::RealtimeAudioEngine lifetimeEngine;
    lifetimeEngine.configure(old.prepared->plan, old.prepared->runtime);
    auto replacement = audio::prepareProcessingPlan(basic(), {}, 64);
    check(replacement.success(), replacement.errorMessage);
    lifetimeEngine.configure(replacement.prepared->plan,
                             replacement.prepared->runtime);
    check(lifetimeProbe.destructions == 0,
          "old processor owner must remain alive until application releases the quiescent bundle");
    old.prepared.reset();
    check(lifetimeProbe.destructions == 1,
          "processor instance must be destroyed with its retired bundle outside RT");

    ProcessorProbe concurrentLifetimeProbe;
    factory.probes = {{{1}, &concurrentLifetimeProbe}};
    auto concurrentOld = audio::prepareProcessingPlan(
        lifetimeSpec, stereoSource, 64,
        audio::defaultProcessingMemoryBudgetBytes, &factory);
    check(concurrentOld.success(), concurrentOld.errorMessage);
    lifetimeEngine.configure(concurrentOld.prepared->plan,
                             concurrentOld.prepared->runtime);
    lifetimeEngine.deviceInitialising();
    std::array<float*, 0> lifetimeNoChannels{};
    lifetimeEngine.processBlock({lifetimeNoChannels.data(), 0, 0},
                                timeline::SampleRate{1000.0});
    check(lifetimeEngine.tryRequestPlay().accepted,
          "lifetime processor plan should play");
    std::mutex callbackSerialization;
    std::binary_semaphore callbackEntered{0};
    std::binary_semaphore finishCallback{0};
    std::thread realtimeUse([&] {
        const std::lock_guard lock{callbackSerialization};
        concurrentLifetimeProbe.activeRealtimeRegions.fetch_add(
            1, std::memory_order_acq_rel);
        callbackEntered.release();
        finishCallback.acquire();
        std::array<float, 8> left{}, right{};
        std::array<float*, 2> channels{left.data(), right.data()};
        lifetimeEngine.processBlock(
            {channels.data(), channels.size(), left.size()},
            timeline::SampleRate{1000.0});
        concurrentLifetimeProbe.activeRealtimeRegions.fetch_sub(
            1, std::memory_order_acq_rel);
    });
    callbackEntered.acquire();
    auto concurrentReplacement = audio::prepareProcessingPlan(basic(), {}, 64);
    check(concurrentReplacement.success(), concurrentReplacement.errorMessage);
    std::thread replaceAfterQuiescence([&] {
        const std::lock_guard lock{callbackSerialization};
        lifetimeEngine.configure(concurrentReplacement.prepared->plan,
                                 concurrentReplacement.prepared->runtime);
        concurrentOld.prepared.reset();
    });
    check(concurrentLifetimeProbe.destructions == 0,
          "retired processor must remain owned while an RT region can observe it");
    finishCallback.release();
    realtimeUse.join();
    replaceAfterQuiescence.join();
    check(concurrentLifetimeProbe.destructions == 1 &&
              concurrentLifetimeProbe.destructionsWhileRealtimeActive.load(
                  std::memory_order_acquire) == 0,
          "processor destruction must occur after the actual processBlock region is quiescent");

    auto scale = basic(48000.0);
    for (std::uint64_t busId = 1; busId <= 8; ++busId) {
        audio::ProcessingPlanBusSpecification busSpecification{
            {busId}, {}, {}, {}};
        for (std::uint64_t insert = 0; insert < 3; ++insert) {
            busSpecification.inserts.processors.push_back(processor(
                1000 + busId * 10 + insert,
                processors::internalGainProcessorType));
        }
        scale.buses.push_back(std::move(busSpecification));
    }
    for (std::uint64_t trackId = 1; trackId <= 32; ++trackId) {
        audio::ProcessingPlanTrackSpecification trackSpecification{
            {trackId}, {}, {}, {}};
        for (std::uint64_t insert = 0; insert < 3; ++insert) {
            trackSpecification.inserts.processors.push_back(processor(
                2000 + trackId * 10 + insert,
                processors::internalGainProcessorType));
        }
        scale.tracks.push_back(std::move(trackSpecification));
        for (std::uint64_t destination = 1; destination <= 8;
             ++destination) {
            scale.sends.push_back(
                {{(trackId - 1) * 8 + destination}, tracks::TrackId{trackId},
                 {destination}, routing::SendTapPoint::postFaderPostPan, {}});
        }
    }
    for (std::uint64_t insert = 0; insert < 3; ++insert) {
        scale.masterInserts.processors.push_back(processor(
            3000 + insert, processors::internalGainProcessorType));
    }
    const std::vector<float> scaleSamples(256, 0.01F);
    const std::array scaleSource{source({1}, scaleSamples, &scaleSamples,
                                        48000.0)};
    auto scalePrepared = audio::prepareProcessingPlan(scale, scaleSource, 64);
    check(scalePrepared.success() &&
              scalePrepared.prepared->plan.processors.size() == 123 &&
              scalePrepared.prepared->plan.sends.size() == 256 &&
              scalePrepared.prepared->plan.runtimeMemoryBytes <=
                  audio::defaultProcessingMemoryBudgetBytes,
          "32 tracks, 8 buses, 123 inserts and 256 sends must prepare within bounded memory");
    Harness scaleHarness{scale, scaleSource};
    static_cast<void>(scaleHarness.render(128));

    std::cout << "Processor and insert core tests passed\n";
    return EXIT_SUCCESS;
}
