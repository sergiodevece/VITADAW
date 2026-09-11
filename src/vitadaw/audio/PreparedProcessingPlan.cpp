#include "vitadaw/audio/PreparedProcessingPlan.h"

#include <algorithm>
#include <array>
#include <limits>
#include <memory>
#include <new>
#include <sstream>
#include <unordered_set>
#include <utility>

namespace vitadaw::audio {

StereoWorkBuffer::StereoWorkBuffer(std::size_t capacity)
    : left(capacity), right(capacity) {}

ProcessingPlanRuntime::ProcessingPlanRuntime(std::size_t busCount,
                                             std::size_t capacity)
    : master(capacity), projectPositions(capacity) {
    buses.reserve(busCount);
    for (std::size_t index = 0; index < busCount; ++index) {
        buses.emplace_back(capacity);
    }
}

PreparedProcessingBundle::PreparedProcessingBundle(
    PreparedProcessingPlan preparedPlan, ProcessingPlanRuntime preparedRuntime)
    : plan(std::move(preparedPlan)), runtime(std::move(preparedRuntime)) {}

ProcessingPlanPreparationResult prepareProcessingPlan(
    const ProcessingPlanSpecification& specification,
    std::span<const PreparedTrackView> sources,
    std::size_t blockCapacity,
    std::size_t memoryBudgetBytes) noexcept {
    try {
        if (!specification.projectSampleRate.isValid()) {
            return {nullptr, "Project sample rate must be finite and positive"};
        }
        if (specification.tracks.size() > maximumPreparedTracks) {
            return {nullptr, "Prepared track capacity exceeded"};
        }
        if (specification.buses.size() > maximumPreparedBuses) {
            return {nullptr, "Prepared bus capacity exceeded"};
        }
        if (blockCapacity == 0 || !specification.masterMix.isValid()) {
            return {nullptr, "Invalid processing format or master state"};
        }

        const auto bufferCount = specification.buses.size() + 1;
        constexpr auto channelCount = std::size_t{2};
        if (bufferCount > std::numeric_limits<std::size_t>::max() /
                              (channelCount * sizeof(float))) {
            return {nullptr, "Processing buffer size overflow"};
        }
        const auto bytesPerFrame =
            bufferCount * channelCount * sizeof(float) + sizeof(double);
        if (blockCapacity >
            std::numeric_limits<std::size_t>::max() / bytesPerFrame) {
            return {nullptr, "Processing buffer size overflow"};
        }
        const auto runtimeBytes = blockCapacity * bytesPerFrame;
        if (runtimeBytes > memoryBudgetBytes) {
            return {nullptr, "Processing buffers exceed the memory budget"};
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
        std::unordered_set<std::uint64_t> sourceIds;
        sourceIds.reserve(sources.size());
        for (const auto& source : sources) {
            const auto known = std::any_of(
                specification.tracks.begin(), specification.tracks.end(),
                [id = source.id](const auto& track) { return track.id == id; });
            if (!known || !source.isAvailable() ||
                !sourceIds.insert(source.id.value).second) {
                return {nullptr, "Invalid, duplicate, or unknown audio source"};
            }
        }
        PreparedProcessingPlan plan;
        plan.projectSampleRate = specification.projectSampleRate;
        plan.blockCapacity = blockCapacity;
        plan.runtimeMemoryBytes = runtimeBytes;
        plan.masterMix = specification.masterMix;
        plan.tracks.reserve(specification.tracks.size());
        plan.buses.reserve(specification.buses.size());
        plan.order.reserve(specification.tracks.size() +
                           specification.buses.size() + 1);

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
            plan.buses.push_back({bus.id, plan.buses.size(), bus.mix,
                                  masterDestinationIndex});
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
        }

        // Validate every bus component, including disconnected and empty buses.
        std::vector<std::uint8_t> colours(plan.buses.size());
        std::vector<std::size_t> path;
        path.reserve(plan.buses.size());
        std::string cycleError;
        const auto visit = [&](auto&& self, std::size_t index) -> bool {
            colours[index] = 1;
            path.push_back(index);
            const auto destination = plan.buses[index].destinationBusIndex;
            if (destination != masterDestinationIndex) {
                if (colours[destination] == 1) {
                    const auto beginning = std::find(path.begin(), path.end(), destination);
                    std::ostringstream message;
                    message << "Routing rejected: ";
                    for (auto cursor = beginning; cursor != path.end(); ++cursor) {
                        message << "Bus " << plan.buses[*cursor].id.value << " -> ";
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
        for (const auto& bus : plan.buses) {
            if (bus.destinationBusIndex != masterDestinationIndex) {
                ++busIndegree[bus.destinationBusIndex];
            }
        }
        std::vector<bool> busEmitted(plan.buses.size());
        std::vector<std::size_t> topologicalBuses;
        topologicalBuses.reserve(plan.buses.size());
        for (std::size_t emitted = 0; emitted < plan.buses.size(); ++emitted) {
            auto selected = masterDestinationIndex;
            for (std::size_t index = 0; index < plan.buses.size(); ++index) {
                if (!busEmitted[index] && busIndegree[index] == 0) {
                    selected = index; // dense order is stable BusId order
                    break;
                }
            }
            if (selected == masterDestinationIndex) {
                return {nullptr, "Routing rejected: cyclic bus graph"};
            }
            busEmitted[selected] = true;
            topologicalBuses.push_back(selected);
            const auto destination = plan.buses[selected].destinationBusIndex;
            if (destination != masterDestinationIndex) {
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
        for (const auto specificationIndex : trackSpecificationOrder) {
            const auto& track = specification.tracks[specificationIndex];
            if (!track.id.isValid() || !track.mix.isValid() ||
                !track.destination.isValid() ||
                !trackIds.insert(track.id.value).second) {
                return {nullptr, "Invalid or duplicate track routing state"};
            }
            std::size_t destination = masterDestinationIndex;
            if (track.destination.kind == routing::DestinationKind::bus) {
                destination = denseBusIndex(track.destination.bus);
                if (destination == masterDestinationIndex) {
                    return {nullptr, "Track output destination does not exist"};
                }
            }

            PreparedTrackView source;
            source.id = track.id;
            source.mix = track.mix;
            const auto preparedSource = std::find_if(
                sources.begin(), sources.end(),
                [id = track.id](const auto& candidate) {
                    return candidate.id == id;
                });
            if (preparedSource != sources.end()) {
                source = *preparedSource;
                source.mix = track.mix;
                if (source.isAvailable()) {
                    if (source.clipDuration.value >
                        std::numeric_limits<std::int64_t>::max() -
                            source.clipStart.value) {
                        return {nullptr, "Prepared clip end overflows project time"};
                    }
                    plan.duration.value = std::max(
                        plan.duration.value,
                        source.clipStart.value + source.clipDuration.value);
                }
            }
            plan.tracks.push_back({source, destination});
            plan.order.push_back(
                {ProcessingStepKind::track, plan.tracks.size() - 1});
        }
        for (const auto index : topologicalBuses) {
            plan.order.push_back({ProcessingStepKind::bus, index});
        }
        plan.order.push_back({ProcessingStepKind::master, 0});

        std::array<AudibilityTrackInput, maximumPreparedTracks>
            audibilityTracks{};
        std::array<AudibilityBusInput, maximumPreparedBuses> audibilityBuses{};
        for (std::size_t index = 0; index < plan.tracks.size(); ++index) {
            audibilityTracks[index] = {
                plan.tracks[index].destinationBusIndex,
                plan.tracks[index].source.mix.solo};
        }
        for (std::size_t index = 0; index < plan.buses.size(); ++index) {
            audibilityBuses[index] = {plan.buses[index].destinationBusIndex,
                                      plan.buses[index].mix.solo};
        }
        plan.audibility = resolveAudibility(
            {audibilityTracks.data(), plan.tracks.size()},
            {audibilityBuses.data(), plan.buses.size()});

        ProcessingPlanRuntime runtime{plan.buses.size(), blockCapacity};
        return {std::make_unique<PreparedProcessingBundle>(
                    std::move(plan), std::move(runtime)),
                {}};
    } catch (const std::bad_alloc&) {
        return {nullptr, "Not enough memory to prepare processing plan"};
    } catch (...) {
        return {nullptr, "Unexpected error while preparing processing plan"};
    }
}

} // namespace vitadaw::audio
