#pragma once

#include <array>
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
    std::span<const bool> busSolos) noexcept {
    PreparedAudibilityState result;
    bool hasSolo{};
    for (const auto& track : tracks) {
        hasSolo = hasSolo || track.solo;
    }
    for (const auto solo : busSolos) {
        hasSolo = hasSolo || solo;
    }
    if (!hasSolo) {
        for (std::size_t index = 0; index < tracks.size(); ++index) {
            result.setTrack(index);
        }
        for (std::size_t index = 0; index < busSolos.size(); ++index) {
            result.setBus(index);
        }
        return result;
    }
    for (std::size_t trackIndex = 0; trackIndex < tracks.size(); ++trackIndex) {
        if (!tracks[trackIndex].solo) {
            continue;
        }
        result.setTrack(trackIndex);
        if (tracks[trackIndex].destinationBusIndex !=
            audibilityMasterDestination) {
            result.setBus(tracks[trackIndex].destinationBusIndex);
        }
    }
    for (std::size_t busIndex = 0; busIndex < busSolos.size(); ++busIndex) {
        if (!busSolos[busIndex]) {
            continue;
        }
        result.setBus(busIndex);
        for (std::size_t trackIndex = 0; trackIndex < tracks.size(); ++trackIndex) {
            if (tracks[trackIndex].destinationBusIndex == busIndex) {
                result.setTrack(trackIndex);
            }
        }
    }
    return result;
}

} // namespace vitadaw::audio
