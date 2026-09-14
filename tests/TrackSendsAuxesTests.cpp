#include "vitadaw/audio/PreparedProcessingPlan.h"
#include "vitadaw/audio/RealtimeAudioEngine.h"
#include "vitadaw/routing/RoutingState.h"

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

vitadaw::audio::PreparedTrackView source(
    vitadaw::tracks::TrackId id, const std::vector<float>& samples,
    bool mono = false, double rate = 48000.0) {
    return {id, {{samples.data(), mono ? nullptr : samples.data()}},
            mono ? 1U : 2U,
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

vitadaw::audio::ProcessingPlanSendSpecification sendSpec(
    std::uint64_t id, std::uint64_t track, std::uint64_t destination,
    vitadaw::routing::SendTapPoint tap,
    vitadaw::mixer::SendMixState mix = {}) {
    return {{id}, vitadaw::tracks::TrackId{track}, {destination}, tap,
            vitadaw::mixer::prepare(mix)};
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
        std::array<float*, 0> none{};
        engine.processBlock({none.data(), 0, 0},
                            vitadaw::timeline::SampleRate{rate});
        check(engine.tryRequestPlay().accepted, "prepared send project should play");
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
              "Track Send processBlock must not allocate");
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
    const std::vector<float> ones(4096, 1.0F);
    const std::vector<float> halves(4096, 0.5F);
    check(!mixer::SendMixState{
               {std::numeric_limits<float>::quiet_NaN()}, false}.isValid() &&
              !mixer::SendMixState{
                  {std::numeric_limits<float>::infinity()}, false}.isValid() &&
              !mixer::SendMixState{{13.0F}, false}.isValid() &&
              !mixer::SendMixState{{-100.1F}, false}.isValid(),
          "send level must reject NaN, infinity and values outside -100..+12 dB");

    routing::RoutingState routingState;
    routingState.addTrack({1});
    const auto aux = routingState.addBus("Aux");
    const auto firstId = routingState.addSend(
        tracks::TrackId{1}, aux, pre, {});
    const auto duplicateId = routingState.addSend(
        tracks::TrackId{1}, aux, pre, {});
    check(firstId == routing::SendId{1} && duplicateId == routing::SendId{2} &&
              routingState.sends().size() == 2,
          "parallel sends need distinct stable identities");
    check(routingState.removeSend(firstId), "an existing send should be removable");
    const auto afterRemoval = routingState.addSend(
        tracks::TrackId{1}, aux, post, {});
    check(afterRemoval == routing::SendId{3},
          "removed SendIds must never be reused");

    auto identity = basic();
    identity.buses = {busSpec(20), busSpec(10)};
    identity.tracks = {{{2}, {}, bus(20)}, {{1}, {}, bus(10)}};
    identity.sends = {sendSpec(30, 2, 10, post), sendSpec(20, 1, 20, post),
                      sendSpec(10, 1, 20, pre)};
    const std::array identitySources{source({2}, halves), source({1}, ones)};
    auto identityPlan = audio::prepareProcessingPlan(identity, identitySources, 64);
    check(identityPlan.success(), identityPlan.errorMessage);
    const auto& preparedIdentity = identityPlan.prepared->plan;
    check(preparedIdentity.sends.size() == 3 &&
              preparedIdentity.sends[0].id == routing::SendId{10} &&
              preparedIdentity.sends[1].id == routing::SendId{20} &&
              preparedIdentity.sends[2].id == routing::SendId{30} &&
              preparedIdentity.tracks[0].preFaderSends.first == 0 &&
              preparedIdentity.tracks[0].preFaderSends.count == 1 &&
              preparedIdentity.tracks[0].postFaderSends.first == 1 &&
              preparedIdentity.tracks[0].postFaderSends.count == 1 &&
              preparedIdentity.sendIndexById[0].id == routing::SendId{10} &&
              preparedIdentity.sendIndexById[2].denseIndex == 2,
          "SendId, editable order, dense index, ranges and destinations must stay distinct");

    auto invalid = identity;
    invalid.sends.push_back(sendSpec(10, 2, 10, pre));
    check(!audio::prepareProcessingPlan(invalid, identitySources).success(),
          "duplicate SendId must be rejected");
    invalid = identity;
    invalid.sends[0].destination = {999};
    check(!audio::prepareProcessingPlan(invalid, identitySources).success(),
          "unknown send destination must be rejected");
    invalid = identity;
    invalid.sends[0].tapPoint = static_cast<routing::SendTapPoint>(255);
    check(!audio::prepareProcessingPlan(invalid, identitySources).success(),
          "unknown send tap must be rejected");
    auto tooManyForTrack = basic();
    tooManyForTrack.buses = {busSpec(1)};
    tooManyForTrack.tracks = {{{1}, {}, bus(1)}};
    for (std::uint64_t id = 1;
         id <= audio::maximumPreparedSendsPerTrack + 1; ++id) {
        tooManyForTrack.sends.push_back(sendSpec(id, 1, 1, pre));
    }
    const std::array oneSource{source({1}, ones)};
    check(!audio::prepareProcessingPlan(tooManyForTrack, oneSource).success(),
          "per-track send capacity must be enforced");
    auto tooManyTotal = basic();
    tooManyTotal.buses = {busSpec(1)};
    tooManyTotal.sends.resize(audio::maximumPreparedSends + 1);
    check(!audio::prepareProcessingPlan(tooManyTotal, {}).success(),
          "total prepared send capacity must be enforced before allocation");
    auto memory = identity;
    const auto withoutSends = [&] {
        auto value = identity;
        value.sends.clear();
        return audio::prepareProcessingPlan(value, identitySources, 512);
    }();
    const auto withSends = audio::prepareProcessingPlan(
        identity, identitySources, 512);
    check(withoutSends.success() && withSends.success() &&
              withSends.prepared->plan.runtimeMemoryBytes >
                  withoutSends.prepared->plan.runtimeMemoryBytes,
          "prepared memory accounting must include send descriptors and runtime");
    check(!audio::prepareProcessingPlan(
               memory, identitySources, 512,
               withoutSends.prepared->plan.runtimeMemoryBytes).success(),
          "memory budget must reject send state that does not fit");

    auto sendCycle = basic();
    sendCycle.buses = {busSpec(1, bus(2)), busSpec(2)};
    sendCycle.sends = {{{9}, routing::BusId{2}, {1}, post,
                        mixer::prepare(mixer::SendMixState{{-100.0F}, true})}};
    const auto cycleResult = audio::prepareProcessingPlan(sendCycle, {});
    check(!cycleResult.success() &&
              cycleResult.errorMessage.find("Routing rejected") != std::string::npos,
          "a muted silent Bus Send must still close a structural cycle");
    auto validFutureBusSend = basic();
    validFutureBusSend.buses = {busSpec(1), busSpec(2)};
    validFutureBusSend.sends = {{{1}, routing::BusId{1}, {2}, post, {}}};
    const auto futureResult = audio::prepareProcessingPlan(validFutureBusSend, {});
    check(futureResult.success() &&
              futureResult.prepared->plan.sends[0].sourceKind ==
                  audio::PreparedSendSourceKind::bus &&
              futureResult.prepared->plan.buses[0].postFaderSends.count == 1,
          "an acyclic Bus Send must prepare as a bus-owned post range");

    mixer::TrackMixState silencedTrack{{-100.0F}, {1.0F}, true, false};
    mixer::BusMixState mutedBus;
    mutedBus.muted = true;
    auto prePolicy = basic();
    prePolicy.buses = {busSpec(1, {}, mutedBus), busSpec(2)};
    prePolicy.tracks = {{{1}, mixer::prepare(silencedTrack), bus(1)}};
    prePolicy.sends = {sendSpec(1, 1, 2, pre)};
    const std::array monoSource{source({1}, ones, true)};
    Harness preHarness{prePolicy, monoSource};
    const auto preOutput = preHarness.render(1);
    const auto preMeters = preHarness.engine.meterSnapshot();
    check(close(preOutput.first[0], 0.70710678F) &&
              close(preOutput.second[0], 0.70710678F) &&
              preMeters.tracks[0].peak == mixer::StereoPeak{},
          "mono pre-tap must centre once and survive track gain, pan and mute");

    mixer::TrackMixState postTrack{{-6.0F}, {-1.0F}, false, false};
    auto postPolicy = prePolicy;
    postPolicy.tracks[0].mix = mixer::prepare(postTrack);
    postPolicy.sends = {sendSpec(1, 1, 2, post)};
    Harness postHarness{postPolicy, monoSource};
    const auto postOutput = postHarness.render(1);
    check(close(postOutput.first[0], std::pow(10.0F, -6.0F / 20.0F)) &&
              postOutput.second[0] == 0.0F,
          "post-send must follow track gain, pan and the original mono pan law");
    postPolicy.tracks[0].mix = mixer::prepare(
        mixer::TrackMixState{{}, {}, true, false});
    Harness mutedPost{postPolicy, monoSource};
    check(mutedPost.render(1).first[0] == 0.0F,
          "Track Mute must close a post-send");
    postPolicy.tracks[0].mix = mixer::prepare(
        mixer::TrackMixState{{-100.0F}, {}, false, false});
    Harness silentFaderPost{postPolicy, monoSource};
    check(silentFaderPost.render(1).first[0] == 0.0F,
          "a Track Gain at exact silence must close a post-send");

    auto duplicate = basic();
    duplicate.buses = {busSpec(1, {}, mutedBus), busSpec(2)};
    duplicate.tracks = {{{1}, {}, bus(1)}};
    duplicate.sends = {
        sendSpec(1, 1, 2, pre),
        sendSpec(2, 1, 2, pre, mixer::SendMixState{{-6.0F}, false})};
    Harness duplicateHarness{duplicate, oneSource};
    check(close(duplicateHarness.render(1).first[0],
                1.0F + std::pow(10.0F, -6.0F / 20.0F)),
          "parallel sends to one Aux must sum with independent levels");
    duplicate.sends[0].mix = mixer::prepare(mixer::SendMixState{{}, true});
    Harness sendMuted{duplicate, oneSource};
    check(close(sendMuted.render(1).first[0],
                std::pow(10.0F, -6.0F / 20.0F)),
          "Send Mute must close only its own branch");
    duplicate.buses[1].mix = mixer::prepare(mutedBus);
    Harness auxMuted{duplicate, oneSource};
    check(auxMuted.render(1).first[0] == 0.0F,
          "Aux Mute must silence accumulated sends downstream");

    auto downstreamMuted = basic();
    downstreamMuted.buses = {busSpec(1, bus(2)), busSpec(2, {}, mutedBus)};
    downstreamMuted.tracks = {
        {{1}, mixer::prepare(silencedTrack),
         routing::OutputDestination::master()}};
    downstreamMuted.sends = {sendSpec(1, 1, 1, pre)};
    Harness downstreamMutedHarness{downstreamMuted, oneSource};
    const auto downstreamMutedOutput = downstreamMutedHarness.render(1);
    const auto downstreamMeters = downstreamMutedHarness.engine.meterSnapshot();
    check(downstreamMutedOutput.first[0] == 0.0F &&
              close(downstreamMeters.buses[0].peak.left, 1.0F) &&
              downstreamMeters.buses[1].peak == mixer::StereoPeak{},
          "downstream Bus Mute must close the chain without erasing the upstream Aux contribution");

    auto severalDestinations = basic();
    severalDestinations.buses = {busSpec(1), busSpec(2)};
    severalDestinations.tracks = {
        {{1}, mixer::prepare(silencedTrack),
         routing::OutputDestination::master()}};
    severalDestinations.sends = {sendSpec(1, 1, 1, pre),
                                 sendSpec(2, 1, 2, pre)};
    Harness severalDestinationsHarness{severalDestinations, oneSource};
    check(close(severalDestinationsHarness.render(1).first[0], 2.0F),
          "one source must feed several Aux buses without an extra render");

    auto soloGraph = basic();
    mixer::BusMixState drumSolo;
    drumSolo.solo = true;
    soloGraph.buses = {busSpec(1, bus(2)), busSpec(2), busSpec(3)};
    mixer::TrackMixState snareSolo;
    snareSolo.solo = true;
    soloGraph.tracks = {{{1}, mixer::prepare(snareSolo), bus(1)},
                        {{2}, {}, bus(1)}};
    soloGraph.sends = {sendSpec(1, 1, 3, pre), sendSpec(2, 2, 3, pre)};
    const std::array soloSources{source({1}, ones), source({2}, halves)};
    Harness snareSoloHarness{soloGraph, soloSources};
    check(close(snareSoloHarness.render(1).first[0], 2.0F),
          "Track Solo must preserve dry output and its auxiliary send");

    soloGraph.tracks[0].mix = {};
    soloGraph.buses[0].mix = mixer::prepare(drumSolo);
    Harness drumSoloHarness{soloGraph, soloSources};
    check(close(drumSoloHarness.render(1).first[0], 1.5F),
          "Drum Bus Solo must not open lateral track sends");

    soloGraph.buses[0].mix = {};
    soloGraph.buses[2].mix = mixer::prepare(drumSolo);
    Harness plateSoloHarness{soloGraph, soloSources};
    check(close(plateSoloHarness.render(1).first[0], 1.5F),
          "Aux Solo must be wet-only and include every send that feeds it");

    soloGraph.tracks[0].mix = mixer::prepare(snareSolo);
    Harness snareAndPlate{soloGraph, soloSources};
    check(close(snareAndPlate.render(1).first[0], 2.5F),
          "Track Solo and Aux Solo permissions must form a union");

    auto twoTrackSolo = basic();
    twoTrackSolo.buses = {busSpec(1)};
    twoTrackSolo.tracks = {
        {{1}, mixer::prepare(snareSolo), routing::OutputDestination::master()},
        {{2}, mixer::prepare(snareSolo), routing::OutputDestination::master()}};
    twoTrackSolo.sends = {sendSpec(1, 1, 1, pre), sendSpec(2, 2, 1, pre)};
    Harness twoSoloHarness{twoTrackSolo, soloSources};
    check(close(twoSoloHarness.render(1).first[0], 3.0F),
          "two Track Solos sharing an Aux must sum main and wet paths once");

    auto smooth = basic(1000.0);
    smooth.buses = {busSpec(1, {}, mutedBus), busSpec(2)};
    smooth.tracks = {{{1}, {}, bus(1)}};
    smooth.sends = {sendSpec(1, 1, 2, post,
                             mixer::SendMixState{{-100.0F}, false})};
    const std::array smoothSource{source({1}, ones, false, 1000.0)};
    Harness smoothHarness{smooth, smoothSource, 2};
    check(smoothHarness.engine.tryUpdateSendMix(
              {1}, mixer::prepare(mixer::SendMixState{})),
          "Send level target must enqueue by stable SendId");
    const auto rising = smoothHarness.render(2);
    check(close(rising.first[0], 0.2F) && close(rising.first[1], 0.4F),
          "send smoothing must advance once per sample across subblocks");
    check(smoothHarness.engine.tryUpdateSendMix(
              {1}, mixer::prepare(mixer::SendMixState{{-100.0F}, false})),
          "send smoother must accept retargeting while active");
    const auto falling = smoothHarness.render(5);
    check(close(falling.first[0], 0.32F) && falling.first[4] == 0.0F,
          "retargeting must start from the instantaneous send level");

    auto closedSmooth = smooth;
    closedSmooth.tracks.push_back(
        {{2}, mixer::prepare(snareSolo), bus(1)});
    const std::array closedSources{source({1}, ones, false, 1000.0),
                                   source({2}, halves, false, 1000.0)};
    Harness closedHarness{closedSmooth, closedSources, 2};
    check(closedHarness.engine.tryUpdateSendMix(
              {1}, mixer::prepare(mixer::SendMixState{})),
          "inaudible send should accept a new target");
    static_cast<void>(closedHarness.render(5));
    auto noSolo = snareSolo;
    noSolo.solo = false;
    check(closedHarness.engine.tryUpdateTrackMix(
              {2}, mixer::prepare(noSolo), audio::fullyAudibleState()),
          "opening the send after its hidden ramp should enqueue");
    check(close(closedHarness.render(1).first[0], 1.0F),
          "an inaudible send smoother must keep advancing continuously");

    auto trackSmooth = basic(1000.0);
    trackSmooth.buses = {busSpec(1), busSpec(2)};
    trackSmooth.tracks = {{{1}, mixer::prepare(
        mixer::TrackMixState{{-100.0F}, {}, false, false}),
        routing::OutputDestination::master()}};
    trackSmooth.sends = {sendSpec(1, 1, 1, post),
                         sendSpec(2, 1, 2, post)};
    Harness trackSmoothHarness{trackSmooth, smoothSource, 2};
    check(trackSmoothHarness.engine.tryUpdateTrackMix(
              {1}, mixer::prepare(mixer::TrackMixState{}),
              audio::fullyAudibleState()),
          "track smoothing target should enqueue");
    const auto trackRamp = trackSmoothHarness.render(5);
    check(close(trackRamp.first[0], 0.6F) && close(trackRamp.first[4], 3.0F),
          "multiple sends must not advance a Track smoother more than once");

    for (const auto callbackSize : {64U, 128U, 256U, 512U, 1024U}) {
        Harness callbackHarness{duplicate, oneSource, 128};
        const auto output = callbackHarness.render(callbackSize);
        const auto snapshot = callbackHarness.engine.transportSnapshot();
        const auto meters = callbackHarness.engine.meterSnapshot();
        check(close(output.first.front(), output.first.back()) &&
                  snapshot.position.value == callbackSize &&
                  close(meters.master.left, output.first.front()),
              "clock, send signal and meters must remain continuous across every callback size");
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
        for (std::uint64_t send = 0; send < 4; ++send) {
            scale.sends.push_back(sendSpec(nextSend++, trackId, send + 1, pre));
        }
    }
    Harness scaleHarness{scale, scaleSources, 128};
    const auto scaleOutput = scaleHarness.render(1024);
    check(close(scaleOutput.first[0], 0.16F, 2.0e-4F) &&
              scaleHarness.prepared->runtime.sendMix.size() == 128,
          "32 tracks, 8 buses and 128 sends must render without RT allocation");

    std::cout << "All Track Sends & Auxes tests passed\n";
    return EXIT_SUCCESS;
}
