#include "vitadaw/audio/RealtimeAudioEngine.h"
#include "vitadaw/audio/RealtimeMeterExchange.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <string_view>
#include <thread>
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
    float gain = 1.0F, float pan = 0.0F, bool muted = false,
    bool solo = false) {
    return vitadaw::mixer::prepareLinear(
        gain, vitadaw::mixer::Pan{pan}, muted, solo);
}

vitadaw::audio::PreparedTrackView stereo(
    vitadaw::tracks::TrackId id, const std::vector<float>& left,
    const std::vector<float>& right,
    vitadaw::mixer::PreparedTrackMixState trackMix = {},
    double rate = 48000.0) {
    return {id, {{left.data(), right.data()}}, 2,
            {static_cast<std::uint64_t>(left.size())},
            vitadaw::timeline::SampleRate{rate}, {0},
            {static_cast<std::int64_t>(left.size())}, {0}, trackMix};
}

vitadaw::audio::PreparedTrackView mono(
    vitadaw::tracks::TrackId id, const std::vector<float>& samples,
    vitadaw::mixer::PreparedTrackMixState trackMix = {},
    double rate = 48000.0) {
    return {id, {{samples.data(), nullptr}}, 1,
            {static_cast<std::uint64_t>(samples.size())},
            vitadaw::timeline::SampleRate{rate}, {0},
            {static_cast<std::int64_t>(samples.size())}, {0}, trackMix};
}

void makeOperational(vitadaw::audio::RealtimeAudioEngine& engine,
                     double rate) {
    engine.deviceInitialising();
    std::array<float*, 0> none{};
    engine.processBlock({none.data(), 0, 0},
                        vitadaw::timeline::SampleRate{rate});
}

void render(vitadaw::audio::RealtimeAudioEngine& engine,
            std::span<float> left, std::span<float> right, double rate) {
    std::array<float*, 2> channels{left.data(), right.data()};
    engine.processBlock({channels.data(), channels.size(), left.size()},
                        vitadaw::timeline::SampleRate{rate});
}

std::vector<float> renderSmoothedGain(std::size_t blockSize) {
    using namespace vitadaw;
    constexpr std::size_t totalFrames = 2048;
    const std::vector<float> signal(totalFrames, 1.0F);
    const std::array tracks{stereo({1}, signal, signal, mix(0.0F))};
    audio::RealtimeAudioEngine engine;
    engine.configure({timeline::SampleRate{48000.0}, {totalFrames}, tracks});
    makeOperational(engine, 48000.0);
    check(engine.tryRequestPlay().accepted &&
              engine.tryUpdateTrackMix({1}, mix(1.0F),
                                       audio::fullyAudibleState()),
          "buffer equivalence setup must enqueue Play and gain");
    std::vector<float> result(totalFrames), right(totalFrames);
    for (std::size_t offset = 0; offset < totalFrames; offset += blockSize) {
        const auto count = std::min(blockSize, totalFrames - offset);
        render(engine, std::span{result}.subspan(offset, count),
               std::span{right}.subspan(offset, count), 48000.0);
    }
    return result;
}

} // namespace

int main() {
    using namespace vitadaw;

    // No pending target: configured values are exact from the first sample.
    const std::vector<float> unitSignal(4096, 1.0F);
    const std::array unityTrack{stereo({1}, unitSignal, unitSignal)};
    audio::RealtimeAudioEngine engine;
    engine.configure({timeline::SampleRate{1000.0}, {4096}, unityTrack});
    makeOperational(engine, 1000.0);
    check(engine.tryRequestPlay().accepted, "unity smoothing test must play");
    std::array<float, 1> firstLeft{}, firstRight{};
    render(engine, firstLeft, firstRight, 1000.0);
    check(firstLeft[0] == 1.0F && firstRight[0] == 1.0F,
          "gain with no pending target must remain exact");

    check(engine.tryUpdateTrackMix({1}, mix(0.0F),
                                   audio::fullyAudibleState()),
          "downward gain target must enqueue");
    std::array<float, 5> downLeft{}, downRight{};
    render(engine, downLeft, downRight, 1000.0);
    check(close(downLeft[0], 0.8F) && close(downLeft[3], 0.2F) &&
              downLeft[4] == 0.0F,
          "track gain must ramp down linearly over exactly 5 ms");

    check(engine.tryUpdateTrackMix({1}, mix(1.0F),
                                   audio::fullyAudibleState()),
          "upward gain target must enqueue");
    std::array<float, 5> upLeft{}, upRight{};
    render(engine, upLeft, upRight, 1000.0);
    check(close(upLeft[0], 0.2F) && close(upLeft[3], 0.8F) &&
              upLeft[4] == 1.0F,
          "track gain must ramp up and land exactly on target");

    // Retarget from the instantaneous value, not the original ramp origin.
    check(engine.tryUpdateTrackMix({1}, mix(0.0F),
                                   audio::fullyAudibleState()),
          "first rapid fader target must enqueue");
    std::array<float, 2> partialLeft{}, partialRight{};
    render(engine, partialLeft, partialRight, 1000.0);
    check(close(partialLeft[1], 0.6F),
          "first rapid fader move must reach its instantaneous midpoint");
    check(engine.tryUpdateTrackMix({1}, mix(1.0F),
                                   audio::fullyAudibleState()),
          "second rapid fader target must enqueue");
    std::array<float, 5> retargetLeft{}, retargetRight{};
    render(engine, retargetLeft, retargetRight, 1000.0);
    check(close(retargetLeft[0], 0.68F) &&
              std::abs(retargetLeft[0] - partialLeft[1]) < 0.1F &&
              retargetLeft[4] == 1.0F,
          "retarget must continue from the current coefficient without a jump");

    // Pan coefficients are prepared outside RT and each ramps sample-accurately.
    const std::array leftTrack{mono({1}, unitSignal, mix(1.0F, -1.0F), 1000.0)};
    audio::RealtimeAudioEngine panEngine;
    panEngine.configure({timeline::SampleRate{1000.0}, {4096}, leftTrack});
    makeOperational(panEngine, 1000.0);
    check(panEngine.tryRequestPlay().accepted &&
              panEngine.tryUpdateTrackMix({1}, mix(1.0F, 1.0F),
                                          audio::fullyAudibleState()),
          "left-to-right pan target must enqueue");
    std::array<float, 5> panLeft{}, panRight{};
    render(panEngine, panLeft, panRight, 1000.0);
    check(close(panLeft[0], 0.8F) && close(panRight[0], 0.2F) &&
              panLeft[4] == 0.0F && panRight[4] == 1.0F,
          "pan coefficients must move continuously to the opposite extreme");

    check(panEngine.tryUpdateTrackMix({1}, mix(1.0F, -1.0F),
                                     audio::fullyAudibleState()),
          "first rapid pan target must enqueue");
    std::array<float, 2> partialPanLeft{}, partialPanRight{};
    render(panEngine, partialPanLeft, partialPanRight, 1000.0);
    check(panEngine.tryUpdateTrackMix({1}, mix(1.0F, 0.0F),
                                     audio::fullyAudibleState()),
          "second rapid pan target must enqueue");
    std::array<float, 5> retargetPanLeft{}, retargetPanRight{};
    render(panEngine, retargetPanLeft, retargetPanRight, 1000.0);
    check(retargetPanLeft[0] > partialPanLeft[1] &&
              std::abs(retargetPanLeft[0] - partialPanLeft[1]) < 0.1F &&
              close(retargetPanLeft[4], 0.70710678F) &&
              close(retargetPanRight[4], 0.70710678F),
          "rapid pan retarget must start at its instantaneous coefficients");

    check(panEngine.tryUpdateMasterMix({0.0F}),
          "master target must enqueue");
    std::array<float, 5> masterLeft{}, masterRight{};
    render(panEngine, masterLeft, masterRight, 1000.0);
    check(close(masterRight[0], 0.8F * 0.70710678F) &&
              masterRight[4] == 0.0F,
          "master gain must use the same sample-accurate 5 ms ramp");

    // Temporal equivalence is independent of callback partitioning.
    const auto reference = renderSmoothedGain(64);
    for (const std::size_t block : {128U, 256U, 512U, 1024U}) {
        const auto candidate = renderSmoothedGain(block);
        for (std::size_t frame = 0; frame < reference.size(); ++frame) {
            check(close(candidate[frame], reference[frame], 2.0e-6F),
                  "smoothing trajectory must not depend on block size");
        }
    }

    // 5 ms resolves to the appropriate integral device-frame count.
    for (const double rate : {44100.0, 48000.0, 96000.0}) {
        const auto rampFrames = static_cast<std::size_t>(
            std::llround(audio::mixerSmoothingSeconds * rate));
        const std::vector<float> signal(rampFrames + 2, 1.0F);
        const std::array tracks{stereo({1}, signal, signal, mix(0.0F), rate)};
        audio::RealtimeAudioEngine rateEngine;
        rateEngine.configure({timeline::SampleRate{rate},
                              {static_cast<std::int64_t>(signal.size())}, tracks});
        makeOperational(rateEngine, rate);
        check(rateEngine.tryRequestPlay().accepted &&
                  rateEngine.tryUpdateTrackMix({1}, mix(1.0F),
                                               audio::fullyAudibleState()),
              "sample-rate smoothing setup must enqueue");
        std::vector<float> left(rampFrames), right(rampFrames);
        render(rateEngine, left, right, rate);
        check(left[rampFrames - 2] < 1.0F && left.back() == 1.0F,
              "smoothing duration must scale with device sample rate");
    }

    // Known-signal block peaks: post track mix and post master gain.
    const std::vector<float> positive(8, 0.25F);
    const std::vector<float> negative(8, -0.4F);
    const std::vector<float> stereoRight(8, 0.5F);
    const std::vector<float> silent(8, 0.0F);
    const std::array meterTracks{
        stereo({11}, positive, stereoRight),
        stereo({22}, negative, positive),
        stereo({33}, silent, silent, mix(1.0F, 0.0F, true, false)),
        stereo({44}, silent, silent)};
    audio::RealtimeAudioEngine meterEngine;
    meterEngine.configure({timeline::SampleRate{48000.0}, {8}, meterTracks,
                           {0.5F}});
    makeOperational(meterEngine, 48000.0);
    check(meterEngine.tryRequestPlay().accepted,
          "known meter project must play");
    std::array<float, 8> meterLeft{}, meterRight{};
    render(meterEngine, meterLeft, meterRight, 48000.0);
    const auto meters = meterEngine.meterSnapshot();
    check(meters.trackCount == 4 && meters.tracks[0].track == tracks::TrackId{11} &&
              close(meters.tracks[0].peak.left, 0.25F) &&
              close(meters.tracks[0].peak.right, 0.5F) &&
              close(meters.tracks[1].peak.left, 0.4F) &&
              close(meters.tracks[1].peak.right, 0.25F) &&
              meters.tracks[2].peak == mixer::StereoPeak{} &&
              meters.tracks[3].peak == mixer::StereoPeak{},
          "track meters must cover silence, mute and stable TrackId");
    check(close(meters.master.left, 0.075F) &&
              close(meters.master.right, 0.375F),
          "master meter must measure the sum after master gain");

    const std::vector<float> sine{0.0F, 1.0F, 0.0F, -1.0F};
    const std::array sineTrack{mono({7}, sine)};
    audio::RealtimeAudioEngine sineEngine;
    sineEngine.configure({timeline::SampleRate{48000.0}, {4}, sineTrack});
    makeOperational(sineEngine, 48000.0);
    check(sineEngine.tryRequestPlay().accepted, "sine meter test must play");
    std::array<float, 4> sineLeft{}, sineRight{};
    render(sineEngine, sineLeft, sineRight, 48000.0);
    check(close(sineEngine.meterSnapshot().tracks[0].peak.left, 0.70710678F),
          "known mono sine peak must include equal-power pan");

    const std::vector<float> loud(4, 0.8F);
    const std::array loudTracks{stereo({1}, loud, loud),
                                stereo({2}, loud, loud)};
    audio::RealtimeAudioEngine loudEngine;
    loudEngine.configure({timeline::SampleRate{48000.0}, {4}, loudTracks});
    makeOperational(loudEngine, 48000.0);
    check(loudEngine.tryRequestPlay().accepted, "headroom meter test must play");
    std::array<float, 4> loudLeft{}, loudRight{};
    render(loudEngine, loudLeft, loudRight, 48000.0);
    check(close(loudEngine.meterSnapshot().master.left, 1.6F) &&
              close(loudLeft[0], 1.6F),
          "meter and output must preserve internal signals above 1.0");

    // Solo excludes a track from both contribution and its post-eligibility meter.
    const std::array soloTracks{stereo({1}, positive, positive,
                                       mix(1.0F, 0.0F, false, true)),
                                stereo({2}, negative, negative)};
    audio::RealtimeAudioEngine soloEngine;
    soloEngine.configure(
        {timeline::SampleRate{48000.0}, {8}, soloTracks, {}, true});
    makeOperational(soloEngine, 48000.0);
    check(soloEngine.tryRequestPlay().accepted, "solo meter test must play");
    std::array<float, 8> soloLeft{}, soloRight{};
    render(soloEngine, soloLeft, soloRight, 48000.0);
    check(close(soloEngine.meterSnapshot().tracks[0].peak.left, 0.25F) &&
              soloEngine.meterSnapshot().tracks[1].peak == mixer::StereoPeak{},
          "non-solo track meter must show no audible contribution");

    for (const std::size_t count : {1U, 4U, 8U, 32U}) {
        std::vector<std::vector<float>> signals(count,
                                                std::vector<float>(2, 0.01F));
        std::vector<audio::PreparedTrackView> views;
        views.reserve(count);
        for (std::size_t index = 0; index < count; ++index) {
            views.push_back(stereo({index + 1}, signals[index], signals[index]));
        }
        audio::RealtimeAudioEngine scaleEngine;
        scaleEngine.configure({timeline::SampleRate{48000.0}, {2}, views});
        makeOperational(scaleEngine, 48000.0);
        check(scaleEngine.tryRequestPlay().accepted,
              "meter scalability project must play");
        std::array<float, 2> left{}, right{};
        render(scaleEngine, left, right, 48000.0);
        const auto snapshot = scaleEngine.meterSnapshot();
        check(snapshot.trackCount == count &&
                  snapshot.tracks[count - 1].track == tracks::TrackId{count} &&
                  close(snapshot.master.left, 0.01F * static_cast<float>(count)),
              "meter snapshot must scale without losing track identity");
    }

    // Concurrent telemetry read: empty fallback is allowed, torn generations are not.
    audio::RealtimeMeterExchange exchange;
    const std::array ids{tracks::TrackId{10}, tracks::TrackId{20}};
    std::atomic<bool> finished{};
    std::size_t validReads{};
    std::thread writer([&] {
        for (std::uint32_t generation = 1; generation <= 20000; ++generation) {
            const float value = static_cast<float>(generation % 1000);
            const std::array peaks{mixer::StereoPeak{value, value + 1.0F},
                                   mixer::StereoPeak{value + 2.0F, value + 3.0F}};
            exchange.publish(ids, peaks, {value + 4.0F, value + 5.0F});
        }
        finished.store(true, std::memory_order_release);
    });
    do {
        const auto snapshot = exchange.snapshot();
        if (snapshot.trackCount == 0) {
            continue;
        }
        ++validReads;
        check(snapshot.trackCount == 2 &&
                  snapshot.tracks[0].track == ids[0] &&
                  snapshot.tracks[1].track == ids[1] &&
                  snapshot.tracks[0].peak.right ==
                      snapshot.tracks[0].peak.left + 1.0F &&
                  snapshot.tracks[1].peak.left ==
                      snapshot.tracks[0].peak.left + 2.0F &&
                  snapshot.master.left ==
                      snapshot.tracks[0].peak.left + 4.0F,
              "meter snapshots must never combine different RT generations");
    } while (!finished.load(std::memory_order_acquire) || validReads == 0);
    writer.join();
    const auto finalSnapshot = exchange.snapshot();
    check(validReads > 0 && finalSnapshot.trackCount == 2,
          "meter exchange stress must observe coherent published snapshots");

    std::cout << "All smoothing and metering tests passed\n";
    return EXIT_SUCCESS;
}
