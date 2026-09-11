#pragma once

#include "vitadaw/timeline/Time.h"

#include <algorithm>
#include <cmath>

namespace vitadaw::audio {

// The only advancing clock in the RT render path. Tracks derive their source
// positions from position(); they never own or advance independent cursors.
class RealtimeProjectClock {
public:
    void prepare(timeline::ProjectFrameCount duration) noexcept {
        duration_ = duration;
        stopAndRewind();
    }

    [[nodiscard]] bool play() noexcept {
        if (duration_.value <= 0) {
            return false;
        }
        if (position_.value >= static_cast<double>(duration_.value)) {
            position_ = {0.0};
            compensation_ = 0.0;
        }
        playing_ = true;
        return true;
    }

    void stopAndRewind() noexcept {
        playing_ = false;
        position_ = {0.0};
        compensation_ = 0.0;
    }

    void advance(timeline::ProjectFrameDuration frames) noexcept {
        if (!playing_) {
            return;
        }

        const auto compensatedIncrement = frames.value - compensation_;
        const auto next = position_.value + compensatedIncrement;
        compensation_ = (next - position_.value) - compensatedIncrement;
        position_.value = next;

        const auto end = static_cast<double>(duration_.value);
        constexpr auto tolerance = 1.0e-7;
        if (position_.value >= end - tolerance) {
            position_.value = end;
            compensation_ = 0.0;
            playing_ = false;
        }
    }

    [[nodiscard]] bool isPlaying() const noexcept { return playing_; }
    [[nodiscard]] timeline::PreciseProjectFramePosition position() const noexcept {
        return position_;
    }
    [[nodiscard]] timeline::ProjectFramePosition publicPosition() const noexcept {
        const auto rounded = static_cast<std::int64_t>(std::llround(position_.value));
        return {std::clamp(rounded, std::int64_t{0}, duration_.value)};
    }
    [[nodiscard]] timeline::ProjectFrameCount duration() const noexcept {
        return duration_;
    }

private:
    timeline::PreciseProjectFramePosition position_;
    timeline::ProjectFrameCount duration_;
    double compensation_{};
    bool playing_{};
};

} // namespace vitadaw::audio
