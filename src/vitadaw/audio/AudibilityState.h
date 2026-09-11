#pragma once

#include <array>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>

namespace vitadaw::audio {

inline constexpr std::size_t audibilityTrackCapacity = 256;
inline constexpr std::size_t audibilityBusCapacity = 64;
inline constexpr std::size_t audibilityMasterDestination =
    std::numeric_limits<std::size_t>::max();

struct AudibilityTrackInput {
    std::size_t destinationBusIndex{audibilityMasterDestination};
    bool solo{};
};

struct AudibilityBusInput {
    std::size_t destinationBusIndex{audibilityMasterDestination};
    bool solo{};
};

// Dense, DSP-ready result of resolving Solo selection against the prepared
// routing graph. Mute remains a local DSP decision at each node.
struct PreparedAudibilityState {
    std::array<std::uint64_t, audibilityTrackCapacity / 64> tracks{};
    std::array<std::uint64_t, audibilityBusCapacity / 64> buses{};

    void setTrack(std::size_t index) noexcept {
        if (index < audibilityTrackCapacity) {
            tracks[index / 64] |= std::uint64_t{1} << (index % 64);
        }
    }
    void setBus(std::size_t index) noexcept {
        if (index < audibilityBusCapacity) {
            buses[index / 64] |= std::uint64_t{1} << (index % 64);
        }
    }
    [[nodiscard]] bool trackIsAudible(std::size_t index) const noexcept {
        return index < audibilityTrackCapacity &&
               (tracks[index / 64] &
                (std::uint64_t{1} << (index % 64))) != 0;
    }
    [[nodiscard]] bool busIsAudible(std::size_t index) const noexcept {
        return index < audibilityBusCapacity &&
               (buses[index / 64] &
                (std::uint64_t{1} << (index % 64))) != 0;
    }
    bool operator==(const PreparedAudibilityState&) const = default;
};

[[nodiscard]] inline PreparedAudibilityState fullyAudibleState() noexcept {
    PreparedAudibilityState result;
    result.tracks.fill(~std::uint64_t{});
    result.buses.fill(~std::uint64_t{});
    return result;
}

[[nodiscard]] inline PreparedAudibilityState resolveAudibility(
    std::span<const AudibilityTrackInput> tracks,
    std::span<const AudibilityBusInput> buses) noexcept {
    PreparedAudibilityState result;
    bool hasSolo{};
    for (const auto& track : tracks) {
        hasSolo = hasSolo || track.solo;
    }
    for (const auto& bus : buses) {
        hasSolo = hasSolo || bus.solo;
    }
    if (!hasSolo) {
        for (std::size_t index = 0; index < tracks.size(); ++index) {
            result.setTrack(index);
        }
        for (std::size_t index = 0; index < buses.size(); ++index) {
            result.setBus(index);
        }
        return result;
    }
    // U contains buses whose complete upstream content is selected by Bus Solo.
    std::array<bool, audibilityBusCapacity> upstreamSelected{};
    for (std::size_t index = 0; index < buses.size(); ++index) {
        upstreamSelected[index] = buses[index].solo;
    }
    for (std::size_t pass = 0; pass < buses.size(); ++pass) {
        bool changed{};
        for (std::size_t index = 0; index < buses.size(); ++index) {
            const auto destination = buses[index].destinationBusIndex;
            if (!upstreamSelected[index] &&
                destination != audibilityMasterDestination &&
                destination < buses.size() && upstreamSelected[destination]) {
                upstreamSelected[index] = true;
                changed = true;
            }
        }
        if (!changed) {
            break;
        }
    }

    // A contains selected buses plus downstream nodes needed only as transport.
    std::array<bool, audibilityBusCapacity> transport = upstreamSelected;
    for (std::size_t trackIndex = 0; trackIndex < tracks.size(); ++trackIndex) {
        const auto destination = tracks[trackIndex].destinationBusIndex;
        if (tracks[trackIndex].solo ||
            (destination != audibilityMasterDestination &&
             destination < buses.size() && upstreamSelected[destination])) {
            result.setTrack(trackIndex);
            if (destination != audibilityMasterDestination &&
                destination < buses.size()) {
                transport[destination] = true;
            }
        }
    }
    // Deliberately do not expand upstream again after opening downstream nodes.
    for (std::size_t pass = 0; pass < buses.size(); ++pass) {
        bool changed{};
        for (std::size_t index = 0; index < buses.size(); ++index) {
            if (!transport[index]) {
                continue;
            }
            const auto destination = buses[index].destinationBusIndex;
            if (destination != audibilityMasterDestination &&
                destination < buses.size() && !transport[destination]) {
                transport[destination] = true;
                changed = true;
            }
        }
        if (!changed) {
            break;
        }
    }
    for (std::size_t index = 0; index < buses.size(); ++index) {
        if (transport[index]) {
            result.setBus(index);
        }
    }
    return result;
}

// Compatibility overload for the pre-DAG flat bus model.
[[nodiscard]] inline PreparedAudibilityState resolveAudibility(
    std::span<const AudibilityTrackInput> tracks,
    std::span<const bool> busSolos) noexcept {
    std::array<AudibilityBusInput, audibilityBusCapacity> buses{};
    const auto count = std::min(busSolos.size(), buses.size());
    for (std::size_t index = 0; index < count; ++index) {
        buses[index].solo = busSolos[index];
    }
    return resolveAudibility(tracks, std::span<const AudibilityBusInput>{buses.data(), count});
}

} // namespace vitadaw::audio
