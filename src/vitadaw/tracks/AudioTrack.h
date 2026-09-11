#pragma once

#include "vitadaw/clips/AudioClip.h"

#include <cstdint>
#include <string>
#include <vector>

namespace vitadaw::tracks {

using TrackId = std::uint64_t;

struct AudioTrack {
    TrackId id{};
    std::string name;
    std::vector<clips::AudioClip> clips;
};

} // namespace vitadaw::tracks

