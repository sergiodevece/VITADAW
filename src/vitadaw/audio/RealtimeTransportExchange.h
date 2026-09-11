#pragma once

#include "vitadaw/timeline/Time.h"

#include <atomic>
#include <cstdint>

namespace vitadaw::audio {

using AudioCommandSequence = std::uint64_t;

struct AudioControlRequestResult {
    bool accepted{};
    AudioCommandSequence sequence{};
};

struct RealtimeTransportSnapshot {
    bool playing{};
    timeline::ProjectFramePosition position;
    timeline::ProjectFrameCount duration;
    AudioCommandSequence lastProcessedCommandSequence{};
};

// Single-writer exchange. publish() is bounded and lock-free; snapshot() is
// called only from a non-RT thread and retries if it overlaps a publication.
class RealtimeTransportExchange {
public:
    void publish(const RealtimeTransportSnapshot& state) noexcept {
        revision_.fetch_add(1, std::memory_order_acq_rel);
        playing_.store(state.playing, std::memory_order_relaxed);
        position_.store(state.position.value, std::memory_order_relaxed);
        duration_.store(state.duration.value, std::memory_order_relaxed);
        processedCommandSequence_.store(state.lastProcessedCommandSequence,
                                        std::memory_order_relaxed);
        revision_.fetch_add(1, std::memory_order_release);
    }

    [[nodiscard]] RealtimeTransportSnapshot snapshot() const noexcept {
        for (;;) {
            const auto before = revision_.load(std::memory_order_acquire);
            if ((before & 1U) != 0U) {
                continue;
            }

            const RealtimeTransportSnapshot result{
                playing_.load(std::memory_order_relaxed),
                {position_.load(std::memory_order_relaxed)},
                {duration_.load(std::memory_order_relaxed)},
                processedCommandSequence_.load(std::memory_order_relaxed)};

            if (revision_.load(std::memory_order_acquire) == before) {
                return result;
            }
        }
    }

private:
    static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
    static_assert(std::atomic<std::int64_t>::is_always_lock_free);
    static_assert(std::atomic<bool>::is_always_lock_free);

    std::atomic<std::uint64_t> revision_{};
    std::atomic<bool> playing_{};
    std::atomic<std::int64_t> position_{};
    std::atomic<std::int64_t> duration_{};
    std::atomic<AudioCommandSequence> processedCommandSequence_{};
};

} // namespace vitadaw::audio
