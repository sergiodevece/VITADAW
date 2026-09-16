#pragma once

#include "vitadaw/audio/CommandLifecycleGate.h"
#include "vitadaw/audio/IRealtimeAudioProcessor.h"
#include "vitadaw/audio/DeviceProcessingState.h"
#include "vitadaw/audio/PreparedProject.h"
#include "vitadaw/audio/PreparedProcessingPlan.h"
#include "vitadaw/audio/PreparedTemporalContext.h"
#include "vitadaw/audio/MixerSmoother.h"
#include "vitadaw/audio/RealtimeMeterExchange.h"
#include "vitadaw/audio/RealtimeProjectClock.h"
#include "vitadaw/audio/RealtimeTransportExchange.h"
#include "vitadaw/audio/TrackMixerProcessing.h"
#include "vitadaw/transport/TransportReducer.h"

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
    friend struct TransportTestAccess;
public:
    static constexpr std::size_t commandCapacity = 8;
    static constexpr std::size_t pendingCommandCapacity = 2 * (commandCapacity - 1);
    static constexpr std::size_t parameterCommandCapacity = 64;
    static constexpr std::size_t maximumTrackCount = maximumPreparedTracks;
    static constexpr std::size_t maximumBusCount = maximumPreparedBuses;

    void configure(PreparedProjectView project) noexcept;
    // Quiescent/offline preparation for the legacy hardware-free entry point.
    // A changed device rate must be certified before its first processBlock.
    [[nodiscard]] bool prepareLegacyDeviceRate(timeline::SampleRate deviceRate) noexcept;
    void configure(const PreparedProcessingPlan& plan,
                   ProcessingPlanRuntime& runtime) noexcept;
    // Called only with a quiescent consumer. The supplied context remains owned
    // off RT and must outlive every callback until the next quiescent swap.
    [[nodiscard]] bool canConfigureTemporalContext(
        const PreparedTemporalContext* context) const noexcept;
    bool configureTemporalContext(const PreparedTemporalContext* context) noexcept;
    struct PendingMetronomeBoundaryState {
        // Minimal musical obligation only. No voices, PCM or scheduler tables.
        bool crossedLoopStart{};
        bool eventPending{};
        bool accent{};
        bool operator==(const PendingMetronomeBoundaryState&) const = default;
    };
    struct TemporalCheckpoint {
        RealtimeProjectClock::Checkpoint clock;
        bool loopEnabled{};
        bool metronomeEnabled{};
        bool runUntilStop{};
        MetronomeLevelDb metronomeLevel;
        PendingMetronomeBoundaryState pendingMetronomeBoundary;
    };
    [[nodiscard]] TemporalCheckpoint temporalCheckpoint() const noexcept;
    bool restoreTemporalCheckpoint(TemporalCheckpoint) noexcept;
    void resetTemporalSessionState() noexcept;
    // Quiescent only. Owners must stay alive until all engine views are retired.
    void releasePreparedReferences() noexcept;
    void deviceErrorPreservingTransport() noexcept;
    // Lifecycle transitions may race with the application command producer.
    // Transitions away from operational are invoked only while render is
    // quiescent (JUCE serialises them against its callback).
    void deviceInitialising() noexcept;
    // Controlled callback re-registration after a quiescent temporal commit.
    // It closes the command generation exactly like initialisation but keeps
    // the already-restored stopped/paused clock position.
    void deviceInitialisingPreservingTransport() noexcept;
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
    [[nodiscard]] AudioControlRequestResult trySetLoopEnabled(bool) noexcept;
    [[nodiscard]] AudioControlRequestResult trySetMetronomeEnabled(bool) noexcept;
    [[nodiscard]] AudioControlRequestResult trySetMetronomeLevel(MetronomeLevelDb) noexcept;
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
    // Single application producer only. Confirmed snapshot plus bounded pure replay.
    [[nodiscard]] RealtimeTransportSnapshot projectedTransportSnapshot() noexcept;
    [[nodiscard]] mixer::MeterSnapshot meterSnapshot() const noexcept;

    void processBlock(AudioBlockView output,
                      timeline::SampleRate deviceSampleRate) noexcept;

private:
    static_assert(std::atomic<std::size_t>::is_always_lock_free);
    static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
    static_assert(std::atomic<std::int64_t>::is_always_lock_free);

    enum class CommandType : std::uint8_t {
        play, pause, stop, seek, setLoopEnabled,
        setMetronomeEnabled, setMetronomeLevel
    };
    struct QueuedCommand {
        CommandType type{CommandType::stop};
        AudioCommandSequence sequence{};
        std::uint64_t generation{};
        timeline::ProjectFramePosition target;
        float value{};
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
        CommandType type, timeline::ProjectFramePosition target = {},
        float value = 0.0F) noexcept;
    void refreshTransportProjection() noexcept;
    [[nodiscard]] transport::TransportReductionPolicy
    transportReductionPolicy() const noexcept;
    void commitTransportProjection(
        const transport::TransportReduction&, AudioCommandSequence) noexcept;
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
                         std::size_t frameCount) noexcept;
    void prepareMetronomeEvents(const DspFramePosition* positions,
                               std::size_t frameCount) noexcept;
    [[nodiscard]] float renderMetronomeSample(std::size_t frame) noexcept;
    void clearMetronomeRuntime() noexcept;
    void clearMetronomeVoices() noexcept;
    void clearMetronomePendingScheduling() noexcept;
    void processLegacySubBlock(
        AudioBlockView output, std::size_t outputOffset,
        std::size_t validFrames, const DspFramePosition* positions) noexcept;
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
    const PreparedTemporalContext* temporalContext_{};
    std::array<PreparedTrackRoute, maximumTrackCount> legacyTracks_{};
    std::array<ProcessingStep, maximumTrackCount + 1> legacyOrder_{};
    std::array<float, defaultProcessingBlockCapacity> legacyMasterLeft_{};
    std::array<float, defaultProcessingBlockCapacity> legacyMasterRight_{};
    std::array<DspFramePosition, defaultProcessingBlockCapacity> legacyPositions_{};
    RealtimeProjectClock clock_;
    exact::ClockFormat baseClockFormat_;
    RealtimeTransportExchange transportExchange_;
    std::array<QueuedCommand, commandCapacity> commands_{};
    std::atomic<std::size_t> commandWriteIndex_{};
    std::atomic<std::size_t> commandReadIndex_{};
    CommandLifecycleGate lifecycleGate_;
    std::atomic<AudioCommandSequence> lastResolvedCommandSequence_{};
    // Single-producer projection: resolved RT snapshot plus accepted commands
    // which have not yet been observed as resolved.
    transport::TransportState projectedTransport_;
    AudioCommandSequence projectedTransportSequence_{};
    transport::TransportBoundaryFacts projectedBoundaries_;
    std::array<QueuedCommand, pendingCommandCapacity> pendingCommands_{};
    std::size_t pendingCommandCount_{};
    bool projectedLoopEnabled_{};
    RealtimeTransportSnapshot projectionBase_;
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
    processors::TemporalDiscontinuity processorDiscontinuity_{
        processors::TemporalDiscontinuity::hardDiscontinuity};
    bool loopEnabled_{};
    bool metronomeEnabled_{};
    MetronomeLevelDb metronomeLevel_{};
    LinearSmoother metronomeLevelSmoother_;
    struct MetronomeVoice { bool active{}; bool accent{}; std::size_t frame{}; };
    std::array<MetronomeVoice, metronomeVoiceCount> metronomeVoices_{};
    struct MetronomeEvent { std::size_t frame{}; bool accent{}; };
    // One entry per distinct device frame is sufficient: musical boundaries
    // which quantise causally to the same frame start one (accent-dominant)
    // click. The prepared processing limit is far below this fixed RT bound.
    std::array<MetronomeEvent, maximumClickTableFrames> metronomeEvents_{};
    std::size_t metronomeEventCount_{};
    bool pendingMetronomeEvent_{};
    bool pendingMetronomeAccent_{};
    bool pendingMetronomeWrap_{};
};

} // namespace vitadaw::audio
