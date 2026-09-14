#pragma once

#include <algorithm>
#include <cmath>
#include <numbers>

namespace vitadaw::mixer {

struct GainDb {
    static constexpr float silence = -100.0F;
    static constexpr float maximum = 12.0F;
    float value{};

    [[nodiscard]] bool isValid() const noexcept {
        return std::isfinite(value) && value >= silence && value <= maximum;
    }
    [[nodiscard]] float linear() const noexcept {
        return value <= silence ? 0.0F : std::pow(10.0F, value / 20.0F);
    }
    bool operator==(const GainDb&) const = default;
};

struct Pan {
    static constexpr float left = -1.0F;
    static constexpr float centre = 0.0F;
    static constexpr float right = 1.0F;
    float value{};

    [[nodiscard]] bool isValid() const noexcept {
        return std::isfinite(value) && value >= left && value <= right;
    }
    bool operator==(const Pan&) const = default;
};

struct TrackMixState {
    GainDb gain;
    Pan pan;
    bool muted{};
    bool solo{};
    [[nodiscard]] bool isValid() const noexcept {
        return gain.isValid() && pan.isValid();
    }
    bool operator==(const TrackMixState&) const = default;
};

struct BusMixState {
    GainDb gain;
    Pan balance;
    bool muted{};
    bool solo{};
    [[nodiscard]] bool isValid() const noexcept {
        return gain.isValid() && balance.isValid();
    }
    bool operator==(const BusMixState&) const = default;
};

struct MasterMixState {
    GainDb gain;
    [[nodiscard]] bool isValid() const noexcept { return gain.isValid(); }
    bool operator==(const MasterMixState&) const = default;
};

struct SendMixState {
    GainDb level;
    bool muted{};
    [[nodiscard]] bool isValid() const noexcept { return level.isValid(); }
    bool operator==(const SendMixState&) const = default;
};

// DSP-ready values. Conversion from dB happens on the application thread.
struct PreparedTrackMixState {
    static constexpr float maximumLinearGain = 3.981072F;
    float linearGain{1.0F};
    float monoLeft{0.70710678F};
    float monoRight{0.70710678F};
    float stereoLeft{1.0F};
    float stereoRight{1.0F};
    bool muted{};
    bool solo{};
    [[nodiscard]] bool isValid() const noexcept {
        return std::isfinite(linearGain) && linearGain >= 0.0F &&
               linearGain <= maximumLinearGain &&
               std::isfinite(monoLeft) && monoLeft >= 0.0F &&
               std::isfinite(monoRight) && monoRight >= 0.0F &&
               std::isfinite(stereoLeft) && stereoLeft >= 0.0F &&
               std::isfinite(stereoRight) && stereoRight >= 0.0F;
    }
};

[[nodiscard]] inline PreparedTrackMixState prepareLinear(
    float linearGain, Pan pan, bool muted = false, bool solo = false) noexcept {
    constexpr auto halfPi = std::numbers::pi_v<float> * 0.5F;
    const auto monoAngle = (pan.value + 1.0F) * (halfPi * 0.5F);
    PreparedTrackMixState result;
    result.linearGain = linearGain;
    result.monoLeft = std::max(0.0F, std::cos(monoAngle));
    result.monoRight = std::max(0.0F, std::sin(monoAngle));
    if (pan.value <= 0.0F) {
        result.stereoLeft = 1.0F;
        result.stereoRight = std::max(
            0.0F, std::sin((pan.value + 1.0F) * halfPi));
    } else {
        result.stereoLeft = std::max(0.0F, std::cos(pan.value * halfPi));
        result.stereoRight = 1.0F;
    }
    result.muted = muted;
    result.solo = solo;
    return result;
}

struct PreparedMasterMixState {
    static constexpr float maximumLinearGain = 3.981072F;
    float linearGain{1.0F};
    [[nodiscard]] bool isValid() const noexcept {
        return std::isfinite(linearGain) && linearGain >= 0.0F &&
               linearGain <= maximumLinearGain;
    }
};

struct PreparedBusMixState {
    static constexpr float maximumLinearGain =
        PreparedTrackMixState::maximumLinearGain;
    float linearGain{1.0F};
    float leftBalance{1.0F};
    float rightBalance{1.0F};
    bool muted{};
    bool solo{};
    [[nodiscard]] bool isValid() const noexcept {
        return std::isfinite(linearGain) && linearGain >= 0.0F &&
               linearGain <= maximumLinearGain &&
               std::isfinite(leftBalance) && leftBalance >= 0.0F &&
               leftBalance <= 1.0F &&
               std::isfinite(rightBalance) && rightBalance >= 0.0F &&
               rightBalance <= 1.0F;
    }
};

struct PreparedSendMixState {
    static constexpr float maximumLinearGain =
        PreparedTrackMixState::maximumLinearGain;
    float linearGain{1.0F};
    bool muted{};
    [[nodiscard]] bool isValid() const noexcept {
        return std::isfinite(linearGain) && linearGain >= 0.0F &&
               linearGain <= maximumLinearGain;
    }
    bool operator==(const PreparedSendMixState&) const = default;
};

[[nodiscard]] inline PreparedTrackMixState prepare(
    const TrackMixState& state) noexcept {
    return prepareLinear(state.gain.linear(), state.pan, state.muted,
                         state.solo);
}

[[nodiscard]] inline PreparedMasterMixState prepare(
    const MasterMixState& state) noexcept {
    return {state.gain.linear()};
}

[[nodiscard]] inline PreparedBusMixState prepare(
    const BusMixState& state) noexcept {
    const auto trackEquivalent = prepareLinear(
        state.gain.linear(), state.balance, state.muted, state.solo);
    return {trackEquivalent.linearGain, trackEquivalent.stereoLeft,
            trackEquivalent.stereoRight, trackEquivalent.muted,
            trackEquivalent.solo};
}

[[nodiscard]] inline PreparedSendMixState prepare(
    const SendMixState& state) noexcept {
    return {state.level.linear(), state.muted};
}

} // namespace vitadaw::mixer
