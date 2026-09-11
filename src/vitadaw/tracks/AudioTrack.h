#pragma once

#include "vitadaw/clips/AudioClip.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

namespace vitadaw::tracks {

using TrackId = std::uint64_t;

enum class AudioTrackSlot : std::uint8_t {
    first = 0,
    second = 1,
};

inline constexpr std::size_t audioTrackCount = 2;

[[nodiscard]] constexpr std::size_t toIndex(AudioTrackSlot slot) noexcept {
    return static_cast<std::size_t>(slot);
}

struct AudioTrack {
    TrackId id{};
    std::string name;
    std::optional<clips::AudioClip> clip;

    [[nodiscard]] bool hasAudio() const noexcept { return clip.has_value(); }
};

} // namespace vitadaw::tracks
