#pragma once

#include "vitadaw/media/AudioSource.h"
#include "vitadaw/timeline/Time.h"

#include <compare>
#include <cstdint>

namespace vitadaw::clips {

struct ClipId {
    std::uint64_t value{};

    [[nodiscard]] constexpr bool isValid() const noexcept { return value != 0; }
    auto operator<=>(const ClipId&) const = default;
};

// Editable project data. This object must never be read directly by the RT thread.
struct AudioClip {
    ClipId id;
    media::SourceId source;
    timeline::ProjectFramePosition projectStart;
    timeline::ProjectFrameDuration duration;
    timeline::SourceFramePosition sourceOffset;

    bool operator==(const AudioClip&) const = default;
};

} // namespace vitadaw::clips
