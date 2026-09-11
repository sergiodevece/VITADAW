#pragma once

#include "vitadaw/tracks/AudioTrack.h"

#include <compare>
#include <cstdint>
#include <string>
#include <vector>

namespace vitadaw::routing {

struct BusId {
    std::uint64_t value{};

    [[nodiscard]] constexpr bool isValid() const noexcept { return value != 0; }
    auto operator<=>(const BusId&) const = default;
};

enum class DestinationKind : std::uint8_t { master, bus };

struct TrackOutputDestination {
    DestinationKind kind{DestinationKind::master};
    BusId bus;

    [[nodiscard]] static constexpr TrackOutputDestination master() noexcept {
        return {};
    }
    [[nodiscard]] static constexpr TrackOutputDestination toBus(BusId id) noexcept {
        return {DestinationKind::bus, id};
    }
    [[nodiscard]] constexpr bool isValid() const noexcept {
        return kind == DestinationKind::master
                   ? !bus.isValid()
                   : kind == DestinationKind::bus && bus.isValid();
    }
    bool operator==(const TrackOutputDestination&) const = default;
};

struct AudioBus {
    BusId id;
    std::string name;
    mixer::BusMixState mix;
};

struct TrackRoute {
    tracks::TrackId track;
    TrackOutputDestination destination;
};

class RoutingState {
public:
    [[nodiscard]] const std::vector<AudioBus>& buses() const noexcept;
    [[nodiscard]] const std::vector<TrackRoute>& trackRoutes() const noexcept;
    [[nodiscard]] const TrackRoute* findTrackRoute(tracks::TrackId track) const noexcept;
    [[nodiscard]] bool containsBus(BusId bus) const noexcept;
    [[nodiscard]] const AudioBus* findBus(BusId bus) const noexcept;

    void addTrack(tracks::TrackId track);
    [[nodiscard]] BusId addBus(std::string name);
    [[nodiscard]] bool setTrackDestination(
        tracks::TrackId track, TrackOutputDestination destination) noexcept;
    [[nodiscard]] bool setBusMix(BusId bus,
                                 mixer::BusMixState state) noexcept;

private:
    std::vector<AudioBus> buses_;
    std::vector<TrackRoute> trackRoutes_;
    BusId nextBusId_{1};
};

} // namespace vitadaw::routing
