#include "vitadaw/audio/PreparedProcessingPlan.h"
#include "vitadaw/audio/RealtimeAudioEngine.h"
#include "vitadaw/routing/RoutingState.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
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

bool close(float actual, float expected, float tolerance = 3.0e-5F) {
    return std::abs(actual - expected) <= tolerance;
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

vitadaw::audio::ProcessingPlanSendSpecification trackSend(
    std::uint64_t id, std::uint64_t track, std::uint64_t destination,
    vitadaw::routing::SendTapPoint tap,
    vitadaw::mixer::SendMixState mix = {}) {
    return {{id}, vitadaw::tracks::TrackId{track}, {destination}, tap,
            vitadaw::mixer::prepare(mix)};
}

vitadaw::audio::ProcessingPlanSendSpecification busSend(
    std::uint64_t id, std::uint64_t source, std::uint64_t destination,
    vitadaw::routing::SendTapPoint tap,
    vitadaw::mixer::SendMixState mix = {}) {
    return {{id}, vitadaw::routing::BusId{source}, {destination}, tap,
            vitadaw::mixer::prepare(mix)};
}

vitadaw::audio::PreparedTrackView source(
    vitadaw::tracks::TrackId id, const std::vector<float>& samples,
    double rate = 48000.0) {
    return {id, {{samples.data(), samples.data()}}, 2,
            {static_cast<std::uint64_t>(samples.size())},
            vitadaw::timeline::SampleRate{rate}, {0},
            {static_cast<std::int64_t>(samples.size())}, {0}, {}};
}

vitadaw::audio::ProcessingPlanSpecification basic(double rate = 48000.0) {
    vitadaw::audio::ProcessingPlanSpecification result;
    result.projectSampleRate = vitadaw::timeline::SampleRate{rate};
    return result;
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
        std::array<float*, 0> noChannels{};
        engine.processBlock({noChannels.data(), 0, 0},
                            vitadaw::timeline::SampleRate{rate});
        check(engine.tryRequestPlay().accepted,
              "prepared Bus Send project should play");
    }

    std::pair<std::vector<float>, std::vector<float>> render(
        std::size_t frames) {
        std::pair<std::vector<float>, std::vector<float>> output{
            std::vector<float>(frames), std::vector<float>(frames)};
        std::array<float*, 2> channels{output.first.data(), output.second.data()};
        realtimeAllocations.store(0, std::memory_order_relaxed);
        observeRealtimeAllocations.store(true, std::memory_order_release);
        engine.processBlock({channels.data(), channels.size(), frames},
                            vitadaw::timeline::SampleRate{rate});
        observeRealtimeAllocations.store(false, std::memory_order_release);
        check(realtimeAllocations.load(std::memory_order_acquire) == 0,
              "Bus Send processBlock must not allocate");
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
    using namespace vitadaw;
    constexpr auto pre = routing::SendTapPoint::preFaderPrePan;
    constexpr auto post = routing::SendTapPoint::postFaderPostPan;
    const std::vector<float> ones(8192, 1.0F);
    const std::vector<float> halves(8192, 0.5F);
    const std::array oneSource{source({1}, ones)};

    routing::RoutingState editable;
    const auto sourceBus = editable.addBus("Source");
    const auto firstAux = editable.addBus("Aux A");
    const auto secondAux = editable.addBus("Aux B");
    const auto firstSend = editable.addSend(sourceBus, firstAux, pre, {});
    const auto secondSend = editable.addSend(sourceBus, firstAux, post, {});
    check(firstSend == routing::SendId{1} && secondSend == routing::SendId{2} &&
              editable.setSendRoute(firstSend, secondAux, post) &&
              editable.findSend(firstSend)->destination == secondAux &&
              editable.findSend(firstSend)->tapPoint == post,
          "Bus Sends need stable identity and stopped structural retargeting");
    check(!editable.setSendRoute({999}, secondAux, post) &&
              !editable.setSendRoute(firstSend, {999}, post) &&
              !editable.setSendRoute(
                  firstSend, secondAux,
                  static_cast<routing::SendTapPoint>(255)),
          "retargeting must reject unknown ids, destinations and taps");

    auto identity = basic();
    identity.buses = {busSpec(30), busSpec(10, bus(20)), busSpec(20, bus(30))};
    identity.sends = {busSend(40, 20, 30, post),
                      busSend(10, 10, 30, pre),
                      busSend(20, 10, 20, post)};
    auto identityResult = audio::prepareProcessingPlan(identity, {}, 64);
    check(identityResult.success(), identityResult.errorMessage);
    const auto& identityPlan = identityResult.prepared->plan;
    check(identityPlan.sends.size() == 3 &&
              identityPlan.sends[0].id == routing::SendId{10} &&
              identityPlan.sends[1].id == routing::SendId{20} &&
              identityPlan.sends[2].id == routing::SendId{40} &&
              identityPlan.sends[0].sourceKind ==
                  audio::PreparedSendSourceKind::bus &&
              identityPlan.sends[0].sourceIndex == 0 &&
              identityPlan.sends[0].destinationBusIndex == 2 &&
              identityPlan.sends[0].destinationBufferIndex == 2 &&
              identityPlan.sends[0].audibilityIndex == 0 &&
              identityPlan.buses[0].preFaderSends ==
                  audio::PreparedSendRange{0, 1} &&
              identityPlan.buses[0].postFaderSends ==
                  audio::PreparedSendRange{1, 1},
          "Bus Send descriptors must resolve source, destination, RT and audibility indices");
    std::vector<std::uint64_t> orderedBusIds;
    for (const auto step : identityPlan.order) {
        if (step.kind == audio::ProcessingStepKind::bus) {
            orderedBusIds.push_back(identityPlan.buses[step.index].id.value);
        }
    }
    check(orderedBusIds == std::vector<std::uint64_t>({10, 20, 30}),
          "Bus Sends must participate in a stable topological order");

    auto tooManyForBus = basic();
    tooManyForBus.buses = {busSpec(1), busSpec(2)};
    for (std::uint64_t id = 1;
         id <= audio::maximumPreparedSendsPerBus + 1; ++id) {
        tooManyForBus.sends.push_back(busSend(id, 1, 2, pre));
    }
    check(!audio::prepareProcessingPlan(tooManyForBus, {}).success(),
          "per-bus send capacity must be validated before RT publication");

    const auto rejectedCycle = [pre](
                                   audio::ProcessingPlanSpecification value) {
        return !audio::prepareProcessingPlan(value, {}).success();
    };
    auto selfCycle = basic();
    selfCycle.buses = {busSpec(1)};
    selfCycle.sends = {busSend(1, 1, 1, pre)};
    check(rejectedCycle(selfCycle), "a Bus Send self-route must be rejected");
    auto outputSendCycle = basic();
    outputSendCycle.buses = {busSpec(1, bus(2)), busSpec(2)};
    outputSendCycle.sends = {busSend(1, 2, 1, post)};
    check(rejectedCycle(outputSendCycle),
          "a cycle combining main output and Bus Send must be rejected");
    outputSendCycle.sends[0].mix =
        mixer::prepare(mixer::SendMixState{{-100.0F}, true});
    check(rejectedCycle(outputSendCycle),
          "mute and -100 dB must not remove a structural dependency");
    auto longCycle = basic();
    longCycle.buses = {busSpec(1, bus(2)), busSpec(2, bus(3)), busSpec(3)};
    longCycle.sends = {busSend(1, 3, 1, pre)};
    check(rejectedCycle(longCycle), "an arbitrary long mixed cycle must fail");

    auto parallel = basic();
    parallel.buses = {busSpec(1, bus(2)), busSpec(2)};
    parallel.tracks = {{{1}, {}, bus(1)}};
    parallel.sends = {busSend(1, 1, 2, post)};
    auto parallelPrepared = audio::prepareProcessingPlan(
        parallel, oneSource, 64);
    check(parallelPrepared.success(), parallelPrepared.errorMessage);
    const auto sourceBusSteps = std::count_if(
        parallelPrepared.prepared->plan.order.begin(),
        parallelPrepared.prepared->plan.order.end(), [](const auto step) {
            return step.kind == audio::ProcessingStepKind::bus && step.index == 0;
        });
    check(sourceBusSteps == 1,
          "parallel output/send edges must not duplicate a bus processing step");
    Harness parallelHarness{parallel, oneSource};
    const auto parallelOutput = parallelHarness.render(1);
    const auto parallelMeters = parallelHarness.engine.meterSnapshot();
    check(close(parallelOutput.first[0], 2.0F) &&
              close(parallelMeters.buses[0].peak.left, 1.0F) &&
              close(parallelMeters.buses[1].peak.left, 2.0F),
          "parallel main and send branches must both sum after one bus render and meter update");

    mixer::BusMixState sinkMute;
    sinkMute.muted = true;
    mixer::BusMixState sourceMute;
    sourceMute.gain = {-6.0F};
    sourceMute.balance = {-1.0F};
    sourceMute.muted = true;
    auto isolatedPre = basic();
    isolatedPre.buses = {busSpec(1, bus(3), sourceMute), busSpec(2),
                          busSpec(3, {}, sinkMute)};
    isolatedPre.tracks = {{{1}, {}, bus(1)}};
    isolatedPre.sends = {busSend(1, 1, 2, pre)};
    Harness preHarness{isolatedPre, oneSource};
    const auto preOutput = preHarness.render(1);
    const auto preMeters = preHarness.engine.meterSnapshot();
    check(close(preOutput.first[0], 1.0F) && close(preOutput.second[0], 1.0F) &&
              preMeters.buses[0].peak == mixer::StereoPeak{},
          "Bus pre-send must ignore Bus Gain, Balance and Mute while the channel meter remains post-mute");
    sourceMute.gain = {-100.0F};
    sourceMute.muted = false;
    isolatedPre.buses[0].mix = mixer::prepare(sourceMute);
    Harness silentGainPre{isolatedPre, oneSource};
    check(close(silentGainPre.render(1).first[0], 1.0F),
          "Bus Gain -100 dB must leave a pre-send available");

    mixer::BusMixState postMix;
    postMix.gain = {-6.0F};
    postMix.balance = {-1.0F};
    auto isolatedPost = isolatedPre;
    isolatedPost.buses[0].mix = mixer::prepare(postMix);
    isolatedPost.sends[0].tapPoint = post;
    Harness postHarness{isolatedPost, oneSource};
    const auto postOutput = postHarness.render(1);
    const auto postMeters = postHarness.engine.meterSnapshot();
    const auto minusSix = std::pow(10.0F, -6.0F / 20.0F);
    check(close(postOutput.first[0], minusSix) &&
              postOutput.second[0] == 0.0F &&
              close(postMeters.buses[0].peak.left, minusSix),
          "Bus post-send must include Bus Gain and Balance before Send Level");
    postMix.muted = true;
    isolatedPost.buses[0].mix = mixer::prepare(postMix);
    Harness mutedPost{isolatedPost, oneSource};
    check(mutedPost.render(1).first[0] == 0.0F,
          "Bus Mute must close a post-send");
    postMix.muted = false;
    postMix.gain = {-100.0F};
    isolatedPost.buses[0].mix = mixer::prepare(postMix);
    Harness silentPost{isolatedPost, oneSource};
    check(silentPost.render(1).first[0] == 0.0F,
          "Bus Gain -100 dB must close a post-send");

    auto sendPolicy = isolatedPre;
    sendPolicy.buses[0].mix = {};
    sendPolicy.sends[0].mix =
        mixer::prepare(mixer::SendMixState{{-6.0F}, false});
    Harness levelHarness{sendPolicy, oneSource};
    check(close(levelHarness.render(1).first[0], minusSix),
          "Bus Send Level must apply after the selected tap");
    sendPolicy.sends[0].mix =
        mixer::prepare(mixer::SendMixState{{}, true});
    Harness sendMuteHarness{sendPolicy, oneSource};
    check(sendMuteHarness.render(1).first[0] == 0.0F,
          "Send Mute must close only the Bus Send branch");
    sendPolicy.sends[0].mix = {};
    sendPolicy.buses[1].mix = mixer::prepare(sinkMute);
    Harness auxMuteHarness{sendPolicy, oneSource};
    check(auxMuteHarness.render(1).first[0] == 0.0F,
          "downstream Aux Mute must silence a Bus Send");

    auto fanOut = basic();
    fanOut.buses = {busSpec(1, bus(4)), busSpec(2), busSpec(3),
                    busSpec(4, {}, sinkMute)};
    fanOut.tracks = {{{1}, {}, bus(1)}};
    fanOut.sends = {busSend(1, 1, 2, pre), busSend(2, 1, 3, pre),
                    busSend(3, 1, 2, pre)};
    Harness fanOutHarness{fanOut, oneSource};
    check(close(fanOutHarness.render(1).first[0], 3.0F),
          "one bus must feed several Auxes and parallel sends without reprocessing");

    auto convergence = basic();
    convergence.buses = {busSpec(1, bus(4)), busSpec(2, bus(4)), busSpec(3),
                          busSpec(4, {}, sinkMute)};
    convergence.tracks = {{{1}, {}, bus(1)}, {{2}, {}, bus(2)}};
    convergence.sends = {busSend(1, 1, 3, pre), busSend(2, 2, 3, pre)};
    const std::array convergenceSources{source({1}, ones), source({2}, halves)};
    Harness convergenceHarness{convergence, convergenceSources};
    check(close(convergenceHarness.render(1).first[0], 1.5F),
          "several buses must converge on the same Aux before it is processed");

    auto smooth = basic(1000.0);
    smooth.buses = {busSpec(1, bus(3)), busSpec(2),
                    busSpec(3, {}, sinkMute)};
    smooth.tracks = {{{1}, {}, bus(1)}};
    smooth.sends = {busSend(
        1, 1, 2, post, mixer::SendMixState{{-100.0F}, false})};
    const std::array smoothSource{source({1}, ones, 1000.0)};
    Harness smoothHarness{smooth, smoothSource, 2};
    check(smoothHarness.engine.tryUpdateSendMix(
              {1}, mixer::prepare(mixer::SendMixState{})),
          "Bus Send level target must enqueue by SendId");
    const auto rising = smoothHarness.render(2);
    check(close(rising.first[0], 0.2F) && close(rising.first[1], 0.4F),
          "each Bus Send owns one 5 ms smoother advanced once per sample");
    check(smoothHarness.engine.tryUpdateSendMix(
              {1}, mixer::prepare(mixer::SendMixState{{-100.0F}, false})),
          "Bus Send level must retarget during Play");
    const auto falling = smoothHarness.render(5);
    check(close(falling.first[0], 0.32F) && falling.first[4] == 0.0F,
          "Bus Send retargeting must start at its instantaneous value");

    auto busSmooth = basic(1000.0);
    busSmooth.buses = {
        busSpec(1, bus(4), mixer::BusMixState{{-100.0F}, {}, false, false}),
        busSpec(2), busSpec(3), busSpec(4, {}, sinkMute)};
    busSmooth.tracks = {{{1}, {}, bus(1)}};
    busSmooth.sends = {busSend(1, 1, 2, post), busSend(2, 1, 3, post)};
    Harness busSmoothHarness{busSmooth, smoothSource, 2};
    check(busSmoothHarness.engine.tryUpdateBusMix(
              {1}, mixer::prepare(mixer::BusMixState{}),
              audio::fullyAudibleState()),
          "Bus Gain target should enqueue");
    const auto firstBusRampFrame = busSmoothHarness.render(1);
    const auto firstBusRampMeters = busSmoothHarness.engine.meterSnapshot();
    const auto remainingBusRamp = busSmoothHarness.render(4);
    check(close(firstBusRampFrame.first[0], 0.4F) &&
              close(firstBusRampMeters.buses[0].peak.left, 0.2F) &&
              close(remainingBusRamp.first[3], 2.0F),
          "multiple Bus Sends must not advance Bus DSP or update its meter more than once");

    mixer::BusMixState unrelatedSolo;
    unrelatedSolo.solo = true;
    auto closedSmooth = smooth;
    closedSmooth.buses.push_back(busSpec(4, {}, unrelatedSolo));
    Harness closedHarness{closedSmooth, smoothSource, 2};
    check(closedHarness.engine.tryUpdateSendMix(
              {1}, mixer::prepare(mixer::SendMixState{})),
          "an inaudible Bus Send should accept a level target");
    static_cast<void>(closedHarness.render(5));
    unrelatedSolo.solo = false;
    check(closedHarness.engine.tryUpdateBusMix(
              {4}, mixer::prepare(unrelatedSolo),
              audio::fullyAudibleState()),
          "audibility should reopen without rebuilding the plan");
    check(close(closedHarness.render(1).first[0], 1.0F),
          "an inaudible Bus Send smoother must continue advancing");

    mixer::TrackMixState trackSolo;
    trackSolo.solo = true;
    auto soloGraph = basic();
    soloGraph.buses = {busSpec(1, bus(2)), busSpec(2), busSpec(3), busSpec(4)};
    soloGraph.tracks = {{{1}, mixer::prepare(trackSolo), bus(1)}};
    soloGraph.sends = {busSend(1, 1, 3, pre), busSend(2, 2, 4, pre)};
    Harness trackSoloHarness{soloGraph, oneSource};
    check(close(trackSoloHarness.render(1).first[0], 1.0F),
          "a bus used only as downstream transport must not open lateral Bus Sends");

    mixer::BusMixState busSolo;
    busSolo.solo = true;
    soloGraph.tracks[0].mix = {};
    soloGraph.buses[0].mix = mixer::prepare(busSolo);
    Harness busSoloHarness{soloGraph, oneSource};
    check(close(busSoloHarness.render(1).first[0], 2.0F),
          "an explicitly soloed bus must open its own main and send branches only");

    auto plateSolo = basic();
    plateSolo.buses = {busSpec(1, bus(2)), busSpec(2),
                       busSpec(3, {}, busSolo)};
    plateSolo.tracks = {{{1}, {}, bus(1)}, {{2}, {}, bus(2)}};
    plateSolo.sends = {busSend(1, 1, 3, pre), trackSend(2, 2, 3, pre)};
    Harness plateHarness{plateSolo, convergenceSources};
    check(close(plateHarness.render(1).first[0], 1.5F),
          "Aux Solo fed by Track and Bus Sends must be wet-only and include all incoming edges");
    plateSolo.tracks[0].mix = mixer::prepare(trackSolo);
    Harness combinedSoloHarness{plateSolo, convergenceSources};
    check(close(combinedSoloHarness.render(1).first[0], 2.5F),
          "multiple Solo selections must form a coherent union at convergences");

    auto upstreamSolo = basic();
    upstreamSolo.buses = {busSpec(1, {}, busSolo), busSpec(2, bus(3)),
                          busSpec(3), busSpec(4)};
    upstreamSolo.tracks = {{{1}, {}, bus(1)}};
    upstreamSolo.sends = {busSend(1, 1, 2, pre), busSend(2, 2, 4, pre)};
    Harness upstreamSoloHarness{upstreamSolo, oneSource};
    check(close(upstreamSoloHarness.render(1).first[0], 2.0F),
          "upstream Bus Solo must cross its send and downstream buses without opening sibling sends");

    for (const auto callbackSize : {64U, 128U, 256U, 512U, 1024U}) {
        Harness callbackHarness{parallel, oneSource, 128};
        const auto output = callbackHarness.render(callbackSize);
        const auto snapshot = callbackHarness.engine.transportSnapshot();
        const auto meters = callbackHarness.engine.meterSnapshot();
        check(close(output.first.front(), 2.0F) &&
                  close(output.first.back(), 2.0F) &&
                  snapshot.position.value == callbackSize &&
                  close(meters.master.left, 2.0F),
              "clock, Bus Send smoothing, meters and accumulators must cross subblocks continuously");
    }

    auto scale = basic();
    for (std::uint64_t busId = 1; busId <= 8; ++busId) {
        scale.buses.push_back(busSpec(busId));
    }
    std::vector<std::vector<float>> scaleSamples(
        32, std::vector<float>(2048, 0.001F));
    std::vector<audio::PreparedTrackView> scaleSources;
    std::uint64_t nextSend{1};
    for (std::uint64_t trackId = 1; trackId <= 32; ++trackId) {
        scale.tracks.push_back(
            {{trackId}, {}, routing::OutputDestination::master()});
        scaleSources.push_back(source({trackId}, scaleSamples[trackId - 1]));
        for (std::uint64_t destination = 1; destination <= 4; ++destination) {
            scale.sends.push_back(
                trackSend(nextSend++, trackId, destination, pre));
        }
    }
    for (std::uint64_t sourceId = 1; sourceId <= 4; ++sourceId) {
        for (std::uint64_t sendIndex = 0; sendIndex < 32; ++sendIndex) {
            scale.sends.push_back(busSend(
                nextSend++, sourceId, 5 + (sendIndex % 4), pre));
        }
    }
    Harness scaleHarness{scale, scaleSources, 128};
    const auto scaleOutput = scaleHarness.render(1024);
    check(scaleHarness.prepared->plan.sends.size() == 256 &&
              scaleHarness.prepared->runtime.sendMix.size() == 256 &&
              close(scaleOutput.first[0], 4.256F, 3.0e-3F),
          "32 tracks, 8 buses, Track Sends and Bus Sends must render 256 branches without RT allocation");

    std::cout << "All Bus Sends tests passed\n";
    return EXIT_SUCCESS;
}
