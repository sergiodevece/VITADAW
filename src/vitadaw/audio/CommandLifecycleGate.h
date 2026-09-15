#pragma once

#include "vitadaw/audio/DeviceProcessingState.h"
#include "vitadaw/audio/RealtimeTransportExchange.h"

#include <atomic>
#include <cstdint>
#include <limits>

namespace vitadaw::audio {

// Single-producer command admission gate. One atomic modification order
// serialises enqueue acceptance with lifecycle closure. The internal claimed
// state belongs to the producer and is reported publicly as operational.
class CommandLifecycleGate final {
public:
    struct EnqueueClaim {
        std::uint64_t token{};
        std::uint64_t generation{};
        bool active{};
    };

    struct Closure {
        std::uint64_t generation{};
        AudioCommandSequence cancellationWatermark{};
    };

    [[nodiscard]] EnqueueClaim tryClaim() noexcept {
        auto observed = gate_.load(std::memory_order_acquire);
        while (encodedState(observed) == EncodedState::operational) {
            const auto desired = pack(generationOf(observed), EncodedState::claimed);
            if (gate_.compare_exchange_weak(observed, desired,
                                            std::memory_order_acq_rel,
                                            std::memory_order_acquire)) {
                return {desired, generationOf(desired), true};
            }
        }
        return {};
    }

    // Called only by the single producer while it owns an active claim.
    // This counter identifies reservations, not consecutive accepted actions.
    // Closure may expose a rejected reservation through its watermark. Only a
    // successful tryAccept introduces an action in the accepted history.
    // Ticket zero is reserved as the failure sentinel. Rather than wrapping,
    // the practically inexhaustible 64-bit space closes to further commands.
    [[nodiscard]] AudioCommandSequence reserveSequence(
        const EnqueueClaim& claim) noexcept {
        if (!claim.active) {
            return 0;
        }
        const auto next = nextSequence_.load(std::memory_order_relaxed);
        if (next == std::numeric_limits<AudioCommandSequence>::max()) {
            return 0;
        }
        nextSequence_.store(next + 1, std::memory_order_relaxed);
        return next;
    }

    // Linearization point for acceptance. All command data and the sequence
    // reservation are sequenced-before this release CAS. Queue publication is
    // deliberately performed by the caller only after this succeeds.
    [[nodiscard]] bool tryAccept(const EnqueueClaim& claim) noexcept {
        if (!claim.active) {
            return false;
        }
        auto expected = claim.token;
        return gate_.compare_exchange_strong(
            expected, pack(claim.generation, EncodedState::operational),
            std::memory_order_release, std::memory_order_relaxed);
    }

    void reject(const EnqueueClaim& claim) noexcept {
        if (!claim.active) {
            return;
        }
        auto expected = claim.token;
        static_cast<void>(gate_.compare_exchange_strong(
            expected, pack(claim.generation, EncodedState::operational),
            std::memory_order_release, std::memory_order_relaxed));
    }

    // Linearization point for lifecycle closure. If an enqueue accepted first,
    // this acquire observes its release CAS and therefore its prior sequence
    // reservation. If closure replaces a claimed token first, acceptance fails.
    [[nodiscard]] Closure close(DeviceProcessingState target) noexcept {
        auto observed = gate_.load(std::memory_order_acquire);
        std::uint64_t desired{};
        do {
            const auto generation = generationOf(observed);
            desired = generation == generationMask
                          ? pack(generationMask, EncodedState::exhausted)
                          : pack(generation + 1ULL, encode(target));
        } while (!gate_.compare_exchange_weak(
            observed, desired, std::memory_order_acq_rel,
            std::memory_order_acquire));

        const auto next = nextSequence_.load(std::memory_order_acquire);
        // An accepted reservation happens-before this load. A rejected one need
        // not: it may be included or absent, with no unresolved accepted action.
        return {generationOf(desired), next == 0 ? 0 : next - 1};
    }

    void consumerStarted() noexcept {
        auto observed = gate_.load(std::memory_order_acquire);
        while (encodedState(observed) == EncodedState::initializing) {
            const auto desired = pack(generationOf(observed),
                                      EncodedState::operational);
            if (gate_.compare_exchange_weak(observed, desired,
                                            std::memory_order_acq_rel,
                                            std::memory_order_acquire)) {
                return;
            }
        }
    }

    [[nodiscard]] DeviceProcessingState state() const noexcept {
        const auto encoded = encodedState(gate_.load(std::memory_order_acquire));
        if (encoded == EncodedState::claimed) {
            return DeviceProcessingState::operational;
        }
        return decode(encoded);
    }

    [[nodiscard]] std::uint64_t generation() const noexcept {
        return generationOf(gate_.load(std::memory_order_acquire));
    }

private:
    enum class EncodedState : std::uint64_t {
        unavailable = 0,
        initializing = 1,
        operational = 2,
        stopped = 3,
        error = 4,
        claimed = 5,
        exhausted = 6,
    };

    static constexpr std::uint64_t stateBits = 3;
    static constexpr std::uint64_t stateMask = (1ULL << stateBits) - 1ULL;
    static constexpr std::uint64_t generationMask =
        std::numeric_limits<std::uint64_t>::max() >> stateBits;

    static_assert(std::atomic<std::uint64_t>::is_always_lock_free);

    [[nodiscard]] static constexpr std::uint64_t pack(
        std::uint64_t generation, EncodedState state) noexcept {
        return ((generation & generationMask) << stateBits) |
               static_cast<std::uint64_t>(state);
    }

    [[nodiscard]] static constexpr std::uint64_t generationOf(
        std::uint64_t token) noexcept {
        return token >> stateBits;
    }

    [[nodiscard]] static constexpr EncodedState encodedState(
        std::uint64_t token) noexcept {
        return static_cast<EncodedState>(token & stateMask);
    }

    [[nodiscard]] static constexpr EncodedState encode(
        DeviceProcessingState state) noexcept {
        switch (state) {
        case DeviceProcessingState::initializing:
            return EncodedState::initializing;
        case DeviceProcessingState::operational:
            return EncodedState::operational;
        case DeviceProcessingState::stopped:
            return EncodedState::stopped;
        case DeviceProcessingState::error:
            return EncodedState::error;
        case DeviceProcessingState::unavailable:
            return EncodedState::unavailable;
        }
        return EncodedState::unavailable;
    }

    [[nodiscard]] static constexpr DeviceProcessingState decode(
        EncodedState state) noexcept {
        switch (state) {
        case EncodedState::initializing:
            return DeviceProcessingState::initializing;
        case EncodedState::operational:
        case EncodedState::claimed:
            return DeviceProcessingState::operational;
        case EncodedState::stopped:
            return DeviceProcessingState::stopped;
        case EncodedState::error:
        case EncodedState::exhausted:
            return DeviceProcessingState::error;
        case EncodedState::unavailable:
            return DeviceProcessingState::unavailable;
        }
        return DeviceProcessingState::unavailable;
    }

    std::atomic<std::uint64_t> gate_{
        pack(0, EncodedState::unavailable)};
    std::atomic<AudioCommandSequence> nextSequence_{1};
};

} // namespace vitadaw::audio
