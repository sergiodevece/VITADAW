#pragma once

#include "vitadaw/timeline/Time.h"

namespace vitadaw::audio {

// Allocation-free playback state. It is owned by the realtime thread except
// while the device callback is detached for resource publication.
class RealtimePlaybackCursor {
public:
    void prepare(timeline::SourceFrameCount frameCount) noexcept {
        frameCount_ = frameCount;
        position_ = {0.0};
        roundingCompensation_ = 0.0;
        playing_ = false;
    }

    [[nodiscard]] bool play() noexcept {
        if (frameCount_.value == 0) {
            return false;
        }
        if (position_.value >= static_cast<double>(frameCount_.value)) {
            position_ = {0.0};
            roundingCompensation_ = 0.0;
        }
        playing_ = true;
        return true;
    }

    void stopAndRewind() noexcept {
        playing_ = false;
        position_ = {0.0};
        roundingCompensation_ = 0.0;
    }

    void advance(timeline::SourceFrameDuration sourceFrames) noexcept {
        const auto compensatedAdvance = sourceFrames.value - roundingCompensation_;
        const auto nextPosition = position_.value + compensatedAdvance;
        roundingCompensation_ =
            (nextPosition - position_.value) - compensatedAdvance;
        position_ = {nextPosition};
        const auto end = static_cast<double>(frameCount_.value);
        constexpr auto roundingTolerance = 1.0e-7;
        if (position_.value >= end || end - position_.value <= roundingTolerance) {
            position_ = {end};
            playing_ = false;
        }
    }

    [[nodiscard]] bool isPlaying() const noexcept { return playing_; }
    [[nodiscard]] timeline::SourceFramePosition position() const noexcept {
        return position_;
    }
    [[nodiscard]] timeline::SourceFrameCount frameCount() const noexcept {
        return frameCount_;
    }

private:
    timeline::SourceFrameCount frameCount_;
    timeline::SourceFramePosition position_;
    double roundingCompensation_{};
    bool playing_{};
};

} // namespace vitadaw::audio
