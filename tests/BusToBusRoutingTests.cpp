#include "vitadaw/audio/PreparedProcessingPlan.h"
#include "vitadaw/audio/RealtimeAudioEngine.h"

#include <array>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <new>
#include <string_view>
#include <vector>

namespace {

std::atomic<bool> observeRealtimeAllocations{};
std::atomic<std::size_t> realtimeAllocations{};

void check(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

bool close(float actual, float expected, float tolerance = 2.0e-5F) {
    return std::abs(actual - expected) <= tolerance;
}

vitadaw::audio::PreparedTrackView source(
    vitadaw::tracks::TrackId id, const std::vector<float>& samples,
    double rate = 48000.0) {
    return {id, {{samples.data(), samples.data()}}, 2,
            {static_cast<std::uint64_t>(samples.size())},
            vitadaw::timeline::SampleRate{rate}, {0},
            {static_cast<std::int64_t>(samples.size())}, {0}, {}};
}

vitadaw::routing::OutputDestination bus(std::uint64_t id) {
    return vitadaw::routing::OutputDestination::toBus({id});
}

vitadaw::audio::ProcessingPlanBusSpecification busSpec(
    std::uint64_t id,
    vitadaw::routing::OutputDestination destination =
        vitadaw::routing::OutputDestination::master(),
    vitadaw::mixer::BusMixState mix = {}) {
    return {{id}, vitadaw::mixer::prepare(mix), destination};
}

struct Harness {
    std::unique_ptr<vitadaw::audio::PreparedProcessingBundle> prepared;
    vitadaw::audio::RealtimeAudioEngine engine;
    double rate{};

    Harness(vitadaw::audio::ProcessingPlanSpecification specification,
            std::span<const vitadaw::audio::PreparedTrackView> sources,
            std::size_t capacity = 512)
        : rate(specification.projectSampleRate.hertz()) {
        auto result = vitadaw::audio::prepareProcessingPlan(
            specification, sources, capacity);
        check(result.success(), result.errorMessage);
        prepared = std::move(result.prepared);
        engine.configure(prepared->plan, prepared->runtime);
        engine.deviceInitialising();
        std::array<float*, 0> none{};
        engine.processBlock({none.data(), 0, 0},
                            vitadaw::timeline::SampleRate{rate});
        check(engine.tryRequestPlay().accepted, "DAG project should play");
    }

    std::pair<std::vector<float>, std::vector<float>> render(std::size_t frames) {
        std::pair<std::vector<float>, std::vector<float>> output{
            std::vector<float>(frames), std::vector<float>(frames)};
        std::array<float*, 2> channels{output.first.data(), output.second.data()};
        realtimeAllocations.store(0, std::memory_order_relaxed);
        observeRealtimeAllocations.store(true, std::memory_order_release);
        engine.processBlock({channels.data(), channels.size(), frames},
                            vitadaw::timeline::SampleRate{rate});
        observeRealtimeAllocations.store(false, std::memory_order_release);
        check(realtimeAllocations.load(std::memory_order_acquire) == 0,
              "Bus-to-Bus processBlock must not allocate");
        return output;
    }
};

vitadaw::audio::ProcessingPlanSpecification basic(double rate = 48000.0) {
    vitadaw::audio::ProcessingPlanSpecification result;
    result.projectSampleRate = vitadaw::timeline::SampleRate{rate};
    return result;
}

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
    using namespace vitadaw;

    // Dense identity, buffer identity, and execution order are deliberately
    // independent of editable storage order.
    auto graph = basic();
    graph.buses = {busSpec(30), busSpec(10, bus(20)), busSpec(5),
                   busSpec(20, bus(30))};
    graph.tracks = {{{2}, {}, bus(10)}, {{1}, {}, bus(5)}};
    const std::vector<float> ones(4096, 1.0F);
    const std::vector<float> halves(4096, 0.5F);
    const std::array graphSources{source({2}, ones), source({1}, halves)};
    auto preparedGraph = audio::prepareProcessingPlan(graph, graphSources, 64);
    check(preparedGraph.success(), preparedGraph.errorMessage);
    const auto& plan = preparedGraph.prepared->plan;
    check(plan.buses[0].id == routing::BusId{5} &&
              plan.buses[1].id == routing::BusId{10} &&
              plan.buses[2].id == routing::BusId{20} &&
              plan.buses[3].id == routing::BusId{30} &&
              plan.buses[1].destinationBusIndex == 2 &&
              plan.buses[2].destinationBusIndex == 3,
          "BusId, dense index, destination index, and storage order must stay distinct");
    std::vector<std::uint64_t> busOrder;
    std::size_t trackSteps{};
    for (const auto& step : plan.order) {
        if (step.kind == audio::ProcessingStepKind::track) {
            ++trackSteps;
        } else if (step.kind == audio::ProcessingStepKind::bus) {
            busOrder.push_back(plan.buses[step.index].id.value);
        }
    }
    check(trackSteps == 2 &&
              busOrder == std::vector<std::uint64_t>({5, 10, 20, 30}) &&
              plan.order.back().kind == audio::ProcessingStepKind::master,
          "each node must occur once in deterministic topological order");
    auto permutedGraph = graph;
    permutedGraph.buses = {graph.buses[1], graph.buses[3], graph.buses[0],
                           graph.buses[2]};
    auto permutedPlan = audio::prepareProcessingPlan(
        permutedGraph, graphSources, 64);
    check(permutedPlan.success(), permutedPlan.errorMessage);
    for (std::size_t index = 0; index < plan.buses.size(); ++index) {
        check(permutedPlan.prepared->plan.buses[index].id == plan.buses[index].id &&
                  permutedPlan.prepared->plan.buses[index].destinationBusIndex ==
                      plan.buses[index].destinationBusIndex,
              "editable storage permutation must not change dense bus identity");
    }
    Harness chain{graph, graphSources, 64};
    const auto chainOutput = chain.render(1);
    const auto chainMeters = chain.engine.meterSnapshot();
    check(chainOutput.first[0] == 1.5F &&
              chainMeters.buses[0].bus == routing::BusId{5} &&
              chainMeters.buses[1].bus == routing::BusId{10} &&
              chainMeters.buses[1].peak.left == 1.0F &&
              chainMeters.buses[2].peak.left == 1.0F &&
              chainMeters.buses[3].peak.left == 1.0F,
          "independent and chained branches must sum without duplicate processing");

    // Cycle validation covers every bus, even graphs without tracks.
    auto selfCycle = basic();
    selfCycle.buses = {busSpec(12, bus(12))};
    const auto selfResult = audio::prepareProcessingPlan(selfCycle, {});
    check(!selfResult.success() && selfResult.errorMessage.find("12") != std::string::npos,
          "self-routing must be rejected with the implicated BusId");
    auto twoCycle = basic();
    twoCycle.buses = {busSpec(1, bus(2)), busSpec(2, bus(1))};
    check(!audio::prepareProcessingPlan(twoCycle, {}).success(),
          "a two-bus cycle without tracks must be rejected");
    auto longCycle = basic();
    longCycle.buses = {busSpec(1, bus(2)), busSpec(2, bus(3)),
                       busSpec(3, bus(1))};
    const auto longResult = audio::prepareProcessingPlan(longCycle, {});
    check(!longResult.success() && longResult.errorMessage.find("Bus 1") != std::string::npos &&
              longResult.errorMessage.find("Bus 3") != std::string::npos,
          "an arbitrary disconnected cycle must report its BusId chain");
    auto missingBus = basic();
    missingBus.buses = {busSpec(1, bus(99))};
    check(!audio::prepareProcessingPlan(missingBus, {}).success(),
          "a missing bus destination must be rejected before topology preparation");

    // Two gains apply exactly once. Initial prepared values require no ramp.
    auto gained = basic();
    gained.buses = {
        busSpec(1, bus(2), mixer::BusMixState{mixer::GainDb{-6.0F}}),
        busSpec(2, routing::OutputDestination::master(),
                mixer::BusMixState{mixer::GainDb{-3.0F}})};
    gained.tracks = {{{1}, {}, bus(1)}};
    const std::array gainedSource{source({1}, ones)};
    Harness gainedChain{gained, gainedSource};
    const auto gainedOutput = gainedChain.render(1);
    const auto expectedGain = std::pow(10.0F, -9.0F / 20.0F);
    check(close(gainedOutput.first[0], expectedGain),
          "-6 dB and -3 dB bus stages must produce one -9 dB chain");

    // Track Solo opens only its downstream transport path, not sibling input.
    auto trackSolo = basic();
    trackSolo.buses = {busSpec(1, bus(2)), busSpec(2)};
    mixer::TrackMixState soloTrack;
    soloTrack.solo = true;
    trackSolo.tracks = {{{1}, mixer::prepare(soloTrack), bus(1)},
                        {{2}, {}, bus(2)}};
    const std::array soloSources{source({1}, ones), source({2}, halves)};
    Harness trackSoloChain{trackSolo, soloSources};
    check(trackSoloChain.render(1).first[0] == 1.0F,
          "Track Solo must open downstream buses without opening sibling sources");

    // Bus Solo selects all upstream content, while downstream nodes only transport it.
    auto childSolo = basic();
    mixer::BusMixState selectedBus;
    selectedBus.solo = true;
    childSolo.buses = {busSpec(1, bus(2), selectedBus), busSpec(2)};
    childSolo.tracks = {{{1}, {}, bus(1)}, {{2}, {}, bus(1)}, {{3}, {}, bus(2)}};
    const std::array childSources{source({1}, ones), source({2}, halves),
                                  source({3}, halves)};
    Harness childSoloChain{childSolo, childSources};
    check(childSoloChain.render(1).first[0] == 1.5F,
          "Bus Solo must include its upstream sources but exclude downstream siblings");
    auto parentSolo = childSolo;
    parentSolo.buses[0].mix = mixer::prepare(mixer::BusMixState{});
    parentSolo.buses[1].mix = mixer::prepare(selectedBus);
    Harness parentSoloChain{parentSolo, childSources};
    check(parentSoloChain.render(1).first[0] == 2.0F,
          "parent Bus Solo must select every source that reaches it");
    parentSolo.buses[0].mix = mixer::prepare(selectedBus);
    Harness parentAndChildSolo{parentSolo, childSources};
    check(parentAndChildSolo.render(1).first[0] == 2.0F,
          "parent and child Bus Solo must form a union without duplication");

    auto unionSelection = basic();
    unionSelection.buses = {busSpec(1, bus(2)), busSpec(2),
                            busSpec(3, routing::OutputDestination::master(),
                                    selectedBus)};
    unionSelection.tracks = {{{1}, mixer::prepare(soloTrack), bus(1)},
                             {{2}, {}, bus(2)}, {{3}, {}, bus(3)}};
    const std::vector<float> quarters(4096, 0.25F);
    const std::array unionSources{source({1}, ones), source({2}, halves),
                                  source({3}, quarters)};
    Harness unionChain{unionSelection, unionSources};
    check(unionChain.render(1).first[0] == 1.25F,
          "Track Solo and Bus Solo on independent branches must form a union");

    auto directSolo = basic();
    directSolo.buses = {busSpec(1, bus(2)), busSpec(2)};
    directSolo.tracks = {{{1}, {}, bus(1)},
                         {{2}, mixer::prepare(soloTrack),
                          routing::OutputDestination::master()}};
    const std::array directSources{source({1}, ones), source({2}, halves)};
    Harness directSoloChain{directSolo, directSources};
    check(directSoloChain.render(1).first[0] == 0.5F,
          "a direct-to-Master Track Solo must not open a bus chain");

    auto intermediateMute = childSolo;
    auto mutedSelectedBus = selectedBus;
    mutedSelectedBus.muted = true;
    intermediateMute.buses[0].mix = mixer::prepare(mutedSelectedBus);
    Harness intermediateMuted{intermediateMute, childSources};
    const auto intermediateOutput = intermediateMuted.render(1);
    const auto intermediateMeters = intermediateMuted.engine.meterSnapshot();
    check(intermediateOutput.first[0] == 0.0F &&
              intermediateMeters.buses[0].peak.left == 0.0F &&
              intermediateMeters.buses[1].peak.left == 0.0F,
          "an intermediate Mute must prevail over Solo without alternate routing");

    selectedBus.muted = true;
    childSolo.buses[1].mix = mixer::prepare(selectedBus);
    Harness downstreamMute{childSolo, childSources};
    const auto mutedOutput = downstreamMute.render(1);
    const auto mutedMeters = downstreamMute.engine.meterSnapshot();
    check(mutedOutput.first[0] == 0.0F && mutedMeters.buses[0].peak.left == 1.5F &&
              mutedMeters.buses[1].peak.left == 0.0F,
          "a downstream Mute must silence the path while preserving upstream metering");

    auto balanced = basic();
    balanced.buses = {
        busSpec(1, bus(2), mixer::BusMixState{{}, mixer::Pan{-1.0F}}),
        busSpec(2, routing::OutputDestination::master(),
                mixer::BusMixState{{}, mixer::Pan{1.0F}})};
    balanced.tracks = {{{1}, {}, bus(1)}};
    Harness balanceChain{balanced, gainedSource};
    const auto balanceOutput = balanceChain.render(1);
    check(balanceOutput.first[0] == 0.0F && balanceOutput.second[0] == 0.0F,
          "each bus balance stage must apply exactly once in chain order");

    // A bus smoother advances once per sample, not once per input or graph edge.
    auto smooth = basic(1000.0);
    smooth.buses = {busSpec(1, bus(2),
                           mixer::BusMixState{mixer::GainDb{-100.0F}}),
                    busSpec(2)};
    smooth.tracks = {{{1}, {}, bus(1)}, {{2}, {}, bus(1)}};
    const std::array smoothSources{source({1}, halves, 1000.0),
                                   source({2}, halves, 1000.0)};
    Harness smoothChain{smooth, smoothSources, 2};
    check(smoothChain.engine.tryUpdateBusMix(
              {1}, mixer::prepare(mixer::BusMixState{}),
              audio::fullyAudibleState()),
          "bus smoother update must enqueue by BusId");
    const auto ramp = smoothChain.render(5);
    check(close(ramp.first[0], 0.2F) && close(ramp.first[4], 1.0F),
          "bus smoothing must advance once through fan-in and prepared subblocks");

    auto shortGraph = basic(1000.0);
    shortGraph.buses = {busSpec(1, bus(2)), busSpec(2)};
    shortGraph.tracks = {{{1}, {}, bus(1)}};
    const std::vector<float> oneFrame{0.75F};
    const std::array shortSource{source({1}, oneFrame, 1000.0)};
    Harness shortChain{shortGraph, shortSource, 2};
    const auto shortOutput = shortChain.render(4);
    check(shortOutput.first[0] == 0.75F && shortOutput.first[1] == 0.0F &&
              shortOutput.first[2] == 0.0F && shortOutput.first[3] == 0.0F,
          "bus buffers must not leak residue across prepared subblocks or natural end");
    check(shortChain.engine.tryRequestPlay().accepted,
          "Play after the chained natural end must restart");
    const auto shortReplay = shortChain.render(1);
    check(shortReplay.first[0] == 0.75F,
          "a chained project must replay cleanly from the beginning");

    // Functional scale: 32 tracks, 8 buses in two four-stage chains, mixed rates.
    auto scale = basic();
    for (std::uint64_t id = 1; id <= 8; ++id) {
        const auto endOfChain = id == 4 || id == 8;
        scale.buses.push_back(busSpec(
            id, endOfChain ? routing::OutputDestination::master() : bus(id + 1)));
    }
    std::vector<std::vector<float>> scaleSignals(32, std::vector<float>(2048, 0.01F));
    std::vector<audio::PreparedTrackView> scaleSources;
    for (std::uint64_t id = 1; id <= 32; ++id) {
        scale.tracks.push_back({{id}, {}, bus(id % 2 == 0 ? 1 : 5)});
        scaleSources.push_back(source({id}, scaleSignals[id - 1],
                                      id % 2 == 0 ? 44100.0 : 48000.0));
    }
    Harness scaleChain{scale, scaleSources, 128};
    const auto scaleOutput = scaleChain.render(1024);
    check(scaleOutput.first[0] > 0.31F && scaleOutput.first[0] < 0.33F &&
              scaleChain.engine.meterSnapshot().busCount == 8,
          "32 tracks and 8 chained buses must remain correct across subblocks");

    std::cout << "All bus-to-bus routing DAG tests passed\n";
    return EXIT_SUCCESS;
}
