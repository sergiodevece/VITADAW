#pragma once

#include "vitadaw/timeline/Time.h"
#include "vitadaw/audio/ExactTemporal.h"
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace vitadaw::musical {
inline constexpr std::int64_t ppq = 15360;
inline constexpr std::size_t maximumEvents = 4096;
// Explicit numerical envelope, separate from the audio document's range.
inline constexpr std::int64_t maximumCoordinate = std::int64_t{1} << 40;
struct MusicalTickPosition { std::int64_t value{}; bool operator==(const MusicalTickPosition&) const = default; };
struct MusicalTickDuration { std::int64_t value{}; bool operator==(const MusicalTickDuration&) const = default; };
struct MusicalLoopRange {
    MusicalTickPosition start;
    MusicalTickPosition end;
    bool operator==(const MusicalLoopRange&) const = default;
};
struct QuarterNotePosition { double value{}; };
struct BarIndex { std::int64_t value{}; bool operator==(const BarIndex&) const = default; };
struct BeatIndex { std::int64_t value{}; bool operator==(const BeatIndex&) const = default; };
struct TickWithinBeat { std::int64_t value{}; bool operator==(const TickWithinBeat&) const = default; };
struct MusicalPosition {
    BarIndex bar; BeatIndex beat; TickWithinBeat tick;
    bool operator==(const MusicalPosition&) const = default;
};
struct TempoBpm {
    double value{120.0};
    [[nodiscard]] bool isValid() const noexcept;
    bool operator==(const TempoBpm&) const = default;
};
struct TempoEventId { std::uint64_t value{}; bool operator==(const TempoEventId&) const = default; };
struct TimeSignatureEventId { std::uint64_t value{}; bool operator==(const TimeSignatureEventId&) const = default; };
enum class TempoCurve { step };
struct TempoEvent {
    TempoEventId id; MusicalTickPosition tick; TempoBpm bpm; TempoCurve curveToNext{TempoCurve::step};
    bool operator==(const TempoEvent&) const = default;
};
struct TimeSignature {
    unsigned numerator{4}, denominator{4};
    [[nodiscard]] bool isValid() const noexcept;
    bool operator==(const TimeSignature&) const = default;
};
struct TimeSignatureEvent {
    TimeSignatureEventId id; BarIndex bar; TimeSignature signature;
    bool operator==(const TimeSignatureEvent&) const = default;
};
struct TempoMap {
    std::vector<TempoEvent> events{{{1}, {0}, {120.0}, TempoCurve::step}};
    TempoEventId nextId{2};
    bool operator==(const TempoMap&) const = default;
};
struct TimeSignatureMap {
    std::vector<TimeSignatureEvent> events{{{1}, {0}, {4,4}}};
    TimeSignatureEventId nextId{2};
    bool operator==(const TimeSignatureMap&) const = default;
};
enum class Error { none, invalidTempo, invalidSignature, invalidId, duplicateEvent,
    missingInitialEvent, protectedInitialEvent, eventNotFound, outOfRange,
    conversionOverflow, unsupportedCurve, capacityExceeded, invalidSampleRate };
const char* errorName(Error) noexcept;
template<class T> struct Result {
    T value{}; Error error{Error::none};
    explicit operator bool() const noexcept { return error == Error::none; }
};
struct MusicalTimeMap {
    std::int64_t resolution{ppq};
    TempoMap tempo;
    TimeSignatureMap signatures;
    [[nodiscard]] Error validate() const;
    // Candidate-only history edit; exact expected values, IDs never recycled.
    [[nodiscard]] bool replace(std::optional<TempoEvent> before, std::optional<TempoEvent> after);
    [[nodiscard]] bool replace(std::optional<TimeSignatureEvent> before, std::optional<TimeSignatureEvent> after);
    bool operator==(const MusicalTimeMap&) const = default;
};
enum class Rounding { nearest, floor, ceil };
Result<timeline::ProjectFramePosition> quantizeFrame(timeline::PreciseProjectFramePosition, Rounding) noexcept;
struct PreparedTempoSegment {
    MusicalTickPosition startTick;
    QuarterNotePosition startQuarter;
    timeline::Seconds startSeconds;
    TempoBpm bpm;
    double secondsPerQuarter{};
};
struct PreparedExactTempoSegment {
    MusicalTickPosition startTick;
    audio::exact::Position startPosition;
    audio::exact::LinearMapping framesFromStart;
};
struct PreparedTimeSignatureSegment {
    BarIndex startBar; MusicalTickPosition startTick; TimeSignature signature;
    std::int64_t ticksPerBeat{}, ticksPerBar{};
};
enum class GridKind { bars, beats, subdivisions };
struct GridSubdivision { GridKind kind{GridKind::beats}; unsigned divisions{1}; };
struct GridLine { timeline::PreciseProjectFramePosition frame; MusicalPosition position; bool barStart{}; };
struct EnumerationResult { std::size_t count{}; bool hasMore{}; timeline::PreciseProjectFramePosition nextStart; Error error{}; };

class PreparedMusicalTimeMap {
public:
    static constexpr std::size_t memoryBudget = 1024 * 1024;
    static Result<std::unique_ptr<const PreparedMusicalTimeMap>> compile(
        const MusicalTimeMap&, timeline::SampleRate, std::uint64_t revision = 0);
    [[nodiscard]] std::uint64_t revision() const noexcept { return revision_; }
    [[nodiscard]] timeline::SampleRate sampleRate() const noexcept { return rate_; }
    [[nodiscard]] Result<QuarterNotePosition> quarterNotePositionAt(timeline::Seconds) const noexcept;
    [[nodiscard]] Result<QuarterNotePosition> quarterNotePositionAt(timeline::PreciseProjectFramePosition) const noexcept;
    [[nodiscard]] Result<QuarterNotePosition> quarterNotePositionAt(timeline::ProjectFramePosition f) const noexcept {
        return quarterNotePositionAt(timeline::PreciseProjectFramePosition{static_cast<double>(f.value)});
    }
    [[nodiscard]] Result<timeline::Seconds> secondsAt(QuarterNotePosition) const noexcept;
    [[nodiscard]] Result<timeline::PreciseProjectFramePosition> preciseProjectFrameAt(QuarterNotePosition) const noexcept;
    [[nodiscard]] Result<timeline::PreciseProjectFramePosition> preciseProjectFrameAtTick(MusicalTickPosition) const noexcept;
    // Returns the complete exact position, including a possible subframe
    // component. It is not necessarily an integer ProjectFramePosition.
    [[nodiscard]] Result<audio::exact::Position> exactProjectFrameAtTick(
        MusicalTickPosition) const noexcept;
    [[nodiscard]] Result<timeline::ProjectFramePosition> projectFrameAt(MusicalTickPosition, Rounding = Rounding::nearest) const noexcept;
    [[nodiscard]] Result<timeline::ProjectFramePosition> projectFrameAt(MusicalPosition, Rounding = Rounding::nearest) const noexcept;
    [[nodiscard]] Result<MusicalTickPosition> tickAt(MusicalPosition) const noexcept;
    [[nodiscard]] Result<MusicalTickPosition> absoluteTickAt(audio::exact::Position) const noexcept;
    [[nodiscard]] Result<MusicalTickPosition> absoluteTickAt(timeline::ProjectFramePosition) const noexcept;
    [[nodiscard]] Result<MusicalPosition> musicalPositionAt(audio::exact::Position) const noexcept;
    [[nodiscard]] Result<MusicalPosition> musicalPositionAt(timeline::PreciseProjectFramePosition) const noexcept;
    [[nodiscard]] Result<MusicalPosition> musicalPositionAt(timeline::ProjectFramePosition) const noexcept;
    [[nodiscard]] Result<TempoBpm> tempoAt(audio::exact::Position) const noexcept;
    [[nodiscard]] Result<TempoBpm> tempoAt(timeline::ProjectFramePosition) const noexcept;
    [[nodiscard]] Result<TimeSignature> timeSignatureAt(audio::exact::Position) const noexcept;
    [[nodiscard]] Result<TimeSignature> timeSignatureAt(timeline::ProjectFramePosition) const noexcept;
    [[nodiscard]] EnumerationResult enumerateGridLines(timeline::PreciseProjectFramePosition start,
        timeline::PreciseProjectFramePosition end, GridSubdivision, std::span<GridLine>) const noexcept;
    [[nodiscard]] EnumerationResult enumerateBeats(timeline::PreciseProjectFramePosition start,
        timeline::PreciseProjectFramePosition end, std::span<GridLine> out) const noexcept {
        return enumerateGridLines(start, end, {GridKind::beats, 1}, out);
    }
private:
    PreparedMusicalTimeMap() = default;
    MusicalPosition positionAtTick(std::int64_t) const noexcept;
    timeline::SampleRate rate_;
    std::uint64_t revision_{};
    std::vector<PreparedTempoSegment> tempos_;
    std::vector<PreparedExactTempoSegment> exactTempos_;
    std::vector<PreparedTimeSignatureSegment> signatures_;
};
} // namespace vitadaw::musical
