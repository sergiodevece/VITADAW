#pragma once

#include "vitadaw/audio/AudioBlockView.h"
#include "vitadaw/audio/RecordingTypes.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace vitadaw::audio {

// Portable single-producer/single-consumer PCM exchange. prepare() and reset()
// are non-RT operations performed only while capture is inactive. The audio
// callback is the sole producer; the recording service is the sole consumer.
class RealtimeCapture final {
public:
    [[nodiscard]] bool prepare(std::size_t frameCapacity);
    void reset() noexcept;
    // Binds a non-RT prepared ring to the request before its RT command is
    // visible. This lets lifecycle closure terminalize a command which has not
    // reached the callback yet.
    [[nodiscard]] bool prepareRequest(const RecordingRequest&) noexcept;
    void clearPreparedRequest() noexcept;

    [[nodiscard]] bool begin(const RecordingRequest&,
                             timeline::ProjectFramePosition,
                             timeline::SampleRate) noexcept;
    void capture(ConstAudioBlockView) noexcept;
    void stop() noexcept;
    void fail(RecordingFailure) noexcept;
    void cancel() noexcept { fail(RecordingFailure::cancelled); }

    [[nodiscard]] std::size_t drain(AudioBlockView) noexcept;
    [[nodiscard]] RecordingSnapshot snapshot() const noexcept;
    [[nodiscard]] bool isCapturing() const noexcept {
        return phase(status_.load(std::memory_order_acquire)) == RecordingPhase::capturing;
    }
    [[nodiscard]] bool isPrepared() const noexcept {
        return phase(status_.load(std::memory_order_acquire)) == RecordingPhase::prepared;
    }
    [[nodiscard]] bool canPrepareRequest() const noexcept {
        return capacity_ != 0 &&
               phase(status_.load(std::memory_order_acquire)) == RecordingPhase::idle;
    }
    [[nodiscard]] bool isActive() const noexcept {
        const auto value = phase(status_.load(std::memory_order_acquire));
        return value == RecordingPhase::prepared || value == RecordingPhase::capturing;
    }
    [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }

private:
    static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
    static_assert(std::atomic<std::uint32_t>::is_always_lock_free);
    static constexpr std::uint32_t encode(RecordingPhase phase,
                                          RecordingFailure failure) noexcept {
        return static_cast<std::uint32_t>(phase) |
               (static_cast<std::uint32_t>(failure) << 8U);
    }
    static constexpr RecordingPhase phase(std::uint32_t value) noexcept {
        return static_cast<RecordingPhase>(value & 0xffU);
    }
    static constexpr RecordingFailure failure(std::uint32_t value) noexcept {
        return static_cast<RecordingFailure>((value >> 8U) & 0xffU);
    }

    std::array<std::vector<float>, 2> samples_;
    std::size_t capacity_{};
    std::atomic<std::uint64_t> writeFrame_{};
    std::atomic<std::uint64_t> readFrame_{};
    std::atomic<std::uint64_t> acceptedFrames_{};
    // A single atomic publication makes terminal snapshots coherent. Stop and
    // failure race by CAS; whichever transition wins is the terminal result.
    std::atomic<std::uint32_t> status_{encode(RecordingPhase::idle,
                                              RecordingFailure::none)};

    // Published before the prepared/capturing status transition. The non-RT
    // reader acquires status before reading them.
    RecordingSessionId session_{};
    tracks::TrackId track_;
    media::AudioChannelLayout layout_{media::AudioChannelLayout::mono};
    timeline::ProjectFramePosition projectStart_;
    timeline::SampleRate deviceSampleRate_;
};

} // namespace vitadaw::audio
