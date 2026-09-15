#pragma once

#include "vitadaw/musical/MusicalTime.h"
#include "vitadaw/audio/ExactTemporal.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cmath>
#include <memory>
#include <optional>
#include <string>
#include <vector>

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
    exact::LoopBounds clockBounds;
    [[nodiscard]] bool isValid() const noexcept;
};

struct PreparedClickTables {
    std::array<float, maximumClickTableFrames> normal{};
    std::array<float, maximumClickTableFrames> accent{};
    std::size_t frameCount{};
    timeline::SampleRate deviceSampleRate;
};

// A bounded analytical beat grid. Anchors are the document's prepared project
// frame values; advancement and placement from those anchors use exact ratios.
struct PreparedBeatSegment {
    std::int64_t tempoTick{}, firstTick{}, endTick{}, signatureTick{}, ticksPerBeat{}, ticksPerBar{};
    std::int64_t firstFrame{}, lastFrame{};
    exact::LinearMapping framesFromTempo;
    [[nodiscard]] exact::Position positionAt(std::int64_t tick) const noexcept {
        return framesFromTempo.at(
            static_cast<std::uint64_t>(tick - tempoTick));
    }
};

struct PreparedTemporalContext {
    musical::MusicalTimeMap documentMap;
    std::unique_ptr<const musical::PreparedMusicalTimeMap> musicalTime;
    std::optional<PreparedLoopRange> loop;
    PreparedClickTables clicks;
    std::vector<PreparedBeatSegment> beats;
    exact::ClockFormat exactClock;
    timeline::SampleRate projectSampleRate;
    timeline::SampleRate deviceSampleRate;
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
