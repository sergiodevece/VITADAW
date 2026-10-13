#pragma once

#include "vitadaw/audio/AudioBlockView.h"
#include "vitadaw/audio/RecordingTypes.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace vitadaw::audio {

// Portable single-producer/single-consumer PCM exchange.
//
// Threads. The audio callback is the sole producer: it calls begin(), capture()
// and stop(), and may fail() a take it cannot capture. The control thread (the
// recording service) is the sole consumer and the only caller of prepare(),
// reset(), prepareRequest(), clearPreparedRequest(), drain() and snapshot().
// Any thread may call fail() or failSession(). prepare() reallocates the ring,
// so it must run with no callback in flight; VitaDAW guarantees this by
// detaching the callback under JUCE's callback lock before every preflight.
//
// State protocol (0.8.3-C2-2b). One 64-bit status word holds the phase, the
// failure reason and the session that owns the ring, and every transition is a
// compare-and-swap on that word, except the control thread's own bookkeeping of
// an inactive ring (prepare, reset):
//   - only the session that is prepared right now can begin; a stale begin of an
//     earlier, cancelled or cleaned-up session fails without writing anything;
//   - begin() claims the ring ("starting") before touching it and publishes
//     "capturing" with a second CAS, so a failure or cancel that lands while it
//     runs is never overwritten;
//   - failSession() terminalizes only the session it names. Failures that the
//     callback detects (missing input, overflow, rate change) name the session
//     it observed capturing; fail() is for lifecycle events that end whichever
//     take is active.
// Request metadata (track, layout, placement; the session is in the status
// word) is written only by the control thread: by prepareRequest() while the
// ring is idle, published by its release CAS to "prepared", and cleared by
// reset() once the take is inactive. The fields the callback reads (track,
// layout) are atomics, because a begin() that lost its claim to a failure may
// still be reading them when cleanup rewrites them. The two values only begin()
// knows (start position and device rate) are atomics written while "starting"
// is held, and published by the release CAS to "capturing".
class RealtimeCapture final {
public:
    [[nodiscard]] bool prepare(std::size_t frameCapacity);
    void reset() noexcept;
    // Binds an idle ring to a request before its RT command is visible, so that
    // lifecycle closure and cancellation can terminalize a take whose command
    // has not reached the callback yet.
    [[nodiscard]] bool prepareRequest(const RecordingRequest&) noexcept;
    // Returns a prepared, never-begun request to idle (its command was not
    // enqueued). No effect once the callback has claimed it.
    void clearPreparedRequest() noexcept;

    // RT. Succeeds only for the session prepared right now; see above.
    [[nodiscard]] bool begin(const RecordingRequest&,
                             timeline::ProjectFramePosition,
                             timeline::SampleRate) noexcept;
    void capture(ConstAudioBlockView) noexcept;
    void stop() noexcept;
    // Terminalizes whichever take is active (prepared, starting or capturing).
    void fail(RecordingFailure) noexcept;
    // Terminalizes the take only if it is still the active session `session`.
    // Returns true if this call performed the terminal transition.
    [[nodiscard]] bool failSession(RecordingSessionId session, RecordingFailure) noexcept;
    void cancel() noexcept { fail(RecordingFailure::cancelled); }

    [[nodiscard]] std::size_t drain(AudioBlockView) noexcept;
    // Control thread only (the thread that writes request metadata).
    [[nodiscard]] RecordingSnapshot snapshot() const noexcept;
    // Bounded diagnostic counters. telemetry() is a non-RT reader; the budget
    // is the nominal frame distance between two service calls and is installed
    // only while capture is inactive.
    [[nodiscard]] RecordingCaptureTelemetry telemetry() const noexcept;
    void setServiceBudgetFrames(std::size_t frames) noexcept;

    [[nodiscard]] bool isCapturing() const noexcept {
        return statusPhase(status_.load(std::memory_order_acquire)) == StatusPhase::capturing;
    }
    // A prepared take whose begin() may be in progress on the callback.
    [[nodiscard]] bool isPrepared() const noexcept {
        const auto value = statusPhase(status_.load(std::memory_order_acquire));
        return value == StatusPhase::prepared || value == StatusPhase::starting;
    }
    [[nodiscard]] bool canPrepareRequest() const noexcept {
        return capacity_ != 0 &&
               statusPhase(status_.load(std::memory_order_acquire)) == StatusPhase::idle;
    }
    [[nodiscard]] bool isActive() const noexcept {
        return active(statusPhase(status_.load(std::memory_order_acquire)));
    }
    // The session of the active take, or 0. Lock-free; any thread.
    [[nodiscard]] RecordingSessionId activeSession() const noexcept {
        const auto value = status_.load(std::memory_order_acquire);
        return active(statusPhase(value)) ? sessionTag(value) : 0;
    }
    // The session of a capturing take, or 0. Lock-free; any thread.
    [[nodiscard]] RecordingSessionId capturingSession() const noexcept {
        const auto value = status_.load(std::memory_order_acquire);
        return statusPhase(value) == StatusPhase::capturing ? sessionTag(value) : 0;
    }
    // RT: the device rate a capturing take began with (written by begin()).
    [[nodiscard]] timeline::SampleRate capturingSampleRate() const noexcept {
        return timeline::SampleRate{deviceSampleRate_.load(std::memory_order_relaxed)};
    }
    [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }
    // Sessions are carried in the status word; larger ids are refused.
    static constexpr RecordingSessionId maximumSession = (std::uint64_t{1} << 48U) - 1U;

private:
    // "starting" is internal: begin() holds it between its claim and its
    // publication. snapshot() reports it as prepared.
    enum class StatusPhase : std::uint8_t {
        idle,
        prepared,
        starting,
        capturing,
        complete,
        failed,
    };

    void resetTelemetryCounters() noexcept;
    void resetRingIndices() noexcept;

    static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
    static_assert(std::atomic<std::int64_t>::is_always_lock_free);
    static_assert(std::atomic<double>::is_always_lock_free);
    static_assert(std::atomic<media::AudioChannelLayout>::is_always_lock_free);
    static_assert(std::atomic<tracks::TrackId>::is_always_lock_free);

    static constexpr std::uint64_t encode(StatusPhase phase, RecordingFailure failure,
                                          RecordingSessionId session) noexcept {
        return static_cast<std::uint64_t>(phase) |
               (static_cast<std::uint64_t>(failure) << 8U) |
               (static_cast<std::uint64_t>(session) << 16U);
    }
    static constexpr StatusPhase statusPhase(std::uint64_t value) noexcept {
        return static_cast<StatusPhase>(value & 0xffU);
    }
    static constexpr RecordingFailure failure(std::uint64_t value) noexcept {
        return static_cast<RecordingFailure>((value >> 8U) & 0xffU);
    }
    static constexpr RecordingSessionId sessionTag(std::uint64_t value) noexcept {
        return value >> 16U;
    }
    static constexpr bool active(StatusPhase phase) noexcept {
        return phase == StatusPhase::prepared || phase == StatusPhase::starting ||
               phase == StatusPhase::capturing;
    }
    static constexpr RecordingPhase publicPhase(StatusPhase phase) noexcept {
        switch (phase) {
        case StatusPhase::idle: return RecordingPhase::idle;
        case StatusPhase::prepared:
        case StatusPhase::starting: return RecordingPhase::prepared;
        case StatusPhase::capturing: return RecordingPhase::capturing;
        case StatusPhase::complete: return RecordingPhase::complete;
        case StatusPhase::failed: return RecordingPhase::failed;
        }
        return RecordingPhase::idle;
    }

    std::array<std::vector<float>, 2> samples_;
    std::size_t capacity_{};
    std::atomic<std::uint64_t> writeFrame_{};
    std::atomic<std::uint64_t> readFrame_{};
    std::atomic<std::uint64_t> acceptedFrames_{};
    // Producer-owned telemetry: written only by the audio callback through
    // relaxed integer atomics, and zeroed with the ring before a take begins.
    std::atomic<std::uint64_t> peakOccupancyFrames_{};
    std::atomic<std::uint64_t> droppedFrames_{};
    std::atomic<std::uint64_t> captureCalls_{};
    std::atomic<std::uint64_t> overflowEvents_{};
    // Consumer-owned telemetry: written only by the recording service.
    std::atomic<std::uint64_t> consumedFrames_{};
    std::atomic<std::uint64_t> drainCalls_{};
    std::atomic<std::uint64_t> emptyDrainCalls_{};
    std::atomic<std::uint64_t> peakServiceBacklogFrames_{};
    std::atomic<std::uint64_t> lateServiceEvents_{};
    // Non-RT configuration read by the consumer only.
    std::atomic<std::uint64_t> serviceBudgetFrames_{};
    // Phase | failure << 8 | session << 16. The single arbiter of every
    // transition; terminal snapshots are coherent because there is one word.
    std::atomic<std::uint64_t> status_{encode(StatusPhase::idle, RecordingFailure::none, 0)};

    // Request metadata, written by the control thread only: by prepareRequest()
    // while idle, published by its release CAS to "prepared"; by reset() and
    // prepare() while inactive. The session itself lives only in the status
    // word. placement_ is also read only by the control thread (snapshot()).
    // track_ and layout_ are read by the callback too - begin() compares the
    // track, capture() and drain() use the layout - and a begin() that lost its
    // claim to a failure may still be reading while cleanup and the next
    // prepareRequest() rewrite them, so both are lock-free atomics.
    std::atomic<tracks::TrackId> track_{};
    RecordingPlacementSnapshot placement_;
    std::atomic<media::AudioChannelLayout> layout_{media::AudioChannelLayout::mono};
    // Begin values: written by begin() while "starting" is held, published by
    // the release CAS to "capturing". Reset to zero by prepareRequest().
    std::atomic<std::int64_t> projectStart_{};
    std::atomic<double> deviceSampleRate_{};
};

} // namespace vitadaw::audio
