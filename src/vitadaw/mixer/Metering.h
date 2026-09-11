#pragma once

#include "vitadaw/tracks/AudioTrack.h"
#include "vitadaw/routing/RoutingState.h"

#include <array>
#include <cstddef>

namespace vitadaw::mixer {

inline constexpr std::size_t maximumMeteredTracks = 256;
inline constexpr std::size_t maximumMeteredBuses = 64;

struct StereoPeak {
    float left{};
    float right{};

    bool operator==(const StereoPeak&) const = default;
};

struct TrackPeak {
    tracks::TrackId track;
    StereoPeak peak;

    bool operator==(const TrackPeak&) const = default;
};

struct BusPeak {
    routing::BusId bus;
    StereoPeak peak;

    bool operator==(const BusPeak&) const = default;
};

struct MeterSnapshot {
    std::array<TrackPeak, maximumMeteredTracks> tracks{};
    std::size_t trackCount{};
    std::array<BusPeak, maximumMeteredBuses> buses{};
    std::size_t busCount{};
    StereoPeak master;
};

} // namespace vitadaw::mixer
