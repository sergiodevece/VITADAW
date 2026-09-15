#pragma once
#include "vitadaw/audio/TemporalInteger.h"
#include <algorithm>
#include <cstdint>

namespace vitadaw::audio::exact {
inline constexpr std::uint64_t maximumFrame = (std::uint64_t{1} << 53) - 1;
inline unsigned trailingZeros(UInt256 value) noexcept {
    for (unsigned i = 0; i < 4; ++i)
        if (value.words[i]) return i * 64 + std::countr_zero(value.words[i]);
    return 256;
}
inline UInt256 powerOfTwo(unsigned exponent) noexcept {
    UInt256 out;
    shiftLeft(wide(1), exponent, out);
    return out;
}

// Prepared dyadic document coordinate, split before any DSP comparisons.
struct Boundary {
    std::uint64_t whole{};
    UInt128 fraction{};
    unsigned denominatorPower{};
    bool valid{};
};
inline Boundary boundaryForPreparation(double value, unsigned maximumPower) noexcept {
    const auto decoded = decodeForPreparation(value);
    if (!decoded.valid) return {};
    const auto division = divmod(wide(decoded.numerator), wide(decoded.denominator));
    const auto power = trailingZeros(wide(decoded.denominator));
    if (power > maximumPower || !fits64(division.quotient) ||
        division.quotient.words[0] > maximumFrame ||
        (division.quotient.words[0] == maximumFrame && !zero(division.remainder))) return {};
    return {division.quotient.words[0], low128(division.remainder), power, true};
}
struct Phase {
    UInt128 magnitude{};
    UInt128 denominator{1, 0};
    bool negative{};
};
struct Position {
    std::int64_t frame{};
    Phase phase;
};
// Prepared non-dyadic boundary used only where musical time requires it.
// Existing clip/source boundaries remain dyadic Boundary values.
struct RationalBoundary {
    std::uint64_t whole{};
    UInt128 numerator{};
    UInt128 denominator{1, 0};
    bool valid{};
};
inline RationalBoundary rationalBoundaryForPreparation(double value) noexcept {
    const auto decoded = decodeForPreparation(value);
    if (!decoded.valid || zero(wide(decoded.denominator))) return {};
    const auto split = divmod(wide(decoded.numerator), wide(decoded.denominator));
    if (!split.valid || !fits64(split.quotient) ||
        split.quotient.words[0] > maximumFrame ||
        (split.quotient.words[0] == maximumFrame && !zero(split.remainder))) return {};
    return {split.quotient.words[0], low128(split.remainder),
            decoded.denominator, true};
}
struct LoopBounds {
    double start{}, end{}; // presentation/legacy document values only
    RationalBoundary exactStart, exactEnd;
    bool valid{};
    LoopBounds() = default;
    // Preparation-only constructor. RT copies this already-prepared value.
    LoopBounds(double first, double last) noexcept
        : start(first), end(last), exactStart(rationalBoundaryForPreparation(first)),
          exactEnd(rationalBoundaryForPreparation(last)) {
        if (!exactStart.valid || !exactEnd.valid) return;
        if (exactStart.whole != exactEnd.whole) valid = exactStart.whole < exactEnd.whole;
        else valid = compare(multiply(exactStart.numerator, exactEnd.denominator),
                             multiply(exactEnd.numerator, exactStart.denominator)) < 0;
    }
    LoopBounds(RationalBoundary first, RationalBoundary last,
               double presentationFirst, double presentationLast) noexcept
        : start(presentationFirst), end(presentationLast), exactStart(first), exactEnd(last) {
        valid = first.valid && last.valid &&
            (first.whole < last.whole ||
             (first.whole == last.whole &&
              compare(multiply(first.numerator, last.denominator),
                      multiply(last.numerator, first.denominator)) < 0));
    }
};
struct FloorPosition { std::int64_t frame{}; UInt128 remainder{}, denominator{1, 0}; };
inline FloorPosition floorPosition(Position position) noexcept {
    auto remainder = wide(position.phase.magnitude);
    const bool negative = position.phase.negative && !zero(remainder);
    if (negative) subtract(wide(position.phase.denominator), remainder, remainder);
    return {position.frame - (negative ? 1 : 0), low128(remainder), position.phase.denominator};
}
inline int compareBoundary(Position position, Boundary boundary) noexcept {
    const auto floor = floorPosition(position);
    if (floor.frame < 0) return -1;
    const auto whole = static_cast<std::uint64_t>(floor.frame);
    if (whole != boundary.whole) return whole < boundary.whole ? -1 : 1;
    UInt256 left;
    shiftLeft(wide(floor.remainder), boundary.denominatorPower, left);
    return compare(left, multiply(boundary.fraction, floor.denominator));
}
inline int comparePositions(Position left, Position right) noexcept {
    const auto a = floorPosition(left), b = floorPosition(right);
    if (a.frame != b.frame) return a.frame < b.frame ? -1 : 1;
    return compare(multiply(a.remainder, b.denominator), multiply(b.remainder, a.denominator));
}
inline int compareBoundary(Position position, RationalBoundary boundary) noexcept {
    const auto floor = floorPosition(position);
    if (floor.frame < 0) return -1;
    const auto whole = static_cast<std::uint64_t>(floor.frame);
    if (whole != boundary.whole) return whole < boundary.whole ? -1 : 1;
    return compare(multiply(floor.remainder, boundary.denominator),
                   multiply(boundary.numerator, floor.denominator));
}
inline RationalBoundary rationalBoundary(Position position) noexcept {
    const auto floor = floorPosition(position);
    if (floor.frame < 0 || static_cast<std::uint64_t>(floor.frame) > maximumFrame ||
        zero(wide(floor.denominator))) return {};
    return {static_cast<std::uint64_t>(floor.frame), floor.remainder,
            floor.denominator, true};
}
inline RationalBoundary rationalBoundaryForPreparation(Position position) noexcept {
    const auto floor = floorPosition(position);
    if (floor.frame < 0 || static_cast<std::uint64_t>(floor.frame) > maximumFrame ||
        zero(wide(floor.denominator))) return {};
    const auto fraction = reduceForPreparation(wide(floor.remainder),
                                               wide(floor.denominator));
    if (!fraction.valid) return {};
    return {static_cast<std::uint64_t>(floor.frame), fraction.numerator,
            fraction.denominator, true};
}
inline Position boundaryPosition(Boundary boundary) noexcept {
    return {static_cast<std::int64_t>(boundary.whole),
            {boundary.fraction, low128(powerOfTwo(boundary.denominatorPower)), false}};
}
inline Position boundaryPosition(RationalBoundary boundary) noexcept {
    return {static_cast<std::int64_t>(boundary.whole),
            {boundary.numerator, boundary.denominator, false}};
}
inline double phaseForPresentation(Phase phase) noexcept {
    const auto value = approximate(wide(phase.magnitude)) / approximate(wide(phase.denominator));
    return phase.negative ? -value : value;
}

struct ClockFormat {
    UInt128 denominator{1, 0};
    std::uint64_t stepWhole{};
    UInt128 stepRemainder{};
    bool valid{};
};
inline ClockFormat clockForPreparation(double projectRate, double deviceRate) noexcept {
    const auto ratio = rateRatioForPreparation(projectRate, deviceRate);
    if (!ratio.valid) return {};
    const auto b = wide(ratio.denominator);
    const auto exponent = trailingZeros(b);
    const auto odd = shiftRight(b, exponent);
    UInt256 denominator;
    if (shiftLeft(odd, std::max(72U, exponent), denominator) || bitWidth(denominator) > 125) return {};
    // Split BEFORE scaling: neither a wide increment nor its product is stored.
    const auto divided = divmod(wide(ratio.numerator), b);
    if (!fits64(divided.quotient) || divided.quotient.words[0] > maximumFrame) return {};
    const auto scale = divmod(denominator, b);
    if (!fits128(scale.quotient) || !zero(scale.remainder)) return {};
    const auto remainder = multiply(low128(divided.remainder), low128(scale.quotient));
    if (!fits128(remainder)) return {};
    return {low128(denominator), divided.quotient.words[0], low128(remainder), true};
}
inline ClockFormat extendClockForPreparation(ClockFormat format,
    RationalBoundary first, RationalBoundary second) noexcept {
    if (!format.valid || !first.valid || !second.valid) return {};
    auto denominator = wide(format.denominator);
    for (const auto boundaryDenominator : {first.denominator, second.denominator}) {
        const auto divisor = gcdForPreparation(denominator, wide(boundaryDenominator));
        const auto reduced = divmod(denominator, divisor);
        if (!reduced.valid || !zero(reduced.remainder)) return {};
        denominator = multiply(low128(reduced.quotient), boundaryDenominator);
        if (!fits128(denominator)) return {};
    }
    const auto scale = divmod(denominator, wide(format.denominator));
    if (!scale.valid || !zero(scale.remainder) || !fits128(scale.quotient)) return {};
    const auto remainder = multiply(format.stepRemainder, low128(scale.quotient));
    if (!fits128(remainder)) return {};
    format.denominator = low128(denominator);
    format.stepRemainder = low128(remainder);
    return format;
}

// Prepared affine mapping anchor + integerDelta * step. Preparation may use
// GCD/divmod; at() is fixed-width and suitable for bounded RT lookup.
struct LinearMapping {
    std::uint64_t anchorWhole{};
    Ratio128 step;
    UInt128 denominator{1, 0};
    UInt128 stepScale{1, 0};
    UInt128 anchorFraction{};
    bool valid{};
    [[nodiscard]] Position at(std::uint64_t delta) const noexcept {
        const auto advance = divmod(multiply({delta, 0}, step.numerator),
                                    wide(step.denominator));
        if (!advance.valid || !fits64(advance.quotient)) return {-1, {}};
        auto remainder = multiply(low128(advance.remainder), stepScale);
        add(remainder, wide(anchorFraction), remainder);
        const bool carry = compare(remainder, wide(denominator)) >= 0;
        if (carry) subtract(remainder, wide(denominator), remainder);
        const auto increment = advance.quotient.words[0] + (carry ? 1 : 0);
        if (increment < advance.quotient.words[0] ||
            anchorWhole > maximumFrame || increment > maximumFrame - anchorWhole)
            return {-1, {}};
        const auto whole = increment + anchorWhole;
        return {static_cast<std::int64_t>(whole),
                {low128(remainder), denominator, false}};
    }
};
inline LinearMapping linearMappingForPreparation(Position anchor,
    Ratio128 step) noexcept {
    LinearMapping result;
    if (!step.valid || zero(wide(step.denominator))) return result;
    const auto floor = floorPosition(anchor);
    if (floor.frame < 0 || static_cast<std::uint64_t>(floor.frame) > maximumFrame)
        return result;
    const auto anchorRatio = reduceForPreparation(wide(floor.remainder),
                                                   wide(floor.denominator));
    if (!anchorRatio.valid) return result;
    const auto divisor = gcdForPreparation(wide(anchorRatio.denominator),
                                           wide(step.denominator));
    const auto anchorReduced = divmod(wide(anchorRatio.denominator), divisor);
    if (!anchorReduced.valid || !zero(anchorReduced.remainder) ||
        !fits128(anchorReduced.quotient)) return result;
    const auto common = multiply(low128(anchorReduced.quotient), step.denominator);
    if (!fits128(common)) return result;
    const auto stepScale = divmod(common, wide(step.denominator));
    const auto anchorScale = divmod(common, wide(anchorRatio.denominator));
    if (!stepScale.valid || !anchorScale.valid ||
        !zero(stepScale.remainder) || !zero(anchorScale.remainder) ||
        !fits128(stepScale.quotient) || !fits128(anchorScale.quotient)) return result;
    const auto anchorFraction = multiply(anchorRatio.numerator,
                                         low128(anchorScale.quotient));
    if (!fits128(anchorFraction)) return result;
    result.anchorWhole = static_cast<std::uint64_t>(floor.frame);
    result.step = step;
    result.denominator = low128(common);
    result.stepScale = low128(stepScale.quotient);
    result.anchorFraction = low128(anchorFraction);
    result.valid = true;
    return result;
}

// Factors, not expanded absolute-coordinate coefficients. All persistent
// numeric components fit 128 bits. Wide values below are stack temporaries.
struct SourceMapping {
    Ratio128 rate;
    Boundary length, offset;
    bool valid{};
};
inline bool sourceMappingSupportsPhaseDenominator(const SourceMapping& mapping,
    UInt128 phaseDenominator) noexcept {
    if (!mapping.rate.valid || zero(wide(phaseDenominator))) return false;
    const auto denominator = multiply(phaseDenominator, mapping.rate.denominator);
    const auto sourcePower = trailingZeros(denominator);
    const auto odd = shiftRight(denominator, sourcePower);
    if (!fits128(odd)) return false;
    const auto commonPower = std::max(sourcePower, mapping.offset.denominatorPower);
    return bitWidth(odd) + commonPower <= 254 &&
        bitWidth(denominator) <= 254 &&
        bitWidth(wide(phaseDenominator)) +
            bitWidth(wide(mapping.rate.numerator)) + 1 <= 255;
}
inline SourceMapping sourceForPreparation(double sourceRate, double projectRate,
    double length, double offset, const ClockFormat& clock) noexcept {
    SourceMapping result;
    if (!clock.valid) return result;
    result.rate = rateRatioForPreparation(sourceRate, projectRate);
    result.length = boundaryForPreparation(length, 84);
    result.offset = boundaryForPreparation(offset, 116);
    if (!result.rate.valid || !result.length.valid || !result.offset.valid ||
        (!result.length.whole && zero(wide(result.length.fraction)))) return result;
    if (!sourceMappingSupportsPhaseDenominator(result, clock.denominator)) return result;
    result.valid = true;
    return result;
}
struct SourceIndex {
    std::uint64_t index{};
    UInt256 remainder{}, denominator{}; // ephemeral; never part of project/RT state
    bool inside{};
    double interpolationWeight() const noexcept {
        return approximate(remainder) / approximate(denominator);
    }
};
inline SourceIndex sourceIndex(const SourceMapping& mapping, Position local,
    std::uint64_t frameCount, Backend backend = defaultBackend) noexcept {
    if (!mapping.valid || frameCount == 0 || frameCount > maximumFrame ||
        compareBoundary(local, Boundary{0, {}, 0, true}) < 0 ||
        compareBoundary(local, mapping.length) >= 0) return {};
    const auto floor = floorPosition(local);
    const auto integerProduct = multiply({static_cast<std::uint64_t>(floor.frame), 0},
                                         mapping.rate.numerator, backend);
    const auto integer = divmod(integerProduct, wide(mapping.rate.denominator), backend);
    auto numerator = multiply(low128(integer.remainder), floor.denominator, backend);
    add(numerator, multiply(floor.remainder, mapping.rate.numerator, backend), numerator);
    const auto sourceDenominator = multiply(floor.denominator,
                                             mapping.rate.denominator, backend);
    const auto sourceDenominatorPower = trailingZeros(sourceDenominator);
    const auto oddWide = shiftRight(sourceDenominator, sourceDenominatorPower);
    if (!fits128(oddWide)) return {};
    const auto denominatorOdd = low128(oddWide);
    const auto phase = divmod(numerator, sourceDenominator, backend);
    const auto commonDenominatorPower = std::max(sourceDenominatorPower,
                                                  mapping.offset.denominatorPower);
    UInt256 commonDenominator, phaseScaled, offsetScaled;
    shiftLeft(wide(denominatorOdd), commonDenominatorPower, commonDenominator);
    shiftLeft(phase.remainder, commonDenominatorPower - sourceDenominatorPower, phaseScaled);
    shiftLeft(multiply(mapping.offset.fraction, denominatorOdd, backend),
              commonDenominatorPower - mapping.offset.denominatorPower, offsetScaled);
    UInt256 sum;
    add(phaseScaled, offsetScaled, sum);
    // Both terms are proper fractions. Carry is exactly 0 or 1; no third divide.
    const bool carry = compare(sum, commonDenominator) >= 0;
    if (carry) subtract(sum, commonDenominator, sum);
    UInt256 index;
    add(integer.quotient, phase.quotient, index);
    add(index, wide(mapping.offset.whole), index);
    if (carry) add(index, wide(1), index);
    if (compare(index, wide(frameCount)) >= 0) return {};
    return {index.words[0], sum, commonDenominator, true};
}
} // namespace vitadaw::audio::exact
