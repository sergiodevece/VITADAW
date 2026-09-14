#include "vitadaw/audio/PreparedProcessingPlan.h"

#include <algorithm>
#include <array>
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

ProcessingPlanRuntime::ProcessingPlanRuntime(std::size_t busCount,
                                             std::size_t sendCount,
                                             std::size_t capacity)
    : sendMix(sendCount), master(capacity), projectPositions(capacity) {
    buses.reserve(busCount);
    for (std::size_t index = 0; index < busCount; ++index) {
        buses.emplace_back(capacity);
    }
}

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

} // namespace

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
        if (specification.sends.size() > maximumPreparedSends) {
            return {nullptr, "Prepared send capacity exceeded"};
        }
        if (blockCapacity == 0 || !specification.masterMix.isValid()) {
            return {nullptr, "Invalid processing format or master state"};
        }

        const auto bufferCount = specification.buses.size() + 1;
        constexpr auto channelCount = std::size_t{2};
        std::size_t bufferSampleBytes{};
        if (!checkedMultiply(bufferCount, channelCount * sizeof(float),
                             bufferSampleBytes)) {
            return {nullptr, "Processing buffer size overflow"};
        }
        std::size_t bytesPerFrame{};
        if (!checkedAdd(bufferSampleBytes, sizeof(double), bytesPerFrame)) {
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
            !addPreparedArray(stepCount, sizeof(ProcessingStep)) ||
            !addPreparedArray(1, sizeof(PreparedAudibilityState))) {
            return {nullptr, "Prepared processing size overflow"};
        }
        if (preparedBytes > memoryBudgetBytes) {
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

        std::unordered_set<std::uint64_t> sendIds;
        sendIds.reserve(specification.sends.size());
        std::unordered_map<std::uint64_t, std::size_t> sendsPerTrack;
        sendsPerTrack.reserve(specification.tracks.size());
        std::unordered_map<std::uint64_t, std::size_t> sendsPerBus;
        sendsPerBus.reserve(specification.buses.size());
        for (const auto& send : specification.sends) {
            if (!send.id.isValid() || !send.destination.isValid() ||
                !busIds.contains(send.destination.value) ||
                !routing::isValid(send.tapPoint) || !send.mix.isValid() ||
                !sendIds.insert(send.id.value).second) {
                return {nullptr, "Invalid or duplicate send routing state"};
            }
            if (const auto* track =
                    std::get_if<tracks::TrackId>(&send.source)) {
                if (!track->isValid() || !trackIds.contains(track->value)) {
                    return {nullptr, "Track send source does not exist"};
                }
                auto& count = sendsPerTrack[track->value];
                ++count;
                if (count > maximumPreparedSendsPerTrack) {
                    return {nullptr, "Prepared sends-per-track capacity exceeded"};
                }
            } else if (const auto* bus =
                           std::get_if<routing::BusId>(&send.source)) {
                if (!bus->isValid() || !busIds.contains(bus->value)) {
                    return {nullptr, "Bus send source does not exist"};
                }
                auto& count = sendsPerBus[bus->value];
                ++count;
                if (count > maximumPreparedSendsPerBus) {
                    return {nullptr, "Prepared sends-per-bus capacity exceeded"};
                }
            } else {
                return {nullptr, "Invalid send source"};
            }
        }

        PreparedProcessingPlan plan;
        plan.projectSampleRate = specification.projectSampleRate;
        plan.blockCapacity = blockCapacity;
        plan.runtimeMemoryBytes = preparedBytes;
        plan.masterMix = specification.masterMix;
        plan.tracks.reserve(specification.tracks.size());
        plan.buses.reserve(specification.buses.size());
        plan.sends.reserve(specification.sends.size());
        plan.sendIndexById.reserve(specification.sends.size());
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
                                  masterDestinationIndex, {}, {}});
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

        // Every bus main output and Bus Send is a structural dependency. Track
        // Sends do not add bus-to-bus dependencies because tracks execute first.
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

        // Validate every bus component, including disconnected and empty buses.
        std::vector<std::uint8_t> colours(plan.buses.size());
        std::vector<std::size_t> path;
        path.reserve(plan.buses.size());
        std::string cycleError;
        const auto visit = [&](auto&& self, std::size_t index) -> bool {
            colours[index] = 1;
            path.push_back(index);
            for (const auto destination : busEdges[index]) {
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
                    selected = index; // dense order is stable BusId order
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
        for (const auto specificationIndex : trackSpecificationOrder) {
            const auto& track = specification.tracks[specificationIndex];
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
            plan.tracks.push_back({source, destination, {}, {}});
            plan.order.push_back(
                {ProcessingStepKind::track, plan.tracks.size() - 1});
        }


        std::vector<std::size_t> sendSpecificationOrder(
            specification.sends.size());
        for (std::size_t index = 0; index < sendSpecificationOrder.size();
             ++index) {
            sendSpecificationOrder[index] = index;
        }
        const auto denseTrackIndex = [&plan](tracks::TrackId id) {
            const auto found = std::lower_bound(
                plan.tracks.begin(), plan.tracks.end(), id,
                [](const auto& track, const auto value) {
                    return track.source.id < value;
                });
            return found != plan.tracks.end() && found->source.id == id
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
                 runtimeIndex, plan.sends.size(), send.mix});
            ++range.count;
        }
        for (std::size_t index = 0; index < plan.sends.size(); ++index) {
            plan.sendIndexById.push_back({plan.sends[index].id, index});
        }
        std::sort(plan.sendIndexById.begin(), plan.sendIndexById.end(),
                  [](const auto& left, const auto& right) {
                      return left.id < right.id;
                  });

        for (const auto index : topologicalBuses) {
            plan.order.push_back({ProcessingStepKind::bus, index});
        }
        plan.order.push_back({ProcessingStepKind::master, 0});

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

        ProcessingPlanRuntime runtime{plan.buses.size(), plan.sends.size(),
                                      blockCapacity};
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

} // namespace vitadaw::audio
