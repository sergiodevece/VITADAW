#pragma once

#include "vitadaw/audio/MixerSmoother.h"
#include "vitadaw/audio/AudibilityState.h"
#include "vitadaw/audio/PreparedProject.h"
#include "vitadaw/routing/RoutingState.h"

#include <cstddef>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace vitadaw::audio {

inline constexpr std::size_t maximumPreparedTracks = 256;
inline constexpr std::size_t maximumPreparedBuses = 64;
inline constexpr std::size_t maximumPreparedSends = 1024;
inline constexpr std::size_t maximumPreparedSendsPerTrack = 64;
inline constexpr std::size_t defaultProcessingBlockCapacity = 512;
inline constexpr std::size_t defaultProcessingMemoryBudgetBytes =
    16U * 1024U * 1024U;
inline constexpr std::size_t masterDestinationIndex =
    std::numeric_limits<std::size_t>::max();
static_assert(maximumPreparedTracks == audibilityTrackCapacity);
static_assert(maximumPreparedBuses == audibilityBusCapacity);
static_assert(maximumPreparedSends == audibilitySendCapacity);

struct ProcessingPlanTrackSpecification {
    tracks::TrackId id;
    mixer::PreparedTrackMixState mix;
    routing::OutputDestination destination;
};

struct ProcessingPlanBusSpecification {
    routing::BusId id;
    mixer::PreparedBusMixState mix;
    routing::OutputDestination destination{routing::OutputDestination::master()};
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
    std::vector<ProcessingPlanBusSpecification> buses;
    std::vector<ProcessingPlanSendSpecification> sends;
    mixer::PreparedMasterMixState masterMix;
};

struct PreparedSendRange {
    std::size_t first{};
    std::size_t count{};
};

struct PreparedTrackRoute {
    PreparedTrackView source;
    std::size_t destinationBusIndex{masterDestinationIndex};
    PreparedSendRange preFaderSends;
    PreparedSendRange postFaderSends;
};

struct PreparedSendDescriptor {
    routing::SendId id;
    std::size_t sourceTrackIndex{};
    std::size_t destinationBusIndex{masterDestinationIndex};
    routing::SendTapPoint tapPoint{routing::SendTapPoint::postFaderPostPan};
    std::size_t runtimeIndex{};
    mixer::PreparedSendMixState mix;
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
    std::vector<PreparedBusNode> buses;
    std::vector<PreparedSendDescriptor> sends;
    std::vector<PreparedSendIndex> sendIndexById;
    std::vector<ProcessingStep> order;
    std::size_t blockCapacity{};
    std::size_t runtimeMemoryBytes{};
    mixer::PreparedMasterMixState masterMix;
    PreparedAudibilityState audibility;
};

struct StereoWorkBuffer {
    std::vector<float> left;
    std::vector<float> right;

    explicit StereoWorkBuffer(std::size_t capacity);
};

struct ProcessingPlanRuntime {
    std::vector<StereoWorkBuffer> buses;
    std::vector<SendMixSmoother> sendMix;
    StereoWorkBuffer master;
    std::vector<double> projectPositions;

    ProcessingPlanRuntime(std::size_t busCount, std::size_t sendCount,
                          std::size_t capacity);
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
    std::size_t memoryBudgetBytes = defaultProcessingMemoryBudgetBytes) noexcept;

} // namespace vitadaw::audio
