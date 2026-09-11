#pragma once

#include "vitadaw/timeline/Time.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace vitadaw::audio {

class LinearSmoother {
public:
    void reset(float value) noexcept {
        current_ = value;
        target_ = value;
        increment_ = 0.0F;
        remaining_ = 0;
    }

    void setTarget(float target, timeline::SampleRate deviceSampleRate,
                   double durationSeconds) noexcept {
        if (target == target_) {
            return;
        }
        target_ = target;
        if (!deviceSampleRate.isValid() || !std::isfinite(durationSeconds) ||
            durationSeconds <= 0.0 || target == current_) {
            reset(target);
            return;
        }
        const auto sampleCount = std::max<std::uint64_t>(
            1, static_cast<std::uint64_t>(std::llround(
                   durationSeconds * deviceSampleRate.hertz())));
        remaining_ = sampleCount;
        increment_ = (target_ - current_) / static_cast<float>(sampleCount);
    }

    [[nodiscard]] float next() noexcept {
        if (remaining_ > 0) {
            current_ += increment_;
            --remaining_;
            if (remaining_ == 0) {
                current_ = target_;
            }
        }
        return current_;
    }

    [[nodiscard]] float current() const noexcept { return current_; }
    [[nodiscard]] float target() const noexcept { return target_; }
    [[nodiscard]] bool isSmoothing() const noexcept { return remaining_ != 0; }
    [[nodiscard]] std::uint64_t remainingSamples() const noexcept {
        return remaining_;
    }

private:
    float current_{};
    float target_{};
    float increment_{};
    std::uint64_t remaining_{};
};

} // namespace vitadaw::audio
