#include "vitadaw/audio/PreparedProcessingPlan.h"
#include "vitadaw/audio/RealtimeAudioEngine.h"

#include <array>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <new>
#include <span>
#include <string_view>
#include <vector>

namespace {

std::atomic<bool> observeAllocations{};
std::atomic<std::size_t> realtimeAllocations{};

void check(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

bool close(float actual, float expected, float tolerance = 1.0e-5F) {
    return std::abs(actual - expected) <= tolerance;
}

vitadaw::audio::PreparedTrackView mono(
    vitadaw::tracks::TrackId id, const std::vector<float>& samples,
    double sourceRate, double projectRate,
    vitadaw::mixer::PreparedTrackMixState mix = {}) {
    const auto frames = vitadaw::timeline::SourceFrameCount{
        static_cast<std::uint64_t>(samples.size())};
    return {id, {{samples.data(), nullptr}}, 1, frames,
            vitadaw::timeline::SampleRate{sourceRate}, {0},
            vitadaw::timeline::sourceFramesToProjectDuration(
                frames, vitadaw::timeline::SampleRate{sourceRate},
                vitadaw::timeline::SampleRate{projectRate}),
            {0}, mix};
}

vitadaw::audio::PreparedTrackView stereo(
    vitadaw::tracks::TrackId id, const std::vector<float>& samples,
    double rate, vitadaw::mixer::PreparedTrackMixState mix = {}) {
    return {id, {{samples.data(), samples.data()}}, 2,
            {static_cast<std::uint64_t>(samples.size())},
            vitadaw::timeline::SampleRate{rate}, {0},
            {static_cast<std::int64_t>(samples.size())}, {0}, mix};
}

vitadaw::audio::ProcessingPlanSpecification specification(
    std::span<const vitadaw::tracks::TrackId> tracks,
    std::span<const vitadaw::routing::AudioBus> buses,
    std::span<const vitadaw::routing::TrackOutputDestination> destinations,
    double rate = 48000.0) {
    vitadaw::audio::ProcessingPlanSpecification result;
    result.projectSampleRate = vitadaw::timeline::SampleRate{rate};
    for (const auto& bus : buses) {
        result.buses.push_back({bus.id, vitadaw::mixer::prepare(bus.mix)});
    }
    for (std::size_t index = 0; index < tracks.size(); ++index) {
        result.tracks.push_back(
            {tracks[index], {}, destinations[index]});
    }
    return result;
}

struct RenderResult {
    std::vector<float> left;
    std::vector<float> right;
    vitadaw::mixer::MeterSnapshot meters;
    vitadaw::audio::RealtimeTransportSnapshot transport;
};

RenderResult render(
    vitadaw::audio::ProcessingPlanPreparationResult preparation,
    std::size_t frameCount, double deviceRate,
    const vitadaw::mixer::PreparedTrackMixState* update = nullptr) {
    using namespace vitadaw;
    check(preparation.success(), preparation.errorMessage);
    audio::RealtimeAudioEngine engine;
    engine.configure(preparation.prepared->plan,
                     preparation.prepared->runtime);
    engine.deviceInitialising();
    std::array<float*, 0> noChannels{};
    engine.processBlock({noChannels.data(), 0, 0},
                        timeline::SampleRate{deviceRate});
    check(engine.tryRequestPlay().accepted, "prepared routed project must play");
    if (update != nullptr) {
        check(engine.tryUpdateTrackMix({1}, *update,
                                       audio::fullyAudibleState()),
              "routed track mixer update must enqueue by resolved identity");
    }
    RenderResult result;
    result.left.resize(frameCount);
    result.right.resize(frameCount);
    std::array<float*, 2> channels{result.left.data(), result.right.data()};
    realtimeAllocations.store(0, std::memory_order_relaxed);
    observeAllocations.store(true, std::memory_order_release);
    engine.processBlock({channels.data(), channels.size(), frameCount},
                        timeline::SampleRate{deviceRate});
    observeAllocations.store(false, std::memory_order_release);
    check(realtimeAllocations.load(std::memory_order_acquire) == 0,
          "routed processBlock must not allocate");
    result.meters = engine.meterSnapshot();
    result.transport = engine.transportSnapshot();
    return result;
}

} // namespace

void* operator new(std::size_t size) {
    if (observeAllocations.load(std::memory_order_acquire)) {
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

    const std::array<tracks::TrackId, 0> noTracks{};
    const std::array<routing::AudioBus, 0> noBuses{};
    const std::array<routing::TrackOutputDestination, 0> noDestinations{};
    auto empty = audio::prepareProcessingPlan(
        specification(noTracks, noBuses, noDestinations), {});
    check(empty.success() && empty.prepared->plan.buses.empty() &&
              empty.prepared->plan.tracks.empty(),
          "a project without WAV or buses must prepare");

    const std::array buses{routing::AudioBus{{1}, "Bus A"},
                           routing::AudioBus{{2}, "Bus B"}};
    auto emptyBuses = audio::prepareProcessingPlan(
        specification(noTracks, buses, noDestinations), {});
    check(emptyBuses.success() && emptyBuses.prepared->plan.order.size() == 3,
          "multiple empty buses must prepare before master");

    auto invalidBusList = buses;
    invalidBusList[0].id = {};
    check(!audio::prepareProcessingPlan(
               specification(noTracks, invalidBusList, noDestinations), {})
               .success(),
          "an invalid BusId must be rejected");
    const std::array oneId{tracks::TrackId{1}};
    const std::array missingDestination{
        routing::TrackOutputDestination::toBus({99})};
    check(!audio::prepareProcessingPlan(
               specification(oneId, buses, missingDestination), {})
               .success(),
          "a missing bus destination must be rejected");
    check(!audio::prepareProcessingPlan(
               specification(noTracks, buses, noDestinations), {}, 512, 1)
               .success(),
          "runtime buffers must respect the memory budget");
    std::vector<routing::AudioBus> tooManyBuses;
    for (std::size_t index = 0; index <= audio::maximumPreparedBuses; ++index) {
        tooManyBuses.push_back({{index + 1}, "Bus"});
    }
    check(!audio::prepareProcessingPlan(
               specification(noTracks, tooManyBuses, noDestinations), {})
               .success(),
          "the prepared bus limit must be enforced outside RT");
    std::vector<tracks::TrackId> tooManyTracks;
    std::vector<routing::TrackOutputDestination> tooManyDestinations;
    for (std::size_t index = 0; index <= audio::maximumPreparedTracks; ++index) {
        tooManyTracks.push_back({index + 1});
        tooManyDestinations.push_back(
            routing::TrackOutputDestination::master());
    }
    check(!audio::prepareProcessingPlan(
               specification(tooManyTracks, noBuses, tooManyDestinations), {})
               .success(),
          "the prepared track limit must be enforced outside RT");

    const std::vector<float> signal(12, 0.5F);
    const std::array source{stereo({1}, signal, 48000.0)};
    const std::array directDestination{
        routing::TrackOutputDestination::master()};
    auto invalidFormat = source;
    invalidFormat[0].channelCount = 3;
    check(!audio::prepareProcessingPlan(
               specification(oneId, noBuses, directDestination),
               invalidFormat)
               .success(),
          "non mono/stereo source formats must be rejected during preparation");
    auto overflowingClip = source;
    overflowingClip[0].clipStart = {
        std::numeric_limits<std::int64_t>::max() - 1};
    overflowingClip[0].clipDuration = {2};
    check(!audio::prepareProcessingPlan(
               specification(oneId, noBuses, directDestination),
               overflowingClip)
               .success(),
          "prepared clip ends must not overflow project time");
    const std::array busDestination{
        routing::TrackOutputDestination::toBus({1})};
    const std::array busA{buses[0]};
    const auto direct = render(
        audio::prepareProcessingPlan(
            specification(oneId, noBuses, directDestination), source, 4),
        signal.size(), 48000.0);
    const auto throughBus = render(
        audio::prepareProcessingPlan(
            specification(oneId, busA, busDestination), source, 4),
        signal.size(), 48000.0);
    check(direct.left == throughBus.left && direct.right == throughBus.right &&
              throughBus.meters.busCount == 1 &&
              throughBus.meters.buses[0].bus == routing::BusId{1} &&
              close(throughBus.meters.buses[0].peak.left, 0.5F),
          "Track to Master and Track to unity Bus to Master must be equivalent");

    const std::vector<float> half(12, 0.25F);
    const std::array twoIds{tracks::TrackId{1}, tracks::TrackId{2}};
    const std::array twoSources{stereo({1}, signal, 48000.0),
                                stereo({2}, half, 48000.0)};
    const std::array sameBus{busDestination[0], busDestination[0]};
    const auto summed = render(
        audio::prepareProcessingPlan(
            specification(twoIds, busA, sameBus), twoSources, 5),
        12, 48000.0);
    check(close(summed.left[0], 0.75F) &&
              close(summed.meters.buses[0].peak.left, 0.75F),
          "multiple tracks routed to one bus must sum once");

    const std::array splitBuses{
        routing::TrackOutputDestination::toBus({1}),
        routing::TrackOutputDestination::toBus({2})};
    const auto isolated = render(
        audio::prepareProcessingPlan(
            specification(twoIds, buses, splitBuses), twoSources, 3),
        12, 48000.0);
    check(isolated.meters.busCount == 2 &&
              close(isolated.meters.buses[0].peak.left, 0.5F) &&
              close(isolated.meters.buses[1].peak.left, 0.25F),
          "separate buses must retain isolated accumulation and metering");

    auto soloSpec = specification(twoIds, busA, sameBus);
    soloSpec.tracks[0].mix.solo = true;
    const auto solo = render(
        audio::prepareProcessingPlan(soloSpec, twoSources, 4), 12, 48000.0);
    check(close(solo.left[0], 0.5F),
          "track Solo must remain audible through a bus");
    auto muteSpec = specification(oneId, busA, busDestination);
    muteSpec.tracks[0].mix.muted = true;
    const auto muted = render(
        audio::prepareProcessingPlan(muteSpec, source, 4), 12, 48000.0);
    check(muted.left[0] == 0.0F &&
              muted.meters.buses[0].peak == mixer::StereoPeak{},
          "track Mute must silence its bus contribution");

    const std::vector<float> rateA(4, 1.0F);
    const std::vector<float> rateB(1, 0.5F);
    const std::array mixedSources{
        mono({1}, rateA, 4.0, 8.0), mono({2}, rateB, 2.0, 8.0)};
    const auto mixed = render(
        audio::prepareProcessingPlan(
            specification(twoIds, busA, sameBus, 8.0), mixedSources, 3),
        14, 8.0);
    check(mixed.transport.position.value == 8 && !mixed.transport.playing &&
              mixed.left[8] == 0.0F && mixed.left[13] == 0.0F,
          "mixed rates and durations must end correctly inside a subblock");

    const std::vector<float> sequence{0.0F, 0.1F, 0.2F, 0.3F, 0.4F,
                                      0.5F, 0.6F, 0.7F, 0.8F, 0.9F};
    const std::array sequenceSource{stereo({1}, sequence, 10.0)};
    const auto sequenced = render(
        audio::prepareProcessingPlan(
            specification(oneId, busA, busDestination, 10.0),
            sequenceSource, 3),
        sequence.size(), 10.0);
    check(sequenced.left == sequence &&
              close(sequenced.meters.master.left, 0.9F),
          "subblock boundaries must not repeat or skip project positions");

    const std::vector<float> smoothingSignal(12, 1.0F);
    const std::array smoothingSource{
        stereo({1}, smoothingSignal, 1000.0,
               mixer::prepareLinear(0.0F, {}))};
    auto smoothingSpec = specification(
        oneId, busA, busDestination, 1000.0);
    smoothingSpec.tracks[0].mix = mixer::prepareLinear(0.0F, {});
    const auto unity = mixer::prepareLinear(1.0F, {});
    const auto smoothed = render(
        audio::prepareProcessingPlan(smoothingSpec, smoothingSource, 2),
        12, 1000.0, &unity);
    check(close(smoothed.left[0], 0.2F) && smoothed.left[4] == 1.0F &&
              close(smoothed.meters.master.left, 1.0F),
          "large callbacks must preserve smoothing and metering across subblocks");

    constexpr std::size_t scaleCount = 32;
    std::vector<float> scaleSamples(1, 0.01F);
    std::vector<tracks::TrackId> scaleIds;
    std::vector<audio::PreparedTrackView> scaleSources;
    std::vector<routing::TrackOutputDestination> directRoutes;
    std::vector<routing::TrackOutputDestination> distributedRoutes;
    std::vector<routing::AudioBus> scaleBuses;
    for (std::size_t index = 0; index < 8; ++index) {
        scaleBuses.push_back({{index + 1}, "Bus"});
    }
    for (std::size_t index = 0; index < scaleCount; ++index) {
        scaleIds.push_back({index + 1});
        scaleSources.push_back(stereo({index + 1}, scaleSamples, 48000.0));
        directRoutes.push_back(routing::TrackOutputDestination::master());
        distributedRoutes.push_back(routing::TrackOutputDestination::toBus(
            scaleBuses[index % scaleBuses.size()].id));
    }
    const auto scaleDirect = render(
        audio::prepareProcessingPlan(
            specification(scaleIds, noBuses, directRoutes), scaleSources, 1),
        1, 48000.0);
    const auto scaleRouted = render(
        audio::prepareProcessingPlan(
            specification(scaleIds, scaleBuses, distributedRoutes),
            scaleSources, 1),
        1, 48000.0);
    check(close(scaleDirect.left[0], 0.32F) &&
              close(scaleRouted.left[0], scaleDirect.left[0]) &&
              scaleRouted.meters.busCount == 8,
          "32 tracks distributed across 8 buses must match direct routing");

    std::cout << "All routing foundation tests passed\n";
    return EXIT_SUCCESS;
}
