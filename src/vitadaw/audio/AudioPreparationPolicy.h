#pragma once

#include "vitadaw/timeline/Time.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>

namespace vitadaw::audio {

enum class PreparationValidationError {
    none,
    invalidSampleRate,
    invalidChannelCount,
    invalidLength,
    sizeOverflow,
    memoryBudgetExceeded,
    nonFiniteSample,
};

struct PreparationValidationResult {
    PreparationValidationError error{};
    std::size_t decodedBytes{};
    [[nodiscard]] bool isValid() const noexcept {
        return error == PreparationValidationError::none;
    }
};

inline PreparationValidationResult validatePreparationShape(
    timeline::SampleRate sampleRate, std::uint64_t channelCount,
    std::uint64_t frameCount, std::size_t alreadyPreparedBytes,
    std::size_t memoryBudgetBytes) noexcept {
    if (!sampleRate.isValid()) {
        return {PreparationValidationError::invalidSampleRate, 0};
    }
    if (channelCount == 0 || channelCount > 2) {
        return {PreparationValidationError::invalidChannelCount, 0};
    }
    if (frameCount == 0) {
        return {PreparationValidationError::invalidLength, 0};
    }
    if (frameCount > std::numeric_limits<std::size_t>::max() / channelCount /
                         sizeof(float)) {
        return {PreparationValidationError::sizeOverflow, 0};
    }
    const auto bytes = static_cast<std::size_t>(frameCount * channelCount * sizeof(float));
    if (alreadyPreparedBytes > memoryBudgetBytes ||
        bytes > memoryBudgetBytes - alreadyPreparedBytes) {
        return {PreparationValidationError::memoryBudgetExceeded, bytes};
    }
    return {PreparationValidationError::none, bytes};
}

inline bool containsOnlyFiniteSamples(std::span<const float> samples) noexcept {
    for (const auto sample : samples) {
        if (!std::isfinite(sample)) {
            return false;
        }
    }
    return true;
}

} // namespace vitadaw::audio
