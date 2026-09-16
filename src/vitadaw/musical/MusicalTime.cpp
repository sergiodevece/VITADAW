#include "vitadaw/musical/MusicalTime.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <unordered_set>

namespace vitadaw::musical {
namespace {
bool musicalCoordinate(double v) noexcept {
    return std::isfinite(v) && v >= 0 && v <= maximumCoordinate;
}
bool projectCoordinate(double v) noexcept {
    return std::isfinite(v) && v >= 0 &&
        v <= static_cast<double>(audio::exact::maximumFrame);
}
template<class Container, class Key> auto presentationSegment(
    const Container& c, double value, Key key) noexcept {
    auto i = std::upper_bound(c.begin(), c.end(), value,
        [&](double v, const auto& item) { return v < key(item); });
    return i == c.begin() ? i : std::prev(i);
}
template<class Container, class Key> auto integerSegment(
    const Container& c, std::int64_t value, Key key) noexcept {
    auto i = std::upper_bound(c.begin(), c.end(), value,
        [&](std::int64_t v, const auto& item) { return v < key(item); });
    return i == c.begin() ? i : std::prev(i);
}
bool normalized(audio::exact::Position position) noexcept {
    const auto magnitude=audio::exact::wide(position.phase.magnitude);
    return !(position.phase.negative&&!audio::exact::zero(magnitude)&&
             position.frame==std::numeric_limits<std::int64_t>::min()) &&
        !audio::exact::zero(audio::exact::wide(position.phase.denominator)) &&
        audio::exact::compare(magnitude,
                              audio::exact::wide(position.phase.denominator)) < 0;
}
Result<audio::exact::Position> exactPresentationPosition(double value) noexcept {
    if (!projectCoordinate(value)) return {{}, Error::outOfRange};
    const auto boundary = audio::exact::rationalBoundaryForPreparation(value);
    if (!boundary.valid) return {{}, Error::capacityExceeded};
    return {audio::exact::boundaryPosition(boundary), {}};
}
timeline::PreciseProjectFramePosition presentationPosition(
    audio::exact::Position position) noexcept {
    const auto floor = audio::exact::floorPosition(position);
    return {static_cast<double>(floor.frame) +
        audio::exact::approximate(audio::exact::wide(floor.remainder)) /
        audio::exact::approximate(audio::exact::wide(floor.denominator))};
}
Result<timeline::ProjectFramePosition> quantizeExact(
    audio::exact::Position position, Rounding policy) noexcept {
    if (!normalized(position)) return {{}, Error::conversionOverflow};
    const auto floor = audio::exact::floorPosition(position);
    if (floor.frame < 0 ||
        static_cast<std::uint64_t>(floor.frame) > audio::exact::maximumFrame)
        return {{}, Error::conversionOverflow};
    const bool fractional = !audio::exact::zero(audio::exact::wide(floor.remainder));
    bool increment = false;
    switch (policy) {
    case Rounding::floor: break;
    case Rounding::ceil: increment = fractional; break;
    case Rounding::nearest: {
        audio::exact::UInt256 twice;
        const auto overflow = audio::exact::shiftLeft(
            audio::exact::wide(floor.remainder), 1, twice);
        increment = overflow ||
            audio::exact::compare(twice,
                                  audio::exact::wide(floor.denominator)) >= 0;
        break;
    }
    default: return {{}, Error::outOfRange};
    }
    if (increment && static_cast<std::uint64_t>(floor.frame) ==
                         audio::exact::maximumFrame)
        return {{}, Error::conversionOverflow};
    return {{floor.frame + (increment ? 1 : 0)}, {}};
}
template<class E, class Map> bool replaceEvent(Map& m, std::optional<E> before, std::optional<E> after) {
    auto& events = m.events;
    if (!before && !after) return false;
    if (before) {
        auto i = std::find(events.begin(), events.end(), *before);
        if (i == events.end()) return false;
        if (after) *i = *after; else events.erase(i);
    } else {
        if (events.size() >= maximumEvents) return false;
        events.push_back(*after);
    }
    if (after) {
        if (after->id.value == 0 || after->id.value == UINT64_MAX) return false;
        m.nextId.value = std::max(m.nextId.value, after->id.value + 1);
    }
    std::sort(events.begin(), events.end(), [](const E& a, const E& b) {
        if constexpr (std::is_same_v<E, TempoEvent>) return a.tick.value < b.tick.value;
        else return a.bar.value < b.bar.value;
    });
    return true;
}
}
bool TempoBpm::isValid() const noexcept { return std::isfinite(value) && value >= 20 && value <= 400; }
bool TimeSignature::isValid() const noexcept {
    return numerator >= 1 && numerator <= 32 && denominator >= 1 && denominator <= 64 &&
           (denominator & (denominator - 1)) == 0;
}
const char* errorName(Error e) noexcept {
    switch (e) {
    case Error::none: return "Musical time updated";
    case Error::invalidTempo: return "invalidTempo";
    case Error::invalidSignature: return "invalidSignature";
    case Error::invalidId: return "invalidId";
    case Error::duplicateEvent: return "duplicateEvent";
    case Error::missingInitialEvent: return "missingInitialEvent";
    case Error::protectedInitialEvent: return "protectedInitialEvent";
    case Error::eventNotFound: return "eventNotFound";
    case Error::outOfRange: return "outOfRange";
    case Error::conversionOverflow: return "conversionOverflow";
    case Error::unsupportedCurve: return "unsupportedCurve";
    case Error::capacityExceeded: return "capacityExceeded";
    case Error::invalidSampleRate: return "invalidSampleRate";
    }
    return "invalidMusicalTime";
}
Error MusicalTimeMap::validate() const {
    if (resolution != ppq) return Error::outOfRange;
    if (tempo.events.size() > maximumEvents || signatures.events.size() > maximumEvents) return Error::capacityExceeded;
    if (tempo.events.empty() || signatures.events.empty() || tempo.events[0].tick.value != 0 ||
        signatures.events[0].bar.value != 0) return Error::missingInitialEvent;
    auto validateEvents = [](const auto& map) {
        if (!map.nextId.value) return Error::invalidId;
        std::unordered_set<std::uint64_t> ids;
        std::int64_t previous = -1;
        for (const auto& e : map.events) {
            if (!e.id.value || e.id.value >= map.nextId.value || !ids.insert(e.id.value).second) return Error::invalidId;
            std::int64_t at;
            if constexpr (std::is_same_v<std::decay_t<decltype(e)>, TempoEvent>) {
                at = e.tick.value;
                if (!e.bpm.isValid()) return Error::invalidTempo;
                if (e.curveToNext != TempoCurve::step) return Error::unsupportedCurve;
            } else {
                at = e.bar.value;
                if (!e.signature.isValid()) return Error::invalidSignature;
            }
            if (at < 0 || at > maximumCoordinate) return Error::outOfRange;
            if (at <= previous) return Error::duplicateEvent;
            previous = at;
        }
        return Error::none;
    };
    if (const auto e = validateEvents(tempo); e != Error::none) return e;
    return validateEvents(signatures);
}
bool MusicalTimeMap::replace(std::optional<TempoEvent> a, std::optional<TempoEvent> b) {
    if (a && a->tick.value == 0 && (!b || b->tick.value != 0 || b->id != a->id)) return false;
    return replaceEvent(tempo, a, b);
}
bool MusicalTimeMap::replace(std::optional<TimeSignatureEvent> a, std::optional<TimeSignatureEvent> b) {
    if (a && a->bar.value == 0 && (!b || b->bar.value != 0 || b->id != a->id)) return false;
    return replaceEvent(signatures, a, b);
}
Result<timeline::ProjectFramePosition> quantizeFrame(timeline::PreciseProjectFramePosition f, Rounding policy) noexcept {
    if (!projectCoordinate(f.value)) return {{}, Error::conversionOverflow};
    double v;
    switch (policy) {
    case Rounding::nearest: v = std::floor(f.value + 0.5); break;
    case Rounding::floor: v = std::floor(f.value); break;
    case Rounding::ceil: v = std::ceil(f.value); break;
    default: return {{}, Error::outOfRange};
    }
    return {{static_cast<std::int64_t>(v)}, {}};
}
Result<std::unique_ptr<const PreparedMusicalTimeMap>> PreparedMusicalTimeMap::compile(
    const MusicalTimeMap& map, timeline::SampleRate rate, std::uint64_t revision) {
    if (!rate.isValid()) return {{}, Error::invalidSampleRate};
    if (auto e = map.validate(); e != Error::none) return {{}, e};
    if (map.tempo.events.size() * sizeof(PreparedTempoSegment) +
        map.tempo.events.size() * sizeof(PreparedExactTempoSegment) +
        map.signatures.events.size() * sizeof(PreparedTimeSignatureSegment) > memoryBudget)
        return {{}, Error::capacityExceeded};
    auto out = std::unique_ptr<PreparedMusicalTimeMap>{new PreparedMusicalTimeMap};
    out->rate_ = rate; out->revision_ = revision;
    out->tempos_.reserve(map.tempo.events.size());
    out->exactTempos_.reserve(map.tempo.events.size());
    out->signatures_.reserve(map.signatures.events.size());
    double seconds = 0, compensation = 0;
    for (const auto& e : map.tempo.events) {
        audio::exact::Position exactAnchor{};
        if (!out->exactTempos_.empty()) {
            const auto& previous = out->exactTempos_.back();
            exactAnchor = previous.framesFromStart.at(
                static_cast<std::uint64_t>(e.tick.value - previous.startTick.value));
            if (exactAnchor.frame < 0 || !normalized(exactAnchor) ||
                static_cast<std::uint64_t>(exactAnchor.frame) >
                    audio::exact::maximumFrame)
                return {{}, Error::conversionOverflow};
        }
        const auto projectPerBpm = audio::exact::rateRatioForPreparation(
            rate.hertz(), e.bpm.value);
        if (!projectPerBpm.valid) return {{}, Error::capacityExceeded};
        audio::exact::UInt256 tickDenominator;
        if (audio::exact::shiftLeft(
                audio::exact::wide(projectPerBpm.denominator), 8,
                tickDenominator))
            return {{}, Error::capacityExceeded};
        const auto framesPerTick = audio::exact::reduceForPreparation(
            audio::exact::wide(projectPerBpm.numerator), tickDenominator);
        if (!framesPerTick.valid) return {{}, Error::capacityExceeded};
        const auto exactSegment = audio::exact::linearMappingForPreparation(
            exactAnchor, framesPerTick);
        if (!exactSegment.valid) return {{}, Error::capacityExceeded};
        out->exactTempos_.push_back({e.tick, exactAnchor, exactSegment});

        if (!out->tempos_.empty()) {
            const auto& p = out->tempos_.back();
            const auto delta = static_cast<double>(e.tick.value - p.startTick.value) / ppq * p.secondsPerQuarter;
            const auto y = delta - compensation, next = seconds + y;
            compensation = (next - seconds) - y;
            if (!std::isfinite(next) || next < seconds ||
                !projectCoordinate(next * rate.hertz()))
                return {{}, Error::conversionOverflow};
            seconds = next;
        }
        out->tempos_.push_back({e.tick, {static_cast<double>(e.tick.value) / ppq}, {seconds}, e.bpm, 60.0 / e.bpm.value});
    }
    const auto& last = out->exactTempos_.back();
    const auto exactEnd = last.framesFromStart.at(static_cast<std::uint64_t>(
        maximumCoordinate - last.startTick.value));
    const audio::exact::Position maximumPosition{
        static_cast<std::int64_t>(audio::exact::maximumFrame), {}};
    if (exactEnd.frame < 0 || !normalized(exactEnd) ||
        audio::exact::comparePositions(exactEnd, maximumPosition) > 0)
        return {{}, Error::conversionOverflow};

    std::int64_t tick = 0;
    for (const auto& e : map.signatures.events) {
        if (!out->signatures_.empty()) {
            const auto& p = out->signatures_.back();
            const auto bars = e.bar.value - p.startBar.value;
            if (bars > (maximumCoordinate - tick) / p.ticksPerBar) return {{}, Error::conversionOverflow};
            tick += bars * p.ticksPerBar;
        }
        const auto beat = ppq * 4 / e.signature.denominator;
        out->signatures_.push_back({e.bar, {tick}, e.signature, beat, beat * e.signature.numerator});
        if (!out->exactProjectFrameAtTick({tick}))
            return {{}, Error::conversionOverflow};
    }
    return {std::move(out), {}};
}
Result<audio::exact::Position> PreparedMusicalTimeMap::exactProjectFrameAtTick(
    MusicalTickPosition tick) const noexcept {
    if (tick.value < 0 || tick.value > maximumCoordinate || exactTempos_.empty())
        return {{}, Error::outOfRange};
    const auto found = std::upper_bound(
        exactTempos_.begin(), exactTempos_.end(), tick.value,
        [](std::int64_t value, const PreparedExactTempoSegment& segment) {
            return value < segment.startTick.value;
        });
    const auto& segment = found == exactTempos_.begin() ? *found : *std::prev(found);
    const auto result = segment.framesFromStart.at(
        static_cast<std::uint64_t>(tick.value - segment.startTick.value));
    if (result.frame < 0 || !normalized(result) ||
        static_cast<std::uint64_t>(result.frame) > audio::exact::maximumFrame)
        return {{}, Error::conversionOverflow};
    return {result, {}};
}
Result<timeline::Seconds> PreparedMusicalTimeMap::secondsAt(QuarterNotePosition q) const noexcept {
    if (!musicalCoordinate(q.value * ppq)) return {{}, Error::outOfRange};
    auto i = presentationSegment(tempos_, q.value,
        [](auto& t) { return t.startQuarter.value; });
    const auto s = i->startSeconds.value + (q.value - i->startQuarter.value) * i->secondsPerQuarter;
    if (!std::isfinite(s) || !projectCoordinate(s * rate_.hertz()))
        return {{}, Error::conversionOverflow};
    return {{s}, {}};
}
Result<QuarterNotePosition> PreparedMusicalTimeMap::quarterNotePositionAt(timeline::Seconds s) const noexcept {
    if (!projectCoordinate(s.value * rate_.hertz())) return {{}, Error::outOfRange};
    auto i = presentationSegment(tempos_, s.value,
        [](auto& t) { return t.startSeconds.value; });
    const auto q = i->startQuarter.value + (s.value - i->startSeconds.value) / i->secondsPerQuarter;
    if (!musicalCoordinate(q * ppq)) return {{}, Error::conversionOverflow};
    return {{q}, {}};
}
Result<QuarterNotePosition> PreparedMusicalTimeMap::quarterNotePositionAt(timeline::PreciseProjectFramePosition f) const noexcept {
    if (!projectCoordinate(f.value)) return {{}, Error::outOfRange};
    return quarterNotePositionAt(timeline::Seconds{f.value / rate_.hertz()});
}
Result<timeline::PreciseProjectFramePosition> PreparedMusicalTimeMap::preciseProjectFrameAt(QuarterNotePosition q) const noexcept {
    auto s = secondsAt(q); if (!s) return {{}, s.error};
    return {{s.value.value * rate_.hertz()}, {}};
}
Result<timeline::PreciseProjectFramePosition>
PreparedMusicalTimeMap::preciseProjectFrameAtTick(
    MusicalTickPosition tick) const noexcept {
    const auto exact = exactProjectFrameAtTick(tick);
    if (!exact) return {{}, exact.error};
    return {presentationPosition(exact.value), {}};
}
Result<timeline::ProjectFramePosition> PreparedMusicalTimeMap::projectFrameAt(MusicalTickPosition t, Rounding p) const noexcept {
    const auto exact = exactProjectFrameAtTick(t);
    if (!exact) return {{}, exact.error};
    return quantizeExact(exact.value, p);
}
Result<MusicalTickPosition> PreparedMusicalTimeMap::tickAt(MusicalPosition p) const noexcept {
    if (p.bar.value < 0 || p.bar.value > maximumCoordinate) return {{}, Error::outOfRange};
    auto i = integerSegment(signatures_, p.bar.value,
        [](const auto& s) { return s.startBar.value; });
    if (p.beat.value < 0 || p.beat.value >= i->signature.numerator || p.tick.value < 0 || p.tick.value >= i->ticksPerBeat)
        return {{}, Error::outOfRange};
    auto bars = p.bar.value - i->startBar.value;
    const auto rest = p.beat.value * i->ticksPerBeat + p.tick.value;
    if (bars > (maximumCoordinate - i->startTick.value - rest) / i->ticksPerBar) return {{}, Error::conversionOverflow};
    const auto t = i->startTick.value + bars * i->ticksPerBar + rest;
    if (t > maximumCoordinate) return {{}, Error::conversionOverflow};
    return {{t}, {}};
}
Result<timeline::ProjectFramePosition> PreparedMusicalTimeMap::projectFrameAt(MusicalPosition p, Rounding rounding) const noexcept {
    auto t = tickAt(p); if (!t) return {{}, t.error}; return projectFrameAt(t.value, rounding);
}
MusicalPosition PreparedMusicalTimeMap::positionAtTick(std::int64_t tick) const noexcept {
    auto i = integerSegment(signatures_, tick,
        [](const auto& s) { return s.startTick.value; });
    auto delta = tick - i->startTick.value;
    return {{i->startBar.value + delta / i->ticksPerBar}, {(delta % i->ticksPerBar) / i->ticksPerBeat}, {delta % i->ticksPerBeat}};
}
Result<MusicalTickPosition> PreparedMusicalTimeMap::absoluteTickAt(
    audio::exact::Position position) const noexcept {
    if (!normalized(position) || exactTempos_.empty())
        return {{}, Error::outOfRange};
    const auto first = exactTempos_.front().startPosition;
    const auto maximum = exactProjectFrameAtTick({maximumCoordinate});
    if (!maximum || audio::exact::comparePositions(position, first) < 0 ||
        audio::exact::comparePositions(position, maximum.value) > 0)
        return {{}, Error::outOfRange};

    // Segment membership is resolved first. At the exact anchor of the next
    // segment, upper_bound selects that new segment; the previous segment's
    // half-open [startTick, nextStartTick) search can never return nextStartTick.
    const auto found = std::upper_bound(
        exactTempos_.begin(), exactTempos_.end(), position,
        [](const audio::exact::Position& value,
           const PreparedExactTempoSegment& candidate) {
            return audio::exact::comparePositions(value,
                                                   candidate.startPosition) < 0;
        });
    const auto index = static_cast<std::size_t>(
        std::distance(exactTempos_.begin(), std::prev(found)));
    const auto& selected = exactTempos_[index];
    std::int64_t low = selected.startTick.value;
    std::int64_t high = index + 1 < exactTempos_.size()
        ? exactTempos_[index + 1].startTick.value
        : maximumCoordinate + 1;
    while (low + 1 < high) {
        const auto middle = low + (high - low) / 2;
        const auto boundary = selected.framesFromStart.at(
            static_cast<std::uint64_t>(middle - selected.startTick.value));
        if (boundary.frame >= 0 &&
            audio::exact::comparePositions(boundary, position) <= 0)
            low = middle;
        else
            high = middle;
    }
    return {{low}, {}};
}
Result<MusicalTickPosition> PreparedMusicalTimeMap::absoluteTickAt(
    timeline::ProjectFramePosition position) const noexcept {
    if (!timeline::isSupportedProjectFramePosition(position))
        return {{}, Error::outOfRange};
    return absoluteTickAt({position.value, {}});
}
Result<MusicalPosition> PreparedMusicalTimeMap::musicalPositionAt(
    audio::exact::Position position) const noexcept {
    const auto tick = absoluteTickAt(position);
    if (!tick) return {{}, tick.error};
    return {positionAtTick(tick.value.value), {}};
}
Result<MusicalPosition> PreparedMusicalTimeMap::musicalPositionAt(timeline::PreciseProjectFramePosition f) const noexcept {
    const auto exact = exactPresentationPosition(f.value);
    if (!exact) return {{}, exact.error};
    return musicalPositionAt(exact.value);
}
Result<MusicalPosition> PreparedMusicalTimeMap::musicalPositionAt(
    timeline::ProjectFramePosition position) const noexcept {
    const auto tick = absoluteTickAt(position);
    if (!tick) return {{}, tick.error};
    return {positionAtTick(tick.value.value), {}};
}
Result<TempoBpm> PreparedMusicalTimeMap::tempoAt(
    audio::exact::Position position) const noexcept {
    const auto tick = absoluteTickAt(position);
    if (!tick) return {{}, tick.error};
    const auto segment = integerSegment(tempos_, tick.value.value,
        [](const auto& item) { return item.startTick.value; });
    return {segment->bpm, {}};
}
Result<TempoBpm> PreparedMusicalTimeMap::tempoAt(timeline::ProjectFramePosition f) const noexcept {
    if (!timeline::isSupportedProjectFramePosition(f))
        return {{}, Error::outOfRange};
    return tempoAt({f.value, {}});
}
Result<TimeSignature> PreparedMusicalTimeMap::timeSignatureAt(
    audio::exact::Position position) const noexcept {
    const auto tick = absoluteTickAt(position);
    if (!tick) return {{}, tick.error};
    const auto segment = integerSegment(signatures_, tick.value.value,
        [](const auto& item) { return item.startTick.value; });
    return {segment->signature, {}};
}
Result<TimeSignature> PreparedMusicalTimeMap::timeSignatureAt(timeline::ProjectFramePosition f) const noexcept {
    if (!timeline::isSupportedProjectFramePosition(f))
        return {{}, Error::outOfRange};
    return timeSignatureAt({f.value, {}});
}
EnumerationResult PreparedMusicalTimeMap::enumerateGridLines(timeline::PreciseProjectFramePosition start,
    timeline::PreciseProjectFramePosition end, GridSubdivision subdivision, std::span<GridLine> output) const noexcept {
    EnumerationResult result;
    const auto exactStart = exactPresentationPosition(start.value);
    const auto exactEnd = exactPresentationPosition(end.value);
    if (!exactStart || !exactEnd ||
        audio::exact::comparePositions(exactEnd.value, exactStart.value) < 0 ||
        subdivision.divisions == 0 || subdivision.divisions > 64) {
        result.error = Error::outOfRange; return result;
    }
    const auto musicalEnd=exactProjectFrameAtTick({maximumCoordinate});
    if (!musicalEnd ||
        audio::exact::comparePositions(exactEnd.value,musicalEnd.value)>0) {
        result.error=Error::outOfRange;return result;
    }
    if (audio::exact::comparePositions(exactStart.value, exactEnd.value) == 0)
        return result;
    const auto firstTick = absoluteTickAt(exactStart.value);
    if (!firstTick) { result.error = firstTick.error; return result; }
    auto si = integerSegment(signatures_, firstTick.value.value,
        [](const auto& item) { return item.startTick.value; });
    auto ti = integerSegment(exactTempos_, firstTick.value.value,
        [](const auto& item) { return item.startTick.value; });
    while (si != signatures_.end()) {
        const auto step = subdivision.kind == GridKind::bars ? si->ticksPerBar :
            subdivision.kind == GridKind::beats ? si->ticksPerBeat : si->ticksPerBeat / subdivision.divisions;
        if (step <= 0 || (subdivision.kind == GridKind::subdivisions && si->ticksPerBeat % subdivision.divisions != 0)) {
            result.error = Error::outOfRange; return result;
        }
        const auto initial = std::max(si->startTick.value, firstTick.value.value);
        auto tick = si->startTick.value +
            ((initial - si->startTick.value) / step) * step;
        if (tick < initial) tick += step;
        if (tick <= maximumCoordinate) {
            while (std::next(ti) != exactTempos_.end() &&
                   std::next(ti)->startTick.value <= tick) ++ti;
            auto boundary = ti->framesFromStart.at(
                static_cast<std::uint64_t>(tick - ti->startTick.value));
            if (boundary.frame < 0) {
                result.error = Error::conversionOverflow; return result;
            }
            if (audio::exact::comparePositions(boundary, exactStart.value) < 0)
                tick += step;
        }
        const auto nextSignature = std::next(si);
        const auto limit = nextSignature == signatures_.end() ? maximumCoordinate : nextSignature->startTick.value;
        for (; tick < limit && tick <= maximumCoordinate;) {
            while (std::next(ti) != exactTempos_.end() &&
                   std::next(ti)->startTick.value <= tick) ++ti;
            const auto exactFrame = ti->framesFromStart.at(
                static_cast<std::uint64_t>(tick - ti->startTick.value));
            if (exactFrame.frame < 0) {
                result.error = Error::conversionOverflow; return result;
            }
            if (audio::exact::comparePositions(exactFrame, exactStart.value) < 0) {
                if (step > maximumCoordinate - tick) break;
                tick += step; continue;
            }
            if (audio::exact::comparePositions(exactFrame, exactEnd.value) >= 0)
                return result;
            const auto frame = presentationPosition(exactFrame);
            if (result.count == output.size()) {
                result.hasMore = true; result.nextStart = frame; return result;
            }
            const auto delta = tick - si->startTick.value;
            const MusicalPosition pos{{si->startBar.value + delta / si->ticksPerBar},
                {(delta % si->ticksPerBar) / si->ticksPerBeat}, {delta % si->ticksPerBeat}};
            output[result.count++] = {frame, pos, delta % si->ticksPerBar == 0};
            if (step > maximumCoordinate - tick) break;
            tick += step;
        }
        si = nextSignature;
    }
    return result;
}
} // namespace vitadaw::musical
