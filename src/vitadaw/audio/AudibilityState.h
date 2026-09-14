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
inline constexpr std::size_t audibilitySendCapacity = 1024;
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

enum class AudibilitySendSourceKind : std::uint8_t { track, bus };

struct AudibilitySendInput {
    AudibilitySendSourceKind sourceKind{AudibilitySendSourceKind::track};
    std::size_t sourceIndex{};
    std::size_t destinationBusIndex{audibilityMasterDestination};
    bool postFader{};
};

// Dense, DSP-ready result of resolving Solo selection against the prepared
// routing graph. Mute remains a local DSP decision at each node.
struct PreparedAudibilityState {
    // Main output permissions are distinct from auxiliary branch permissions.
    std::array<std::uint64_t, audibilityTrackCapacity / 64> tracks{};
    std::array<std::uint64_t, audibilityBusCapacity / 64> buses{};
    std::array<std::uint64_t, audibilitySendCapacity / 64> sends{};
    std::array<std::uint64_t, audibilityTrackCapacity / 64> trackMeters{};
    std::array<std::uint64_t, audibilityBusCapacity / 64> busMeters{};

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
    void setSend(std::size_t index) noexcept {
        if (index < audibilitySendCapacity) {
            sends[index / 64] |= std::uint64_t{1} << (index % 64);
        }
    }
    void setTrackMeter(std::size_t index) noexcept {
        if (index < audibilityTrackCapacity) {
            trackMeters[index / 64] |= std::uint64_t{1} << (index % 64);
        }
    }
    void setBusMeter(std::size_t index) noexcept {
        if (index < audibilityBusCapacity) {
            busMeters[index / 64] |= std::uint64_t{1} << (index % 64);
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
    [[nodiscard]] bool sendIsAudible(std::size_t index) const noexcept {
        return index < audibilitySendCapacity &&
               (sends[index / 64] &
                (std::uint64_t{1} << (index % 64))) != 0;
    }
    [[nodiscard]] bool trackMeterIsAudible(std::size_t index) const noexcept {
        return index < audibilityTrackCapacity &&
               (trackMeters[index / 64] &
                (std::uint64_t{1} << (index % 64))) != 0;
    }
    [[nodiscard]] bool busMeterIsAudible(std::size_t index) const noexcept {
        return index < audibilityBusCapacity &&
               (busMeters[index / 64] &
                (std::uint64_t{1} << (index % 64))) != 0;
    }
    bool operator==(const PreparedAudibilityState&) const = default;
};

[[nodiscard]] inline PreparedAudibilityState fullyAudibleState() noexcept {
    PreparedAudibilityState result;
    result.tracks.fill(~std::uint64_t{});
    result.buses.fill(~std::uint64_t{});
    result.sends.fill(~std::uint64_t{});
    result.trackMeters.fill(~std::uint64_t{});
    result.busMeters.fill(~std::uint64_t{});
    return result;
}

[[nodiscard]] inline PreparedAudibilityState resolveAudibility(
    std::span<const AudibilityTrackInput> tracks,
    std::span<const AudibilityBusInput> buses,
    std::span<const AudibilitySendInput> sends = {}) noexcept {
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
            result.setBusMeter(index);
        }
        for (std::size_t index = 0; index < sends.size(); ++index) {
            result.setSend(index);
        }
        for (std::size_t index = 0; index < tracks.size(); ++index) {
            result.setTrackMeter(index);
        }
        return result;
    }
    // A bus can need its complete input content without its main output being
    // audible: this is the defining case for a wet-only Bus Send path.
    std::array<bool, audibilityBusCapacity> needsFullBusContent{};
    std::array<bool, audibilityTrackCapacity> trackMain{};
    std::array<bool, audibilityBusCapacity> busMain{};
    std::array<bool, audibilitySendCapacity> sendOpen{};
    const auto openDownstreamMain = [&buses, &busMain](
                                        std::size_t first) noexcept {
        auto current = first;
        for (std::size_t hop = 0;
             current < buses.size() && hop < buses.size(); ++hop) {
            busMain[current] = true;
            current = buses[current].destinationBusIndex;
        }
    };

    // Explicit Track Solo selects its own dry and send roots. Downstream buses
    // transport only those selected contributions; their sibling sends stay shut.
    for (std::size_t index = 0; index < tracks.size(); ++index) {
        if (!tracks[index].solo) {
            continue;
        }
        trackMain[index] = true;
        openDownstreamMain(tracks[index].destinationBusIndex);
        for (std::size_t sendIndex = 0; sendIndex < sends.size(); ++sendIndex) {
            const auto& send = sends[sendIndex];
            if (send.sourceKind == AudibilitySendSourceKind::track &&
                send.sourceIndex == index) {
                sendOpen[sendIndex] = true;
                openDownstreamMain(send.destinationBusIndex);
            }
        }
    }

    // Explicit Bus Solo selects the bus's complete content and its own main and
    // send roots. This privilege is never inherited by a transport-only bus.
    for (std::size_t index = 0; index < buses.size(); ++index) {
        if (!buses[index].solo) {
            continue;
        }
        needsFullBusContent[index] = true;
        busMain[index] = true;
        openDownstreamMain(buses[index].destinationBusIndex);
        for (std::size_t sendIndex = 0; sendIndex < sends.size(); ++sendIndex) {
            const auto& send = sends[sendIndex];
            if (send.sourceKind == AudibilitySendSourceKind::bus &&
                send.sourceIndex == index) {
                sendOpen[sendIndex] = true;
                openDownstreamMain(send.destinationBusIndex);
            }
        }
    }

    // Backward closure for every bus whose full content is selected. Each
    // incoming edge is opened, but no unrelated outgoing edge is inferred.
    for (std::size_t pass = 0; pass <= buses.size(); ++pass) {
        bool changed{};
        for (std::size_t index = 0; index < tracks.size(); ++index) {
            const auto destination = tracks[index].destinationBusIndex;
            if (destination < buses.size() &&
                needsFullBusContent[destination]) {
                trackMain[index] = true;
            }
        }
        for (std::size_t index = 0; index < buses.size(); ++index) {
            const auto destination = buses[index].destinationBusIndex;
            if (destination < buses.size() &&
                needsFullBusContent[destination]) {
                busMain[index] = true;
                if (!needsFullBusContent[index]) {
                    needsFullBusContent[index] = true;
                    changed = true;
                }
            }
        }
        for (std::size_t index = 0; index < sends.size(); ++index) {
            const auto& send = sends[index];
            if (send.destinationBusIndex >= buses.size() ||
                !needsFullBusContent[send.destinationBusIndex]) {
                continue;
            }
            sendOpen[index] = true;
            if (send.sourceKind == AudibilitySendSourceKind::bus &&
                send.sourceIndex < buses.size() &&
                !needsFullBusContent[send.sourceIndex]) {
                needsFullBusContent[send.sourceIndex] = true;
                changed = true;
            }
        }
        if (!changed) {
            break;
        }
    }
    for (std::size_t index = 0; index < buses.size(); ++index) {
        if (busMain[index]) {
            result.setBus(index);
            result.setBusMeter(index);
        }
    }
    for (std::size_t index = 0; index < tracks.size(); ++index) {
        if (trackMain[index]) {
            result.setTrack(index);
        }
    }
    for (std::size_t index = 0; index < sends.size(); ++index) {
        if (!sendOpen[index]) {
            continue;
        }
        result.setSend(index);
        if (sends[index].postFader &&
            sends[index].sourceKind == AudibilitySendSourceKind::track &&
            sends[index].sourceIndex < tracks.size()) {
            result.setTrackMeter(sends[index].sourceIndex);
        } else if (sends[index].postFader &&
                   sends[index].sourceKind == AudibilitySendSourceKind::bus &&
                   sends[index].sourceIndex < buses.size()) {
            result.setBusMeter(sends[index].sourceIndex);
        }
    }
    for (std::size_t index = 0; index < tracks.size(); ++index) {
        if (trackMain[index]) {
            result.setTrackMeter(index);
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
    return resolveAudibility(
        tracks, std::span<const AudibilityBusInput>{buses.data(), count}, {});
}

} // namespace vitadaw::audio
