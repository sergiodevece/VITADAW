// VitaDAW 0.8.3-C2-2b - recording state protocol.
//
// The capture state machine (RealtimeCapture) and the way RealtimeAudioEngine
// drives it: which command may begin a take, which transitions are terminal and
// stay terminal, and what a control-thread snapshot may observe.
//
// Sections A and B fix one interleaving per case and are deterministic. Section
// C runs the callback on its own thread: before every round both threads meet
// at a rendezvous and start together, one of them delayed by a swept offset so
// that across rounds either operation comes first, and each round asserts only
// the outcomes the protocol allows - never which thread won. Each race prints
// how often each outcome occurred. The number of rounds can be raised with
// VITADAW_STATE_PROTOCOL_ROUNDS. Section C is also meant to run under
// ThreadSanitizer.

#include "vitadaw/audio/RealtimeAudioEngine.h"
#include "vitadaw/audio/RealtimeCapture.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace {
using namespace vitadaw;
using audio::RecordingFailure;
using audio::RecordingPhase;
using media::AudioChannelLayout;

constexpr timeline::SampleRate deviceRate{48000.0};

void check(bool value, const char* message) {
    if (!value) {
        std::cerr << "FAILED: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

void observed(const std::string& text) { std::cout << "OBSERVED: " << text << '\n'; }

int rounds(int fallback) {
    if (const auto* text = std::getenv("VITADAW_STATE_PROTOCOL_ROUNDS")) {
        if (const auto value = std::atoi(text); value > 0) return value;
    }
    return fallback;
}

// The placement carries the session too, so a snapshot that mixed the
// metadata of two requests would be visible.
audio::RecordingRequest take(audio::RecordingSessionId session, std::uint64_t track,
                             AudioChannelLayout layout = AudioChannelLayout::mono) {
    audio::RecordingRequest result{session, tracks::TrackId{track}, layout};
    result.placement.deviceBufferFrames = static_cast<std::uint32_t>(session);
    return result;
}

// Frame `first + i` of take `session`; exact in float, distinct per take.
std::vector<float> frames(audio::RecordingSessionId session, std::size_t first,
                          std::size_t count) {
    std::vector<float> result(count);
    for (std::size_t index = 0; index < count; ++index)
        result[index] = static_cast<float>((session % 1000) * 1000 + (first + index) % 1000);
    return result;
}

std::vector<float> joined(const std::vector<std::vector<float>>& blocks) {
    std::vector<float> result;
    for (const auto& block : blocks) result.insert(result.end(), block.begin(), block.end());
    return result;
}

// One callback block; a stereo take receives the same samples on both channels.
void push(audio::RealtimeCapture& capture, const std::vector<float>& block) {
    const std::array<const float*, 2> channels{block.data(), block.data()};
    capture.capture({channels.data(), channels.size(), block.size()});
}

std::vector<float> drainAll(audio::RealtimeCapture& capture, std::size_t channelCount = 1) {
    std::vector<float> left, right;
    std::array<float, 64> a{}, b{};
    const std::array<float*, 2> channels{a.data(), b.data()};
    while (const auto count = capture.drain({channels.data(), channelCount, a.size()})) {
        left.insert(left.end(), a.begin(), a.begin() + static_cast<std::ptrdiff_t>(count));
        right.insert(right.end(), b.begin(), b.begin() + static_cast<std::ptrdiff_t>(count));
    }
    check(channelCount == 1 || left == right, "both channels of a stereo take hold the same frames");
    return left;
}

// ===========================================================================
// A - RealtimeCapture, one interleaving per case
// ===========================================================================

// P-1: begin() starts only the session that is prepared right now, with the
// request it was prepared with. Every refusal leaves the prepared take exactly
// as it was, and a capturing take cannot be begun again.
void beginOnlyFromThePreparedSession() {
    audio::RealtimeCapture capture;
    check(capture.prepare(64), "ring prepared");
    const auto request = take(10, 3);
    check(!capture.begin(request, {5}, deviceRate) &&
              capture.snapshot().phase == RecordingPhase::idle,
          "an idle ring is not begun: begin() no longer prepares a request itself");
    check(capture.prepareRequest(request), "session 10 is prepared");
    check(!capture.prepareRequest(take(11, 3)), "a prepared take cannot be replaced");
    check(!capture.begin(take(11, 3), {5}, deviceRate),
          "another session cannot begin the prepared take");
    check(!capture.begin(take(10, 4), {5}, deviceRate),
          "a command with another track gives the claim back");
    check(!capture.begin(take(10, 3, AudioChannelLayout::stereo), {5}, deviceRate),
          "a command with another layout gives the claim back");
    check(!capture.begin(request, {5}, timeline::SampleRate{}),
          "an invalid device rate is refused");
    const auto prepared = capture.snapshot();
    check(prepared.phase == RecordingPhase::prepared && prepared.session == 10 &&
              prepared.track == tracks::TrackId{3} &&
              prepared.layout == AudioChannelLayout::mono &&
              prepared.placement.deviceBufferFrames == 10 &&
              prepared.projectStart.value == 0 && !prepared.deviceSampleRate.isValid() &&
              prepared.acceptedDeviceFrames.value == 0,
          "every refused begin left the prepared take exactly as it was");

    check(capture.begin(request, {5}, deviceRate), "the prepared session begins");
    const auto block = frames(10, 0, 4);
    push(capture, block);
    check(!capture.begin(request, {9}, deviceRate), "a capturing take cannot be begun again");
    const auto capturing = capture.snapshot();
    check(capturing.phase == RecordingPhase::capturing && capturing.session == 10 &&
              capturing.projectStart.value == 5 && capturing.deviceSampleRate == deviceRate &&
              capturing.acceptedDeviceFrames.value == 4,
          "the refused begin neither restarted the take nor moved its start");
    check(drainAll(capture) == block, "the take holds exactly its frames");
}

// A take that reached a terminal state, or was cleaned up, is never begun
// again by its own command arriving late.
void beginRefusedAfterTerminalStates() {
    {
        audio::RealtimeCapture capture;
        const auto request = take(20, 1);
        check(capture.prepare(16) && capture.prepareRequest(request), "fixture prepared");
        capture.cancel();
        check(!capture.begin(request, {}, deviceRate), "begin after a cancel is refused");
        const auto state = capture.snapshot();
        check(state.phase == RecordingPhase::failed &&
                  state.failure == RecordingFailure::cancelled && state.session == 20,
              "the cancelled take stays cancelled");
    }
    {
        audio::RealtimeCapture capture;
        const auto request = take(21, 1);
        check(capture.prepare(16) && capture.prepareRequest(request), "fixture prepared");
        check(capture.failSession(21, RecordingFailure::deviceStalled),
              "the watchdog's failure terminalizes the prepared take");
        check(!capture.begin(request, {}, deviceRate) &&
                  capture.snapshot().failure == RecordingFailure::deviceStalled,
              "begin after a session failure is refused");
    }
    {
        audio::RealtimeCapture capture;
        const auto request = take(22, 1);
        check(capture.prepare(16) && capture.prepareRequest(request) &&
                  capture.begin(request, {}, deviceRate),
              "fixture begins");
        push(capture, frames(22, 0, 3));
        capture.stop();
        check(!capture.begin(request, {}, deviceRate), "begin after Stop is refused");
        const auto state = capture.snapshot();
        check(state.phase == RecordingPhase::complete &&
                  state.failure == RecordingFailure::none &&
                  state.acceptedDeviceFrames.value == 3,
              "the completed take is unchanged");
    }
    {
        audio::RealtimeCapture capture;
        const auto request = take(23, 1);
        check(capture.prepare(16) && capture.prepareRequest(request) &&
                  capture.begin(request, {}, deviceRate),
              "fixture begins");
        push(capture, frames(23, 0, 2));
        check(capture.failSession(23, RecordingFailure::writerFailed),
              "the capturing take fails");
        check(!capture.begin(request, {}, deviceRate),
              "begin after a capturing failure is refused");
        const auto state = capture.snapshot();
        check(state.phase == RecordingPhase::failed &&
                  state.failure == RecordingFailure::writerFailed &&
                  state.acceptedDeviceFrames.value == 2,
              "the failed take is unchanged");
    }
    {
        // P-1: cleanup returned the ring to idle; the session's command is stale.
        audio::RealtimeCapture capture;
        const auto request = take(24, 1);
        check(capture.prepare(16) && capture.prepareRequest(request), "fixture prepared");
        capture.cancel();
        capture.reset();
        check(!capture.begin(request, {}, deviceRate), "begin after cleanup is refused");
        push(capture, frames(24, 0, 4));
        const auto state = capture.snapshot();
        check(state.phase == RecordingPhase::idle && state.session == 0 &&
                  drainAll(capture).empty() && capture.telemetry().captureCalls == 0,
              "a cleaned-up session is not prepared, begun or fed again");
    }
    {
        // The session travels in the status word: both ends of its range.
        audio::RealtimeCapture capture;
        check(capture.prepare(16), "fixture ring");
        check(!capture.prepareRequest(take(0, 1)), "session 0 is refused");
        check(!capture.prepareRequest(take(audio::RealtimeCapture::maximumSession + 1, 1)),
              "a session beyond the status word's tag is refused");
        const auto largest = take(audio::RealtimeCapture::maximumSession, 1);
        check(capture.prepareRequest(largest) && capture.begin(largest, {}, deviceRate) &&
                  capture.snapshot().session == audio::RealtimeCapture::maximumSession,
              "the largest session round-trips through the status word");
    }
}

// P-1: the stale command of an earlier take cannot begin the take that is
// prepared now; that take keeps its own request.
void staleBeginAgainstANewPreparedSession() {
    audio::RealtimeCapture capture;
    const auto first = take(30, 1);
    const auto second = take(31, 2, AudioChannelLayout::stereo);
    check(capture.prepare(16) && capture.prepareRequest(first), "take 30 prepared");
    capture.cancel();
    capture.reset();
    check(capture.prepareRequest(second), "take 31 prepared");
    check(!capture.begin(first, {}, deviceRate), "take 30's stale command is refused");
    const auto prepared = capture.snapshot();
    check(prepared.phase == RecordingPhase::prepared && prepared.session == 31 &&
              prepared.track == tracks::TrackId{2} &&
              prepared.layout == AudioChannelLayout::stereo &&
              prepared.placement.deviceBufferFrames == 31,
          "take 31 is still prepared with its own request");
    check(capture.begin(second, {7}, deviceRate), "take 31's own command begins it");
    const auto capturing = capture.snapshot();
    check(capturing.phase == RecordingPhase::capturing && capturing.session == 31 &&
              capturing.projectStart.value == 7,
          "take 31 is capturing");
}

// failSession() acts on the session it names, once; the first terminal
// transition keeps its reason.
void failSessionIsScopedAndFinal() {
    audio::RealtimeCapture capture;
    check(capture.prepare(16) && capture.prepareRequest(take(40, 1)), "fixture prepared");
    check(!capture.failSession(39, RecordingFailure::cancelled) &&
              !capture.failSession(41, RecordingFailure::cancelled) &&
              !capture.failSession(0, RecordingFailure::cancelled) &&
              !capture.failSession(40, RecordingFailure::none),
          "another session, session 0 and 'no failure' are refused");
    check(capture.snapshot().phase == RecordingPhase::prepared, "the take is untouched");
    check(capture.failSession(40, RecordingFailure::cancelled), "its own session fails it");
    check(!capture.failSession(40, RecordingFailure::writerFailed),
          "a second failure of the same session reports no transition");
    capture.fail(RecordingFailure::deviceLost);
    capture.stop();
    const auto state = capture.snapshot();
    check(state.phase == RecordingPhase::failed &&
              state.failure == RecordingFailure::cancelled && state.session == 40,
          "later failures and Stop leave the first terminal transition and its reason");
}

void lateCallbackAfterFailure() {
    audio::RealtimeCapture capture;
    const auto request = take(50, 1);
    check(capture.prepare(16) && capture.prepareRequest(request) &&
              capture.begin(request, {}, deviceRate),
          "fixture begins");
    const auto accepted = frames(50, 0, 4);
    push(capture, accepted);
    check(capture.failSession(50, RecordingFailure::writerFailed), "the take fails");
    push(capture, frames(50, 4, 2));
    const auto state = capture.snapshot();
    check(state.phase == RecordingPhase::failed &&
              state.failure == RecordingFailure::writerFailed &&
              state.acceptedDeviceFrames.value == 4 && capture.telemetry().captureCalls == 1,
          "a late callback is not captured and does not change the failure");
    check(drainAll(capture) == accepted, "the failed take retains exactly its accepted frames");
}

void stopBeforeAnyFrame() {
    audio::RealtimeCapture capture;
    const auto request = take(60, 1);
    check(capture.prepare(16) && capture.prepareRequest(request) &&
              capture.begin(request, {}, deviceRate),
          "fixture begins");
    capture.stop();
    const auto state = capture.snapshot();
    check(state.phase == RecordingPhase::complete &&
              state.failure == RecordingFailure::none &&
              state.acceptedDeviceFrames.value == 0 && drainAll(capture).empty(),
          "a take stopped before any frame completes empty");
}

// ===========================================================================
// B - RealtimeAudioEngine: commands consumed by the callback, one thread
// ===========================================================================
struct EngineRig {
    audio::RealtimeAudioEngine engine;

    explicit EngineRig(std::size_t capacity = 256) {
        engine.configure({timeline::SampleRate{48000.0}, {1000}, {}, {}, false});
        check(engine.prepareLegacyDeviceRate(deviceRate), "prepare device rate");
        check(engine.prepareRecordingCapture(capacity), "prepare capture ring");
        engine.deviceInitialising();
        engine.processBlock({nullptr, 0, 0}, deviceRate);
    }
    // One device callback; its input, if any, is mono.
    void callback(const std::vector<float>& mono = {}) {
        const std::array<const float*, 1> channels{mono.data()};
        engine.processBlock({mono.empty() ? nullptr : channels.data(), mono.empty() ? 0U : 1U,
                             mono.size()},
                            {nullptr, 0, mono.size()}, deviceRate);
    }
    std::vector<float> drain() {
        std::vector<float> result;
        std::array<float, 64> block{};
        const std::array<float*, 1> channels{block.data()};
        while (const auto count = engine.drainRecording({channels.data(), 1, block.size()}))
            result.insert(result.end(), block.begin(),
                          block.begin() + static_cast<std::ptrdiff_t>(count));
        return result;
    }
    // What DawApplication does after a terminal take: reset the ring, then stop
    // a transport that is still projected as running. One callback consumes
    // whatever that queued.
    void cleanUp() {
        engine.resetRecordingCapture();
        if (engine.projectedTransportSnapshot().playing)
            check(engine.tryRequestStop().accepted, "cleanup stops the transport");
        callback();
    }
    void stopTake() {
        check(engine.tryRequestStop().accepted, "Stop accepted");
        callback();
    }
    audio::RecordingSnapshot state() const { return engine.recordingSnapshot(); }
};

// P-1: the take is cancelled before its first callback and cleaned up; the
// beginRecord still queued is consumed when the device resumes. It is refused
// on its own: this case queues no Stop that could hide a revival.
void staleBeginAfterCleanup() {
    EngineRig rig;
    check(rig.engine.tryRequestRecord(take(70, 1)).accepted, "Record accepted; beginRecord queued");
    check(rig.engine.tryCancelRecording(), "the prepared take is cancelled");
    rig.engine.resetRecordingCapture();
    check(rig.state().phase == RecordingPhase::idle, "cleanup returned the ring to idle");
    rig.callback(frames(70, 0, 32));
    const auto state = rig.state();
    check(state.phase == RecordingPhase::idle && state.session == 0 &&
              rig.engine.recordingTelemetry().captureCalls == 0 &&
              !rig.engine.transportSnapshot().playing,
          "the stale beginRecord neither revives the take nor starts the transport");
}

// P-1: queue [beginRecord 80 (stale), Stop, beginRecord 81]. Take 80 was
// cancelled and cleaned up, and take 81 was prepared, before any callback ran.
void staleBeginCannotStartANewPreparedSession() {
    EngineRig rig;
    check(rig.engine.tryRequestRecord(take(80, 1)).accepted, "take 80 accepted");
    check(rig.engine.tryCancelRecording(), "take 80 cancelled before its first callback");
    rig.engine.resetRecordingCapture();
    check(rig.engine.projectedTransportSnapshot().playing &&
              rig.engine.tryRequestStop().accepted,
          "the application stops the transport projected by the queued beginRecord");
    check(rig.engine.tryRequestRecord(take(81, 2)).accepted,
          "take 81 is prepared while beginRecord 80 is still queued");
    const auto block = frames(81, 0, 24);
    rig.callback(block);
    const auto state = rig.state();
    check(state.phase == RecordingPhase::capturing && state.session == 81 &&
              state.track == tracks::TrackId{2} && state.failure == RecordingFailure::none &&
              state.acceptedDeviceFrames.value == 24 && rig.engine.transportSnapshot().playing,
          "only take 81 began, with its own request, and the transport records it");
    rig.stopTake();
    check(rig.state().phase == RecordingPhase::complete && rig.drain() == block,
          "take 81 holds exactly its own frames");
}

// A Cancel queued for take 90 is still in the queue when take 90 fails on the
// control thread, is cleaned up, and take 91 is prepared:
// queue [cancelRecord 90, beginRecord 91]. The stale Cancel must neither fail
// take 91 nor keep it from beginning.
void staleCancelCannotFailANewSession() {
    EngineRig rig;
    check(rig.engine.tryRequestRecord(take(90, 1)).accepted, "take 90 accepted");
    const auto first = frames(90, 0, 16);
    rig.callback(first);
    check(rig.state().phase == RecordingPhase::capturing, "take 90 is capturing");
    check(rig.engine.tryCancelRecording(), "the Cancel of take 90 is queued for the callback");
    check(rig.engine.failRecording(90, RecordingFailure::writerFailed),
          "the writer fails take 90 before any callback consumes the Cancel");
    check(rig.drain() == first, "take 90's frames are drained");
    rig.engine.resetRecordingCapture();
    check(!rig.engine.projectedTransportSnapshot().playing,
          "the queued Cancel is projected as a stop, so the application sends no Stop");
    check(rig.engine.tryRequestRecord(take(91, 2)).accepted, "take 91 is prepared");
    const auto second = frames(91, 0, 16);
    rig.callback(second);
    const auto state = rig.state();
    check(state.phase == RecordingPhase::capturing && state.session == 91 &&
              state.failure == RecordingFailure::none &&
              state.acceptedDeviceFrames.value == 16 && rig.engine.transportSnapshot().playing,
          "the stale Cancel of take 90 left take 91 to begin and record");
    rig.stopTake();
    check(rig.state().phase == RecordingPhase::complete && rig.drain() == second,
          "take 91 completes with exactly its own frames");
}

void cancelBeforeFirstCallback() {
    EngineRig rig;
    check(rig.engine.tryRequestRecord(take(100, 1)).accepted, "Record accepted");
    check(rig.engine.tryCancelRecording(), "Cancel accepted without a callback");
    auto state = rig.state();
    check(state.phase == RecordingPhase::failed &&
              state.failure == RecordingFailure::cancelled && state.session == 100,
          "the take is cancelled without waiting for a callback");
    rig.callback(frames(100, 0, 32));
    state = rig.state();
    check(state.phase == RecordingPhase::failed &&
              state.failure == RecordingFailure::cancelled &&
              state.acceptedDeviceFrames.value == 0 && !rig.engine.transportSnapshot().playing,
          "the consumed beginRecord neither begins the take nor starts the transport");
    rig.cleanUp();
    check(rig.engine.tryRequestRecord(take(101, 1)).accepted, "the next take is accepted");
    const auto block = frames(101, 0, 32);
    rig.callback(block);
    rig.stopTake();
    check(rig.state().phase == RecordingPhase::complete && rig.drain() == block,
          "the next take records normally");
}

// One callback consumes beginRecord and Stop together.
void stopBeforeFirstCallback() {
    EngineRig rig;
    check(rig.engine.tryRequestRecord(take(110, 1)).accepted, "Record accepted");
    check(rig.engine.tryRequestStop().accepted, "Stop accepted before any callback");
    check(rig.state().phase == RecordingPhase::prepared, "the take is still prepared");
    rig.callback(frames(110, 0, 32));
    const auto state = rig.state();
    check(state.phase == RecordingPhase::complete &&
              state.failure == RecordingFailure::none &&
              state.acceptedDeviceFrames.value == 0 &&
              !rig.engine.transportSnapshot().playing && rig.drain().empty(),
          "the take completes with zero frames and the transport is stopped");
}

void lateCallbackAfterEngineFailure() {
    EngineRig rig;
    check(rig.engine.tryRequestRecord(take(120, 1)).accepted, "Record accepted");
    const auto accepted = frames(120, 0, 32);
    rig.callback(accepted);
    check(rig.engine.failRecording(120, RecordingFailure::deviceStalled),
          "the watchdog's failure");
    rig.callback(frames(120, 32, 32));
    rig.callback(frames(120, 64, 32));
    const auto state = rig.state();
    check(state.phase == RecordingPhase::failed &&
              state.failure == RecordingFailure::deviceStalled &&
              state.acceptedDeviceFrames.value == 32 &&
              rig.engine.recordingTelemetry().captureCalls == 1,
          "late callbacks are not captured and keep the failure");
    check(rig.drain() == accepted, "the failed take retains exactly its accepted frames");
}

void healthyFirstTakeAndSilence() {
    EngineRig rig;
    check(rig.engine.tryRequestRecord(take(130, 1)).accepted, "first take accepted");
    std::vector<std::vector<float>> blocks;
    for (std::size_t index = 0; index < 4; ++index) {
        blocks.push_back(frames(130, index * 32, 32));
        rig.callback(blocks.back());
    }
    rig.stopTake();
    const auto first = rig.state();
    const auto telemetry = rig.engine.recordingTelemetry();
    check(first.phase == RecordingPhase::complete && first.failure == RecordingFailure::none &&
              first.deviceSampleRate == deviceRate && first.acceptedDeviceFrames.value == 128 &&
              telemetry.droppedFrames == 0 && telemetry.overflowEvents == 0,
          "the first take of a fresh engine completes without drops");
    check(rig.drain() == joined(blocks), "the first take holds exactly its frames");

    rig.cleanUp();
    check(rig.engine.tryRequestRecord(take(131, 1)).accepted, "silent take accepted");
    const std::vector<float> silence(32, 0.0F);
    for (int index = 0; index < 4; ++index) rig.callback(silence);
    rig.stopTake();
    const auto silent = rig.state();
    const auto pcm = rig.drain();
    check(silent.phase == RecordingPhase::complete &&
              silent.failure == RecordingFailure::none && pcm.size() == 128 &&
              std::all_of(pcm.begin(), pcm.end(), [](float sample) { return sample == 0.0F; }),
          "digital silence is captured like any other input");
}

// Every ending the application meets, in rotation, on one engine. Nothing of
// one take leaks into the next.
void repeatedRecordFailRecoveryCycles() {
    EngineRig rig;
    audio::RecordingSessionId session = 200;
    for (int cycle = 0; cycle < 30; ++cycle) {
        ++session;
        check(rig.engine.tryRequestRecord(take(session, 1 + static_cast<std::uint64_t>(cycle % 3)))
                  .accepted,
              "every cycle's Record is accepted");
        std::vector<float> expected;
        auto phase = RecordingPhase::complete;
        auto failure = RecordingFailure::none;
        switch (cycle % 5) {
        case 0: { // a normal take
            std::vector<std::vector<float>> blocks;
            for (std::size_t index = 0; index < 3; ++index) {
                blocks.push_back(frames(session, index * 32, 32));
                rig.callback(blocks.back());
            }
            rig.stopTake();
            expected = joined(blocks);
            break;
        }
        case 1: // Cancel before the first callback
            check(rig.engine.tryCancelRecording(), "Cancel");
            rig.callback(frames(session, 0, 32));
            phase = RecordingPhase::failed;
            failure = RecordingFailure::cancelled;
            break;
        case 2: // writer failure while capturing, then late callbacks
            expected = frames(session, 0, 32);
            rig.callback(expected);
            check(rig.engine.failRecording(session, RecordingFailure::writerFailed),
                  "writer failure");
            rig.callback(frames(session, 32, 32));
            phase = RecordingPhase::failed;
            failure = RecordingFailure::writerFailed;
            break;
        case 3: // device lost while capturing, then the device starts again
            expected = frames(session, 0, 32);
            rig.callback(expected);
            rig.engine.deviceStopped();
            rig.engine.deviceInitialising();
            rig.callback();
            phase = RecordingPhase::failed;
            failure = RecordingFailure::deviceLost;
            break;
        default: // Stop before the first callback
            check(rig.engine.tryRequestStop().accepted, "Stop");
            rig.callback(frames(session, 0, 32));
            break;
        }
        const auto state = rig.state();
        check(state.session == session && state.phase == phase && state.failure == failure &&
                  state.acceptedDeviceFrames.value == expected.size(),
              "each cycle ends in its own terminal state");
        check(rig.drain() == expected, "each cycle retains exactly its own frames");
        rig.cleanUp();
        const auto telemetry = rig.engine.recordingTelemetry();
        check(rig.state().phase == RecordingPhase::idle && telemetry.captureCalls == 0 &&
                  telemetry.producedFrames == 0 && telemetry.consumedFrames == 0 &&
                  !rig.engine.transportSnapshot().playing,
              "cleanup leaves an idle ring, zeroed counters and a stopped transport");
    }
}

// ===========================================================================
// C - concurrency: allowed outcomes only
// ===========================================================================

using Clock = std::chrono::steady_clock;

// A volatile counter: an optimizer may not delete the loop.
void spin(std::uint32_t iterations) noexcept {
    for (volatile std::uint32_t index = 0; index < iterations; index = index + 1) {
    }
}

// Spin iterations per microsecond on this build and machine, measured once, so
// that an offset means the same time in Debug, optimized and sanitizer builds.
double spinsPerMicrosecond() {
    static const double value = [] {
        constexpr std::uint32_t probe = 1U << 22;
        const auto started = Clock::now();
        spin(probe);
        const auto elapsed =
            std::chrono::duration<double, std::micro>(Clock::now() - started).count();
        return probe / std::max(elapsed, 1.0);
    }();
    return value;
}

void waitUntil(Clock::time_point when) noexcept {
    while (Clock::now() < when) {
    }
}

// Two-party barrier; it also orders each round's set-up and verification
// against the other thread (acquire/release). The thread that would leave first
// - the one that arrives last - instead chooses a start time shortly ahead, and
// both threads return it, so that neither gains the head start of releasing the
// other: the caller waits until then.
class Rendezvous {
public:
    Clock::time_point arriveAndWait() noexcept {
        const auto phase = phase_.load(std::memory_order_acquire);
        if (waiting_.fetch_add(1, std::memory_order_acq_rel) == 1) {
            waiting_.store(0, std::memory_order_relaxed);
            start_.store((Clock::now() + std::chrono::microseconds(20)).time_since_epoch().count(),
                         std::memory_order_relaxed);
            phase_.fetch_add(1, std::memory_order_acq_rel);
        } else {
            for (std::uint32_t spins = 1; phase_.load(std::memory_order_acquire) == phase; ++spins)
                if (spins % 65536 == 0) std::this_thread::yield();
        }
        return Clock::time_point{Clock::duration{start_.load(std::memory_order_relaxed)}};
    }

private:
    std::atomic<std::uint32_t> waiting_{};
    std::atomic<std::uint32_t> phase_{};
    std::atomic<Clock::rep> start_{};
};

// From the common start, one of the two threads is delayed by an offset taken
// from a fixed pseudo-random sweep of [0, range) microseconds, alternating
// sides, so that across rounds either operation comes first - including by the
// few nanoseconds that decide a collision.
struct StartOffsets {
    std::uint32_t callback{};
    std::uint32_t control{};
};

StartOffsets sweep(int round, double rangeMicroseconds) {
    if (rangeMicroseconds <= 0.0) return {};
    const auto range = static_cast<std::uint64_t>(
        std::max(1.0, rangeMicroseconds * spinsPerMicrosecond()));
    const auto slot = static_cast<std::uint64_t>(round) * 2654435761ULL % (2 * range);
    return slot < range ? StartOffsets{static_cast<std::uint32_t>(slot), 0}
                        : StartOffsets{0, static_cast<std::uint32_t>(slot - range)};
}

// `prepare` and `verify` run on this (control) thread while the callback
// thread waits; `callback` and `control` then run concurrently, from a common
// start time, each after its offset for the round.
template <typename Prepare, typename Callback, typename Control, typename Verify>
void runRounds(int count, double offsetRangeMicroseconds, Prepare prepare, Callback callback,
               Control control, Verify verify) {
    Rendezvous start, end;
    std::thread audioThread([&] {
        for (int round = 0; round < count; ++round) {
            waitUntil(start.arriveAndWait());
            spin(sweep(round, offsetRangeMicroseconds).callback);
            callback(round);
            end.arriveAndWait();
        }
    });
    for (int round = 0; round < count; ++round) {
        prepare(round);
        waitUntil(start.arriveAndWait());
        spin(sweep(round, offsetRangeMicroseconds).control);
        control(round);
        end.arriveAndWait();
        verify(round);
    }
    audioThread.join();
}

// "A terminal transition is never lost to a concurrent begin()." The callback
// begins the prepared take and feeds it one block while the control thread
// fails it - session-scoped in even rounds, lifecycle-wide in odd ones.
// Allowed: begin() returns true or false, and the block is captured or not.
// Required: the take ends failed with the control thread's reason and retains
// exactly the frames it accepted. A failure that lands while begin() holds
// "starting" is recognisable afterwards: begin() returned false, yet the start
// position it writes after its claim is already there.
void failureRacesBegin() {
    audio::RealtimeCapture capture;
    check(capture.prepare(256), "ring prepared");
    audio::RecordingSessionId session = 1000;
    audio::RecordingRequest request;
    std::vector<float> block;
    bool begun{};
    bool transitioned{};
    std::size_t begunRounds{}, capturedRounds{}, publicationLostRounds{};
    const auto count = rounds(20000);
    runRounds(
        count, 2.0,
        [&](int) {
            capture.reset();
            ++session;
            request = take(session, 1 + session % 7);
            block = frames(session, 0, 16);
            check(capture.prepareRequest(request), "round prepared");
        },
        [&](int) {
            begun = capture.begin(request, {static_cast<std::int64_t>(session)}, deviceRate);
            if (begun) push(capture, block);
        },
        [&](int round) {
            if (round % 2 == 0) {
                transitioned = capture.failSession(session, RecordingFailure::cancelled);
            } else {
                capture.fail(RecordingFailure::deviceLost);
                transitioned = true;
            }
        },
        [&](int round) {
            const auto state = capture.snapshot();
            const auto reason = round % 2 == 0 ? RecordingFailure::cancelled
                                               : RecordingFailure::deviceLost;
            check(transitioned && state.phase == RecordingPhase::failed &&
                      state.failure == reason && state.session == session,
                  "the failure is never lost, whatever begin() did");
            const auto pcm = drainAll(capture);
            check(pcm.size() == state.acceptedDeviceFrames.value &&
                      (pcm.empty() || (begun && pcm == block)),
                  "the failed take retains exactly the frames it accepted");
            begunRounds += begun ? 1 : 0;
            capturedRounds += pcm.empty() ? 0 : 1;
            publicationLostRounds +=
                !begun && state.projectStart.value == static_cast<std::int64_t>(session) ? 1 : 0;
        });
    observed("failure versus begin, " + std::to_string(count) + " rounds: begin() succeeded in " +
             std::to_string(begunRounds) + " (a block captured before the failure in " +
             std::to_string(capturedRounds) + "); the failure landed while begin() held "
             "\"starting\" in " + std::to_string(publicationLostRounds) +
             "; every round ended failed with its reason");
}

// The control thread fails the take and, without waiting for the callback,
// runs the cleanup (reset) and prepares the next take, while the callback may
// still be inside begin() for the failed one. Allowed: begin() true or false.
// Required: the next take is prepared with exactly its own request, and the
// last one records normally. Under ThreadSanitizer this case also shows that
// whatever begin() reads of the request metadata during the cleanup is
// race-free.
void cleanupRacesBegin() {
    audio::RealtimeCapture capture;
    check(capture.prepare(256), "ring prepared");
    audio::RecordingSessionId session = 40000;
    auto current = take(session, 1);
    check(capture.prepareRequest(current), "the first take is prepared");
    audio::RecordingRequest next;
    std::vector<float> block;
    bool begun{};
    std::int64_t failedStart{};
    std::size_t begunRounds{}, publicationLostRounds{};
    const auto count = rounds(20000);
    runRounds(
        count, 2.0,
        [&](int) {
            next = take(session + 1, 1 + (session + 1) % 7,
                        (session + 1) % 2 == 0 ? AudioChannelLayout::mono
                                               : AudioChannelLayout::stereo);
            block = frames(session, 0, 16);
        },
        [&](int) {
            begun = capture.begin(current, {static_cast<std::int64_t>(session)}, deviceRate);
            if (begun) push(capture, block);
        },
        [&](int) {
            check(capture.failSession(session, RecordingFailure::cancelled),
                  "the take is failed, whatever begin() did");
            failedStart = capture.snapshot().projectStart.value;
            capture.reset();
            check(capture.prepareRequest(next), "the next take is prepared at once");
        },
        [&](int) {
            const auto state = capture.snapshot();
            check(state.phase == RecordingPhase::prepared && state.session == next.session &&
                      state.track == next.track && state.layout == next.layout &&
                      state.placement.deviceBufferFrames == next.placement.deviceBufferFrames &&
                      state.projectStart.value == 0 && !state.deviceSampleRate.isValid() &&
                      state.acceptedDeviceFrames.value == 0,
                  "the next take is prepared with exactly its own request");
            begunRounds += begun ? 1 : 0;
            publicationLostRounds +=
                !begun && failedStart == static_cast<std::int64_t>(session) ? 1 : 0;
            current = next;
            ++session;
        });
    check(capture.begin(current, {1}, deviceRate), "the last prepared take begins");
    const auto last = frames(session, 0, 32);
    push(capture, last);
    capture.stop();
    check(capture.snapshot().phase == RecordingPhase::complete &&
              drainAll(capture, static_cast<std::size_t>(media::channelCount(current.layout))) ==
                  last,
          "and records exactly its own frames on the same ring");
    observed("cleanup versus begin, " + std::to_string(count) + " rounds: begin() succeeded in " +
             std::to_string(begunRounds) + "; the failure and cleanup ran while begin() held "
             "\"starting\" in at least " + std::to_string(publicationLostRounds) +
             "; every next take was prepared intact");
}

// Stop (callback) versus a session failure (control) on a capturing take:
// exactly one terminal transition, and failSession() reports whether it was its
// own. Either may win.
void stopRacesFailure() {
    audio::RealtimeCapture capture;
    check(capture.prepare(64), "ring prepared");
    audio::RecordingSessionId session = 5000;
    audio::RecordingRequest request;
    Rendezvous begun;
    bool began{};
    bool failedByControl{};
    std::size_t completedRounds{}, failedRounds{};
    const auto count = rounds(20000);
    runRounds(
        count, 0.0,
        [&](int) {
            capture.reset();
            request = take(++session, 2);
            check(capture.prepareRequest(request), "round prepared");
        },
        [&](int round) {
            began = capture.begin(request, {}, deviceRate);
            waitUntil(begun.arriveAndWait());
            spin(sweep(round, 2.0).callback);
            capture.stop();
        },
        [&](int round) {
            waitUntil(begun.arriveAndWait());
            spin(sweep(round, 2.0).control);
            failedByControl = capture.failSession(session, RecordingFailure::writerFailed);
        },
        [&](int) {
            const auto state = capture.snapshot();
            const bool completed = state.phase == RecordingPhase::complete &&
                                   state.failure == RecordingFailure::none;
            const bool failed = state.phase == RecordingPhase::failed &&
                                state.failure == RecordingFailure::writerFailed;
            check(began && state.session == session && completed != failed &&
                      failedByControl == failed,
                  "exactly one terminal transition, reported by the call that made it");
            (completed ? completedRounds : failedRounds) += 1;
        });
    observed("Stop versus failure, " + std::to_string(count) + " rounds: " +
             std::to_string(completedRounds) + " completed, " + std::to_string(failedRounds) +
             " failed");
}

// snapshot() while the callback begins a take and feeds it. Every snapshot
// describes one take, whole: this round's session, track, layout and
// placement; no begin values while prepared; once begun, its begin values and
// a whole, non-decreasing frame count; never prepared again after capturing.
void snapshotDuringBegin() {
    audio::RealtimeCapture capture;
    check(capture.prepare(512), "ring prepared");
    constexpr std::size_t blockCount = 8;
    constexpr std::size_t blockFrames = 16;
    audio::RecordingSessionId session = 9000;
    audio::RecordingRequest request;
    std::vector<std::vector<float>> blocks;
    std::atomic<bool> finished{};
    std::size_t snapshots{}, preparedSnapshots{}, begunSnapshots{}, roundsWithBoth{};
    const auto count = rounds(4000);
    runRounds(
        count, 0.0,
        [&](int) {
            capture.reset();
            ++session;
            request = take(session, 1 + session % 5,
                           session % 2 == 0 ? AudioChannelLayout::mono
                                            : AudioChannelLayout::stereo);
            blocks.clear();
            for (std::size_t index = 0; index < blockCount; ++index)
                blocks.push_back(frames(session, index * blockFrames, blockFrames));
            finished.store(false, std::memory_order_relaxed);
            check(capture.prepareRequest(request), "round prepared");
        },
        [&](int) {
            if (capture.begin(request, {static_cast<std::int64_t>(session * 3)}, deviceRate))
                for (const auto& block : blocks) push(capture, block);
            capture.stop();
            finished.store(true, std::memory_order_release);
        },
        [&](int) {
            bool sawPrepared{}, sawBegun{};
            std::uint64_t lastAccepted{};
            for (bool done = false; !done;) {
                done = finished.load(std::memory_order_acquire);
                const auto state = capture.snapshot();
                ++snapshots;
                check(state.session == session && state.track == request.track &&
                          state.layout == request.layout &&
                          state.placement.deviceBufferFrames ==
                              request.placement.deviceBufferFrames &&
                          state.failure == RecordingFailure::none,
                      "every snapshot describes this round's take, whole");
                if (state.phase == RecordingPhase::prepared) {
                    check(!sawBegun && state.projectStart.value == 0 &&
                              !state.deviceSampleRate.isValid() &&
                              state.acceptedDeviceFrames.value == 0,
                          "a prepared snapshot has no begin values and never follows capturing");
                    sawPrepared = true;
                    ++preparedSnapshots;
                    continue;
                }
                const auto accepted = state.acceptedDeviceFrames.value;
                check((state.phase == RecordingPhase::capturing ||
                       state.phase == RecordingPhase::complete) &&
                          state.projectStart.value == static_cast<std::int64_t>(session * 3) &&
                          state.deviceSampleRate == deviceRate && accepted >= lastAccepted &&
                          accepted % blockFrames == 0 && accepted <= blockCount * blockFrames,
                      "a begun snapshot carries its begin values and a whole, growing count");
                lastAccepted = accepted;
                sawBegun = true;
                ++begunSnapshots;
            }
            roundsWithBoth += sawPrepared && sawBegun ? 1 : 0;
        },
        [&](int) {
            const auto state = capture.snapshot();
            check(state.phase == RecordingPhase::complete &&
                      state.acceptedDeviceFrames.value == blockCount * blockFrames,
                  "the take completes with every block");
            check(drainAll(capture, static_cast<std::size_t>(media::channelCount(request.layout))) ==
                      joined(blocks),
                  "and holds exactly its own frames");
        });
    observed("snapshot during begin, " + std::to_string(count) + " rounds: " +
             std::to_string(snapshots) + " snapshots (" + std::to_string(preparedSnapshots) +
             " prepared, " + std::to_string(begunSnapshots) + " begun); " +
             std::to_string(roundsWithBoth) + " rounds observed the transition itself");
}

// The application's Cancel (even rounds) or the watchdog's failure (odd rounds)
// races the callback that consumes beginRecord and captures one block.
// Allowed: begin is refused, or the take begins and is failed directly or
// through the queued cancel command. Required: the control call succeeds; the
// take ends failed with its reason and exactly its accepted frames; after the
// application's cleanup the ring is idle and the transport stopped.
void cancelRacesBeginOnTheEngine() {
    EngineRig rig;
    audio::RecordingSessionId session = 20000;
    std::vector<float> block;
    bool controlResult{};
    std::size_t withFrames{}, withoutFrames{};
    const auto count = rounds(3000);
    runRounds(
        count, 4.0,
        [&](int) {
            rig.cleanUp();
            check(rig.state().phase == RecordingPhase::idle &&
                      !rig.engine.transportSnapshot().playing,
                  "each round starts idle and stopped");
            block = frames(++session, 0, 32);
            check(rig.engine.tryRequestRecord(take(session, 1)).accepted, "round Record accepted");
        },
        [&](int) { rig.callback(block); },
        [&](int round) {
            controlResult = round % 2 == 0
                ? rig.engine.tryCancelRecording()
                : rig.engine.failRecording(session, RecordingFailure::deviceStalled);
        },
        [&](int round) {
            check(controlResult, "the cancel or failure is accepted");
            rig.callback(); // consumes a cancel command queued if the take had begun
            const auto state = rig.state();
            const auto reason = round % 2 == 0 ? RecordingFailure::cancelled
                                               : RecordingFailure::deviceStalled;
            check(state.session == session && state.phase == RecordingPhase::failed &&
                      state.failure == reason,
                  "the take always ends failed with the control thread's reason");
            const auto pcm = rig.drain();
            check(pcm.size() == state.acceptedDeviceFrames.value && (pcm.empty() || pcm == block),
                  "it retains exactly the frames it accepted");
            (pcm.empty() ? withoutFrames : withFrames) += 1;
        });
    rig.cleanUp();
    check(rig.state().phase == RecordingPhase::idle && !rig.engine.transportSnapshot().playing,
          "after the last round the engine is idle and stopped");
    check(rig.engine.tryRequestRecord(take(++session, 1)).accepted, "a normal take follows");
    const auto last = frames(session, 0, 32);
    rig.callback(last);
    rig.stopTake();
    check(rig.state().phase == RecordingPhase::complete && rig.drain() == last,
          "and records exactly its frames");
    observed("Cancel/failure versus beginRecord on the engine, " + std::to_string(count) +
             " rounds: " + std::to_string(withFrames) + " ended with the block captured, " +
             std::to_string(withoutFrames) + " without");
}

} // namespace

int main() {
    beginOnlyFromThePreparedSession();
    beginRefusedAfterTerminalStates();
    staleBeginAgainstANewPreparedSession();
    failSessionIsScopedAndFinal();
    lateCallbackAfterFailure();
    stopBeforeAnyFrame();
    staleBeginAfterCleanup();
    staleBeginCannotStartANewPreparedSession();
    staleCancelCannotFailANewSession();
    cancelBeforeFirstCallback();
    stopBeforeFirstCallback();
    lateCallbackAfterEngineFailure();
    healthyFirstTakeAndSilence();
    repeatedRecordFailRecoveryCycles();
    failureRacesBegin();
    cleanupRacesBegin();
    stopRacesFailure();
    snapshotDuringBegin();
    cancelRacesBeginOnTheEngine();
    std::cout << "Recording state protocol tests passed\n";
}
