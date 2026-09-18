#pragma once

#include "vitadaw/timeline/Time.h"
#include "vitadaw/transport/PlaybackState.h"

#include <atomic>
#include <bit>
#include <cstdint>

namespace vitadaw::audio {

// Monotonic reservation/resolution ticket, NOT a consecutive accepted-command
// ordinal. A rejected claim may leave a gap or an observable cancellation watermark.
using AudioCommandSequence = std::uint64_t;

enum class AudioControlDisposition : std::uint8_t { rejected, scheduled, alreadySatisfied };

enum class AudioControlRejection : std::uint8_t {
    none,
    unavailable,
    queueFull,
    invalidPosition,
    disallowedState,
};

struct AudioControlRequestResult {
    bool accepted{};
    AudioCommandSequence sequence{};
    AudioControlRejection rejection{AudioControlRejection::none};
    transport::PlaybackState projectedPlayback{
        transport::PlaybackState::stopped};
    timeline::ProjectFramePosition projectedPosition;
    bool hasProjection{};
    AudioControlDisposition disposition{accepted ? AudioControlDisposition::scheduled
                                                 : AudioControlDisposition::rejected};
};

struct RealtimeTransportSnapshot {
    // `playing` remains as a compatibility view; production publishes it from
    // the same clock state as `playback` in one coherent generation.
    bool playing{};
    timeline::ProjectFramePosition position;
    timeline::ProjectFrameCount duration;
    AudioCommandSequence lastProcessedCommandSequence{};
    transport::PlaybackState playback{transport::PlaybackState::stopped};
    bool loopEnabled{};
    bool metronomeEnabled{};
    float metronomeLevelDb{-12.0F};
    std::uint64_t temporalRevision{};
    std::uint64_t commandGeneration{};
    bool beforeContentEnd{};
    bool beforeLoopEnd{};
    // Non-RT projected view only; RT's resolution watermark remains separate.
    AudioCommandSequence projectedThroughTicket{};
    // Ephemeral Monitoring state. It is intentionally not a ProjectState field
    // and is published by the RT owner together with transport state. Appending
    // it preserves positional construction of the established transport fields.
    bool monitoringEnabled{};
    float monitorGainDb{-12.0F};
    // False means the callback observed a missing/limited route. This is a
    // compact observability bit only; device diagnostics remain non-RT work.
    bool monitoringRouteSupported{true};
    // Set only for an unexpected lifecycle/input-route loss.  It lets the
    // application distinguish a confirmed forced OFF from a normal Disable
    // command without treating desired state as RT state.
    bool monitoringLifecycleForcedOff{};
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
        loopEnabled_.store(state.loopEnabled, std::memory_order_seq_cst);
        metronomeEnabled_.store(state.metronomeEnabled, std::memory_order_seq_cst);
        metronomeLevelBits_.store(std::bit_cast<std::uint32_t>(state.metronomeLevelDb),
                                  std::memory_order_seq_cst);
        temporalRevision_.store(state.temporalRevision, std::memory_order_seq_cst);
        commandGeneration_.store(state.commandGeneration, std::memory_order_seq_cst);
        beforeContentEnd_.store(state.beforeContentEnd, std::memory_order_seq_cst);
        beforeLoopEnd_.store(state.beforeLoopEnd, std::memory_order_seq_cst);
        monitoringEnabled_.store(state.monitoringEnabled, std::memory_order_seq_cst);
        monitorGainBits_.store(std::bit_cast<std::uint32_t>(state.monitorGainDb),
                               std::memory_order_seq_cst);
        monitoringRouteSupported_.store(state.monitoringRouteSupported,
                                        std::memory_order_seq_cst);
        monitoringLifecycleForcedOff_.store(state.monitoringLifecycleForcedOff,
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

            RealtimeTransportSnapshot result{
                playback_.load(std::memory_order_seq_cst) ==
                    static_cast<std::uint8_t>(transport::PlaybackState::playing),
                {position_.load(std::memory_order_seq_cst)},
                {duration_.load(std::memory_order_seq_cst)},
                processedCommandSequence_.load(std::memory_order_seq_cst),
                static_cast<transport::PlaybackState>(
                    playback_.load(std::memory_order_seq_cst)),
                loopEnabled_.load(std::memory_order_seq_cst),
                metronomeEnabled_.load(std::memory_order_seq_cst),
                std::bit_cast<float>(metronomeLevelBits_.load(std::memory_order_seq_cst)),
                temporalRevision_.load(std::memory_order_seq_cst),
                commandGeneration_.load(std::memory_order_seq_cst),
                beforeContentEnd_.load(std::memory_order_seq_cst),
                beforeLoopEnd_.load(std::memory_order_seq_cst)};
            result.monitoringEnabled =
                monitoringEnabled_.load(std::memory_order_seq_cst);
            result.monitorGainDb = std::bit_cast<float>(
                monitorGainBits_.load(std::memory_order_seq_cst));
            result.monitoringRouteSupported =
                monitoringRouteSupported_.load(std::memory_order_seq_cst);
            result.monitoringLifecycleForcedOff =
                monitoringLifecycleForcedOff_.load(std::memory_order_seq_cst);

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
    static_assert(std::atomic<bool>::is_always_lock_free);
    static_assert(std::atomic<std::uint32_t>::is_always_lock_free);

    std::atomic<std::uint64_t> revision_{};
    std::atomic<std::uint8_t> playback_{};
    std::atomic<std::int64_t> position_{};
    std::atomic<std::int64_t> duration_{};
    std::atomic<AudioCommandSequence> processedCommandSequence_{};
    std::atomic<bool> loopEnabled_{};
    std::atomic<bool> metronomeEnabled_{};
    std::atomic<std::uint32_t> metronomeLevelBits_{std::bit_cast<std::uint32_t>(-12.0F)};
    std::atomic<std::uint64_t> temporalRevision_{};
    std::atomic<std::uint64_t> commandGeneration_{};
    std::atomic<bool> beforeContentEnd_{};
    std::atomic<bool> beforeLoopEnd_{};
    std::atomic<bool> monitoringEnabled_{};
    std::atomic<std::uint32_t> monitorGainBits_{std::bit_cast<std::uint32_t>(-12.0F)};
    std::atomic<bool> monitoringRouteSupported_{true};
    std::atomic<bool> monitoringLifecycleForcedOff_{};
    // Read and written only by the single non-RT consumer.
    mutable RealtimeTransportSnapshot lastCoherentSnapshot_{};
};

} // namespace vitadaw::audio
