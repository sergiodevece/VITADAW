#include "vitadaw/audio/PreparedProcessingPlan.h"
#include "vitadaw/audio/RealtimeAudioEngine.h"

#include <array>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <span>
#include <string_view>
#include <vector>

namespace {

void check(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

bool close(float actual, float expected, float tolerance = 1.0e-5F) {
    return std::abs(actual - expected) <= tolerance;
}

vitadaw::audio::PreparedTrackView stereo(
    vitadaw::tracks::TrackId id, const std::vector<float>& left,
    const std::vector<float>& right, double rate,
    vitadaw::mixer::TrackMixState mix = {}) {
    return {id, {{left.data(), right.data()}}, 2,
            {static_cast<std::uint64_t>(left.size())},
            vitadaw::timeline::SampleRate{rate}, {0},
            {static_cast<std::int64_t>(left.size())}, {0},
            vitadaw::mixer::prepare(mix)};
}

vitadaw::audio::ProcessingPlanSpecification makeSpecification(
    std::span<const vitadaw::routing::TrackOutputDestination> destinations,
    std::span<const vitadaw::mixer::TrackMixState> trackMixes,
    std::span<const vitadaw::mixer::BusMixState> busMixes,
    double rate = 48000.0) {
    vitadaw::audio::ProcessingPlanSpecification result;
    result.projectSampleRate = vitadaw::timeline::SampleRate{rate};
    for (std::size_t index = 0; index < busMixes.size(); ++index) {
        result.buses.push_back(
            {{index + 1}, vitadaw::mixer::prepare(busMixes[index])});
    }
    for (std::size_t index = 0; index < destinations.size(); ++index) {
        result.tracks.push_back(
            {{index + 1}, vitadaw::mixer::prepare(trackMixes[index]),
             destinations[index]});
    }
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
        std::array<float*, 0> noOutput{};
        engine.processBlock({noOutput.data(), 0, 0},
                            vitadaw::timeline::SampleRate{rate});
        check(engine.tryRequestPlay().accepted, "prepared bus project must play");
    }

    std::pair<std::vector<float>, std::vector<float>> render(
        std::size_t frames) {
        std::pair<std::vector<float>, std::vector<float>> result{
            std::vector<float>(frames), std::vector<float>(frames)};
        std::array<float*, 2> channels{result.first.data(),
                                       result.second.data()};
        engine.processBlock({channels.data(), channels.size(), frames},
                            vitadaw::timeline::SampleRate{rate});
        return result;
    }
};

vitadaw::routing::TrackOutputDestination bus(std::uint64_t id) {
    return vitadaw::routing::TrackOutputDestination::toBus({id});
}

} // namespace

int main() {
    using namespace vitadaw;

    const std::vector<float> ones(4096, 1.0F);
    const std::vector<float> halves(4096, 0.5F);
    const std::array oneBusRoute{bus(1)};
    const std::array defaultTrack{mixer::TrackMixState{}};

    const auto renderBusState = [&](mixer::BusMixState state) {
        const std::array busStates{state};
        const std::array source{stereo({1}, ones, halves, 48000.0)};
        Harness harness{makeSpecification(oneBusRoute, defaultTrack, busStates),
                        source};
        return std::pair{harness.render(1), harness.engine.meterSnapshot()};
    };

    const auto unity = renderBusState({});
    check(unity.first.first[0] == 1.0F && unity.first.second[0] == 0.5F,
          "bus gain at 0 dB and centred balance must be unity");
    const auto attenuation = renderBusState({mixer::GainDb{-6.0206F}});
    check(close(attenuation.first.first[0], 0.5F, 2.0e-5F),
          "bus gain must attenuate using prepared linear amplitude");
    const auto boost = renderBusState({mixer::GainDb{6.0206F}});
    check(close(boost.first.first[0], 2.0F, 3.0e-5F),
          "bus gain must support the bounded positive range");
    const auto silence = renderBusState({mixer::GainDb{-100.0F}});
    check(silence.first.first[0] == 0.0F &&
              silence.second.buses[0].peak == mixer::StereoPeak{},
          "minimum bus gain must be exact silence at the post-processing meter");
    check(!mixer::BusMixState{mixer::GainDb{
                  std::numeric_limits<float>::quiet_NaN()}}
               .isValid() &&
              !mixer::BusMixState{mixer::GainDb{13.0F}}.isValid() &&
              !mixer::BusMixState{{}, mixer::Pan{2.0F}}.isValid(),
          "invalid bus gain and balance values must be rejected");
    auto invalidPrepared = makeSpecification(
        oneBusRoute, defaultTrack,
        std::array{mixer::BusMixState{}});
    invalidPrepared.buses[0].mix.leftBalance = 2.0F;
    check(!audio::prepareProcessingPlan(invalidPrepared, {}).success(),
          "the portable plan must reject malformed prepared bus coefficients");

    const auto hardLeft = renderBusState({{}, mixer::Pan{-1.0F}});
    const auto centre = renderBusState({{}, mixer::Pan{0.0F}});
    const auto hardRight = renderBusState({{}, mixer::Pan{1.0F}});
    const auto intermediate = renderBusState({{}, mixer::Pan{0.5F}});
    check(hardLeft.first.first[0] == 1.0F &&
              hardLeft.first.second[0] == 0.0F &&
              centre.first.first[0] == 1.0F &&
              centre.first.second[0] == 0.5F &&
              hardRight.first.first[0] == 0.0F &&
              hardRight.first.second[0] == 0.5F &&
              intermediate.first.first[0] > 0.0F &&
              intermediate.first.first[0] < 1.0F &&
              intermediate.first.second[0] == 0.5F,
          "stereo bus balance must preserve centre and attenuate the opposite channel");

    // Five frames at 1 kHz: gain and balance reuse the sample-accurate ramp.
    const std::array smoothBus{mixer::BusMixState{mixer::GainDb{-100.0F},
                                                  mixer::Pan{-1.0F}}};
    const std::array smoothSource{stereo({1}, ones, ones, 1000.0)};
    Harness smooth{makeSpecification(oneBusRoute, defaultTrack, smoothBus, 1000.0),
                   smoothSource, 2};
    mixer::BusMixState smoothTarget{{}, mixer::Pan{1.0F}};
    check(smooth.engine.tryUpdateBusMix(
              {1}, mixer::prepare(smoothTarget), audio::fullyAudibleState()),
          "bus gain and balance target must enqueue without rebuilding routing");
    const auto ramp = smooth.render(5);
    check(close(ramp.first[0], 0.16F) && ramp.first[4] == 0.0F &&
              close(ramp.second[0], 0.04F) && ramp.second[4] == 1.0F,
          "bus gain and balance must remain continuous across prepared subblocks");
    mixer::BusMixState backToCentre{{mixer::GainDb{-100.0F}}, {}};
    check(smooth.engine.tryUpdateBusMix(
              {1}, mixer::prepare(backToCentre), audio::fullyAudibleState()),
          "bus smoother must accept a new target after a completed ramp");
    static_cast<void>(smooth.render(2));
    check(smooth.engine.tryUpdateBusMix(
              {1}, mixer::prepare(smoothTarget), audio::fullyAudibleState()),
          "bus smoother must retarget from its instantaneous value");
    const auto retarget = smooth.render(1);
    check(retarget.second[0] > 0.0F && retarget.second[0] < 1.0F,
          "bus retarget must not jump directly to its endpoint");

    const auto smoothedForBlock = [&](std::size_t blockSize) {
        const std::array initial{mixer::BusMixState{mixer::GainDb{-100.0F}}};
        const std::array source{stereo({1}, ones, ones, 48000.0)};
        Harness harness{makeSpecification(oneBusRoute, defaultTrack, initial),
                        source, 512};
        check(harness.engine.tryUpdateBusMix(
                  {1}, mixer::prepare(mixer::BusMixState{}),
                  audio::fullyAudibleState()),
              "block-size smoothing target must enqueue");
        std::vector<float> output;
        output.reserve(2048);
        for (std::size_t offset = 0; offset < 2048; offset += blockSize) {
            const auto count = std::min(blockSize, std::size_t{2048} - offset);
            auto block = harness.render(count);
            output.insert(output.end(), block.first.begin(), block.first.end());
        }
        return output;
    };
    const auto smoothingReference = smoothedForBlock(64);
    for (const auto blockSize : {128U, 256U, 512U, 1024U}) {
        const auto candidate = smoothedForBlock(blockSize);
        for (std::size_t frame = 0; frame < candidate.size(); ++frame) {
            check(close(candidate[frame], smoothingReference[frame], 2.0e-6F),
                  "bus smoothing must not depend on callback or subblock size");
        }
    }
    for (const auto rate : {44100.0, 48000.0, 96000.0}) {
        const auto rampFrames = static_cast<std::size_t>(
            std::llround(audio::mixerSmoothingSeconds * rate));
        const std::vector<float> signal(rampFrames + 2, 1.0F);
        const std::array source{stereo({1}, signal, signal, rate)};
        const std::array initial{mixer::BusMixState{mixer::GainDb{-100.0F}}};
        Harness harness{makeSpecification(oneBusRoute, defaultTrack, initial,
                                          rate),
                        source};
        check(harness.engine.tryUpdateBusMix(
                  {1}, mixer::prepare(mixer::BusMixState{}),
                  audio::fullyAudibleState()),
              "sample-rate bus smoothing target must enqueue");
        const auto output = harness.render(rampFrames);
        check(output.first[rampFrames - 2] < 1.0F &&
                  output.first.back() == 1.0F,
              "bus smoothing duration must remain 5 ms at 44.1, 48 and 96 kHz");
    }

    // Mute is post-processing: inputs still run, output/meter are zero, and
    // direct and other-bus paths remain independent.
    const std::array muteRoutes{bus(1), bus(1),
                                routing::TrackOutputDestination::master(),
                                bus(2)};
    const std::array muteTrackMix{mixer::TrackMixState{},
                                  mixer::TrackMixState{},
                                  mixer::TrackMixState{},
                                  mixer::TrackMixState{}};
    const std::array mutedBuses{mixer::BusMixState{{}, {}, true, false},
                                mixer::BusMixState{}};
    const std::vector<float> quarters(4096, 0.25F);
    const std::array muteSources{stereo({1}, ones, ones, 48000.0),
                                 stereo({2}, halves, halves, 48000.0),
                                 stereo({3}, halves, halves, 48000.0),
                                 stereo({4}, quarters, quarters, 48000.0)};
    Harness mute{makeSpecification(muteRoutes, muteTrackMix, mutedBuses),
                 muteSources};
    const auto mutedOutput = mute.render(1);
    check(mutedOutput.first[0] == 0.75F &&
              mute.engine.meterSnapshot().buses[0].peak == mixer::StereoPeak{} &&
              mute.engine.meterSnapshot().buses[1].peak.left == 0.25F &&
              mute.engine.transportSnapshot().position.value == 1,
          "bus mute must preserve other buses, direct audio and transport time");
    check(mute.engine.tryUpdateBusMix(
              {1}, mixer::prepare(mixer::BusMixState{}),
              audio::fullyAudibleState()),
          "bus unmute must be a lightweight parameter update");
    const auto unmutedOutput = mute.render(1);
    check(unmutedOutput.first[0] == 2.25F,
          "unmute must restore only the bus contribution");

    // Solo is resolved as selected nodes plus every path needed to Master.
    const std::array routes{bus(1), bus(1), bus(2),
                            routing::TrackOutputDestination::master()};
    const std::array sources{stereo({1}, ones, ones, 48000.0),
                             stereo({2}, halves, halves, 48000.0),
                             stereo({3}, ones, ones, 48000.0),
                             stereo({4}, halves, halves, 48000.0)};
    const std::array noTrackSolo{mixer::TrackMixState{},
                                  mixer::TrackMixState{},
                                  mixer::TrackMixState{},
                                  mixer::TrackMixState{}};
    const std::array noBusSolo{mixer::BusMixState{}, mixer::BusMixState{}};

    auto trackSolo = noTrackSolo;
    trackSolo[0].solo = true;
    Harness soloThroughBus{makeSpecification(routes, trackSolo, noBusSolo), sources};
    check(soloThroughBus.render(1).first[0] == 1.0F &&
              soloThroughBus.engine.meterSnapshot().buses[0].peak.left == 1.0F,
          "a non-solo bus must remain open for a solo track that needs it");

    auto busASolo = noBusSolo;
    busASolo[0].solo = true;
    Harness selectedBus{makeSpecification(routes, noTrackSolo, busASolo), sources};
    check(selectedBus.render(1).first[0] == 1.5F,
          "bus Solo must include every track routed to that bus and exclude direct routes");

    auto bothBusesSolo = noBusSolo;
    bothBusesSolo[0].solo = true;
    bothBusesSolo[1].solo = true;
    Harness twoBusSolo{makeSpecification(routes, noTrackSolo, bothBusesSolo), sources};
    check(twoBusSolo.render(1).first[0] == 2.5F,
          "multiple solo buses must form a union of their routed sources");

    auto mixedTrackSolo = noTrackSolo;
    mixedTrackSolo[0].solo = true;
    auto busBSolo = noBusSolo;
    busBSolo[1].solo = true;
    Harness unionSolo{makeSpecification(routes, mixedTrackSolo, busBSolo), sources};
    check(unionSolo.render(1).first[0] == 2.0F,
          "track Solo and bus Solo must combine as a logical union");

    mixedTrackSolo[0].muted = true;
    Harness trackMuteWins{makeSpecification(routes, mixedTrackSolo, busBSolo), sources};
    check(trackMuteWins.render(1).first[0] == 1.0F,
          "track Mute must prevail inside the union of Solo selections");

    busASolo[0].muted = true;
    Harness busMuteWins{makeSpecification(routes, noTrackSolo, busASolo), sources};
    check(busMuteWins.render(1).first[0] == 0.0F &&
              busMuteWins.engine.meterSnapshot().buses[0].peak ==
                  mixer::StereoPeak{},
          "bus Mute must prevail over its own Solo and meter post-mute silence");

    auto directSolo = noTrackSolo;
    directSolo[3].solo = true;
    Harness directTrackSolo{makeSpecification(routes, directSolo, noBusSolo), sources};
    check(directTrackSolo.render(1).first[0] == 0.5F,
          "a direct-to-Master solo track must remain independently audible");

    // Functional scale: mixed rates, empty source and 32 tracks across 8 buses.
    std::vector<routing::TrackOutputDestination> scaleRoutes;
    std::vector<mixer::TrackMixState> scaleTrackMix(32);
    std::vector<mixer::BusMixState> scaleBusMix(8);
    std::vector<std::vector<float>> scaleSignals(31);
    std::vector<audio::PreparedTrackView> scaleSources;
    for (std::size_t index = 0; index < 32; ++index) {
        scaleRoutes.push_back(index == 31
                                  ? routing::TrackOutputDestination::master()
                                  : bus(index % 8 + 1));
        if (index < scaleSignals.size()) {
            scaleSignals[index].assign(1100, 0.01F);
            const auto rate = index % 2 == 0 ? 44100.0 : 48000.0;
            scaleSources.push_back(stereo(
                {index + 1}, scaleSignals[index], scaleSignals[index], rate));
        }
    }
    Harness scale{makeSpecification(scaleRoutes, scaleTrackMix, scaleBusMix),
                  scaleSources, 128};
    const auto scaleOutput = scale.render(1024);
    check(scaleOutput.first[0] > 0.30F &&
              scale.engine.meterSnapshot().busCount == 8,
          "32 tracks, 8 buses, an empty track and mixed rates must use the routed engine");

    std::cout << "All bus mixer controls tests passed\n";
    return EXIT_SUCCESS;
}
