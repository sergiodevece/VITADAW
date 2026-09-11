#include "vitadaw/audio/RealtimeAudioEngine.h"
#include "vitadaw/audio/TrackMixerProcessing.h"

#include <array>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
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

vitadaw::mixer::PreparedTrackMixState mix(
    float linearGain = 1.0F, float pan = 0.0F, bool muted = false,
    bool solo = false) {
    return vitadaw::mixer::prepareLinear(
        linearGain, vitadaw::mixer::Pan{pan}, muted, solo);
}

vitadaw::audio::PreparedTrackView mono(
    vitadaw::tracks::TrackId id, const std::vector<float>& samples,
    vitadaw::mixer::PreparedTrackMixState mix = {}, double sourceRate = 48000.0,
    double projectRate = 48000.0) {
    return {id, {{samples.data(), nullptr}}, 1,
            {static_cast<std::uint64_t>(samples.size())},
            vitadaw::timeline::SampleRate{sourceRate}, {0},
            vitadaw::timeline::sourceFramesToProjectDuration(
                {static_cast<std::uint64_t>(samples.size())},
                vitadaw::timeline::SampleRate{sourceRate},
                vitadaw::timeline::SampleRate{projectRate}),
            {0}, mix};
}

void makeOperational(vitadaw::audio::RealtimeAudioEngine& engine,
                     double rate = 48000.0) {
    engine.deviceInitialising();
    std::array<float*, 0> none{};
    engine.processBlock({none.data(), 0, 0},
                        vitadaw::timeline::SampleRate{rate});
}

vitadaw::audio::StereoSample renderOne(
    vitadaw::audio::RealtimeAudioEngine& engine, double rate = 48000.0) {
    float left{}, right{};
    std::array<float*, 2> channels{&left, &right};
    engine.processBlock({channels.data(), channels.size(), 1},
                        vitadaw::timeline::SampleRate{rate});
    return {left, right};
}

vitadaw::audio::StereoSample renderLast(
    vitadaw::audio::RealtimeAudioEngine& engine, std::size_t frameCount,
    double rate = 48000.0) {
    std::vector<float> left(frameCount), right(frameCount);
    std::array<float*, 2> channels{left.data(), right.data()};
    engine.processBlock({channels.data(), channels.size(), frameCount},
                        vitadaw::timeline::SampleRate{rate});
    return {left.back(), right.back()};
}

} // namespace

int main() {
    using namespace vitadaw;
    constexpr float rootHalf = 0.70710678F;

    check(mixer::GainDb{0.0F}.isValid() &&
              close(mixer::GainDb{0.0F}.linear(), 1.0F),
          "0 dB must be unity gain");
    check(close(mixer::GainDb{-6.0F}.linear(), 0.5011872F) &&
              mixer::GainDb{mixer::GainDb::silence}.linear() == 0.0F &&
              close(mixer::GainDb{6.0F}.linear(), 1.9952623F),
          "gain conversion must cover attenuation, silence and positive gain");
    check(!mixer::GainDb{13.0F}.isValid() &&
              !mixer::GainDb{-101.0F}.isValid() &&
              !mixer::GainDb{std::numeric_limits<float>::quiet_NaN()}.isValid() &&
              !mixer::GainDb{std::numeric_limits<float>::infinity()}.isValid(),
          "invalid gains must be rejected");

    const auto attenuated = audio::applyTrackMix(
        {1.0F, 1.0F}, 1,
        mixer::prepare(mixer::TrackMixState{{-6.0F}, {}, false, false}),
        false);
    const auto silent = audio::applyTrackMix(
        {1.0F, 1.0F}, 1,
        mixer::prepare(mixer::TrackMixState{
            {mixer::GainDb::silence}, {}, false, false}),
        false);
    const auto boosted = audio::applyTrackMix(
        {1.0F, 1.0F}, 1,
        mixer::prepare(mixer::TrackMixState{{6.0F}, {}, false, false}),
        false);
    check(close(attenuated.left, 0.5011872F * rootHalf) &&
              silent == audio::StereoSample{} &&
              close(boosted.left, 1.9952623F * rootHalf),
          "track DSP must apply attenuation, silence and allowed positive gain");

    const auto centre = mix();
    const auto monoCentre = audio::applyTrackMix({1.0F, 1.0F}, 1, centre, false);
    const auto monoLeft = audio::applyTrackMix(
        {1.0F, 1.0F}, 1, mix(1.0F, -1.0F), false);
    const auto monoRight = audio::applyTrackMix(
        {1.0F, 1.0F}, 1, mix(1.0F, 1.0F), false);
    const auto monoQuarter = audio::applyTrackMix(
        {1.0F, 1.0F}, 1, mix(1.0F, -0.5F), false);
    check(close(monoCentre.left, rootHalf) && close(monoCentre.right, rootHalf) &&
              close(monoLeft.left, 1.0F) && close(monoLeft.right, 0.0F) &&
              close(monoRight.left, 0.0F) && close(monoRight.right, 1.0F),
          "mono equal-power pan must define centre and both extremes");
    check(close(monoQuarter.left * monoQuarter.left +
                    monoQuarter.right * monoQuarter.right,
                1.0F),
          "mono equal-power pan must conserve power at intermediate points");
    const auto stereoCentre = audio::applyTrackMix(
        {0.25F, 0.5F}, 2, centre, false);
    const auto stereoLeft = audio::applyTrackMix(
        {0.25F, 0.5F}, 2, mix(1.0F, -1.0F), false);
    check(close(stereoCentre.left, 0.25F) && close(stereoCentre.right, 0.5F) &&
              close(stereoLeft.left, 0.25F) && close(stereoLeft.right, 0.0F),
          "stereo balance must preserve centre and attenuate the opposite side");

    check(audio::applyTrackMix({1.0F, 1.0F}, 1,
                               mix(1.0F, 0.0F, true, true), true) ==
              audio::StereoSample{} &&
              audio::applyTrackMix({1.0F, 1.0F}, 1,
                                   mix(1.0F, 0.0F, false, false), true) ==
                  audio::StereoSample{} &&
              audio::applyTrackMix({1.0F, 1.0F}, 1,
                                   mix(1.0F, 0.0F, false, true), true) !=
                  audio::StereoSample{},
          "mute must override solo and non-solo tracks must be silent when any solo exists");

    const std::vector<float> signal(4096, 0.25F);
    const std::array baseTracks{mono({1}, signal), mono({2}, signal)};
    audio::RealtimeAudioEngine engine;
    engine.configure({timeline::SampleRate{48000.0}, {4096}, baseTracks, {}});
    makeOperational(engine);
    check(engine.tryUpdateTrackMix({1}, mix(1.0F, 0.0F, true, false), false) &&
              engine.tryRequestPlay().accepted,
          "a prepared track must accept a lightweight mute update");
    const auto muted = renderOne(engine);
    check(close(muted.left, 0.25F * rootHalf) &&
              engine.transportSnapshot().position.value == 1,
          "muted track must contribute zero while the master clock advances");
    check(engine.tryUpdateTrackMix({1}, mix(), false),
          "unmute update must be accepted");
    const auto unmuted = renderOne(engine);
    check(close(unmuted.left, 0.5F * rootHalf) &&
              engine.transportSnapshot().position.value == 2,
          "unmute must restore audio without changing clock continuity");

    check(engine.tryUpdateTrackMix(
              {1}, mix(1.0F, 0.0F, false, true), true) &&
              engine.tryUpdateTrackMix(
                  {2}, mix(1.0F, 0.0F, false, false), true),
          "solo and pan updates must enqueue");
    const auto oneSolo = renderOne(engine);
    check(close(oneSolo.left, 0.25F * rootHalf) &&
              close(oneSolo.right, 0.25F * rootHalf),
          "one solo must exclude every non-solo track");
    check(engine.tryUpdateTrackMix(
              {2}, mix(1.0F, 0.0F, false, true), true),
          "a second solo must enqueue");
    const auto twoSolo = renderOne(engine);
    check(close(twoSolo.left, 0.5F * rootHalf) &&
              close(twoSolo.right, 0.5F * rootHalf),
          "multiple solos must contribute together");
    check(engine.tryUpdateTrackMix(
              {2}, mix(1.0F, 0.0F, true, true), true),
          "mute plus solo must enqueue");
    const auto muteSolo = renderOne(engine);
    check(close(muteSolo.left, 0.25F * rootHalf) &&
              close(muteSolo.right, 0.25F * rootHalf),
          "mute must retain precedence over solo");

    check(engine.tryUpdateMasterMix({0.5F}),
          "master attenuation must enqueue");
    const auto masterHalf = renderLast(engine, 240);
    check(close(masterHalf.left, 0.125F * rootHalf),
          "master gain must run after track accumulation");
    check(engine.tryUpdateMasterMix({0.0F}), "master silence must enqueue");
    check(renderLast(engine, 240) == audio::StereoSample{},
          "silent master must zero both output channels");
    check(engine.tryUpdateMasterMix({2.0F}), "positive master gain must enqueue");
    check(close(renderLast(engine, 240).left, 0.5F * rootHalf),
          "positive master gain must remain unclipped");

    for (const std::size_t count : {1U, 2U, 4U, 8U, 32U}) {
        std::vector<std::vector<float>> samples(
            count, std::vector<float>(2, 0.01F));
        std::vector<audio::PreparedTrackView> tracks;
        tracks.reserve(count + 1);
        float expectedLeft{};
        float expectedRight{};
        bool anySolo{};
        for (std::size_t index = 0; index < count; ++index) {
            const auto trackMix = mix(index % 3 == 0 ? 0.5F : 1.0F,
                                      index % 2 == 0 ? -0.5F : 0.5F,
                                      index % 7 == 0 && index != 0,
                                      count >= 4 && index % 4 == 0);
            anySolo = anySolo || trackMix.solo;
            tracks.push_back(mono({index + 1}, samples[index], trackMix,
                                  index % 2 == 0 ? 44100.0 : 48000.0));
        }
        tracks.push_back({});
        for (std::size_t index = 0; index < count; ++index) {
            const auto contribution = audio::applyTrackMix(
                {0.01F, 0.01F}, 1, tracks[index].mix, anySolo);
            expectedLeft += contribution.left;
            expectedRight += contribution.right;
        }
        audio::RealtimeAudioEngine scaleEngine;
        scaleEngine.configure(
            {timeline::SampleRate{48000.0}, {2}, tracks, {0.75F}, anySolo});
        makeOperational(scaleEngine);
        check(scaleEngine.tryRequestPlay().accepted,
              "N-track mixer topology must accept playback");
        const auto output = renderOne(scaleEngine);
        check(close(output.left, expectedLeft * 0.75F) &&
                  close(output.right, expectedRight * 0.75F),
              "production processBlock must match the complete N-track DSP chain");
    }

    const auto durationBeforeSolo = engine.transportSnapshot().duration;
    const auto positionBeforeSolo = engine.transportSnapshot().position;
    check(engine.tryUpdateTrackMix({1}, mix(), true) &&
              engine.tryUpdateTrackMix({2}, mix(), false),
          "removing every solo must enqueue");
    static_cast<void>(renderOne(engine));
    check(engine.transportSnapshot().duration == durationBeforeSolo &&
              engine.transportSnapshot().position.value ==
                  positionBeforeSolo.value + 1,
          "adding or removing solo must not alter duration or clock progression");

    check(engine.tryUpdateGlobalSolo(true),
          "solo eligibility from an empty track must enqueue");
    check(renderOne(engine) == audio::StereoSample{},
          "an empty solo track must silence loaded non-solo tracks");
    check(engine.tryUpdateGlobalSolo(false),
          "removing empty-track solo eligibility must enqueue");
    static_cast<void>(renderOne(engine));

    for (std::size_t index = 0;
         index < audio::RealtimeAudioEngine::parameterCommandCapacity - 1;
         ++index) {
        check(engine.tryUpdateMasterMix({1.0F}),
              "every usable parameter ring slot must accept an update");
    }
    check(!engine.tryUpdateMasterMix({1.0F}),
          "a full parameter queue must reject explicitly");
    static_cast<void>(renderOne(engine));
    check(engine.tryUpdateMasterMix({1.0F}),
          "the parameter queue must remain usable after wrap-around");

    std::cout << "All mixer core tests passed\n";
    return EXIT_SUCCESS;
}
