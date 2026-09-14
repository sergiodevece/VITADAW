#pragma once

#include "vitadaw/clips/AudioClip.h"
#include "vitadaw/mixer/MixerState.h"
#include "vitadaw/processors/ProcessorState.h"

#include <compare>
#include <cstdint>
#include <string>
#include <vector>

namespace vitadaw::tracks {

struct TrackId {
    std::uint64_t value{};

    [[nodiscard]] constexpr bool isValid() const noexcept { return value != 0; }
    auto operator<=>(const TrackId&) const = default;
};

struct AudioTrack {
    TrackId id{};
    std::string name;
    media::AudioChannelLayout layout{media::AudioChannelLayout::mono};
    std::vector<clips::AudioClip> clips;
    mixer::TrackMixState mix;
    processors::InsertChain inserts;

    [[nodiscard]] bool hasAudio() const noexcept { return !clips.empty(); }
};

} // namespace vitadaw::tracks
