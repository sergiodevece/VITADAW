#pragma once

#include "vitadaw/audio/CommandLifecycleGate.h"
#include "vitadaw/audio/IRealtimeAudioProcessor.h"
#include "vitadaw/audio/DeviceProcessingState.h"
#include "vitadaw/audio/PreparedProject.h"
#include "vitadaw/audio/PreparedProcessingPlan.h"
#include "vitadaw/audio/MixerSmoother.h"
#include "vitadaw/audio/RealtimeMeterExchange.h"
#include "vitadaw/audio/RealtimeProjectClock.h"
#include "vitadaw/audio/RealtimeTransportExchange.h"
#include "vitadaw/audio/TrackMixerProcessing.h"

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
    static constexpr std::size_t maximumTrackCount = maximumPreparedTracks;
    static constexpr std::size_t maximumBusCount = maximumPreparedBuses;

    void configure(PreparedProjectView project) noexcept;
    void configure(const PreparedProcessingPlan& plan,
                   ProcessingPlanRuntime& runtime) noexcept;
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
    [[nodiscard]] AudioControlRequestResult tryRequestPause() noexcept;
    [[nodiscard]] AudioControlRequestResult tryRequestStop() noexcept;
    [[nodiscard]] AudioControlRequestResult tryRequestSeek(
        timeline::ProjectFramePosition) noexcept;
    [[nodiscard]] bool tryUpdateTrackMix(
        tracks::TrackId track, mixer::PreparedTrackMixState mix,
        PreparedAudibilityState audibility) noexcept;
    [[nodiscard]] bool tryUpdateBusMix(
        routing::BusId bus, mixer::PreparedBusMixState mix,
        PreparedAudibilityState audibility) noexcept;
    [[nodiscard]] bool tryUpdateSendMix(
        routing::SendId send, mixer::PreparedSendMixState mix) noexcept;
    [[nodiscard]] bool tryUpdateMasterMix(
        mixer::PreparedMasterMixState mix) noexcept;
    [[nodiscard]] bool tryUpdateProcessorBypass(
        processors::ProcessorInstanceId processor, bool bypassed) noexcept;
    [[nodiscard]] bool tryUpdateProcessorParameter(
        processors::ProcessorInstanceId processor,
        processors::ParameterId parameter, float preparedValue,
        std::uint32_t frameOffset = 0) noexcept;
    [[nodiscard]] RealtimeTransportSnapshot transportSnapshot() const noexcept;
    [[nodiscard]] mixer::MeterSnapshot meterSnapshot() const noexcept;

    void processBlock(AudioBlockView output,
                      timeline::SampleRate deviceSampleRate) noexcept;

private:
    static_assert(std::atomic<std::size_t>::is_always_lock_free);
    static_assert(std::atomic<std::uint64_t>::is_always_lock_free);

    enum class CommandType : std::uint8_t { play, pause, stop, seek };
    struct QueuedCommand {
        CommandType type{CommandType::stop};
        AudioCommandSequence sequence{};
        std::uint64_t generation{};
        timeline::ProjectFramePosition target;
    };
    struct TrackMixCommand {
        std::size_t trackIndex{};
        mixer::PreparedTrackMixState mix;
        PreparedAudibilityState audibility;
    };
    struct BusMixCommand {
        std::size_t busIndex{};
        mixer::PreparedBusMixState mix;
        PreparedAudibilityState audibility;
    };
    struct MasterMixCommand {
        mixer::PreparedMasterMixState mix;
    };
    struct SendMixCommand {
        std::size_t sendIndex{};
        mixer::PreparedSendMixState mix;
    };
    struct ProcessorBypassCommand {
        std::uint64_t planGeneration{};
        std::size_t processorIndex{};
        bool bypassed{};
    };
    struct ProcessorParameterCommand {
        std::uint64_t planGeneration{};
        std::size_t processorIndex{};
        processors::ParameterId parameter;
        float preparedValue{};
        std::uint32_t frameOffset{};
    };
    using ParameterCommand =
        std::variant<TrackMixCommand, BusMixCommand, MasterMixCommand,
                     SendMixCommand, ProcessorBypassCommand,
                     ProcessorParameterCommand>;
    static_assert(std::is_trivially_copyable_v<ParameterCommand>);

    [[nodiscard]] AudioControlRequestResult enqueue(
        CommandType type, timeline::ProjectFramePosition target = {}) noexcept;
    void consumeCommands() noexcept;
    void consumeParameterCommands(
        timeline::SampleRate deviceSampleRate) noexcept;
    [[nodiscard]] bool enqueueParameter(ParameterCommand command) noexcept;
    void publishTransport() noexcept;
    void publishMeters() noexcept;
    void clearMeters() noexcept;
    void advanceSmoothers(std::size_t frameCount) noexcept;
    void distributeSends(PreparedSendRange range, StereoSample tap,
                         std::size_t frame) noexcept;
    void processSubBlock(AudioBlockView output, std::size_t outputOffset,
                         std::size_t frameCount,
                         timeline::ProjectFrameDuration projectFramesPerDeviceFrame) noexcept;
    void processLegacySubBlock(
        AudioBlockView output, std::size_t outputOffset,
        std::size_t validFrames, const double* positions) noexcept;
    struct ProcessedNodeBlock {
        float* left{};
        float* right{};
        std::size_t channelCount{};
    };
    [[nodiscard]] ProcessedNodeBlock processInsertChain(
        PreparedInsertRange range,
        const processors::ProcessorProcessContext& context) noexcept;
    void resetProcessors() noexcept;
    void transitionAwayFromOperational(DeviceProcessingState state) noexcept;
    void resolveCommandsThrough(AudioCommandSequence sequence) noexcept;
    [[nodiscard]] bool hasPreparedAudio() const noexcept;

    timeline::SampleRate projectSampleRate_;
    timeline::ProjectFrameCount projectDuration_;
    std::span<const PreparedTrackRoute> tracks_;
    std::span<const PreparedBusNode> buses_;
    std::span<const PreparedSendDescriptor> sends_;
    std::span<const PreparedSendIndex> sendIndexById_;
    std::span<const PreparedProcessorDescriptor> processors_;
    std::span<const PreparedProcessorIndex> processorIndexById_;
    std::span<const ProcessingStep> order_;
    PreparedInsertRange masterInserts_;
    processors::ProcessingFormat processingFormat_;
    std::size_t blockCapacity_{defaultProcessingBlockCapacity};
    ProcessingPlanRuntime* runtime_{};
    const PreparedProcessingPlan* plan_{};
    std::array<PreparedTrackRoute, maximumTrackCount> legacyTracks_{};
    std::array<ProcessingStep, maximumTrackCount + 1> legacyOrder_{};
    std::array<float, defaultProcessingBlockCapacity> legacyMasterLeft_{};
    std::array<float, defaultProcessingBlockCapacity> legacyMasterRight_{};
    std::array<double, defaultProcessingBlockCapacity> legacyPositions_{};
    RealtimeProjectClock clock_;
    RealtimeTransportExchange transportExchange_;
    std::array<QueuedCommand, commandCapacity> commands_{};
    std::atomic<std::size_t> commandWriteIndex_{};
    std::atomic<std::size_t> commandReadIndex_{};
    CommandLifecycleGate lifecycleGate_;
    std::atomic<AudioCommandSequence> lastResolvedCommandSequence_{};
    std::array<TrackMixSmoother, maximumTrackCount> trackMix_{};
    std::size_t trackMixCount_{};
    std::array<BusMixSmoother, maximumBusCount> busMix_{};
    std::size_t busMixCount_{};
    std::size_t sendMixCount_{};
    MasterMixSmoother masterMix_;
    PreparedAudibilityState audibility_;
    std::array<ParameterCommand, parameterCommandCapacity> parameterCommands_{};
    std::atomic<std::size_t> parameterWriteIndex_{};
    std::atomic<std::size_t> parameterReadIndex_{};
    std::atomic<std::uint64_t> planGeneration_{1};
    std::array<tracks::TrackId, maximumTrackCount> meterTrackIds_{};
    std::array<mixer::StereoPeak, maximumTrackCount> trackPeaks_{};
    std::array<routing::BusId, maximumBusCount> meterBusIds_{};
    std::array<mixer::StereoPeak, maximumBusCount> busPeaks_{};
    mixer::StereoPeak masterPeak_;
    RealtimeMeterExchange meterExchange_;
    bool processorDiscontinuity_{true};
};

} // namespace vitadaw::audio
