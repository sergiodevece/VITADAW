#pragma once

#include <cmath>

namespace vitadaw::audio {

inline constexpr double inputMonitoringSmoothingSeconds = 0.005;

// Monitoring is deliberately independent from project/master gain state.
// Validation and dB-to-linear conversion happen on the control side; the
// callback only consumes the prepared linear representation.
struct MonitorGainDb {
    float value{-12.0F};

    [[nodiscard]] bool isValid() const noexcept {
        return std::isfinite(value) && value >= -100.0F && value <= 0.0F;
    }

    bool operator==(const MonitorGainDb&) const = default;
};

[[nodiscard]] inline float prepareMonitorGain(MonitorGainDb gain) noexcept {
    return gain.value <= -100.0F ? 0.0F :
        std::pow(10.0F, gain.value / 20.0F);
}

} // namespace vitadaw::audio
