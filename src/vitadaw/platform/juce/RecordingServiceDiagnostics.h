#pragma once

// VitaDAW 0.8.3-B2 - development-only recording service-latency diagnostics.
//
// Measures, on the thread that services the recording writer, the wall-clock
// gap between consecutive service passes, and combines it with the capture
// ring telemetry at the end of a take. Everything here is plain bounded state
// with injected time: no JUCE, no clock reads, no I/O, no allocation until a
// finished take is formatted. The adapter wires it in only when the build
// defines VITADAW_RECORDING_SERVICE_DIAGNOSTICS; a normal build carries none
// of it. Nothing here is ever touched by the audio callback.

#include "vitadaw/audio/RecordingTypes.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>

namespace vitadaw::platform::juce_adapter {

// A gap is counted in every threshold it strictly exceeds.
inline constexpr std::array<std::uint32_t, 8> recordingServiceGapThresholdsMs{
    50, 100, 250, 500, 1000, 1500, 1900, 2000};

// 0.8.3-B2 acceptance bands for the worst remaining headroom of a take.
enum class RecordingHeadroomClass : std::uint8_t { green, yellow, orange, red };

[[nodiscard]] constexpr RecordingHeadroomClass classifyRecordingHeadroom(
    double remainingHeadroomSeconds, bool lossObserved) noexcept {
    if (lossObserved || remainingHeadroomSeconds < 0.2) return RecordingHeadroomClass::red;
    if (remainingHeadroomSeconds < 0.5) return RecordingHeadroomClass::orange;
    if (remainingHeadroomSeconds <= 1.0) return RecordingHeadroomClass::yellow;
    return RecordingHeadroomClass::green;
}

struct RecordingServiceSummary {
    audio::RecordingSessionId session{};
    double sampleRate{};
    std::uint32_t deviceBufferFrames{};
    double nominalPeriodSeconds{};
    // From the Record request to the last counted service pass.
    double durationSeconds{};
    std::uint64_t serviceCalls{};
    double maximumGapSeconds{};
    std::uint64_t gapsAboveNominal{};
    std::array<std::uint64_t, recordingServiceGapThresholdsMs.size()> gapsAboveThreshold{};
    // Ring telemetry read once, after the producer has stopped.
    std::size_t capacityFrames{};
    std::size_t peakOccupancyFrames{};
    std::size_t peakServiceBacklogFrames{};
    // capacity - peakOccupancy, and its duration at the device rate: how much
    // longer the service could have stalled at the worst moment of the take.
    std::size_t remainingHeadroomFrames{};
    double remainingHeadroomSeconds{};
    std::uint64_t lateServiceEvents{};
    std::uint64_t droppedFrames{};
    std::uint64_t overflowEvents{};
    std::uint64_t injectedStalls{};
    audio::RecordingPhase terminalPhase{audio::RecordingPhase::idle};
    audio::RecordingFailure terminalFailure{audio::RecordingFailure::none};
    RecordingHeadroomClass classification{RecordingHeadroomClass::green};
};

// Per-take accumulator. Single-threaded by contract: the adapter calls it
// only from the thread that services the writer.
class RecordingServiceDiagnostics {
public:
    using Clock = std::chrono::steady_clock;

    // Starts a take and discards every statistic of the previous one. `now`
    // is taken before the Record request reaches the audio callback, so the
    // first gap covers the whole time the ring is unattended after capture
    // can start.
    void begin(Clock::time_point now, audio::RecordingSessionId session,
               double sampleRate, std::uint32_t deviceBufferFrames,
               double nominalPeriodSeconds) noexcept {
        *this = RecordingServiceDiagnostics{};
        active_ = true;
        anchor_ = now;
        last_ = now;
        summary_.session = session;
        summary_.sampleRate = sampleRate;
        summary_.deviceBufferFrames = deviceBufferFrames;
        summary_.nominalPeriodSeconds = nominalPeriodSeconds;
        nominal_ = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::duration<double>(nominalPeriodSeconds));
    }

    // True while service passes still matter: the producer may be running.
    [[nodiscard]] bool accepting() const noexcept { return active_ && !terminalObserved_; }
    [[nodiscard]] Clock::time_point anchor() const noexcept { return anchor_; }

    // Records one service pass with the capture phase observed at its entry.
    // The pass that first observes a terminal phase is still counted - its gap
    // is how long the ring was unattended until the end was noticed - and
    // every later pass (finalization drains) is ignored.
    void recordService(Clock::time_point now, audio::RecordingPhase phase,
                       audio::RecordingFailure failure) noexcept {
        if (!accepting()) return;
        const auto gap = now >= last_ ? now - last_ : Clock::duration::zero();
        last_ = now;
        ++summary_.serviceCalls;
        const auto gapNs = std::chrono::duration_cast<std::chrono::nanoseconds>(gap);
        if (gapNs > maximumGap_) maximumGap_ = gapNs;
        if (gapNs > nominal_) ++summary_.gapsAboveNominal;
        for (std::size_t index = 0; index < recordingServiceGapThresholdsMs.size(); ++index)
            if (gapNs > std::chrono::milliseconds(recordingServiceGapThresholdsMs[index]))
                ++summary_.gapsAboveThreshold[index];
        if (phase == audio::RecordingPhase::complete ||
            phase == audio::RecordingPhase::failed) {
            terminalObserved_ = true;
            summary_.terminalPhase = phase;
            summary_.terminalFailure = failure;
        }
    }

    void recordInjectedStall() noexcept {
        if (active_) ++summary_.injectedStalls;
    }

    // Ends the take. Returns nothing when no take was being measured, so a
    // cleanup path that runs twice, or for a take that never started, emits
    // no summary. `finalPhase`/`finalFailure` describe the capture at cleanup
    // and are used only when no service pass observed the terminal state
    // itself (for instance a cancellation resolved inside cleanup).
    [[nodiscard]] std::optional<RecordingServiceSummary> finish(
        const audio::RecordingCaptureTelemetry& telemetry,
        audio::RecordingPhase finalPhase = audio::RecordingPhase::idle,
        audio::RecordingFailure finalFailure = audio::RecordingFailure::none) noexcept {
        if (!active_) return std::nullopt;
        auto result = summary_;
        if (!terminalObserved_) {
            result.terminalPhase = finalPhase;
            result.terminalFailure = finalFailure;
        }
        result.durationSeconds = std::chrono::duration<double>(last_ - anchor_).count();
        result.maximumGapSeconds = std::chrono::duration<double>(maximumGap_).count();
        result.capacityFrames = telemetry.ringCapacityFrames;
        result.peakOccupancyFrames = telemetry.peakOccupancyFrames;
        result.peakServiceBacklogFrames = telemetry.peakServiceBacklogFrames;
        result.remainingHeadroomFrames =
            telemetry.ringCapacityFrames > telemetry.peakOccupancyFrames
                ? telemetry.ringCapacityFrames - telemetry.peakOccupancyFrames
                : 0;
        result.remainingHeadroomSeconds = result.sampleRate > 0.0
            ? static_cast<double>(result.remainingHeadroomFrames) / result.sampleRate
            : 0.0;
        result.lateServiceEvents = telemetry.lateServiceEvents;
        result.droppedFrames = telemetry.droppedFrames;
        result.overflowEvents = telemetry.overflowEvents;
        result.classification = classifyRecordingHeadroom(
            result.remainingHeadroomSeconds,
            telemetry.droppedFrames != 0 || telemetry.overflowEvents != 0);
        active_ = false;
        return result;
    }

private:
    RecordingServiceSummary summary_;
    Clock::time_point anchor_{};
    Clock::time_point last_{};
    std::chrono::nanoseconds nominal_{};
    std::chrono::nanoseconds maximumGap_{};
    bool active_{};
    bool terminalObserved_{};
};

// ---------------------------------------------------------------------------
// Development stall plan
// ---------------------------------------------------------------------------
// A bounded list of deliberate service stalls, read once from the environment
// by a diagnostics build. Stall k becomes due `firstAfterMs + k * spacingMs`
// after the Record request; at most one stall is taken per service pass.
struct RecordingServiceStallPlan {
    static constexpr std::size_t maximumStalls = 16;
    static constexpr std::uint32_t maximumStallMs = 10000;
    static constexpr std::uint32_t maximumOffsetMs = 3600000;

    std::array<std::uint32_t, maximumStalls> stallsMs{};
    std::size_t count{};
    std::uint32_t firstAfterMs{5000};
    std::uint32_t spacingMs{5000};
    bool valid{true};
};

namespace detail {
// Parses one unsigned decimal in [minimum, maximum]; surrounding spaces are
// allowed. Returns false for anything else, including an empty token.
[[nodiscard]] inline bool parseMilliseconds(const char*& cursor, std::uint32_t minimum,
                                            std::uint32_t maximum,
                                            std::uint32_t& value) noexcept {
    while (*cursor == ' ') ++cursor;
    if (*cursor < '0' || *cursor > '9') return false;
    std::uint64_t parsed{};
    while (*cursor >= '0' && *cursor <= '9') {
        parsed = parsed * 10 + static_cast<std::uint64_t>(*cursor - '0');
        if (parsed > maximum) return false;
        ++cursor;
    }
    while (*cursor == ' ') ++cursor;
    if (parsed < minimum) return false;
    value = static_cast<std::uint32_t>(parsed);
    return true;
}

[[nodiscard]] inline bool parseSingleMilliseconds(const char* text, std::uint32_t minimum,
                                                  std::uint32_t maximum,
                                                  std::uint32_t& value) noexcept {
    if (text == nullptr || *text == '\0') return true; // keep the default
    const char* cursor = text;
    std::uint32_t parsed{};
    if (!parseMilliseconds(cursor, minimum, maximum, parsed) || *cursor != '\0') return false;
    value = parsed;
    return true;
}
} // namespace detail

// `stalls` is a comma-separated list such as "50,100,250". Any malformed,
// out-of-range or excess entry invalidates the whole plan rather than being
// silently skipped: a diagnostics run must do exactly what it was asked to.
[[nodiscard]] inline RecordingServiceStallPlan parseRecordingServiceStallPlan(
    const char* stalls, const char* firstAfterMs, const char* spacingMs) noexcept {
    RecordingServiceStallPlan plan;
    const auto invalid = [] {
        RecordingServiceStallPlan rejected;
        rejected.valid = false;
        return rejected;
    };
    if (!detail::parseSingleMilliseconds(firstAfterMs, 0,
                                         RecordingServiceStallPlan::maximumOffsetMs,
                                         plan.firstAfterMs) ||
        !detail::parseSingleMilliseconds(spacingMs, 1,
                                         RecordingServiceStallPlan::maximumOffsetMs,
                                         plan.spacingMs))
        return invalid();
    if (stalls == nullptr || *stalls == '\0') return plan;
    const char* cursor = stalls;
    for (;;) {
        if (plan.count == RecordingServiceStallPlan::maximumStalls) return invalid();
        std::uint32_t value{};
        if (!detail::parseMilliseconds(cursor, 1, RecordingServiceStallPlan::maximumStallMs,
                                       value))
            return invalid();
        plan.stallsMs[plan.count++] = value;
        if (*cursor == '\0') return plan;
        if (*cursor != ',') return invalid();
        ++cursor;
    }
}

class RecordingServiceStallSchedule {
public:
    void begin(const RecordingServiceStallPlan& plan) noexcept {
        plan_ = plan;
        next_ = 0;
    }

    // Returns the stall due `elapsed` after the Record request, or 0. At most
    // one stall is returned per call, in plan order.
    [[nodiscard]] std::uint32_t take(std::chrono::nanoseconds elapsed) noexcept {
        if (!plan_.valid || next_ >= plan_.count) return 0;
        const auto dueAt = std::chrono::milliseconds(
            static_cast<std::int64_t>(plan_.firstAfterMs) +
            static_cast<std::int64_t>(next_) * static_cast<std::int64_t>(plan_.spacingMs));
        if (elapsed < dueAt) return 0;
        return plan_.stallsMs[next_++];
    }

    [[nodiscard]] std::size_t pending() const noexcept {
        return plan_.valid && next_ < plan_.count ? plan_.count - next_ : 0;
    }

private:
    RecordingServiceStallPlan plan_;
    std::size_t next_{};
};

// ---------------------------------------------------------------------------
// Formatting (non-RT, after the take only)
// ---------------------------------------------------------------------------
[[nodiscard]] constexpr const char* recordingHeadroomClassName(
    RecordingHeadroomClass value) noexcept {
    switch (value) {
    case RecordingHeadroomClass::green: return "VERDE";
    case RecordingHeadroomClass::yellow: return "AMARILLO";
    case RecordingHeadroomClass::orange: return "NARANJA";
    case RecordingHeadroomClass::red: return "ROJO";
    }
    return "?";
}

[[nodiscard]] constexpr const char* recordingPhaseName(audio::RecordingPhase value) noexcept {
    switch (value) {
    case audio::RecordingPhase::idle: return "idle";
    case audio::RecordingPhase::prepared: return "prepared";
    case audio::RecordingPhase::capturing: return "capturing";
    case audio::RecordingPhase::finalizing: return "finalizing";
    case audio::RecordingPhase::complete: return "complete";
    case audio::RecordingPhase::failed: return "failed";
    }
    return "?";
}

[[nodiscard]] constexpr const char* recordingFailureName(audio::RecordingFailure value) noexcept {
    switch (value) {
    case audio::RecordingFailure::none: return "none";
    case audio::RecordingFailure::overflow: return "overflow";
    case audio::RecordingFailure::missingInput: return "missingInput";
    case audio::RecordingFailure::deviceLost: return "deviceLost";
    case audio::RecordingFailure::sampleRateChanged: return "sampleRateChanged";
    case audio::RecordingFailure::cancelled: return "cancelled";
    case audio::RecordingFailure::writerFailed: return "writerFailed";
    case audio::RecordingFailure::finalizationFailed: return "finalizationFailed";
    case audio::RecordingFailure::shutdown: return "shutdown";
    }
    return "?";
}

[[nodiscard]] inline std::string formatRecordingServiceSummary(
    const RecordingServiceSummary& summary) {
    std::string text;
    char line[160];
    // Every snprintf below takes a literal format, so the compiler checks each
    // one against its arguments.
    const auto take = [&](int written) {
        if (written > 0)
            text.append(line, std::min(static_cast<std::size_t>(written), sizeof line - 1));
    };
    const auto ull = [](auto value) { return static_cast<unsigned long long>(value); };
    const double peakMs = summary.sampleRate > 0.0
        ? 1000.0 * static_cast<double>(summary.peakOccupancyFrames) / summary.sampleRate
        : 0.0;
    take(std::snprintf(line, sizeof line, "Recording service diagnostics (session %llu)\n",
                       ull(summary.session)));
    take(std::snprintf(line, sizeof line, "  Terminal state:              %s / %s\n",
                       recordingPhaseName(summary.terminalPhase),
                       recordingFailureName(summary.terminalFailure)));
    take(std::snprintf(line, sizeof line, "  Device:                      %.0f Hz, %u-frame buffer\n",
                       summary.sampleRate, static_cast<unsigned>(summary.deviceBufferFrames)));
    take(std::snprintf(line, sizeof line, "  Duration:                    %.3f s\n",
                       summary.durationSeconds));
    take(std::snprintf(line, sizeof line, "  Service calls:               %llu\n",
                       ull(summary.serviceCalls)));
    take(std::snprintf(line, sizeof line, "  Nominal period:              %.1f ms\n",
                       summary.nominalPeriodSeconds * 1000.0));
    take(std::snprintf(line, sizeof line, "  Maximum service gap:         %.1f ms\n",
                       summary.maximumGapSeconds * 1000.0));
    take(std::snprintf(line, sizeof line, "  Gaps > nominal:              %llu\n",
                       ull(summary.gapsAboveNominal)));
    for (std::size_t index = 0; index < recordingServiceGapThresholdsMs.size(); ++index)
        take(std::snprintf(line, sizeof line, "  Gaps > %4u ms:              %llu\n",
                           static_cast<unsigned>(recordingServiceGapThresholdsMs[index]),
                           ull(summary.gapsAboveThreshold[index])));
    take(std::snprintf(line, sizeof line, "  Ring capacity:               %llu frames\n",
                       ull(summary.capacityFrames)));
    take(std::snprintf(line, sizeof line, "  Peak ring occupancy:         %llu frames (%.1f ms)\n",
                       ull(summary.peakOccupancyFrames), peakMs));
    take(std::snprintf(line, sizeof line, "  Peak service backlog:        %llu frames\n",
                       ull(summary.peakServiceBacklogFrames)));
    take(std::snprintf(line, sizeof line, "  Minimum remaining headroom:  %llu frames (%.3f s)\n",
                       ull(summary.remainingHeadroomFrames), summary.remainingHeadroomSeconds));
    take(std::snprintf(line, sizeof line, "  Late service events:         %llu\n",
                       ull(summary.lateServiceEvents)));
    take(std::snprintf(line, sizeof line, "  Dropped frames:              %llu\n",
                       ull(summary.droppedFrames)));
    take(std::snprintf(line, sizeof line, "  Overflow events:             %llu\n",
                       ull(summary.overflowEvents)));
    take(std::snprintf(line, sizeof line, "  Injected development stalls: %llu\n",
                       ull(summary.injectedStalls)));
    take(std::snprintf(line, sizeof line, "  Headroom class:              %s\n",
                       recordingHeadroomClassName(summary.classification)));
    return text;
}

} // namespace vitadaw::platform::juce_adapter
