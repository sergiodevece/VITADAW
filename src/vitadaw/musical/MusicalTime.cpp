#include "vitadaw/musical/MusicalTime.h"
#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <unordered_set>

namespace vitadaw::musical {
namespace {
bool coordinate(double v) noexcept { return std::isfinite(v) && v >= 0 && v <= maximumCoordinate; }
template<class Container, class Key> auto segment(const Container& c, double value, Key key) noexcept {
    auto i = std::upper_bound(c.begin(), c.end(), value,
        [&](double v, const auto& item) { return v < key(item); });
    return i == c.begin() ? i : std::prev(i);
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
    if (!coordinate(f.value)) return {{}, Error::conversionOverflow};
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
        map.signatures.events.size() * sizeof(PreparedTimeSignatureSegment) +
        (2 * map.tempo.events.size() - 1) * sizeof(TempoLookupNode) > memoryBudget)
        return {{}, Error::capacityExceeded};
    auto out = std::unique_ptr<PreparedMusicalTimeMap>{new PreparedMusicalTimeMap};
    out->rate_ = rate; out->revision_ = revision;
    out->tempos_.reserve(map.tempo.events.size());
    out->exactTempos_.reserve(map.tempo.events.size());
    out->signatures_.reserve(map.signatures.events.size());
    double seconds = 0, compensation = 0;
    for (const auto& e : map.tempo.events) {
        if (out->exactDspCertified_) {
            audio::exact::Position exactAnchor{};
            if (!out->exactTempos_.empty()) {
                const auto& previous = out->exactTempos_.back();
                exactAnchor = previous.framesFromStart.at(
                    static_cast<std::uint64_t>(e.tick.value - previous.startTick.value));
                if (exactAnchor.frame < 0 ||
                    static_cast<std::uint64_t>(exactAnchor.frame) > audio::exact::maximumFrame)
                    out->exactDspCertified_ = false;
            }
            const auto projectPerBpm = audio::exact::rateRatioForPreparation(
                rate.hertz(), e.bpm.value);
            audio::exact::UInt256 tickDenominator;
            if (!projectPerBpm.valid ||
                audio::exact::shiftLeft(audio::exact::wide(projectPerBpm.denominator),
                                        8, tickDenominator)) {
                out->exactDspCertified_ = false;
            }
            if (out->exactDspCertified_) {
                const auto framesPerTick = audio::exact::reduceForPreparation(
                    audio::exact::wide(projectPerBpm.numerator), tickDenominator);
                const auto exactSegment = audio::exact::linearMappingForPreparation(
                    exactAnchor, framesPerTick);
                if (!exactSegment.valid) out->exactDspCertified_ = false;
                else out->exactTempos_.push_back({e.tick, exactSegment});
            }
            if (!out->exactDspCertified_) out->exactTempos_.clear();
        }
        if (!out->tempos_.empty()) {
            const auto& p = out->tempos_.back();
            const auto delta = static_cast<double>(e.tick.value - p.startTick.value) / ppq * p.secondsPerQuarter;
            const auto y = delta - compensation, next = seconds + y;
            compensation = (next - seconds) - y;
            if (!std::isfinite(next) || next <= seconds || !coordinate(next * rate.hertz()))
                return {{}, Error::conversionOverflow};
            seconds = next;
        }
        out->tempos_.push_back({e.tick, {static_cast<double>(e.tick.value) / ppq}, {seconds}, e.bpm, 60.0 / e.bpm.value});
    }
    if (out->exactDspCertified_) {
        const auto& last = out->exactTempos_.back();
        if (last.framesFromStart.at(static_cast<std::uint64_t>(
                maximumCoordinate - last.startTick.value)).frame < 0) {
            out->exactDspCertified_ = false;
            out->exactTempos_.clear();
        }
    }
    out->gridTempoIndex_.reserve(2 * out->tempos_.size() - 1);
    auto buildIndex = [&](auto&& self, std::size_t begin, std::size_t end) -> std::uint32_t {
        const auto node = static_cast<std::uint32_t>(out->gridTempoIndex_.size());
        out->gridTempoIndex_.push_back({0, UINT32_MAX, UINT32_MAX, static_cast<std::uint32_t>(begin)});
        if (end - begin == 1) return node;
        const auto first = static_cast<std::uint64_t>(out->tempos_[begin].startTick.value);
        const auto last = static_cast<std::uint64_t>(out->tempos_[end - 1].startTick.value);
        const auto bit = std::bit_width(first ^ last) - 1;
        const auto pivot = ((first >> bit) + 1) << bit;
        const auto split = static_cast<std::size_t>(std::lower_bound(out->tempos_.begin() + begin,
            out->tempos_.begin() + end, pivot, [](const auto& t, std::uint64_t v) {
                return static_cast<std::uint64_t>(t.startTick.value) < v;
            }) - out->tempos_.begin());
        const auto left = self(self, begin, split), right = self(self, split, end);
        out->gridTempoIndex_[node] = {out->tempos_[split].startQuarter.value, left, right, 0};
        return node;
    };
    buildIndex(buildIndex, 0, out->tempos_.size());
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
        if (out->exactDspCertified_) {
            if (!out->exactProjectFrameAtTick({tick}))
                return {{}, Error::conversionOverflow};
        } else if (!out->preciseProjectFrameAt(
                       {static_cast<double>(tick) / ppq})) {
            return {{}, Error::conversionOverflow};
        }
    }
    return {std::move(out), {}};
}
Result<audio::exact::Position> PreparedMusicalTimeMap::exactProjectFrameAtTick(
    MusicalTickPosition tick) const noexcept {
    if (tick.value < 0 || tick.value > maximumCoordinate ||
        !exactDspCertified_ || exactTempos_.empty())
        return {{}, Error::outOfRange};
    const auto found = std::upper_bound(
        exactTempos_.begin(), exactTempos_.end(), tick.value,
        [](std::int64_t value, const PreparedExactTempoSegment& segment) {
            return value < segment.startTick.value;
        });
    const auto& segment = found == exactTempos_.begin() ? *found : *std::prev(found);
    const auto result = segment.framesFromStart.at(
        static_cast<std::uint64_t>(tick.value - segment.startTick.value));
    if (result.frame < 0 || static_cast<std::uint64_t>(result.frame) > audio::exact::maximumFrame)
        return {{}, Error::conversionOverflow};
    return {result, {}};
}
Result<timeline::Seconds> PreparedMusicalTimeMap::secondsAt(QuarterNotePosition q) const noexcept {
    if (!coordinate(q.value * ppq)) return {{}, Error::outOfRange};
    auto i = segment(tempos_, q.value, [](auto& t) { return t.startQuarter.value; });
    const auto s = i->startSeconds.value + (q.value - i->startQuarter.value) * i->secondsPerQuarter;
    if (!std::isfinite(s) || !coordinate(s * rate_.hertz())) return {{}, Error::conversionOverflow};
    return {{s}, {}};
}
Result<QuarterNotePosition> PreparedMusicalTimeMap::quarterNotePositionAt(timeline::Seconds s) const noexcept {
    if (!coordinate(s.value * rate_.hertz())) return {{}, Error::outOfRange};
    auto i = segment(tempos_, s.value, [](auto& t) { return t.startSeconds.value; });
    const auto q = i->startQuarter.value + (s.value - i->startSeconds.value) / i->secondsPerQuarter;
    if (!coordinate(q * ppq)) return {{}, Error::conversionOverflow};
    return {{q}, {}};
}
Result<QuarterNotePosition> PreparedMusicalTimeMap::quarterNotePositionAt(timeline::PreciseProjectFramePosition f) const noexcept {
    if (!coordinate(f.value)) return {{}, Error::outOfRange};
    return quarterNotePositionAt(timeline::Seconds{f.value / rate_.hertz()});
}
Result<timeline::PreciseProjectFramePosition> PreparedMusicalTimeMap::preciseProjectFrameAt(QuarterNotePosition q) const noexcept {
    auto s = secondsAt(q); if (!s) return {{}, s.error};
    return {{s.value.value * rate_.hertz()}, {}};
}
Result<timeline::ProjectFramePosition> PreparedMusicalTimeMap::projectFrameAt(MusicalTickPosition t, Rounding p) const noexcept {
    auto f = preciseProjectFrameAt({static_cast<double>(t.value) / ppq});
    if (!f) return {{}, f.error};
    return quantizeFrame(f.value, p);
}
Result<MusicalTickPosition> PreparedMusicalTimeMap::tickAt(MusicalPosition p) const noexcept {
    if (p.bar.value < 0 || p.bar.value > maximumCoordinate) return {{}, Error::outOfRange};
    auto i = segment(signatures_, static_cast<double>(p.bar.value), [](auto& s) { return s.startBar.value; });
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
    auto i = segment(signatures_, static_cast<double>(tick), [](auto& s) { return s.startTick.value; });
    auto delta = tick - i->startTick.value;
    return {{i->startBar.value + delta / i->ticksPerBar}, {(delta % i->ticksPerBar) / i->ticksPerBeat}, {delta % i->ticksPerBeat}};
}
Result<MusicalPosition> PreparedMusicalTimeMap::musicalPositionAt(timeline::PreciseProjectFramePosition f) const noexcept {
    auto q = quarterNotePositionAt(f); if (!q) return {{}, q.error};
    auto tick = static_cast<std::int64_t>(std::floor(q.value.value * ppq));
    const auto currentBoundary = preciseProjectFrameAt({static_cast<double>(tick) / ppq});
    if (tick > 0 && currentBoundary && f.value < currentBoundary.value.value) --tick;
    // Normalize inversion error only by comparing the boundary's actual frame.
    if (tick < maximumCoordinate) {
        const auto boundary = preciseProjectFrameAt({static_cast<double>(tick + 1) / ppq});
        if (boundary && f.value >= boundary.value.value) ++tick;
    }
    return {positionAtTick(tick), {}};
}
Result<TempoBpm> PreparedMusicalTimeMap::tempoAt(timeline::ProjectFramePosition f) const noexcept {
    auto q = quarterNotePositionAt(f); if (!q) return {{}, q.error};
    return {segment(tempos_, q.value.value, [](auto& s) { return s.startQuarter.value; })->bpm, {}};
}
Result<TimeSignature> PreparedMusicalTimeMap::timeSignatureAt(timeline::ProjectFramePosition f) const noexcept {
    auto q = quarterNotePositionAt(f); if (!q) return {{}, q.error};
    return {segment(signatures_, q.value.value * ppq, [](auto& s) { return s.startTick.value; })->signature, {}};
}
EnumerationResult PreparedMusicalTimeMap::enumerateGridLines(timeline::PreciseProjectFramePosition start,
    timeline::PreciseProjectFramePosition end, GridSubdivision subdivision, std::span<GridLine> output) const noexcept {
    EnumerationResult result;
    auto q = quarterNotePositionAt(start);
    if (!q || !coordinate(end.value) || end.value < start.value || subdivision.divisions == 0 || subdivision.divisions > 64) {
        result.error = Error::outOfRange; return result;
    }
    auto si = segment(signatures_, q.value.value * ppq, [](auto& s) { return s.startTick.value; });
    while (si != signatures_.end()) {
        const auto step = subdivision.kind == GridKind::bars ? si->ticksPerBar :
            subdivision.kind == GridKind::beats ? si->ticksPerBeat : si->ticksPerBeat / subdivision.divisions;
        if (step <= 0 || (subdivision.kind == GridKind::subdivisions && si->ticksPerBeat % subdivision.divisions != 0)) {
            result.error = Error::outOfRange; return result;
        }
        const auto initial = std::max<double>(si->startTick.value, q.value.value * ppq);
        auto tick = si->startTick.value + static_cast<std::int64_t>(std::floor((initial - si->startTick.value) / step)) * step;
        const auto nextSignature = std::next(si);
        const auto limit = nextSignature == signatures_.end() ? maximumCoordinate : nextSignature->startTick.value;
        for (; tick < limit; tick += step) {
            const auto quarter = static_cast<double>(tick) / ppq;
            const auto& tempo = gridTempoAt(quarter);
            const auto frame = (tempo.startSeconds.value + (quarter - tempo.startQuarter.value) * tempo.secondsPerQuarter) * rate_.hertz();
            if (!coordinate(frame)) { result.error = Error::conversionOverflow; return result; }
            if (frame < start.value) continue;
            if (frame >= end.value) return result;
            if (result.count == output.size()) { result.hasMore = true; result.nextStart = {frame}; return result; }
            const auto delta = tick - si->startTick.value;
            const MusicalPosition pos{{si->startBar.value + delta / si->ticksPerBar},
                {(delta % si->ticksPerBar) / si->ticksPerBeat}, {delta % si->ticksPerBeat}};
            output[result.count++] = {{frame}, pos, delta % si->ticksPerBar == 0};
        }
        si = nextSignature;
    }
    return result;
}
const PreparedTempoSegment& PreparedMusicalTimeMap::gridTempoAt(double quarter) const noexcept {
    std::uint32_t index = 0;
    // Each branch removes the highest differing tick bit; at most 41 branches.
    while (gridTempoIndex_[index].left != UINT32_MAX) {
        const auto& node = gridTempoIndex_[index];
        index = quarter < node.splitQuarter ? node.left : node.right;
    }
    return tempos_[gridTempoIndex_[index].tempoIndex];
}
} // namespace vitadaw::musical
