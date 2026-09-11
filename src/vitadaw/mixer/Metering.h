#pragma once

#include "vitadaw/tracks/AudioTrack.h"

#include <array>
#include <cstddef>

namespace vitadaw::mixer {

inline constexpr std::size_t maximumMeteredTracks = 256;

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

struct MeterSnapshot {
    std::array<TrackPeak, maximumMeteredTracks> tracks{};
    std::size_t trackCount{};
    StereoPeak master;
};

} // namespace vitadaw::mixer
