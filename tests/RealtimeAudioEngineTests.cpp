#include "vitadaw/audio/RealtimeAudioEngine.h"

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

vitadaw::audio::PreparedTrackView mono(vitadaw::tracks::TrackId id,
                                       const std::vector<float>& samples,
                                       double sourceRate,
                                       double projectRate) {
    return {id, {{samples.data(), nullptr}}, 1,
            {static_cast<std::uint64_t>(samples.size())},
            vitadaw::timeline::SampleRate{sourceRate}, {0},
            vitadaw::timeline::sourceFramesToProjectDuration(
                {static_cast<std::uint64_t>(samples.size())},
                vitadaw::timeline::SampleRate{sourceRate},
                vitadaw::timeline::SampleRate{projectRate}),
            {0}};
}

void render(vitadaw::audio::RealtimeAudioEngine& engine,
            std::vector<float>& left, std::vector<float>& right,
            double deviceRate) {
    std::array<float*, 2> channels{left.data(), right.data()};
    engine.processBlock({channels.data(), channels.size(), left.size()},
                        vitadaw::timeline::SampleRate{deviceRate});
}

void enterOperational(vitadaw::audio::RealtimeAudioEngine& engine,
                      double deviceRate) {
    engine.deviceInitialising();
    std::array<float*, 0> noChannels{};
    engine.processBlock({noChannels.data(), 0, 0},
                        vitadaw::timeline::SampleRate{deviceRate});
    check(engine.deviceState() ==
              vitadaw::audio::DeviceProcessingState::operational,
          "callback entry should confirm an initialising device consumer");
}

void testShortResource(std::size_t sourceFrames, double sourceRate,
                       double projectRate) {
    using namespace vitadaw;
    const std::vector<float> signal(sourceFrames, 1.0F);
    const auto duration = timeline::sourceFramesToProjectDuration(
        {static_cast<std::uint64_t>(sourceFrames)},
        timeline::SampleRate{sourceRate}, timeline::SampleRate{projectRate});
    audio::RealtimeAudioEngine engine;
    const std::array tracks{mono({1}, signal, sourceRate, projectRate)};
    engine.configure({timeline::SampleRate{projectRate}, duration, tracks});
    enterOperational(engine, projectRate);
    check(engine.tryRequestPlay().accepted, "short resource should accept Play");

    const auto outputFrames = static_cast<std::size_t>(duration.value) + 3;
    std::vector<float> left(outputFrames, -1.0F), right(outputFrames, -1.0F);
    render(engine, left, right, projectRate);
    for (std::size_t frame = 0; frame < outputFrames; ++frame) {
        const auto expected = frame < static_cast<std::size_t>(duration.value)
                                  ? 0.125F
                                  : 0.0F;
        check(std::abs(left[frame] - expected) < 1.0e-6F,
              "short resource should render only through its exclusive end");
    }
    const auto ended = engine.transportSnapshot();
    check(!ended.playing && ended.position.value == duration.value,
          "short resource should stop exactly at its logical duration");
}
} // namespace

int main() {
    using namespace vitadaw;

    audio::RealtimeAudioEngine emptyEngine;
    const std::vector<audio::PreparedTrackView> noTracks;
    emptyEngine.configure({timeline::SampleRate{48000.0}, {0}, noTracks});
    enterOperational(emptyEngine, 48000.0);
    check(!emptyEngine.tryRequestPlay().accepted,
          "zero prepared tracks must reject Play coherently");

    const std::vector<float> one(4, 1.0F);
    audio::RealtimeAudioEngine engine;
    const std::array oneTrack{mono({1}, one, 4.0, 4.0)};
    engine.configure({timeline::SampleRate{4.0}, {4}, oneTrack});
    check(engine.deviceState() == audio::DeviceProcessingState::unavailable,
          "configured engine should not imply a live device");
    engine.deviceInitialising();
    check(!engine.tryRequestPlay().accepted,
          "about-to-start without a consumer must not accept Play");
    enterOperational(engine, 3.0);
    check(engine.tryRequestPlay().accepted, "one prepared track should play");
    std::vector<float> left(5, -1.0F), right(5, -1.0F);
    render(engine, left, right, 3.0);
    check(left[0] == 0.125F && left[1] == 0.125F && left[2] == 0.125F &&
              left[3] == 0.0F && left[4] == 0.0F,
          "natural end inside a block should leave the remaining output silent");
    auto ended = engine.transportSnapshot();
    check(!ended.playing && ended.position.value == ended.duration.value,
          "offline production processBlock should stop at the exclusive end");

    check(engine.tryRequestPlay().accepted,
          "Play after natural end should be accepted");
    std::vector<float> variableA(1), variableB(1);
    render(engine, variableA, variableB, 4.0);
    check(variableA[0] == 0.125F && engine.transportSnapshot().position.value == 1,
          "first variable block should render from zero");
    std::vector<float> variableC(2), variableD(2);
    render(engine, variableC, variableD, 4.0);
    check(variableC[0] == 0.125F && variableC[1] == 0.125F &&
              engine.transportSnapshot().position.value == 3,
          "variable block sizes should share one continuous clock");
    check(engine.tryRequestStop().accepted, "Stop should enqueue");
    render(engine, variableA, variableB, 4.0);
    check(engine.transportSnapshot().position.value == 0,
          "processed Stop should rewind the portable engine");

    const std::vector<float> half(2, 0.5F);
    engine.deviceUnavailable();
    const std::array twoTracks{mono({1}, one, 4.0, 4.0),
                               mono({2}, half, 2.0, 4.0)};
    engine.configure({timeline::SampleRate{4.0}, {4}, twoTracks});
    enterOperational(engine, 4.0);
    check(engine.tryRequestPlay().accepted, "two tracks should play");
    std::vector<float> mixedLeft(4), mixedRight(4);
    render(engine, mixedLeft, mixedRight, 4.0);
    check(std::abs(mixedLeft[0] - 0.1875F) < 1.0e-6F &&
              std::abs(mixedLeft[3] - 0.1875F) < 1.0e-6F,
          "same production processBlock should mix two rates offline");

    const std::vector<float> longSignal(100, 0.2F);
    engine.deviceUnavailable();
    const std::array longTrack{mono({1}, longSignal, 100.0, 100.0)};
    engine.configure({timeline::SampleRate{100.0}, {100}, longTrack});
    enterOperational(engine, 100.0);

    const auto orderedPlay = engine.tryRequestPlay();
    const auto orderedStop = engine.tryRequestStop();
    check(orderedPlay.accepted && orderedStop.accepted,
          "Play then Stop should enqueue in order");
    std::array<float*, 0> noChannels{};
    engine.processBlock({noChannels.data(), 0, 0}, timeline::SampleRate{100.0});
    check(!engine.transportSnapshot().playing &&
              engine.transportSnapshot().position.value == 0 &&
              engine.transportSnapshot().lastProcessedCommandSequence ==
                  orderedStop.sequence,
          "Play then Stop must execute as stopped at zero");

    const auto orderedStopFirst = engine.tryRequestStop();
    const auto orderedPlayLast = engine.tryRequestPlay();
    check(orderedStopFirst.accepted && orderedPlayLast.accepted,
          "Stop then Play should enqueue in order");
    engine.processBlock({noChannels.data(), 0, 0}, timeline::SampleRate{100.0});
    check(engine.transportSnapshot().playing &&
              engine.transportSnapshot().lastProcessedCommandSequence ==
                  orderedPlayLast.sequence,
          "Stop then Play must execute as playing");

    check(engine.tryRequestStop().accepted && engine.tryRequestPlay().accepted &&
              engine.tryRequestStop().accepted,
          "multiple consecutive commands should enqueue");
    engine.processBlock({noChannels.data(), 0, 0}, timeline::SampleRate{100.0});
    check(!engine.transportSnapshot().playing,
          "multiple commands must preserve exact FIFO order");

    audio::AudioCommandSequence lastAccepted{};
    for (std::size_t index = 0; index < audio::RealtimeAudioEngine::commandCapacity - 1;
         ++index) {
        const auto request = index % 2 == 0 ? engine.tryRequestPlay()
                                            : engine.tryRequestStop();
        check(request.accepted, "queue should accept every usable ring slot");
        lastAccepted = request.sequence;
    }
    check(!engine.tryRequestPlay().accepted, "full command queue should reject explicitly");
    engine.processBlock({noChannels.data(), 0, 0}, timeline::SampleRate{100.0});
    check(engine.transportSnapshot().lastProcessedCommandSequence == lastAccepted &&
              engine.transportSnapshot().playing,
          "wrapped queue commands should execute in FIFO order, not just resolve a maximum");
    const auto firstAfterFull = engine.tryRequestStop();
    check(firstAfterFull.accepted && firstAfterFull.sequence == lastAccepted + 1,
          "queue-full rejection must not consume a command sequence");
    for (std::size_t index = 1; index < audio::RealtimeAudioEngine::commandCapacity - 1;
         ++index) {
        check(engine.tryRequestStop().accepted,
              "ring queue should remain usable after index wrap-around");
    }
    engine.processBlock({noChannels.data(), 0, 0}, timeline::SampleRate{100.0});

    const auto pendingPlay = engine.tryRequestPlay();
    check(pendingPlay.accepted, "lifecycle test needs a pending Play");
    engine.deviceStopped();
    const auto stopped = engine.transportSnapshot();
    check(!stopped.playing && stopped.position.value == 0 &&
              stopped.lastProcessedCommandSequence >= pendingPlay.sequence,
          "device stop/error/reinitialisation failure must cancel pending commands");
    check(engine.deviceState() == audio::DeviceProcessingState::stopped,
          "stopped lifecycle state should remain explicit");
    engine.deviceInitialising();
    check(!engine.tryRequestPlay().accepted,
          "retained device object or callback registration cannot imply operation");
    engine.processBlock({noChannels.data(), 0, 0}, timeline::SampleRate{100.0});
    const auto readdedPlay = engine.tryRequestPlay();
    check(readdedPlay.accepted,
          "re-registered callback should accept Play only after consumer confirmation");
    engine.deviceError();
    check(engine.deviceState() == audio::DeviceProcessingState::error &&
              !engine.tryRequestPlay().accepted &&
              engine.transportSnapshot().lastProcessedCommandSequence >=
                  readdedPlay.sequence,
          "device error must reject Play and cancel a pending command");
    engine.deviceInitialising();
    engine.deviceError();
    check(engine.deviceState() == audio::DeviceProcessingState::error &&
              !engine.tryRequestPlay().accepted,
          "failed reinitialisation must end in error without pending Play");
    engine.deviceUnavailable();
    check(engine.deviceState() == audio::DeviceProcessingState::unavailable,
          "device close should end in unavailable");
    enterOperational(engine, 100.0);
    engine.processBlock({noChannels.data(), 0, 0}, timeline::SampleRate{100.0});
    check(!engine.transportSnapshot().playing,
          "a cancelled command from an old lifecycle generation must stay cancelled");

    const std::vector<float> replacement(4, 0.2F);
    engine.deviceUnavailable();
    const std::array replacementTrack{mono({1}, replacement, 4.0, 4.0)};
    engine.configure({timeline::SampleRate{4.0}, {4}, replacementTrack});
    enterOperational(engine, 4.0);
    check(engine.tryRequestPlay().accepted, "replacement resource should play");
    std::vector<float> replacementOut(1), replacementRight(1);
    render(engine, replacementOut, replacementRight, 4.0);
    check(std::abs(replacementOut[0] - 0.025F) < 1.0e-6F,
          "repeated quiescent resource replacement should publish the new view");

    // Producer/lifecycle race: either Play is rejected during the transition,
    // or it was accepted first and the transition's cancellation watermark
    // resolves it. No accepted sequence may remain pending indefinitely.
    audio::RealtimeAudioEngine racingEngine;
    const std::array racingTrack{mono({1}, longSignal, 100.0, 100.0)};
    racingEngine.configure({timeline::SampleRate{100.0}, {100}, racingTrack});
    for (int iteration = 0; iteration < 100; ++iteration) {
        enterOperational(racingEngine, 100.0);
        std::atomic<bool> go{};
        std::thread lifecycle([&] {
            while (!go.load(std::memory_order_acquire)) {
            }
            racingEngine.deviceStopped();
        });
        go.store(true, std::memory_order_release);
        const auto request = racingEngine.tryRequestPlay();
        lifecycle.join();
        const auto snapshot = racingEngine.transportSnapshot();
        check(racingEngine.deviceState() ==
                  audio::DeviceProcessingState::stopped &&
                  !snapshot.playing && snapshot.position.value == 0,
              "concurrent lifecycle transition must leave transport stopped");
        check(!request.accepted ||
                  snapshot.lastProcessedCommandSequence >= request.sequence,
              "accepted Play racing device stop must be resolved or cancelled");
        racingEngine.processBlock({noChannels.data(), 0, 0},
                                  timeline::SampleRate{100.0});
    }

    testShortResource(1, 48000.0, 44100.0);
    testShortResource(2, 44100.0, 48000.0);
    testShortResource(3, 96000.0, 48000.0);

    std::cout << "All realtime audio engine tests passed\n";
    return EXIT_SUCCESS;
}
