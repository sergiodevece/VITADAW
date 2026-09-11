#pragma once

#include "vitadaw/audio/CommandLifecycleGate.h"
#include "vitadaw/audio/IRealtimeAudioProcessor.h"
#include "vitadaw/audio/DeviceProcessingState.h"
#include "vitadaw/audio/RealtimeProjectClock.h"
#include "vitadaw/audio/RealtimeTransportExchange.h"
#include "vitadaw/audio/TwoTrackMixer.h"
#include "vitadaw/tracks/AudioTrack.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace vitadaw::audio {

struct RealtimeProjectContext {
    timeline::SampleRate projectSampleRate;
    timeline::ProjectFrameCount duration;
};

// Portable two-track render used by both the JUCE callback and offline tests.
// configure() and lifecycle transitions that mutate the clock are called only
// while processBlock() is quiescent. Consumer confirmation may happen at RT
// callback entry. Resources remain owned by the adapter and must outlive this
// engine's track views.
class RealtimeAudioEngine final {
public:
    static constexpr std::size_t commandCapacity = 8;

    void configure(RealtimeProjectContext context,
                   std::array<PreparedTrackView, tracks::audioTrackCount> tracks) noexcept;
    // Lifecycle transitions may race with the application command producer.
    // Transitions away from operational are invoked only while render is
    // quiescent (JUCE serialises them against its callback).
    void deviceInitialising() noexcept;
    void deviceConsumerStarted() noexcept;
    void deviceStopped() noexcept;
    void deviceError() noexcept;
    void deviceUnavailable() noexcept;
    [[nodiscard]] DeviceProcessingState deviceState() const noexcept;

    [[nodiscard]] AudioControlRequestResult tryRequestPlay() noexcept;
    [[nodiscard]] AudioControlRequestResult tryRequestStop() noexcept;
    [[nodiscard]] RealtimeTransportSnapshot transportSnapshot() const noexcept;

    void processBlock(AudioBlockView output,
                      timeline::SampleRate deviceSampleRate) noexcept;

private:
    static_assert(std::atomic<std::size_t>::is_always_lock_free);
    static_assert(std::atomic<std::uint64_t>::is_always_lock_free);

    enum class CommandType : std::uint8_t { play, stop };
    struct QueuedCommand {
        CommandType type{CommandType::stop};
        AudioCommandSequence sequence{};
        std::uint64_t generation{};
    };

    [[nodiscard]] AudioControlRequestResult enqueue(CommandType type) noexcept;
    void consumeCommands() noexcept;
    void publishTransport() noexcept;
    void transitionAwayFromOperational(DeviceProcessingState state) noexcept;
    void resolveCommandsThrough(AudioCommandSequence sequence) noexcept;
    [[nodiscard]] bool hasPreparedAudio() const noexcept;

    RealtimeProjectContext context_;
    std::array<PreparedTrackView, tracks::audioTrackCount> tracks_{};
    RealtimeProjectClock clock_;
    RealtimeTransportExchange transportExchange_;
    std::array<QueuedCommand, commandCapacity> commands_{};
    std::atomic<std::size_t> commandWriteIndex_{};
    std::atomic<std::size_t> commandReadIndex_{};
    CommandLifecycleGate lifecycleGate_;
    std::atomic<AudioCommandSequence> lastResolvedCommandSequence_{};
};

} // namespace vitadaw::audio
