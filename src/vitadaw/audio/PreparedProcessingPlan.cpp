#include "vitadaw/audio/PreparedProcessingPlan.h"

#include <algorithm>
#include <array>
#include <limits>
#include <memory>
#include <new>
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
            if (!bus.id.isValid() || !busIds.insert(bus.id.value).second) {
                return {nullptr, "Invalid or duplicate BusId"};
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

        for (std::size_t index = 0; index < specification.buses.size(); ++index) {
            if (!specification.buses[index].mix.isValid()) {
                return {nullptr, "Invalid bus mixer state"};
            }
            plan.buses.push_back({specification.buses[index].id, index,
                                  specification.buses[index].mix});
        }
        for (const auto& track : specification.tracks) {
            if (!track.id.isValid() || !track.mix.isValid() ||
                !track.destination.isValid() ||
                !trackIds.insert(track.id.value).second) {
                return {nullptr, "Invalid or duplicate track routing state"};
            }
            std::size_t destination = masterDestinationIndex;
            if (track.destination.kind == routing::DestinationKind::bus) {
                const auto found = std::find_if(
                    plan.buses.begin(), plan.buses.end(),
                    [id = track.destination.bus](const auto& bus) {
                        return bus.id == id;
                    });
                if (found == plan.buses.end()) {
                    return {nullptr, "Track output destination does not exist"};
                }
                destination = static_cast<std::size_t>(found - plan.buses.begin());
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
        for (std::size_t index = 0; index < plan.buses.size(); ++index) {
            plan.order.push_back({ProcessingStepKind::bus, index});
        }
        plan.order.push_back({ProcessingStepKind::master, 0});

        std::array<AudibilityTrackInput, maximumPreparedTracks>
            audibilityTracks{};
        std::array<bool, maximumPreparedBuses> busSolos{};
        for (std::size_t index = 0; index < plan.tracks.size(); ++index) {
            audibilityTracks[index] = {
                plan.tracks[index].destinationBusIndex,
                specification.tracks[index].mix.solo};
        }
        for (std::size_t index = 0; index < plan.buses.size(); ++index) {
            busSolos[index] = specification.buses[index].mix.solo;
        }
        plan.audibility = resolveAudibility(
            {audibilityTracks.data(), plan.tracks.size()},
            {busSolos.data(), plan.buses.size()});

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
