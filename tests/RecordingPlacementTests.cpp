#include "vitadaw/audio/RecordingTypes.h"
#include "vitadaw/audio/RealtimeCapture.h"

#include <cstdlib>
#include <iostream>
#include <limits>

namespace {
using namespace vitadaw;
void check(bool condition, const char* message) {
    if (!condition) { std::cerr << "FAILED: " << message << '\n'; std::exit(EXIT_FAILURE); }
}

audio::RecordingPlacementSnapshot snapshot(std::int64_t automatic,
                                           std::int64_t manual = 0) {
    audio::RecordingPlacementSnapshot value;
    value.projectSampleRate = timeline::SampleRate{48000.0};
    value.deviceSampleRate = timeline::SampleRate{48000.0};
    value.latencyStatus = audio::RecordingLatencyStatus::reported;
    value.reportedInputLatencyProjectFrames = {automatic};
    value.manualOffsetProjectFrames = {manual};
    value.effectiveCompensationProjectFrames = {automatic - manual};
    return value;
}
}

int main() {
    using namespace vitadaw;
    check(audio::convertRecordingLatencyToProjectFrames(
              {256}, timeline::SampleRate{48000.0}, timeline::SampleRate{48000.0}) == timeline::ProjectFrameCount{256},
          "equal-rate input latency conversion");
    check(audio::convertRecordingLatencyToProjectFrames(
              {256}, timeline::SampleRate{44100.0}, timeline::SampleRate{48000.0}) == timeline::ProjectFrameCount{279},
          "44.1 to 48 nearest conversion");
    check(audio::convertRecordingLatencyToProjectFrames(
              {256}, timeline::SampleRate{48000.0}, timeline::SampleRate{44100.0}) == timeline::ProjectFrameCount{235},
          "48 to 44.1 nearest conversion");
    check(audio::convertRecordingLatencyToProjectFrames(
              {1}, timeline::SampleRate{2.0}, timeline::SampleRate{3.0}) == timeline::ProjectFrameCount{2},
          "nearest rounding resolves an exact positive half deterministically");
    check(!audio::convertRecordingLatencyToProjectFrames({1}, timeline::SampleRate{0.0}, timeline::SampleRate{48000.0}),
          "invalid rate degrades automatic latency");
    check(!audio::convertRecordingLatencyToProjectFrames(
              {std::numeric_limits<std::uint64_t>::max()},
              timeline::SampleRate{48000.0}, timeline::SampleRate{48000.0}),
          "unrepresentable input latency degrades automatic compensation");

    check(audio::computeRecordingPlacement({100000}, snapshot(256, -16)).start.value == 99728,
          "automatic plus negative manual offset places earlier");
    check(audio::computeRecordingPlacement({100000}, snapshot(256, 16)).start.value == 99760,
          "positive manual offset places later");
    check(audio::computeEffectiveRecordingCompensation(
              audio::RecordingLatencyStatus::unavailable, {}, {-16}) ==
              timeline::ProjectFrameCount{16},
          "shared effective compensation preserves a negative manual offset without automatic latency");
    check(audio::computeRecordingPlacement({256}, snapshot(256)).start.value == 0,
          "exact frame zero is preserved");
    const auto clamped = audio::computeRecordingPlacement({100}, snapshot(256));
    check(clamped.start.value == 0 && clamped.unappliedEarlyFrames == 156,
          "before-zero placement clamps with diagnostic");
    check(audio::computeRecordingPlacement({1000}, snapshot(256)).unappliedEarlyFrames == 0,
          "fully applicable placement reports no unapplied frames");
    auto unavailable = snapshot(0, 16);
    unavailable.latencyStatus = audio::RecordingLatencyStatus::unavailable;
    check(audio::computeRecordingPlacement({100}, unavailable).start.value == 116,
          "unknown latency retains a valid manual offset");
    unavailable.latencyStatus = audio::RecordingLatencyStatus::invalid;
    check(audio::computeRecordingPlacement({100}, unavailable).start.value == 116,
          "invalid latency retains a valid manual offset");
    const auto upper = audio::computeRecordingPlacement(
        timeline::maximumSupportedProjectFrame(), snapshot(0, 1));
    check(upper.upperBoundExceeded, "upper project-frame bound cannot wrap");

    audio::RealtimeCapture capture;
    check(capture.prepare(8), "prepare capture");
    auto frozen = snapshot(128, 0);
    audio::RecordingRequest request{1, {1}, media::AudioChannelLayout::mono, frozen};
    check(capture.prepareRequest(request), "prepare placement request");
    frozen.manualOffsetProjectFrames = {-48};
    request.placement = frozen;
    check(capture.begin(request, {512}, timeline::SampleRate{48000.0}), "begin capture");
    const auto captured = capture.snapshot();
    check(captured.placement.manualOffsetProjectFrames.value == 0 &&
              captured.placement.reportedInputLatencyProjectFrames.value == 128,
          "prepared placement snapshot remains immutable once the take starts");
    std::cout << "Recording placement tests passed\n";
    return EXIT_SUCCESS;
}
