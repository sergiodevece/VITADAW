#pragma once

#include "vitadaw/timeline/Time.h"
#include "vitadaw/transport/PlaybackState.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <optional>
#include <limits>

namespace vitadaw::audio {

// The only advancing clock in the RT render path. Tracks derive their source
// positions from position(); they never own or advance independent cursors.
class RealtimeProjectClock {
public:
    void prepare(timeline::ProjectFrameCount duration) noexcept {
        duration_ = duration;
        navigationLimit_ = duration;
        playbackLimit_ = duration;
        loop_.reset();
        runUntilStop_ = false;
        stopAndRewind();
    }

    struct LoopBounds { double start{}, end{}; };
    void setPlaybackPolicy(std::optional<LoopBounds> loop,
                           bool runUntilStop) noexcept {
        loop_ = loop;
        runUntilStop_ = runUntilStop;
        playbackLimit_ = navigationLimit_;
        if (loop_) {
            playbackLimit_.value = std::max(
                playbackLimit_.value,
                static_cast<std::int64_t>(std::ceil(loop_->end)));
        }
    }

    // A documentary locator may legitimately extend beyond audible content even
    // while looping is disabled. Preparation publishes that bound quiescently;
    // it is navigation metadata, not an alternative playback clock.
    void setNavigationLimit(timeline::ProjectFrameCount limit) noexcept {
        navigationLimit_.value = std::max(duration_.value, limit.value);
        playbackLimit_ = navigationLimit_;
        if (loop_) {
            playbackLimit_.value = std::max(
                playbackLimit_.value,
                static_cast<std::int64_t>(std::ceil(loop_->end)));
        }
    }

    [[nodiscard]] bool play() noexcept {
        if (duration_.value <= 0 && !loop_ && !runUntilStop_) {
            return false;
        }
        if (loop_ && position_.value >= loop_->end) {
            position_ = {loop_->start};
            compensation_ = 0.0;
        } else if (!loop_ && duration_.value > 0 &&
                   position_.value >= static_cast<double>(duration_.value)) {
            position_ = {0.0};
            compensation_ = 0.0;
        }
        playback_ = transport::PlaybackState::playing;
        return true;
    }

    void pause() noexcept {
        if (playback_ == transport::PlaybackState::playing)
            playback_ = transport::PlaybackState::paused;
    }

    void stop() noexcept {
        if (playback_ == transport::PlaybackState::stopped) {
            position_ = {0.0};
            compensation_ = 0.0;
        }
        playback_ = transport::PlaybackState::stopped;
    }

    [[nodiscard]] bool seek(timeline::ProjectFramePosition target) noexcept {
        if (target.value < 0 || target.value > playbackLimit_.value || isPlaying()) return false;
        position_ = {static_cast<double>(target.value)};
        compensation_ = 0.0;
        wrapped_ = false;
        return true;
    }

    void stopAndRewind() noexcept {
        playback_ = transport::PlaybackState::stopped;
        position_ = {0.0};
        compensation_ = 0.0;
    }

    void advance(timeline::ProjectFrameDuration frames) noexcept {
        if (!isPlaying()) {
            return;
        }

        const auto compensatedIncrement = frames.value - compensation_;
        const auto next = position_.value + compensatedIncrement;
        compensation_ = (next - position_.value) - compensatedIncrement;
        position_.value = next;

        if (loop_) {
            if (position_.value >= loop_->end) {
                const auto length = loop_->end - loop_->start;
                position_.value = loop_->start +
                    std::fmod(position_.value - loop_->start, length);
                if (position_.value < loop_->start) position_.value += length;
                wrapped_ = true;
            }
            return;
        }
        if (runUntilStop_) return;
        const auto end = static_cast<double>(duration_.value);
        constexpr auto tolerance = 1.0e-7;
        if (position_.value >= end - tolerance) {
            position_.value = end;
            compensation_ = 0.0;
            playback_ = transport::PlaybackState::stopped;
        }
    }

    [[nodiscard]] bool isPlaying() const noexcept {
        return playback_ == transport::PlaybackState::playing;
    }
    [[nodiscard]] transport::PlaybackState playback() const noexcept { return playback_; }
    [[nodiscard]] timeline::PreciseProjectFramePosition position() const noexcept {
        return position_;
    }
    [[nodiscard]] timeline::ProjectFramePosition publicPosition() const noexcept {
        const auto rounded = static_cast<std::int64_t>(std::llround(position_.value));
        return {std::max(rounded, std::int64_t{0})};
    }
    [[nodiscard]] timeline::ProjectFrameCount duration() const noexcept {
        return duration_;
    }
    [[nodiscard]] timeline::ProjectFrameCount playbackLimit() const noexcept {
        return playbackLimit_;
    }
    [[nodiscard]] std::optional<LoopBounds> loop() const noexcept { return loop_; }
    [[nodiscard]] bool isRunUntilStop() const noexcept { return runUntilStop_; }
    [[nodiscard]] std::size_t continuousFramesAvailable(
        std::size_t requested, timeline::ProjectFrameDuration increment) const noexcept {
        if (!loop_ || !isPlaying() || increment.value <= 0.0) return requested;
        const auto remaining = loop_->end - position_.value;
        if (remaining <= 0.0) return 1;
        const auto quotient = std::nextafter(
            remaining / increment.value,
            -std::numeric_limits<double>::infinity());
        const auto frames = static_cast<std::size_t>(std::ceil(quotient));
        return std::max<std::size_t>(1, std::min(requested, frames));
    }
    [[nodiscard]] bool consumeWrapped() noexcept {
        const auto result = wrapped_;
        wrapped_ = false;
        return result;
    }
    struct Checkpoint {
        timeline::PreciseProjectFramePosition position;
        transport::PlaybackState playback{transport::PlaybackState::stopped};
    };
    [[nodiscard]] Checkpoint checkpoint() const noexcept {
        return {position_, playback_};
    }
    void restoreQuiescentCheckpoint(Checkpoint value) noexcept {
        position_.value = std::max(0.0, value.position.value);
        compensation_ = 0.0;
        playback_ = value.playback;
        wrapped_ = false;
    }

private:
    timeline::PreciseProjectFramePosition position_;
    timeline::ProjectFrameCount duration_;
    timeline::ProjectFrameCount navigationLimit_;
    timeline::ProjectFrameCount playbackLimit_;
    std::optional<LoopBounds> loop_;
    bool runUntilStop_{};
    bool wrapped_{};
    double compensation_{};
    transport::PlaybackState playback_{transport::PlaybackState::stopped};
};

} // namespace vitadaw::audio
