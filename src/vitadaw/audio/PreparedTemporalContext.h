#pragma once

#include "vitadaw/musical/MusicalTime.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cmath>
#include <memory>
#include <optional>
#include <string>

namespace vitadaw::audio {

inline constexpr std::size_t maximumClickTableFrames = 4096;
inline constexpr std::size_t metronomeVoiceCount = 4;
inline constexpr double metronomeSmoothingSeconds = 0.005;

struct MetronomeLevelDb {
    float value{-12.0F};
    [[nodiscard]] bool isValid() const noexcept {
        return std::isfinite(value) && value >= -100.0F && value <= 0.0F;
    }
    bool operator==(const MetronomeLevelDb&) const = default;
};

[[nodiscard]] inline float prepareMetronomeLevel(MetronomeLevelDb level) noexcept {
    return level.value <= -100.0F ? 0.0F :
        std::pow(10.0F, level.value / 20.0F);
}

struct PreparedLoopRange {
    musical::MusicalLoopRange musical;
    timeline::PreciseProjectFramePosition start;
    timeline::PreciseProjectFramePosition end;
    timeline::ProjectFrameDuration duration;
    std::uint64_t musicalMapRevision{};
    [[nodiscard]] bool isValid() const noexcept;
};

struct PreparedClickTables {
    std::array<float, maximumClickTableFrames> normal{};
    std::array<float, maximumClickTableFrames> accent{};
    std::size_t frameCount{};
    timeline::SampleRate deviceSampleRate;
};

struct PreparedTemporalContext {
    musical::MusicalTimeMap documentMap;
    std::unique_ptr<const musical::PreparedMusicalTimeMap> musicalTime;
    std::optional<PreparedLoopRange> loop;
    PreparedClickTables clicks;
    std::uint64_t revision{};
};

struct TemporalContextPreparationResult {
    std::unique_ptr<PreparedTemporalContext> prepared;
    std::string errorMessage;
    [[nodiscard]] bool success() const noexcept { return prepared != nullptr; }
};

[[nodiscard]] TemporalContextPreparationResult prepareTemporalContext(
    const musical::MusicalTimeMap&, std::optional<musical::MusicalLoopRange>,
    timeline::SampleRate projectSampleRate, timeline::SampleRate deviceSampleRate,
    std::uint64_t revision);

} // namespace vitadaw::audio
