#include "vitadaw/audio/PreparedProcessingPlan.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <memory>
#include <new>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace vitadaw::audio {

StereoWorkBuffer::StereoWorkBuffer(std::size_t capacity)
    : left(capacity), right(capacity) {}

ProcessorNodeScratch::ProcessorNodeScratch(std::size_t capacity)
    : first(capacity), second(capacity) {}

BypassDelayLine::BypassDelayLine(processors::ProcessingFrameCount latency,
                                 std::size_t channelCount)
    : latency_(static_cast<std::size_t>(latency.value)),
      channelCount_(channelCount) {
    for (std::size_t channel = 0; channel < channelCount_; ++channel) {
        samples_[channel].resize(latency_);
    }
}

void BypassDelayLine::reset() noexcept {
    for (std::size_t channel = 0; channel < channelCount_; ++channel) {
        std::fill(samples_[channel].begin(), samples_[channel].end(), 0.0F);
    }
    writePosition_ = 0;
}

void BypassDelayLine::process(audio::ConstAudioBlockView input,
                              audio::AudioBlockView output,
                              bool writeOutput) noexcept {
    const auto channels = std::min({input.channelCount, output.channelCount,
                                    channelCount_});
    const auto frames = std::min(input.frameCount, output.frameCount);
    if (input.channels == nullptr || output.channels == nullptr) {
        return;
    }
    for (std::size_t frame = 0; frame < frames; ++frame) {
        const auto position = writePosition_;
        for (std::size_t channel = 0; channel < channels; ++channel) {
            const auto source = input.channels[channel][frame];
            const auto delayed = latency_ == 0 ? source
                                               : samples_[channel][position];
            if (latency_ != 0) {
                samples_[channel][position] = source;
            }
            if (writeOutput) {
                output.channels[channel][frame] = delayed;
            }
        }
        if (latency_ != 0) {
            writePosition_ = (writePosition_ + 1) % latency_;
        }
    }
}

std::size_t BypassDelayLine::memoryBytes() const noexcept {
    return latency_ * channelCount_ * sizeof(float);
}

ProcessingPlanRuntime::ProcessingPlanRuntime(std::size_t busCount,
                                             std::size_t sendCount,
                                             std::size_t nodeCount,
                                             std::size_t capacity)
    : sendMix(sendCount), master(capacity), projectPositions(capacity) {
    buses.reserve(busCount);
    for (std::size_t index = 0; index < busCount; ++index) {
        buses.emplace_back(capacity);
    }
    processorScratch.reserve(nodeCount);
    for (std::size_t index = 0; index < nodeCount; ++index) {
        processorScratch.emplace_back(capacity);
    }
}

PreparedProcessingBundle::PreparedProcessingBundle(
    PreparedProcessingPlan preparedPlan, ProcessingPlanRuntime preparedRuntime)
    : plan(std::move(preparedPlan)), runtime(std::move(preparedRuntime)) {}

namespace {

[[nodiscard]] bool checkedAdd(std::size_t left, std::size_t right,
                              std::size_t& result) noexcept {
    if (left > std::numeric_limits<std::size_t>::max() - right) {
        return false;
    }
    result = left + right;
    return true;
}

[[nodiscard]] bool checkedMultiply(std::size_t left, std::size_t right,
                                   std::size_t& result) noexcept {
    if (left != 0 && right > std::numeric_limits<std::size_t>::max() / left) {
        return false;
    }
    result = left * right;
    return true;
}

[[nodiscard]] bool checkedLatencyAdd(
    processors::ProcessingFrameCount left,
    processors::ProcessingFrameCount right,
    processors::ProcessingFrameCount& result) noexcept {
    if (left.value > std::numeric_limits<std::uint64_t>::max() - right.value) {
        return false;
    }
    result.value = left.value + right.value;
    return true;
}

[[nodiscard]] bool addLatency(PreparedLatencyRange input,
                              processors::ProcessingFrameCount added,
                              PreparedLatencyRange& result) noexcept {
    return checkedLatencyAdd(input.minimum, added, result.minimum) &&
           checkedLatencyAdd(input.maximum, added, result.maximum);
}

void mergeLatency(bool& hasValue, PreparedLatencyRange& destination,
                  PreparedLatencyRange value) noexcept {
    if (!hasValue) {
        destination = value;
        hasValue = true;
        return;
    }
    destination.minimum.value =
        std::min(destination.minimum.value, value.minimum.value);
    destination.maximum.value =
        std::max(destination.maximum.value, value.maximum.value);
}

struct PendingProcessorRuntime {
    std::unique_ptr<processors::IAudioProcessor> instance;
    processors::ProcessingFrameCount latency;
    std::size_t channelCount{};
    bool bypassed{};
};

} // namespace

static ProcessingPlanPreparationResult prepareProcessingPlanImpl(
    const ProcessingPlanSpecification& specification,
    std::span<const PreparedTrackView> legacySources,
    std::span<const PreparedSourceView> preparedSources,
    std::size_t blockCapacity,
    std::size_t memoryBudgetBytes,
    const processors::IAudioProcessorFactory* processorFactory) noexcept {
    try {
        const auto* factory = processorFactory != nullptr
                                  ? processorFactory
                                  : &processors::internalAudioProcessorFactory();
        if (!specification.projectSampleRate.isValid()) {
            return {nullptr, "Project sample rate must be finite and positive"};
        }
        const auto processingSampleRate =
            specification.processingSampleRate.isValid()
                ? specification.processingSampleRate
                : specification.projectSampleRate;
        if (!processingSampleRate.isValid() || blockCapacity == 0 ||
            !specification.masterMix.isValid()) {
            return {nullptr, "Invalid processing format or master state"};
        }
        if (specification.tracks.size() > maximumPreparedTracks) {
            return {nullptr, "Prepared track capacity exceeded"};
        }
        if (specification.buses.size() > maximumPreparedBuses) {
            return {nullptr, "Prepared bus capacity exceeded"};
        }
        if (specification.sends.size() > maximumPreparedSends) {
            return {nullptr, "Prepared send capacity exceeded"};
        }
        if (specification.sources.size() > maximumPreparedSources) {
            return {nullptr, "Prepared source capacity exceeded"};
        }
        std::size_t totalClipCount{};
        for (const auto& track : specification.tracks) {
            if (track.clips.size() > maximumPreparedClipsPerTrack ||
                !checkedAdd(totalClipCount, track.clips.size(), totalClipCount)) {
                return {nullptr, "Prepared per-track clip capacity exceeded"};
            }
        }
        if (totalClipCount > maximumPreparedClips) {
            return {nullptr, "Prepared clip capacity exceeded"};
        }

        std::size_t processorCount = specification.masterInserts.processors.size();
        if (specification.masterInserts.processors.size() >
            maximumPreparedProcessorsPerChain) {
            return {nullptr, "Prepared processors-per-chain capacity exceeded"};
        }
        for (const auto& track : specification.tracks) {
            if (track.inserts.processors.size() >
                maximumPreparedProcessorsPerChain ||
                !checkedAdd(processorCount, track.inserts.processors.size(),
                            processorCount)) {
                return {nullptr, "Prepared processors-per-chain capacity exceeded"};
            }
        }
        for (const auto& bus : specification.buses) {
            if (bus.inserts.processors.size() >
                maximumPreparedProcessorsPerChain ||
                !checkedAdd(processorCount, bus.inserts.processors.size(),
                            processorCount)) {
                return {nullptr, "Prepared processors-per-chain capacity exceeded"};
            }
        }
        if (processorCount > maximumPreparedProcessors) {
            return {nullptr, "Prepared processor capacity exceeded"};
        }

        const auto nodeCount = specification.tracks.size() +
                               specification.buses.size() + 1;
        const auto mixBufferCount = specification.buses.size() + 1;
        constexpr auto stereoSampleBytes = std::size_t{2} * sizeof(float);
        constexpr auto nodeScratchSampleBytes = std::size_t{4} * sizeof(float);
        std::size_t mixBytesPerFrame{};
        std::size_t scratchBytesPerFrame{};
        std::size_t bytesPerFrame{};
        if (!checkedMultiply(mixBufferCount, stereoSampleBytes,
                             mixBytesPerFrame) ||
            !checkedMultiply(nodeCount, nodeScratchSampleBytes,
                             scratchBytesPerFrame) ||
            !checkedAdd(mixBytesPerFrame, scratchBytesPerFrame,
                        bytesPerFrame) ||
            !checkedAdd(bytesPerFrame, sizeof(DspFramePosition), bytesPerFrame)) {
            return {nullptr, "Processing buffer size overflow"};
        }
        std::size_t preparedBytes{};
        if (!checkedMultiply(blockCapacity, bytesPerFrame, preparedBytes)) {
            return {nullptr, "Processing buffer size overflow"};
        }
        const auto addPreparedArray = [&preparedBytes](std::size_t count,
                                                       std::size_t itemSize) {
            std::size_t bytes{};
            std::size_t total{};
            return checkedMultiply(count, itemSize, bytes) &&
                   checkedAdd(preparedBytes, bytes, total) &&
                   (preparedBytes = total, true);
        };
        const auto stepCount = specification.tracks.size() +
                               specification.buses.size() + 1;
        if (!addPreparedArray(specification.tracks.size(),
                              sizeof(PreparedTrackRoute)) ||
            !addPreparedArray(specification.buses.size(),
                              sizeof(PreparedBusNode)) ||
            !addPreparedArray(specification.sends.size(),
                              sizeof(PreparedSendDescriptor)) ||
            !addPreparedArray(specification.sends.size(),
                              sizeof(PreparedSendIndex)) ||
            !addPreparedArray(specification.sends.size(),
                              sizeof(SendMixSmoother)) ||
            !addPreparedArray(specification.sources.size(),
                              sizeof(PreparedSourceView)) ||
            !addPreparedArray(totalClipCount, sizeof(PreparedClipView)) ||
            !addPreparedArray(totalClipCount, sizeof(double)) ||
            !addPreparedArray(processorCount,
                              sizeof(PreparedProcessorDescriptor)) ||
            !addPreparedArray(processorCount,
                              sizeof(PreparedProcessorIndex)) ||
            !addPreparedArray(processorCount, sizeof(ProcessorRuntime)) ||
            !addPreparedArray(stepCount, sizeof(ProcessingStep)) ||
            !addPreparedArray(1, sizeof(PreparedAudibilityState))) {
            return {nullptr, "Prepared processing size overflow"};
        }

        std::unordered_set<std::uint64_t> busIds;
        busIds.reserve(specification.buses.size());
        for (const auto& bus : specification.buses) {
            if (!bus.id.isValid() || !bus.mix.isValid() ||
                !bus.destination.isValid() ||
                !busIds.insert(bus.id.value).second) {
                return {nullptr, "Invalid or duplicate BusId"};
            }
        }
        for (const auto& bus : specification.buses) {
            if (bus.destination.kind == routing::DestinationKind::bus &&
                !busIds.contains(bus.destination.bus.value)) {
                return {nullptr, "Bus output destination does not exist"};
            }
        }

        std::unordered_set<std::uint64_t> trackIds;
        trackIds.reserve(specification.tracks.size());
        for (const auto& track : specification.tracks) {
            if (!track.id.isValid() || !track.mix.isValid() ||
                !track.destination.isValid() ||
                !trackIds.insert(track.id.value).second) {
                return {nullptr, "Invalid or duplicate track routing state"};
            }
            if (track.destination.kind == routing::DestinationKind::bus &&
                !busIds.contains(track.destination.bus.value)) {
                return {nullptr, "Track output destination does not exist"};
            }
        }
        std::unordered_set<std::uint64_t> sourceIds;
        sourceIds.reserve(legacySources.size());
        for (const auto& source : legacySources) {
            const auto known = std::any_of(
                specification.tracks.begin(), specification.tracks.end(),
                [id = source.id](const auto& track) { return track.id == id; });
            if (!known || !source.isAvailable() ||
                !sourceIds.insert(source.id.value).second) {
                return {nullptr, "Invalid, duplicate, or unknown audio source"};
            }
        }
        std::unordered_set<std::uint64_t> catalogSourceIds;
        catalogSourceIds.reserve(specification.sources.size());
        if (preparedSources.size() != specification.sources.size()) {
            return {nullptr, "Prepared PCM source catalog is incomplete"};
        }
        for (const auto& source : specification.sources) {
            const auto prepared = std::find_if(
                preparedSources.begin(), preparedSources.end(),
                [id = source.id](const auto& candidate) {
                    return candidate.id == id;
                });
            if (!source.id.isValid() || source.frameCount.value == 0 ||
                !source.sampleRate.isValid() ||
                !catalogSourceIds.insert(source.id.value).second ||
                prepared == preparedSources.end() || !prepared->isAvailable() ||
                prepared->frameCount != source.frameCount ||
                prepared->sampleRate != source.sampleRate ||
                prepared->layout != source.layout) {
                return {nullptr, "Invalid, duplicate, or missing prepared source"};
            }
        }

        std::unordered_set<std::uint64_t> sendIds;
        sendIds.reserve(specification.sends.size());
        std::unordered_map<std::uint64_t, std::size_t> sendsPerTrack;
        std::unordered_map<std::uint64_t, std::size_t> sendsPerBus;
        for (const auto& send : specification.sends) {
            if (!send.id.isValid() || !send.destination.isValid() ||
                !busIds.contains(send.destination.value) ||
                !routing::isValid(send.tapPoint) || !send.mix.isValid() ||
                !sendIds.insert(send.id.value).second) {
                return {nullptr, "Invalid or duplicate send routing state"};
            }
            if (const auto* track = std::get_if<tracks::TrackId>(&send.source)) {
                if (!track->isValid() || !trackIds.contains(track->value) ||
                    ++sendsPerTrack[track->value] >
                        maximumPreparedSendsPerTrack) {
                    return {nullptr, "Invalid track send source or capacity"};
                }
            } else if (const auto* bus =
                           std::get_if<routing::BusId>(&send.source)) {
                if (!bus->isValid() || !busIds.contains(bus->value) ||
                    ++sendsPerBus[bus->value] > maximumPreparedSendsPerBus) {
                    return {nullptr, "Invalid bus send source or capacity"};
                }
            } else {
                return {nullptr, "Invalid send source"};
            }
        }

        PreparedProcessingPlan plan;
        plan.exactClock = exact::clockForPreparation(specification.projectSampleRate.hertz(), processingSampleRate.hertz());
        if (!plan.exactClock.valid) return {nullptr, "Unsupported exact project/device rate configuration"};
        plan.projectSampleRate = specification.projectSampleRate;
        plan.blockCapacity = blockCapacity;
        plan.stereoProcessingFormat = {
            processingSampleRate, blockCapacity,
            processors::ChannelLayout::stereo, specification.processingMode};
        plan.masterMix = specification.masterMix;
        plan.tracks.reserve(specification.tracks.size());
        plan.sources.reserve(specification.sources.size());
        plan.clips.reserve(totalClipCount);
        plan.clipPrefixMaximumEnd.reserve(totalClipCount);
        plan.buses.reserve(specification.buses.size());
        plan.sends.reserve(specification.sends.size());
        plan.sendIndexById.reserve(specification.sends.size());
        plan.processors.reserve(processorCount);
        plan.processorIndexById.reserve(processorCount);
        plan.order.reserve(stepCount);

        std::vector<std::size_t> sourceSpecificationOrder(
            specification.sources.size());
        for (std::size_t index = 0; index < sourceSpecificationOrder.size();
             ++index) {
            sourceSpecificationOrder[index] = index;
        }
        std::sort(sourceSpecificationOrder.begin(),
                  sourceSpecificationOrder.end(),
                  [&specification](const auto left, const auto right) {
                      return specification.sources[left].id <
                             specification.sources[right].id;
                  });
        for (const auto index : sourceSpecificationOrder) {
            const auto id = specification.sources[index].id;
            const auto prepared = std::find_if(
                preparedSources.begin(), preparedSources.end(),
                [id](const auto& candidate) { return candidate.id == id; });
            if (prepared != preparedSources.end()) {
                plan.sources.push_back(*prepared);
            }
        }

        std::vector<PendingProcessorRuntime> pendingProcessors;
        pendingProcessors.reserve(processorCount);
        std::unordered_set<std::uint64_t> processorIds;
        processorIds.reserve(processorCount);
        const auto prepareChain = [&](const processors::InsertChain& chain,
                                      processors::ChannelLayout layout,
                                      std::size_t scratchIndex,
                                      PreparedInsertRange& result,
                                      auto&& self) -> std::string {
            static_cast<void>(self);
            result.first = plan.processors.size();
            result.count = chain.processors.size();
            result.scratchIndex = scratchIndex;
            result.layout = layout;
            processors::ProcessingFrameCount chainLatency;
            std::size_t chainIndex{};
            for (const auto& state : chain.processors) {
                if (!state.id.isValid() || !state.type.isValid() ||
                    !processorIds.insert(state.id.value).second) {
                    return "Invalid or duplicate ProcessorInstanceId";
                }
                std::unordered_set<std::uint32_t> parameterIds;
                for (const auto& parameter : state.parameters) {
                    if (!parameter.id.isValid() ||
                        !std::isfinite(parameter.value) ||
                        !parameterIds.insert(parameter.id.value).second) {
                        return "Invalid or duplicate processor parameter";
                    }
                }
                auto instance = factory->create(state);
                if (instance == nullptr) {
                    return "Processor type or state could not be created";
                }
                const processors::ProcessingFormat format{
                    processingSampleRate, blockCapacity, layout,
                    specification.processingMode};
                const auto capabilities = instance->capabilities();
                const auto layoutSupported =
                    layout == processors::ChannelLayout::mono
                        ? capabilities.supportsMono
                        : capabilities.supportsStereo;
                if (!layoutSupported ||
                    (!capabilities.supportsInPlace &&
                     !capabilities.supportsOutOfPlace) ||
                    !instance->prepare(format)) {
                    return "Processor does not support the prepared format";
                }
                const auto latency = instance->latency();
                processors::ProcessingFrameCount updatedLatency;
                if (latency.value >
                        static_cast<std::uint64_t>(
                            std::numeric_limits<std::size_t>::max()) ||
                    !checkedLatencyAdd(chainLatency, latency,
                                       updatedLatency)) {
                    return "Processor chain latency overflow";
                }
                chainLatency = updatedLatency;
                std::size_t delayBytes{};
                if (!checkedMultiply(static_cast<std::size_t>(latency.value),
                                     format.channelCount() * sizeof(float),
                                     delayBytes) ||
                    !addPreparedArray(1, instance->runtimeMemoryBytes()) ||
                    !addPreparedArray(1, delayBytes)) {
                    return "Processor runtime size overflow";
                }
                const auto denseIndex = plan.processors.size();
                plan.processors.push_back(
                    {state.id, denseIndex, chainIndex, format, latency,
                     instance->tail(), capabilities, state.bypassed});
                plan.processorIndexById.push_back({state.id, denseIndex});
                pendingProcessors.push_back(
                    {std::move(instance), latency, format.channelCount(),
                     state.bypassed});
                ++chainIndex;
            }
            result.latency = chainLatency;
            return {};
        };

        std::vector<std::size_t> busSpecificationOrder(
            specification.buses.size());
        for (std::size_t index = 0; index < busSpecificationOrder.size(); ++index) {
            busSpecificationOrder[index] = index;
        }
        std::sort(busSpecificationOrder.begin(), busSpecificationOrder.end(),
                  [&specification](const auto left, const auto right) {
                      return specification.buses[left].id <
                             specification.buses[right].id;
                  });
        for (const auto specificationIndex : busSpecificationOrder) {
            const auto& bus = specification.buses[specificationIndex];
            PreparedBusNode node;
            node.id = bus.id;
            node.bufferIndex = plan.buses.size();
            node.mix = bus.mix;
            plan.buses.push_back(std::move(node));
        }
        const auto denseBusIndex = [&plan](routing::BusId id) {
            const auto found = std::lower_bound(
                plan.buses.begin(), plan.buses.end(), id,
                [](const auto& bus, const auto value) { return bus.id < value; });
            return found != plan.buses.end() && found->id == id
                       ? static_cast<std::size_t>(found - plan.buses.begin())
                       : masterDestinationIndex;
        };
        for (const auto specificationIndex : busSpecificationOrder) {
            const auto& bus = specification.buses[specificationIndex];
            const auto source = denseBusIndex(bus.id);
            if (bus.destination.kind == routing::DestinationKind::bus) {
                plan.buses[source].destinationBusIndex =
                    denseBusIndex(bus.destination.bus);
            }
            const auto error = prepareChain(
                bus.inserts, processors::ChannelLayout::stereo,
                specification.tracks.size() + source,
                plan.buses[source].inserts, prepareChain);
            if (!error.empty()) {
                return {nullptr, error};
            }
        }

        std::vector<std::vector<std::size_t>> busEdges(plan.buses.size());
        for (std::size_t index = 0; index < plan.buses.size(); ++index) {
            if (plan.buses[index].destinationBusIndex != masterDestinationIndex) {
                busEdges[index].push_back(plan.buses[index].destinationBusIndex);
            }
        }
        for (const auto& send : specification.sends) {
            if (const auto* sourceBus =
                    std::get_if<routing::BusId>(&send.source)) {
                busEdges[denseBusIndex(*sourceBus)].push_back(
                    denseBusIndex(send.destination));
            }
        }
        for (auto& edges : busEdges) {
            std::sort(edges.begin(), edges.end());
            edges.erase(std::unique(edges.begin(), edges.end()), edges.end());
        }

        std::vector<std::uint8_t> colours(plan.buses.size());
        std::vector<std::size_t> path;
        path.reserve(plan.buses.size());
        std::string cycleError;
        const auto visit = [&](auto&& self, std::size_t index) -> bool {
            colours[index] = 1;
            path.push_back(index);
            for (const auto destination : busEdges[index]) {
                if (colours[destination] == 1) {
                    const auto beginning =
                        std::find(path.begin(), path.end(), destination);
                    std::ostringstream message;
                    message << "Routing rejected: ";
                    for (auto cursor = beginning; cursor != path.end(); ++cursor) {
                        message << "Bus " << plan.buses[*cursor].id.value
                                << " -> ";
                    }
                    message << "Bus " << plan.buses[destination].id.value;
                    cycleError = message.str();
                    return false;
                }
                if (colours[destination] == 0 && !self(self, destination)) {
                    return false;
                }
            }
            path.pop_back();
            colours[index] = 2;
            return true;
        };
        for (std::size_t index = 0; index < plan.buses.size(); ++index) {
            if (colours[index] == 0 && !visit(visit, index)) {
                return {nullptr, std::move(cycleError)};
            }
        }

        std::vector<std::size_t> busIndegree(plan.buses.size());
        for (const auto& edges : busEdges) {
            for (const auto destination : edges) {
                ++busIndegree[destination];
            }
        }
        std::vector<bool> busEmitted(plan.buses.size());
        std::vector<std::size_t> topologicalBuses;
        topologicalBuses.reserve(plan.buses.size());
        for (std::size_t emitted = 0; emitted < plan.buses.size(); ++emitted) {
            auto selected = masterDestinationIndex;
            for (std::size_t index = 0; index < plan.buses.size(); ++index) {
                if (!busEmitted[index] && busIndegree[index] == 0) {
                    selected = index;
                    break;
                }
            }
            if (selected == masterDestinationIndex) {
                return {nullptr, "Routing rejected: cyclic bus graph"};
            }
            busEmitted[selected] = true;
            topologicalBuses.push_back(selected);
            for (const auto destination : busEdges[selected]) {
                --busIndegree[destination];
            }
        }

        std::vector<std::size_t> trackSpecificationOrder(
            specification.tracks.size());
        for (std::size_t index = 0; index < trackSpecificationOrder.size(); ++index) {
            trackSpecificationOrder[index] = index;
        }
        std::sort(trackSpecificationOrder.begin(), trackSpecificationOrder.end(),
                  [&specification](const auto left, const auto right) {
                      return specification.tracks[left].id <
                             specification.tracks[right].id;
                  });
        std::unordered_set<std::uint64_t> clipIds;
        clipIds.reserve(totalClipCount);
        for (const auto specificationIndex : trackSpecificationOrder) {
            const auto& track = specification.tracks[specificationIndex];
            std::size_t destination = masterDestinationIndex;
            if (track.destination.kind == routing::DestinationKind::bus) {
                destination = denseBusIndex(track.destination.bus);
            }
            PreparedTrackView source;
            source.id = track.id;
            source.mix = track.mix;
            const auto preparedSource = std::find_if(
                legacySources.begin(), legacySources.end(),
                [id = track.id](const auto& candidate) {
                    return candidate.id == id;
                });
            if (preparedSource != legacySources.end()) {
                source = *preparedSource;
                source.mix = track.mix;
                source.exactSource = exact::sourceForPreparation(source.sourceSampleRate.hertz(),
                    specification.projectSampleRate.hertz(), static_cast<double>(source.clipDuration.value),
                    static_cast<double>(source.sourceOffset.value), plan.exactClock);
                if (!source.exactSource.valid) return {nullptr, "Legacy source exceeds exact DSP capacity"};
                if (source.clipDuration.value >
                    std::numeric_limits<std::int64_t>::max() -
                        source.clipStart.value) {
                    return {nullptr, "Prepared clip end overflows project time"};
                }
                plan.duration.value = std::max(
                    plan.duration.value,
                    source.clipStart.value + source.clipDuration.value);
            }
            PreparedTrackRoute route;
            route.source = source;
            route.id = track.id;
            route.layout = track.clips.empty() && preparedSource != legacySources.end() &&
                                   preparedSource->channelCount == 2
                               ? media::AudioChannelLayout::stereo
                               : track.layout;
            route.clips.first = plan.clips.size();
            route.destinationBusIndex = destination;
            std::int64_t prefixMaximumEnd{};
            for (const auto& clip : track.clips) {
                const auto sourceFound = std::lower_bound(
                    plan.sources.begin(), plan.sources.end(), clip.source,
                    [](const auto& candidate, const auto id) {
                        return candidate.id < id;
                    });
                if (!clip.id.isValid() ||
                    !clipIds.insert(clip.id.value).second ||
                    sourceFound == plan.sources.end() ||
                    sourceFound->id != clip.source ||
                    sourceFound->layout != track.layout ||
                    !timeline::isSupportedProjectFramePosition(clip.projectStart) ||
                    !std::isfinite(clip.duration.value) ||
                    clip.duration.value <= 0.0 ||
                    !std::isfinite(clip.sourceOffset.value) ||
                    clip.sourceOffset.value < 0.0) {
                    return {nullptr, "Invalid clip or track/source layout mismatch"};
                }
                const auto projectStart =
                    static_cast<double>(clip.projectStart.value);
                // Conservative candidate-search bound. DSP uses local duration.
                const auto projectEnd = std::nextafter(projectStart + clip.duration.value,
                    std::numeric_limits<double>::infinity());
                const auto sourceEnd =
                    static_cast<long double>(clip.sourceOffset.value) +
                    static_cast<long double>(clip.duration.value) *
                        static_cast<long double>(sourceFound->sampleRate.hertz()) /
                        static_cast<long double>(
                            specification.projectSampleRate.hertz());
                const auto sourceLimit = static_cast<long double>(
                    sourceFound->frameCount.value);
                // ProjectState admits the tiny round-trip error introduced when
                // an exact source length is represented as a double project
                // duration. Plan preparation must use the same bound or a valid
                // 44.1/48 kHz clip can be rejected after an otherwise no-op edit.
                const auto roundTripTolerance =
                    std::numeric_limits<double>::epsilon() * 64.0L *
                    std::max(1.0L,
                             std::max(std::abs(sourceEnd), sourceLimit));
                if (!std::isfinite(projectEnd) ||
                    projectEnd > static_cast<double>(
                                     std::numeric_limits<std::int64_t>::max()) ||
                    !std::isfinite(sourceEnd) ||
                    sourceEnd > sourceLimit + roundTripTolerance) {
                    return {nullptr, "Prepared clip exceeds project/source bounds"};
                }
                if (!plan.clips.empty() &&
                    plan.clips.size() > route.clips.first) {
                    const auto& previous = plan.clips.back();
                    if (projectStart < previous.projectStart ||
                        (projectStart == previous.projectStart &&
                         clip.id < previous.id)) {
                        return {nullptr, "Track clips are not in canonical order"};
                    }
                }
                plan.clips.push_back(
                    {clip.id,
                     static_cast<std::size_t>(sourceFound - plan.sources.begin()),
                     projectStart, projectEnd, clip.sourceOffset.value,
                     sourceFound->sampleRate.hertz() /
                         specification.projectSampleRate.hertz(), clip.duration.value, {}, {}});
                auto& numericClip = plan.clips.back();
                numericClip.exactStart = clip.projectStart.value;
                numericClip.exactSource = exact::sourceForPreparation(sourceFound->sampleRate.hertz(),
                    specification.projectSampleRate.hertz(), clip.duration.value, clip.sourceOffset.value, plan.exactClock);
                if (!numericClip.exactSource.valid)
                    return {nullptr, "Clip exceeds the certified exact DSP domain"};
                ++route.clips.count;
                const auto end = timeline::checkedExclusiveProjectEnd(clip.projectStart, clip.duration);
                if (!end || !timeline::isSupportedProjectFramePosition(*end))
                    return {nullptr, "Prepared clip exceeds the supported numerical domain"};
                const auto exclusiveEnd = end->value;
                prefixMaximumEnd = std::max(prefixMaximumEnd, exclusiveEnd);
                plan.clipPrefixMaximumEnd.push_back(prefixMaximumEnd);
                plan.duration.value = std::max(plan.duration.value,
                                               exclusiveEnd);
            }
            plan.tracks.push_back(std::move(route));
            auto& preparedRoute = plan.tracks.back();
            const auto layout = preparedRoute.layout == media::AudioChannelLayout::stereo
                                    ? processors::ChannelLayout::stereo
                                    : processors::ChannelLayout::mono;
            const auto error = prepareChain(
                track.inserts, layout, plan.tracks.size() - 1,
                preparedRoute.inserts, prepareChain);
            if (!error.empty()) {
                return {nullptr, error};
            }
            const PreparedLatencyRange chainLatency{
                preparedRoute.inserts.latency,
                preparedRoute.inserts.latency};
            preparedRoute.preFaderTapLatency = chainLatency;
            preparedRoute.postFaderTapLatency = chainLatency;
            preparedRoute.mainOutputLatency = chainLatency;
            plan.order.push_back(
                {ProcessingStepKind::track, plan.tracks.size() - 1});
        }

        const auto denseTrackIndex = [&plan](tracks::TrackId id) {
            const auto found = std::lower_bound(
                plan.tracks.begin(), plan.tracks.end(), id,
                [](const auto& track, const auto value) {
                    return track.id < value;
                });
            return found != plan.tracks.end() && found->id == id
                       ? static_cast<std::size_t>(found - plan.tracks.begin())
                       : maximumPreparedTracks;
        };
        const auto sendSourceKind = [](const routing::SendSource& source) {
            return std::holds_alternative<tracks::TrackId>(source)
                       ? PreparedSendSourceKind::track
                       : PreparedSendSourceKind::bus;
        };
        const auto sendSourceIndex = [&denseTrackIndex, &denseBusIndex](
                                         const routing::SendSource& source) {
            if (const auto* track = std::get_if<tracks::TrackId>(&source)) {
                return denseTrackIndex(*track);
            }
            return denseBusIndex(std::get<routing::BusId>(source));
        };
        std::vector<std::size_t> sendSpecificationOrder(
            specification.sends.size());
        for (std::size_t index = 0; index < sendSpecificationOrder.size();
             ++index) {
            sendSpecificationOrder[index] = index;
        }
        std::sort(sendSpecificationOrder.begin(), sendSpecificationOrder.end(),
                  [&specification, &sendSourceKind,
                   &sendSourceIndex](const auto left, const auto right) {
                      const auto& a = specification.sends[left];
                      const auto& b = specification.sends[right];
                      const auto aKind = sendSourceKind(a.source);
                      const auto bKind = sendSourceKind(b.source);
                      if (aKind != bKind) {
                          return aKind < bKind;
                      }
                      const auto aSource = sendSourceIndex(a.source);
                      const auto bSource = sendSourceIndex(b.source);
                      if (aSource != bSource) {
                          return aSource < bSource;
                      }
                      if (a.tapPoint != b.tapPoint) {
                          return a.tapPoint ==
                                 routing::SendTapPoint::preFaderPrePan;
                      }
                      return a.id < b.id;
                  });
        for (const auto specificationIndex : sendSpecificationOrder) {
            const auto& send = specification.sends[specificationIndex];
            const auto sourceKind = sendSourceKind(send.source);
            const auto sourceIndex = sendSourceIndex(send.source);
            auto& range = sourceKind == PreparedSendSourceKind::track
                ? (send.tapPoint == routing::SendTapPoint::preFaderPrePan
                       ? plan.tracks[sourceIndex].preFaderSends
                       : plan.tracks[sourceIndex].postFaderSends)
                : (send.tapPoint == routing::SendTapPoint::preFaderPrePan
                       ? plan.buses[sourceIndex].preFaderSends
                       : plan.buses[sourceIndex].postFaderSends);
            if (range.count == 0) {
                range.first = plan.sends.size();
            }
            const auto runtimeIndex = plan.sends.size();
            const auto destinationBusIndex = denseBusIndex(send.destination);
            plan.sends.push_back(
                {send.id, sourceKind, sourceIndex, destinationBusIndex,
                 plan.buses[destinationBusIndex].bufferIndex, send.tapPoint,
                 runtimeIndex, plan.sends.size(), send.mix, {}});
            ++range.count;
        }
        for (std::size_t index = 0; index < plan.sends.size(); ++index) {
            plan.sendIndexById.push_back({plan.sends[index].id, index});
        }
        std::sort(plan.sendIndexById.begin(), plan.sendIndexById.end(),
                  [](const auto& left, const auto& right) {
                      return left.id < right.id;
                  });

        const auto masterError = prepareChain(
            specification.masterInserts, processors::ChannelLayout::stereo,
            specification.tracks.size() + specification.buses.size(),
            plan.masterInserts, prepareChain);
        if (!masterError.empty()) {
            return {nullptr, masterError};
        }
        std::sort(plan.processorIndexById.begin(),
                  plan.processorIndexById.end(),
                  [](const auto& left, const auto& right) {
                      return left.id < right.id;
                  });

        for (const auto index : topologicalBuses) {
            plan.order.push_back({ProcessingStepKind::bus, index});
        }
        plan.order.push_back({ProcessingStepKind::master, 0});

        std::vector<std::uint8_t> busHasInput(plan.buses.size());
        std::vector<PreparedLatencyRange> busInput(plan.buses.size());
        bool masterHasInput{};
        PreparedLatencyRange masterInput{};
        const auto routeLatency = [&](std::size_t destination,
                                      PreparedLatencyRange latency) {
            if (destination == masterDestinationIndex) {
                mergeLatency(masterHasInput, masterInput, latency);
            } else {
                auto hasInput = busHasInput[destination] != 0;
                mergeLatency(hasInput, busInput[destination], latency);
                busHasInput[destination] = hasInput ? 1 : 0;
            }
        };
        for (const auto& track : plan.tracks) {
            routeLatency(track.destinationBusIndex, track.mainOutputLatency);
            for (std::size_t index = 0; index < plan.sends.size(); ++index) {
                auto& send = plan.sends[index];
                if (send.sourceKind == PreparedSendSourceKind::track &&
                    send.sourceIndex ==
                        static_cast<std::size_t>(&track - plan.tracks.data())) {
                    send.sourceTapLatency =
                        send.tapPoint == routing::SendTapPoint::preFaderPrePan
                            ? track.preFaderTapLatency
                            : track.postFaderTapLatency;
                    routeLatency(send.destinationBusIndex,
                                 send.sourceTapLatency);
                }
            }
        }
        for (const auto busIndex : topologicalBuses) {
            auto& bus = plan.buses[busIndex];
            bus.inputLatency = busHasInput[busIndex]
                                   ? busInput[busIndex]
                                   : PreparedLatencyRange{};
            if (!addLatency(bus.inputLatency, bus.inserts.latency,
                            bus.preFaderTapLatency)) {
                return {nullptr, "Prepared bus latency overflow"};
            }
            bus.postFaderTapLatency = bus.preFaderTapLatency;
            bus.mainOutputLatency = bus.preFaderTapLatency;
            routeLatency(bus.destinationBusIndex, bus.mainOutputLatency);
            for (auto& send : plan.sends) {
                if (send.sourceKind == PreparedSendSourceKind::bus &&
                    send.sourceIndex == busIndex) {
                    send.sourceTapLatency =
                        send.tapPoint == routing::SendTapPoint::preFaderPrePan
                            ? bus.preFaderTapLatency
                            : bus.postFaderTapLatency;
                    routeLatency(send.destinationBusIndex,
                                 send.sourceTapLatency);
                }
            }
        }
        plan.masterInputLatency =
            masterHasInput ? masterInput : PreparedLatencyRange{};
        if (!addLatency(plan.masterInputLatency, plan.masterInserts.latency,
                        plan.masterOutputLatency)) {
            return {nullptr, "Prepared master latency overflow"};
        }

        std::array<AudibilityTrackInput, maximumPreparedTracks>
            audibilityTracks{};
        std::array<AudibilityBusInput, maximumPreparedBuses> audibilityBuses{};
        std::array<AudibilitySendInput, maximumPreparedSends> audibilitySends{};
        for (std::size_t index = 0; index < plan.tracks.size(); ++index) {
            audibilityTracks[index] = {
                plan.tracks[index].destinationBusIndex,
                plan.tracks[index].source.mix.solo};
        }
        for (std::size_t index = 0; index < plan.buses.size(); ++index) {
            audibilityBuses[index] = {plan.buses[index].destinationBusIndex,
                                      plan.buses[index].mix.solo};
        }
        for (std::size_t index = 0; index < plan.sends.size(); ++index) {
            audibilitySends[index] = {
                plan.sends[index].sourceKind == PreparedSendSourceKind::track
                    ? AudibilitySendSourceKind::track
                    : AudibilitySendSourceKind::bus,
                plan.sends[index].sourceIndex,
                plan.sends[index].destinationBusIndex,
                plan.sends[index].tapPoint ==
                    routing::SendTapPoint::postFaderPostPan};
        }
        plan.audibility = resolveAudibility(
            {audibilityTracks.data(), plan.tracks.size()},
            {audibilityBuses.data(), plan.buses.size()},
            {audibilitySends.data(), plan.sends.size()});

        if (preparedBytes > memoryBudgetBytes) {
            return {nullptr, "Processing buffers exceed the memory budget"};
        }
        plan.runtimeMemoryBytes = preparedBytes;
        ProcessingPlanRuntime runtime{
            plan.buses.size(), plan.sends.size(), nodeCount, blockCapacity};
        runtime.processors.reserve(pendingProcessors.size());
        for (auto& pending : pendingProcessors) {
            runtime.processors.push_back(
                {std::move(pending.instance),
                 BypassDelayLine{pending.latency, pending.channelCount},
                 pending.bypassed});
        }
        for (std::size_t index = 0; index < plan.sends.size(); ++index) {
            runtime.sendMix[index].reset(plan.sends[index].mix);
        }
        return {std::make_unique<PreparedProcessingBundle>(
                    std::move(plan), std::move(runtime)),
                {}};
    } catch (const std::bad_alloc&) {
        return {nullptr, "Not enough memory to prepare processing plan"};
    } catch (...) {
        return {nullptr, "Unexpected error while preparing processing plan"};
    }
}

ProcessingPlanPreparationResult prepareProcessingPlan(
    const ProcessingPlanSpecification& specification,
    std::span<const PreparedTrackView> sources,
    std::size_t blockCapacity,
    std::size_t memoryBudgetBytes,
    const processors::IAudioProcessorFactory* processorFactory) noexcept {
    return prepareProcessingPlanImpl(specification, sources, {}, blockCapacity,
                                     memoryBudgetBytes, processorFactory);
}

ProcessingPlanPreparationResult prepareProcessingPlanFromSources(
    const ProcessingPlanSpecification& specification,
    std::span<const PreparedSourceView> sources,
    std::size_t blockCapacity,
    std::size_t memoryBudgetBytes,
    const processors::IAudioProcessorFactory* processorFactory) noexcept {
    return prepareProcessingPlanImpl(specification, {}, sources, blockCapacity,
                                     memoryBudgetBytes, processorFactory);
}

} // namespace vitadaw::audio
