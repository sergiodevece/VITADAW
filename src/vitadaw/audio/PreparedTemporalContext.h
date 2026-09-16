#pragma once

#include "vitadaw/musical/MusicalTime.h"
#include "vitadaw/audio/DspFramePosition.h"
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

struct TemporalContextPreparationResult;

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

// Read-only value derived from one certified temporal-context revision. Exact
// helpers are pure and bounded; the common denominator remains an implementation
// detail and is never a second mutable clock authority.
struct ExactProjectFrameDuration {
    DspFramePosition value;
    [[nodiscard]] double approximate() const noexcept { return value.approximate(); }
};

class PreparedLoopView {
public:
    [[nodiscard]] const musical::MusicalLoopRange& musical() const noexcept {
        return musical_;
    }
    [[nodiscard]] const DspFramePosition& exactStart() const noexcept {
        return exactStart_;
    }
    [[nodiscard]] const DspFramePosition& exactEnd() const noexcept {
        return exactEnd_;
    }
    [[nodiscard]] timeline::PreciseProjectFramePosition
        presentationStart() const noexcept { return presentationStart_; }
    [[nodiscard]] timeline::PreciseProjectFramePosition
        presentationEnd() const noexcept { return presentationEnd_; }
    [[nodiscard]] std::uint64_t temporalRevision() const noexcept {
        return temporalRevision_;
    }

    [[nodiscard]] bool contains(DspFramePosition) const noexcept;
    [[nodiscard]] std::optional<ExactProjectFrameDuration>
        distanceToEnd(DspFramePosition) const noexcept;
    [[nodiscard]] std::optional<DspFramePosition>
        positionAfterWrap(DspFramePosition) const noexcept;
    bool operator==(const PreparedLoopView&) const noexcept;

private:
    friend struct PreparedTemporalContext;
    friend TemporalContextPreparationResult prepareTemporalContext(
        const musical::MusicalTimeMap&, std::optional<musical::MusicalLoopRange>,
        timeline::SampleRate, timeline::SampleRate, std::uint64_t);
    musical::MusicalLoopRange musical_;
    DspFramePosition exactStart_;
    DspFramePosition exactEnd_;
    timeline::PreciseProjectFramePosition presentationStart_;
    timeline::PreciseProjectFramePosition presentationEnd_;
    std::uint64_t temporalRevision_{};
    exact::UInt128 commonDenominator_{1, 0};
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
    std::optional<PreparedLoopView> loopView;
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
