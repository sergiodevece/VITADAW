// VitaDAW 0.8.3-B2 - recording service-latency diagnostics.
//
// Unit tests for the development-only accumulator. Time is injected, so every
// gap is exact and nothing sleeps. The capture ring itself is covered by the
// 0.8.3-B endurance suite; here the ring telemetry is a plain input value.

#include "vitadaw/platform/juce/RecordingServiceDiagnostics.h"

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <string>

namespace {
using namespace vitadaw;
using namespace vitadaw::platform::juce_adapter;
using Clock = RecordingServiceDiagnostics::Clock;
using std::chrono::milliseconds;
using std::chrono::microseconds;

void check(bool value, const char* message) {
    if (!value) {
        std::cerr << "FAILED: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

constexpr double rate = 48000.0;
constexpr double nominal = 1.0 / 30.0;

audio::RecordingCaptureTelemetry ring(std::size_t peak, std::uint64_t dropped = 0,
                                      std::uint64_t overflows = 0) {
    audio::RecordingCaptureTelemetry telemetry;
    telemetry.ringCapacityFrames = 96000;
    telemetry.peakOccupancyFrames = peak;
    telemetry.minimumHeadroomFrames = peak < 96000 ? 96000 - peak : 0;
    telemetry.peakServiceBacklogFrames = peak;
    telemetry.lateServiceEvents = 7;
    telemetry.droppedFrames = dropped;
    telemetry.overflowEvents = overflows;
    return telemetry;
}

void gapBucketsAndMaximum() {
    RecordingServiceDiagnostics diagnostics;
    const auto start = Clock::time_point{} + std::chrono::hours(1);
    diagnostics.begin(start, 11, rate, 512, nominal);
    // Gaps chosen to land just above, exactly on and just below each edge.
    const long long gapsMs[] = {30, 34, 50, 51, 100, 101, 250, 251, 500, 501,
                                1000, 1001, 1500, 1501, 1900, 1901, 2000, 2001};
    auto now = start;
    for (const auto gap : gapsMs) {
        now += milliseconds(gap);
        diagnostics.recordService(now, audio::RecordingPhase::capturing,
                                  audio::RecordingFailure::none);
    }
    const auto summary = diagnostics.finish(ring(4096));
    check(summary.has_value(), "an active take produces a summary");
    check(summary->serviceCalls == 18, "every service pass is counted");
    // Strictly greater than: a gap exactly on a threshold does not count.
    const std::uint64_t expected[] = {15, 13, 11, 9, 7, 5, 3, 1};
    for (std::size_t index = 0; index < recordingServiceGapThresholdsMs.size(); ++index)
        check(summary->gapsAboveThreshold[index] == expected[index],
              "each bucket counts gaps strictly above its threshold");
    check(summary->gapsAboveNominal == 17,
          "gaps above the nominal 33.3 ms period exclude the 30 ms gap");
    check(std::abs(summary->maximumGapSeconds - 2.001) < 1e-9,
          "the maximum gap is the largest single interval");
    check(std::abs(summary->durationSeconds -
                   std::chrono::duration<double>(now - start).count()) < 1e-9,
          "duration spans the Record request to the last counted pass");
}

void firstGapStartsAtRecordRequest() {
    RecordingServiceDiagnostics diagnostics;
    const auto start = Clock::time_point{} + std::chrono::hours(2);
    diagnostics.begin(start, 12, rate, 512, nominal);
    // The UI blocks 600 ms between Record and the first timer pass.
    diagnostics.recordService(start + milliseconds(600), audio::RecordingPhase::prepared,
                              audio::RecordingFailure::none);
    const auto summary = diagnostics.finish(ring(0));
    check(summary->serviceCalls == 1 && summary->gapsAboveThreshold[3] == 1 &&
              std::abs(summary->maximumGapSeconds - 0.6) < 1e-9,
          "the time before the first service pass is measured, not hidden");
}

void headroomAndClassification() {
    RecordingServiceDiagnostics diagnostics;
    const auto start = Clock::time_point{} + std::chrono::hours(3);
    diagnostics.begin(start, 13, rate, 512, nominal);
    diagnostics.recordService(start + milliseconds(33), audio::RecordingPhase::capturing,
                              audio::RecordingFailure::none);
    const auto summary = diagnostics.finish(ring(2048));
    check(summary->capacityFrames == 96000 && summary->peakOccupancyFrames == 2048 &&
              summary->remainingHeadroomFrames == 93952,
          "remaining headroom is capacity minus peak occupancy");
    check(std::abs(summary->remainingHeadroomSeconds - 93952.0 / 48000.0) < 1e-12,
          "remaining headroom in seconds uses the device rate");
    check(summary->lateServiceEvents == 7 && summary->peakServiceBacklogFrames == 2048,
          "ring telemetry is carried into the summary unchanged");
    check(summary->classification == RecordingHeadroomClass::green,
          "1.957 s of headroom is green");

    RecordingServiceDiagnostics full;
    full.begin(start, 14, rate, 512, nominal);
    const auto saturated = full.finish(ring(96000, 512, 1));
    check(saturated->remainingHeadroomFrames == 0 &&
              saturated->remainingHeadroomSeconds == 0.0 &&
              saturated->droppedFrames == 512 && saturated->overflowEvents == 1 &&
              saturated->classification == RecordingHeadroomClass::red,
          "a full ring has no headroom and an overflow is red");

    using C = RecordingHeadroomClass;
    check(classifyRecordingHeadroom(1.001, false) == C::green &&
              classifyRecordingHeadroom(1.0, false) == C::yellow &&
              classifyRecordingHeadroom(0.5, false) == C::yellow &&
              classifyRecordingHeadroom(0.499, false) == C::orange &&
              classifyRecordingHeadroom(0.2, false) == C::orange &&
              classifyRecordingHeadroom(0.199, false) == C::red &&
              classifyRecordingHeadroom(1.9, true) == C::red,
          "headroom bands are VERDE >1.0, AMARILLO 0.5-1.0, NARANJA 0.2-0.5, ROJO <0.2 or loss");
}

void resetBetweenTakes() {
    RecordingServiceDiagnostics diagnostics;
    const auto start = Clock::time_point{} + std::chrono::hours(4);
    diagnostics.begin(start, 21, rate, 512, nominal);
    diagnostics.recordService(start + milliseconds(1800), audio::RecordingPhase::capturing,
                              audio::RecordingFailure::none);
    diagnostics.recordInjectedStall();
    const auto first = diagnostics.finish(ring(90000));
    check(first->gapsAboveThreshold[6] == 0 && first->gapsAboveThreshold[5] == 1 &&
              first->injectedStalls == 1,
          "first take records its long gap and stall");

    const auto second = start + std::chrono::hours(1);
    diagnostics.begin(second, 22, 44100.0, 256, nominal);
    diagnostics.recordService(second + milliseconds(33), audio::RecordingPhase::capturing,
                              audio::RecordingFailure::none);
    const auto summary = diagnostics.finish(ring(1024));
    check(summary->session == 22 && summary->sampleRate == 44100.0 &&
              summary->deviceBufferFrames == 256 && summary->serviceCalls == 1 &&
              summary->injectedStalls == 0 && summary->gapsAboveNominal == 0 &&
              std::abs(summary->maximumGapSeconds - 0.033) < 1e-9,
          "begin discards every statistic of the previous take");
    for (const auto count : summary->gapsAboveThreshold)
        check(count == 0, "no bucket carries over from the previous take");
    check(summary->terminalPhase == audio::RecordingPhase::idle,
          "the previous terminal state does not leak into the next take");
}

void terminalAndInactiveBehaviour() {
    RecordingServiceDiagnostics diagnostics;
    check(!diagnostics.finish(ring(0)).has_value(),
          "an idle accumulator emits no summary");
    diagnostics.recordService(Clock::now(), audio::RecordingPhase::capturing,
                              audio::RecordingFailure::none);
    diagnostics.recordInjectedStall();

    const auto start = Clock::time_point{} + std::chrono::hours(5);
    diagnostics.begin(start, 31, rate, 512, nominal);
    check(diagnostics.accepting(), "a begun take accepts service passes");
    diagnostics.recordService(start + milliseconds(33), audio::RecordingPhase::capturing,
                              audio::RecordingFailure::none);
    // The pass that notices the overflow is counted with its whole gap...
    diagnostics.recordService(start + milliseconds(2133), audio::RecordingPhase::failed,
                              audio::RecordingFailure::overflow);
    check(!diagnostics.accepting(), "a terminal phase closes the measurement window");
    // ...but finalization drains after it are not service latency.
    diagnostics.recordService(start + milliseconds(2134), audio::RecordingPhase::failed,
                              audio::RecordingFailure::overflow);
    const auto summary = diagnostics.finish(ring(95744, 512, 1));
    check(summary->serviceCalls == 2 && summary->gapsAboveThreshold[7] == 1 &&
              std::abs(summary->maximumGapSeconds - 2.1) < 1e-9 &&
              std::abs(summary->durationSeconds - 2.133) < 1e-9,
          "the overflow-revealing gap is kept and later drains are ignored");
    check(summary->terminalPhase == audio::RecordingPhase::failed &&
              summary->terminalFailure == audio::RecordingFailure::overflow &&
              summary->injectedStalls == 0,
          "the summary reports the terminal reason and no stray stall");
    check(!diagnostics.finish(ring(0)).has_value(),
          "a finished take cannot be summarised twice");

    // A take cancelled inside cleanup was never seen terminal by a pass: the
    // cleanup snapshot supplies the reason. An observed reason always wins.
    diagnostics.begin(start, 32, rate, 512, nominal);
    diagnostics.recordService(start + milliseconds(33), audio::RecordingPhase::capturing,
                              audio::RecordingFailure::none);
    const auto cancelled = diagnostics.finish(ring(1024), audio::RecordingPhase::failed,
                                              audio::RecordingFailure::cancelled);
    check(cancelled->terminalPhase == audio::RecordingPhase::failed &&
              cancelled->terminalFailure == audio::RecordingFailure::cancelled,
          "an unobserved end takes its reason from the cleanup snapshot");
    diagnostics.begin(start, 33, rate, 512, nominal);
    diagnostics.recordService(start + milliseconds(33), audio::RecordingPhase::failed,
                              audio::RecordingFailure::overflow);
    const auto observed = diagnostics.finish(ring(95744, 512, 1), audio::RecordingPhase::failed,
                                             audio::RecordingFailure::cancelled);
    check(observed->terminalFailure == audio::RecordingFailure::overflow,
          "the reason a service pass observed is not overwritten by cleanup");
}

void stallPlanParsing() {
    const auto plan = parseRecordingServiceStallPlan("50, 100,250,500,1000,1500,1900,2100",
                                                     nullptr, "");
    check(plan.valid && plan.count == 8 && plan.stallsMs[0] == 50 &&
              plan.stallsMs[6] == 1900 && plan.stallsMs[7] == 2100 &&
              plan.firstAfterMs == 5000 && plan.spacingMs == 5000,
          "a stall list parses with default timing");
    const auto timed = parseRecordingServiceStallPlan("1900", "8000", "12000");
    check(timed.valid && timed.count == 1 && timed.firstAfterMs == 8000 &&
              timed.spacingMs == 12000,
          "first-stall offset and spacing are configurable");
    check(parseRecordingServiceStallPlan(nullptr, nullptr, nullptr).valid &&
              parseRecordingServiceStallPlan("", nullptr, nullptr).count == 0,
          "an absent plan is valid and empty");
    for (const char* bad : {"50,", ",50", "50,,100", "abc", "0", "10001", "50;100", "-5"})
        check(!parseRecordingServiceStallPlan(bad, nullptr, nullptr).valid &&
                  parseRecordingServiceStallPlan(bad, nullptr, nullptr).count == 0,
              "a malformed or out-of-range stall list is rejected whole");
    check(!parseRecordingServiceStallPlan("1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17",
                                          nullptr, nullptr).valid,
          "more stalls than the bounded plan holds are rejected");
    check(!parseRecordingServiceStallPlan("50", "x", nullptr).valid &&
              !parseRecordingServiceStallPlan("50", nullptr, "0").valid,
          "malformed timing is rejected");
}

void stallScheduleOrdering() {
    RecordingServiceStallSchedule schedule;
    schedule.begin(parseRecordingServiceStallPlan("50,1900", "1000", "4000"));
    check(schedule.pending() == 2, "both stalls are pending");
    check(schedule.take(milliseconds(999)) == 0, "no stall before the first offset");
    check(schedule.take(milliseconds(1000)) == 50, "first stall is due at its offset");
    check(schedule.take(milliseconds(1001)) == 0, "the second stall waits for its spacing");
    // A late pass takes one stall at a time, never several at once.
    check(schedule.take(milliseconds(9000)) == 1900 && schedule.take(milliseconds(9000)) == 0,
          "at most one stall is taken per service pass");
    check(schedule.pending() == 0, "the plan is exhausted");

    RecordingServiceStallSchedule rejected;
    rejected.begin(parseRecordingServiceStallPlan("oops", nullptr, nullptr));
    check(rejected.take(std::chrono::hours(1)) == 0 && rejected.pending() == 0,
          "an invalid plan never stalls");

    schedule.begin(parseRecordingServiceStallPlan("250", "0", nullptr));
    check(schedule.pending() == 1 && schedule.take(microseconds(0)) == 250,
          "begin rearms the schedule for the next take");
}

void summaryCarriesEveryMagnitude() {
    RecordingServiceDiagnostics diagnostics;
    const auto start = Clock::time_point{} + std::chrono::hours(6);
    diagnostics.begin(start, 41, rate, 512, nominal);
    diagnostics.recordService(start + milliseconds(40), audio::RecordingPhase::complete,
                              audio::RecordingFailure::none);
    const auto text = formatRecordingServiceSummary(*diagnostics.finish(ring(2048)));
    for (const char* label :
         {"Duration:", "Service calls:", "Nominal period:", "Maximum service gap:",
          "Gaps > nominal:", "Gaps >   50 ms:", "Gaps >  100 ms:", "Gaps >  250 ms:",
          "Gaps >  500 ms:", "Gaps > 1000 ms:", "Gaps > 1500 ms:", "Gaps > 1900 ms:",
          "Gaps > 2000 ms:", "Peak ring occupancy:", "Peak service backlog:",
          "Minimum remaining headroom:", "Late service events:", "Dropped frames:",
          "Overflow events:", "Headroom class:", "complete / none", "93952 frames (1.957 s)"})
        check(text.find(label) != std::string::npos,
              "the summary reports every mandatory magnitude");
}

} // namespace

int main() {
    gapBucketsAndMaximum();
    firstGapStartsAtRecordRequest();
    headroomAndClassification();
    resetBetweenTakes();
    terminalAndInactiveBehaviour();
    stallPlanParsing();
    stallScheduleOrdering();
    summaryCarriesEveryMagnitude();
    std::cout << "Recording service diagnostics tests passed\n";
}
