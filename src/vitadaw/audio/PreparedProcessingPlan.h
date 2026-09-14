#pragma once

#include "vitadaw/audio/AudibilityState.h"
#include "vitadaw/audio/MixerSmoother.h"
#include "vitadaw/audio/PreparedProject.h"
#include "vitadaw/processors/IAudioProcessor.h"
#include "vitadaw/routing/RoutingState.h"

#include <array>
#include <cstddef>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace vitadaw::audio {

inline constexpr std::size_t maximumPreparedTracks = 256;
inline constexpr std::size_t maximumPreparedSources = 2048;
inline constexpr std::size_t maximumPreparedClips = 32768;
inline constexpr std::size_t maximumPreparedClipsPerTrack = 4096;
inline constexpr std::size_t maximumPreparedBuses = 64;
inline constexpr std::size_t maximumPreparedSends = 1024;
inline constexpr std::size_t maximumPreparedSendsPerTrack = 64;
inline constexpr std::size_t maximumPreparedSendsPerBus = 64;
inline constexpr std::size_t maximumPreparedProcessors = 512;
inline constexpr std::size_t maximumPreparedProcessorsPerChain = 16;
inline constexpr std::size_t defaultProcessingBlockCapacity = 512;
inline constexpr std::size_t defaultProcessingMemoryBudgetBytes =
    32U * 1024U * 1024U;
inline constexpr std::size_t masterDestinationIndex =
    std::numeric_limits<std::size_t>::max();
static_assert(maximumPreparedTracks == audibilityTrackCapacity);
static_assert(maximumPreparedBuses == audibilityBusCapacity);
static_assert(maximumPreparedSends == audibilitySendCapacity);

struct ProcessingPlanTrackSpecification {
    tracks::TrackId id;
    mixer::PreparedTrackMixState mix;
    routing::OutputDestination destination;
    processors::InsertChain inserts;
    media::AudioChannelLayout layout{media::AudioChannelLayout::mono};
    std::vector<clips::AudioClip> clips;
};

struct ProcessingPlanSourceSpecification {
    media::SourceId id;
    timeline::SourceFrameCount frameCount;
    timeline::SampleRate sampleRate;
    media::AudioChannelLayout layout{media::AudioChannelLayout::mono};
};

struct ProcessingPlanBusSpecification {
    routing::BusId id;
    mixer::PreparedBusMixState mix;
    routing::OutputDestination destination{routing::OutputDestination::master()};
    processors::InsertChain inserts;
};

struct ProcessingPlanSendSpecification {
    routing::SendId id;
    routing::SendSource source;
    routing::BusId destination;
    routing::SendTapPoint tapPoint{routing::SendTapPoint::postFaderPostPan};
    mixer::PreparedSendMixState mix;
};

struct ProcessingPlanSpecification {
    timeline::SampleRate projectSampleRate;
    std::vector<ProcessingPlanTrackSpecification> tracks;
    std::vector<ProcessingPlanSourceSpecification> sources;
    std::vector<ProcessingPlanBusSpecification> buses;
    std::vector<ProcessingPlanSendSpecification> sends;
    mixer::PreparedMasterMixState masterMix;
    processors::InsertChain masterInserts;
    timeline::SampleRate processingSampleRate;
    processors::ProcessingMode processingMode{processors::ProcessingMode::realtime};
};

struct PreparedSendRange {
    std::size_t first{};
    std::size_t count{};

    bool operator==(const PreparedSendRange&) const = default;
};

struct PreparedInsertRange {
    std::size_t first{};
    std::size_t count{};
    std::size_t scratchIndex{};
    processors::ChannelLayout layout{processors::ChannelLayout::stereo};
    processors::ProcessingFrameCount latency;
};

struct PreparedLatencyRange {
    processors::ProcessingFrameCount minimum;
    processors::ProcessingFrameCount maximum;

    bool operator==(const PreparedLatencyRange&) const = default;
};

struct PreparedTrackRoute {
    PreparedTrackView source;
    tracks::TrackId id;
    media::AudioChannelLayout layout{media::AudioChannelLayout::mono};
    PreparedClipRange clips;
    std::size_t destinationBusIndex{masterDestinationIndex};
    PreparedSendRange preFaderSends;
    PreparedSendRange postFaderSends;
    PreparedInsertRange inserts;
    PreparedLatencyRange preFaderTapLatency;
    PreparedLatencyRange postFaderTapLatency;
    PreparedLatencyRange mainOutputLatency;
};

enum class PreparedSendSourceKind : std::uint8_t { track, bus };

struct PreparedSendDescriptor {
    routing::SendId id;
    PreparedSendSourceKind sourceKind{PreparedSendSourceKind::track};
    std::size_t sourceIndex{};
    std::size_t destinationBusIndex{masterDestinationIndex};
    std::size_t destinationBufferIndex{masterDestinationIndex};
    routing::SendTapPoint tapPoint{routing::SendTapPoint::postFaderPostPan};
    std::size_t runtimeIndex{};
    std::size_t audibilityIndex{};
    mixer::PreparedSendMixState mix;
    PreparedLatencyRange sourceTapLatency;
};

struct PreparedSendIndex {
    routing::SendId id;
    std::size_t denseIndex{};
};

struct PreparedBusNode {
    routing::BusId id;
    std::size_t bufferIndex{};
    mixer::PreparedBusMixState mix;
    std::size_t destinationBusIndex{masterDestinationIndex};
    PreparedSendRange preFaderSends;
    PreparedSendRange postFaderSends;
    PreparedInsertRange inserts;
    PreparedLatencyRange inputLatency;
    PreparedLatencyRange preFaderTapLatency;
    PreparedLatencyRange postFaderTapLatency;
    PreparedLatencyRange mainOutputLatency;
};

struct PreparedProcessorDescriptor {
    processors::ProcessorInstanceId id;
    std::size_t runtimeIndex{};
    std::size_t chainIndex{};
    processors::ProcessingFormat format;
    processors::ProcessingFrameCount latency;
    processors::TailInfo tail;
    processors::ProcessorCapabilities capabilities;
    bool initiallyBypassed{};
};

struct PreparedProcessorIndex {
    processors::ProcessorInstanceId id;
    std::size_t denseIndex{};
};

enum class ProcessingStepKind : std::uint8_t { track, bus, master };

struct ProcessingStep {
    ProcessingStepKind kind{ProcessingStepKind::master};
    std::size_t index{};
};

struct PreparedProcessingPlan {
    timeline::SampleRate projectSampleRate;
    timeline::ProjectFrameCount duration;
    std::vector<PreparedTrackRoute> tracks;
    std::vector<PreparedSourceView> sources;
    std::vector<PreparedClipView> clips;
    std::vector<double> clipPrefixMaximumEnd;
    std::vector<PreparedBusNode> buses;
    std::vector<PreparedSendDescriptor> sends;
    std::vector<PreparedSendIndex> sendIndexById;
    std::vector<PreparedProcessorDescriptor> processors;
    std::vector<PreparedProcessorIndex> processorIndexById;
    std::vector<ProcessingStep> order;
    std::size_t blockCapacity{};
    std::size_t runtimeMemoryBytes{};
    processors::ProcessingFormat stereoProcessingFormat;
    mixer::PreparedMasterMixState masterMix;
    PreparedInsertRange masterInserts;
    PreparedLatencyRange masterInputLatency;
    PreparedLatencyRange masterOutputLatency;
    PreparedAudibilityState audibility;
};

struct StereoWorkBuffer {
    std::vector<float> left;
    std::vector<float> right;

    explicit StereoWorkBuffer(std::size_t capacity);
};

struct ProcessorNodeScratch {
    StereoWorkBuffer first;
    StereoWorkBuffer second;

    explicit ProcessorNodeScratch(std::size_t capacity);
};

class BypassDelayLine {
public:
    BypassDelayLine() = default;
    BypassDelayLine(processors::ProcessingFrameCount latency,
                    std::size_t channelCount);

    void reset() noexcept;
    void process(audio::ConstAudioBlockView input,
                 audio::AudioBlockView output, bool writeOutput) noexcept;
    [[nodiscard]] std::size_t memoryBytes() const noexcept;

private:
    std::array<std::vector<float>, 2> samples_;
    std::size_t latency_{};
    std::size_t channelCount_{};
    std::size_t writePosition_{};
};

struct ProcessorRuntime {
    std::unique_ptr<processors::IAudioProcessor> instance;
    BypassDelayLine bypassDelay;
    bool bypassed{};
};

struct ProcessingPlanRuntime {
    std::vector<StereoWorkBuffer> buses;
    std::vector<SendMixSmoother> sendMix;
    StereoWorkBuffer master;
    std::vector<double> projectPositions;
    std::vector<ProcessorNodeScratch> processorScratch;
    std::vector<ProcessorRuntime> processors;

    ProcessingPlanRuntime(std::size_t busCount, std::size_t sendCount,
                          std::size_t nodeCount, std::size_t capacity);
};

struct PreparedProcessingBundle {
    PreparedProcessingPlan plan;
    ProcessingPlanRuntime runtime;

    PreparedProcessingBundle(PreparedProcessingPlan preparedPlan,
                             ProcessingPlanRuntime preparedRuntime);
};

struct ProcessingPlanPreparationResult {
    std::unique_ptr<PreparedProcessingBundle> prepared;
    std::string errorMessage;

    [[nodiscard]] bool success() const noexcept { return prepared != nullptr; }
};

[[nodiscard]] ProcessingPlanPreparationResult prepareProcessingPlan(
    const ProcessingPlanSpecification& specification,
    std::span<const PreparedTrackView> sources,
    std::size_t blockCapacity = defaultProcessingBlockCapacity,
    std::size_t memoryBudgetBytes = defaultProcessingMemoryBudgetBytes,
    const processors::IAudioProcessorFactory* processorFactory = nullptr)
    noexcept;

[[nodiscard]] ProcessingPlanPreparationResult prepareProcessingPlanFromSources(
    const ProcessingPlanSpecification& specification,
    std::span<const PreparedSourceView> sources,
    std::size_t blockCapacity = defaultProcessingBlockCapacity,
    std::size_t memoryBudgetBytes = defaultProcessingMemoryBudgetBytes,
    const processors::IAudioProcessorFactory* processorFactory = nullptr)
    noexcept;

} // namespace vitadaw::audio
