#pragma once

#include "vitadaw/timeline/Time.h"

namespace vitadaw::timeline {

struct FrameRange {
    ProjectFramePosition start{};
    ProjectFrameCount length{};

    [[nodiscard]] constexpr ProjectFramePosition end() const noexcept {
        return {start.value + length.value};
    }

    [[nodiscard]] constexpr bool isValid() const noexcept {
        return start.value >= 0 && length.value >= 0;
    }
};

} // namespace vitadaw::timeline
