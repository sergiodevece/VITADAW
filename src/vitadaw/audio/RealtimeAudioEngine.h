#pragma once

#include "vitadaw/audio/CommandLifecycleGate.h"
#include "vitadaw/audio/IRealtimeAudioProcessor.h"
#include "vitadaw/audio/DeviceProcessingState.h"
#include "vitadaw/audio/PreparedProject.h"
#include "vitadaw/audio/RealtimeProjectClock.h"
#include "vitadaw/audio/RealtimeTransportExchange.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <variant>

namespace vitadaw::audio {

// Portable N-track render used by both the JUCE callback and offline tests.
// configure() and lifecycle transitions that mutate the clock are called only
// while processBlock() is quiescent. Consumer confirmation may happen at RT
// callback entry. Resources remain owned by the adapter and must outlive this
// engine's prepared project view.
class RealtimeAudioEngine final {
public:
    static constexpr std::size_t commandCapacity = 8;
    static constexpr std::size_t parameterCommandCapacity = 64;
    static constexpr std::size_t maximumTrackCount = 256;

    void configure(PreparedProjectView project) noexcept;
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
    [[nodiscard]] bool tryUpdateTrackMix(
        tracks::TrackId track, mixer::PreparedTrackMixState mix,
        bool anySolo) noexcept;
    [[nodiscard]] bool tryUpdateGlobalSolo(bool anySolo) noexcept;
    [[nodiscard]] bool tryUpdateMasterMix(
        mixer::PreparedMasterMixState mix) noexcept;
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
    struct TrackMixCommand {
        tracks::TrackId track;
        mixer::PreparedTrackMixState mix;
        bool anySolo{};
    };
    struct MasterMixCommand {
        mixer::PreparedMasterMixState mix;
    };
    struct GlobalSoloCommand { bool anySolo{}; };
    using ParameterCommand = std::variant<TrackMixCommand, MasterMixCommand,
                                          GlobalSoloCommand>;
    static_assert(std::is_trivially_copyable_v<ParameterCommand>);

    [[nodiscard]] AudioControlRequestResult enqueue(CommandType type) noexcept;
    void consumeCommands() noexcept;
    void consumeParameterCommands() noexcept;
    [[nodiscard]] bool enqueueParameter(ParameterCommand command) noexcept;
    void publishTransport() noexcept;
    void transitionAwayFromOperational(DeviceProcessingState state) noexcept;
    void resolveCommandsThrough(AudioCommandSequence sequence) noexcept;
    [[nodiscard]] bool hasPreparedAudio() const noexcept;

    PreparedProjectView project_;
    RealtimeProjectClock clock_;
    RealtimeTransportExchange transportExchange_;
    std::array<QueuedCommand, commandCapacity> commands_{};
    std::atomic<std::size_t> commandWriteIndex_{};
    std::atomic<std::size_t> commandReadIndex_{};
    CommandLifecycleGate lifecycleGate_;
    std::atomic<AudioCommandSequence> lastResolvedCommandSequence_{};
    std::array<mixer::PreparedTrackMixState, maximumTrackCount> trackMix_{};
    std::size_t trackMixCount_{};
    mixer::PreparedMasterMixState masterMix_;
    bool anySolo_{};
    std::array<ParameterCommand, parameterCommandCapacity> parameterCommands_{};
    std::atomic<std::size_t> parameterWriteIndex_{};
    std::atomic<std::size_t> parameterReadIndex_{};
};

} // namespace vitadaw::audio
