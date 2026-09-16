#pragma once
#include "vitadaw/audio/ExactTemporal.h"
#include <optional>

namespace vitadaw::audio::exact {
// Integer public locator + exact conversion residue. No elapsed-device counter
// and no advancing per-track clock. Formats are certified before installation.
class ProjectPhase {
public:
    ProjectPhase() = default;
    // Copies already-certified runtime data; no preparation or conversion.
    ProjectPhase(ClockFormat format, Position position) noexcept : format_(format), position_(position) {}
    struct Loop { RationalBoundary start, end; };
    struct Checkpoint { Position position; };

    // Quiescent/non-RT. Failure leaves both format and position untouched.
    bool installPreparedFormat(ClockFormat format) noexcept {
        if (!format.valid) return false;
        const auto product = multiply(position_.phase.magnitude, format.denominator);
        const auto converted = divmod(product, wide(position_.phase.denominator));
        if (!converted.valid || !zero(converted.remainder) || !fits128(converted.quotient)) return false;
        position_.phase.magnitude = low128(converted.quotient);
        position_.phase.denominator = format.denominator;
        format_ = format;
        return true;
    }
    bool restore(Checkpoint checkpoint) noexcept {
        const auto denominator = wide(checkpoint.position.phase.denominator);
        UInt256 twice;
        if (zero(denominator) ||
            shiftLeft(wide(checkpoint.position.phase.magnitude), 1, twice) ||
            compare(twice, denominator) > 0) return false;
        const auto old = position_;
        position_ = checkpoint.position;
        if (position_.frame < 0 || static_cast<std::uint64_t>(position_.frame) > maximumFrame ||
            !installPreparedFormat(format_)) {
            position_ = old;
            return false;
        }
        return true;
    }
    Checkpoint checkpoint() const noexcept { return {position_}; }
    Position position() const noexcept { return position_; }
    const ClockFormat& format() const noexcept { return format_; }
    void locate(std::uint64_t frame) noexcept {
        position_ = {static_cast<std::int64_t>(frame), {{}, format_.denominator, false}};
    }
    void locate(Boundary boundary) noexcept { setTicks(ticks(boundary)); }
    void locate(RationalBoundary boundary) noexcept { setTicks(ticks(boundary)); }
    bool before(Boundary boundary) const noexcept { return compareBoundary(position_, boundary) < 0; }
    bool before(RationalBoundary boundary) const noexcept { return compareBoundary(position_, boundary) < 0; }
    // All policy decisions stay in RealtimeProjectClock/TransportReducer.
    // Returns true iff a prepared loop boundary was crossed.
    // Preparation certifies loopLength >= one device-frame step, so one call
    // can cross at most one boundary and a boolean preserves the full event.
    bool advance(std::optional<Loop> loop) noexcept {
        const auto current = floorPosition(position_);
        auto remainder = wide(current.remainder);
        add(remainder, wide(format_.stepRemainder), remainder);
        const bool carry = compare(remainder, wide(format_.denominator)) >= 0;
        if (carry) subtract(remainder, wide(format_.denominator), remainder);
        const auto frame = static_cast<std::uint64_t>(current.frame) + format_.stepWhole + (carry ? 1 : 0);
        setFloor(frame, low128(remainder));
        if (loop && !before(loop->end)) {
            const auto start = ticks(loop->start), end = ticks(loop->end);
            UInt256 distance, length;
            subtract(ticks(), end, distance);
            subtract(end, start, length);
            const auto overshoot = divmod(distance, length);
            UInt256 wrapped;
            add(start, overshoot.remainder, wrapped);
            setTicks(wrapped);
            return true;
        }
        return false;
    }
    std::size_t framesBefore(Boundary boundary, std::size_t limit) const noexcept {
        if (!before(boundary)) return 1;
        UInt256 distance;
        subtract(ticks(boundary), ticks(), distance);
        auto step = multiply({format_.stepWhole, 0}, format_.denominator);
        add(step, wide(format_.stepRemainder), step);
        const auto count = divmod(distance, step);
        if (!count.valid || compare(count.quotient, wide(limit)) >= 0) return limit;
        const auto rounded = count.quotient.words[0] + (zero(count.remainder) ? 0 : 1);
        return static_cast<std::size_t>(std::max<std::uint64_t>(1, rounded));
    }
    std::size_t framesBefore(RationalBoundary boundary, std::size_t limit) const noexcept {
        if (!before(boundary)) return 1;
        UInt256 distance;
        subtract(ticks(boundary), ticks(), distance);
        auto step = multiply({format_.stepWhole, 0}, format_.denominator);
        add(step, wide(format_.stepRemainder), step);
        const auto count = divmod(distance, step);
        if (!count.valid || compare(count.quotient, wide(limit)) >= 0) return limit;
        const auto rounded = count.quotient.words[0] + (zero(count.remainder) ? 0 : 1);
        return static_cast<std::size_t>(std::max<std::uint64_t>(1, rounded));
    }
private:
    UInt256 ticks(Boundary boundary) const noexcept {
        auto result = multiply({boundary.whole, 0}, format_.denominator);
        const auto scale = shiftRight(wide(format_.denominator), boundary.denominatorPower);
        add(result, multiply(boundary.fraction, low128(scale)), result);
        return result;
    }
    UInt256 ticks(RationalBoundary boundary) const noexcept {
        auto result = multiply({boundary.whole, 0}, format_.denominator);
        const auto scale = divmod(wide(format_.denominator),
                                  wide(boundary.denominator));
        UInt256 fraction;
        if (!scale.valid || !zero(scale.remainder) || !fits128(scale.quotient) ||
            !fits128((fraction = multiply(boundary.numerator,
                                          low128(scale.quotient))))) return {};
        add(result, fraction, result);
        return result;
    }
    UInt256 ticks() const noexcept {
        const auto floor = floorPosition(position_);
        auto result = multiply({static_cast<std::uint64_t>(floor.frame), 0}, floor.denominator);
        add(result, wide(floor.remainder), result);
        return result;
    }
    void setTicks(UInt256 value) noexcept {
        const auto at = divmod(value, wide(format_.denominator));
        setFloor(at.quotient.words[0], low128(at.remainder));
    }
    void setFloor(std::uint64_t frame, UInt128 remainder) noexcept {
        UInt256 twice;
        shiftLeft(wide(remainder), 1, twice);
        const bool roundUp = compare(twice, wide(format_.denominator)) >= 0;
        auto magnitude = wide(remainder);
        if (roundUp) subtract(wide(format_.denominator), magnitude, magnitude);
        position_ = {static_cast<std::int64_t>(frame + (roundUp ? 1 : 0)),
                     {low128(magnitude), format_.denominator, roundUp}};
    }
    ClockFormat format_;
    Position position_;
};
} // namespace vitadaw::audio::exact
