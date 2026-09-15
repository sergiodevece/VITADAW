#pragma once

#include "vitadaw/timeline/Time.h"
#include "vitadaw/transport/PlaybackState.h"

#include <atomic>
#include <cstdint>

namespace vitadaw::audio {

using AudioCommandSequence = std::uint64_t;

struct AudioControlRequestResult {
    bool accepted{};
    AudioCommandSequence sequence{};
};

struct RealtimeTransportSnapshot {
    // `playing` remains as a compatibility view; production publishes it from
    // the same clock state as `playback` in one coherent generation.
    bool playing{};
    timeline::ProjectFramePosition position;
    timeline::ProjectFrameCount duration;
    AudioCommandSequence lastProcessedCommandSequence{};
    transport::PlaybackState playback{transport::PlaybackState::stopped};
};

// Single-writer/single-reader exchange. Every operation participates in C++20's
// single sequentially-consistent order: observing a payload store from a new
// generation necessarily places the writer's preceding odd revision before the
// final reader revision check. Every payload field is also lock-free atomic.
// The non-RT reader makes bounded attempts and otherwise retains its last value.
class RealtimeTransportExchange {
public:
    void publish(const RealtimeTransportSnapshot& state) noexcept {
        revision_.fetch_add(1, std::memory_order_seq_cst);
        const auto playback = state.playback == transport::PlaybackState::stopped &&
                                      state.playing
                                  ? transport::PlaybackState::playing
                                  : state.playback;
        playback_.store(static_cast<std::uint8_t>(playback),
                        std::memory_order_seq_cst);
        position_.store(state.position.value, std::memory_order_seq_cst);
        duration_.store(state.duration.value, std::memory_order_seq_cst);
        processedCommandSequence_.store(state.lastProcessedCommandSequence,
                                        std::memory_order_seq_cst);
        revision_.fetch_add(1, std::memory_order_seq_cst);
    }

    [[nodiscard]] RealtimeTransportSnapshot snapshot() const noexcept {
        constexpr auto maxAttempts = 3;
        for (auto attempt = 0; attempt < maxAttempts; ++attempt) {
            const auto before = revision_.load(std::memory_order_seq_cst);
            if ((before & 1U) != 0U) {
                continue;
            }

            const RealtimeTransportSnapshot result{
                playback_.load(std::memory_order_seq_cst) ==
                    static_cast<std::uint8_t>(transport::PlaybackState::playing),
                {position_.load(std::memory_order_seq_cst)},
                {duration_.load(std::memory_order_seq_cst)},
                processedCommandSequence_.load(std::memory_order_seq_cst),
                static_cast<transport::PlaybackState>(
                    playback_.load(std::memory_order_seq_cst))};

            if (revision_.load(std::memory_order_seq_cst) == before) {
                lastCoherentSnapshot_ = result;
                return lastCoherentSnapshot_;
            }
        }
        return lastCoherentSnapshot_;
    }

private:
    static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
    static_assert(std::atomic<std::int64_t>::is_always_lock_free);
    static_assert(std::atomic<std::uint8_t>::is_always_lock_free);

    std::atomic<std::uint64_t> revision_{};
    std::atomic<std::uint8_t> playback_{};
    std::atomic<std::int64_t> position_{};
    std::atomic<std::int64_t> duration_{};
    std::atomic<AudioCommandSequence> processedCommandSequence_{};
    // Read and written only by the single non-RT consumer.
    mutable RealtimeTransportSnapshot lastCoherentSnapshot_{};
};

} // namespace vitadaw::audio
