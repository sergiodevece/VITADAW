#pragma once

// Fixed storage for temporal arithmetic only. No allocation, signed overflow,
// variable-width storage, or dependency on compiler extended integer types.
#include <array>
#include <bit>
#include <cstdint>
#include <limits>

namespace vitadaw::audio::exact {
struct UInt128 {
    std::uint64_t lo{}, hi{};
    bool operator==(const UInt128&) const = default;
};
struct UInt256 {
    std::array<std::uint64_t, 4> words{}; // least significant first
    bool operator==(const UInt256&) const = default;
};
enum class Backend { portable, accelerated };
#if defined(__SIZEOF_INT128__) && !defined(VITADAW_FORCE_PORTABLE_TEMPORAL)
inline constexpr Backend defaultBackend = Backend::accelerated;
#else
inline constexpr Backend defaultBackend = Backend::portable;
#endif

constexpr UInt256 wide(UInt128 a) noexcept { return {{a.lo, a.hi, 0, 0}}; }
constexpr UInt256 wide(std::uint64_t a) noexcept { return {{a, 0, 0, 0}}; }
constexpr bool zero(UInt256 a) noexcept {
    return (a.words[0] | a.words[1] | a.words[2] | a.words[3]) == 0;
}
constexpr bool fits128(UInt256 a) noexcept { return (a.words[2] | a.words[3]) == 0; }
constexpr bool fits64(UInt256 a) noexcept { return (a.words[1] | a.words[2] | a.words[3]) == 0; }
constexpr UInt128 low128(UInt256 a) noexcept { return {a.words[0], a.words[1]}; }
constexpr int compare(UInt256 a, UInt256 b) noexcept {
    for (unsigned i = 4; i-- != 0;) {
        if (a.words[i] < b.words[i]) return -1;
        if (a.words[i] > b.words[i]) return 1;
    }
    return 0;
}
constexpr unsigned bitWidth(UInt256 a) noexcept {
    for (unsigned i = 4; i-- != 0;)
        if (a.words[i]) return i * 64 + 64 - std::countl_zero(a.words[i]);
    return 0;
}
// Return carry/borrow explicitly; callers never infer it from signed overflow.
constexpr bool add(UInt256 a, UInt256 b, UInt256& out) noexcept {
    std::uint64_t carry = 0;
    for (unsigned i = 0; i < 4; ++i) {
        const auto first = a.words[i] + b.words[i];
        const auto second = first + carry;
        carry = (first < a.words[i]) | (second < first);
        out.words[i] = second;
    }
    return carry != 0;
}
constexpr bool subtract(UInt256 a, UInt256 b, UInt256& out) noexcept {
    std::uint64_t borrow = 0;
    for (unsigned i = 0; i < 4; ++i) {
        const auto first = a.words[i] - b.words[i];
        const auto second = first - borrow;
        borrow = (a.words[i] < b.words[i]) | (first < borrow);
        out.words[i] = second;
    }
    return borrow != 0;
}
constexpr bool shiftLeft(UInt256 a, unsigned count, UInt256& out) noexcept {
    out = {};
    if (count >= 256) return !zero(a);
    const bool overflow = bitWidth(a) > 256 - count;
    const auto whole = count / 64, bits = count % 64;
    for (unsigned i = whole; i < 4; ++i) {
        out.words[i] = a.words[i - whole] << bits;
        if (bits && i > whole) out.words[i] |= a.words[i - whole - 1] >> (64 - bits);
    }
    return overflow;
}
constexpr UInt256 shiftRight(UInt256 a, unsigned count) noexcept {
    UInt256 out;
    if (count >= 256) return out;
    const auto whole = count / 64, bits = count % 64;
    for (unsigned i = 0; i + whole < 4; ++i) {
        out.words[i] = a.words[i + whole] >> bits;
        if (bits && i + whole + 1 < 4)
            out.words[i] |= a.words[i + whole + 1] << (64 - bits);
    }
    return out;
}
inline UInt128 product64(std::uint64_t a, std::uint64_t b, Backend backend) noexcept {
#if defined(__SIZEOF_INT128__) && !defined(VITADAW_FORCE_PORTABLE_TEMPORAL)
    if (backend == Backend::accelerated) {
        __extension__ using Native = unsigned __int128;
        const Native product = static_cast<Native>(a) * b;
        return {static_cast<std::uint64_t>(product), static_cast<std::uint64_t>(product >> 64)};
    }
#else
    static_cast<void>(backend);
#endif
    constexpr auto mask = std::uint64_t{0xffffffff};
    const auto a0 = a & mask, a1 = a >> 32, b0 = b & mask, b1 = b >> 32;
    const auto p0 = a0 * b0;
    const auto p1 = a1 * b0 + (p0 >> 32);
    const auto p2 = a0 * b1 + (p1 & mask);
    return {(p2 << 32) | (p0 & mask), a1 * b1 + (p1 >> 32) + (p2 >> 32)};
}
inline UInt256 multiply(UInt128 a, UInt128 b, Backend backend = defaultBackend) noexcept {
    UInt256 result;
    const std::uint64_t aw[2]{a.lo, a.hi}, bw[2]{b.lo, b.hi};
    for (unsigned i = 0; i < 2; ++i) {
        for (unsigned j = 0; j < 2; ++j) {
            const auto p = product64(aw[i], bw[j], backend);
            UInt256 part;
            part.words[i + j] = p.lo;
            part.words[i + j + 1] = p.hi;
            add(result, part, result); // a 128x128 product cannot overflow 256 bits
        }
    }
    return result;
}
struct Division { UInt256 quotient, remainder; bool valid{}; };
inline Division divmod(UInt256 numerator, UInt256 denominator,
                       Backend backend = defaultBackend) noexcept {
    Division out;
    if (zero(denominator)) return out;
    out.valid = true;
    if (fits64(numerator) && fits64(denominator)) {
        out.quotient = wide(numerator.words[0] / denominator.words[0]);
        out.remainder = wide(numerator.words[0] % denominator.words[0]);
        return out;
    }
#if defined(__SIZEOF_INT128__) && !defined(VITADAW_FORCE_PORTABLE_TEMPORAL)
    if (backend == Backend::accelerated && fits128(numerator) && fits128(denominator)) {
        __extension__ using Native = unsigned __int128;
        const auto n = (static_cast<Native>(numerator.words[1]) << 64) | numerator.words[0];
        const auto d = (static_cast<Native>(denominator.words[1]) << 64) | denominator.words[0];
        const auto q = n / d, r = n % d;
        out.quotient = wide(UInt128{static_cast<std::uint64_t>(q), static_cast<std::uint64_t>(q >> 64)});
        out.remainder = wide(UInt128{static_cast<std::uint64_t>(r), static_cast<std::uint64_t>(r >> 64)});
        return out;
    }
#else
    static_cast<void>(backend);
#endif
    // Exactly 256 iterations. The shifted remainder may carry a 257th bit;
    // retain that carry rather than silently losing it before comparison.
    for (unsigned bit = 256; bit-- != 0;) {
        UInt256 next;
        const bool carry = shiftLeft(out.remainder, 1, next);
        next.words[0] |= (numerator.words[bit / 64] >> (bit % 64)) & 1;
        if (carry || compare(next, denominator) >= 0) {
            subtract(next, denominator, next);
            out.quotient.words[bit / 64] |= std::uint64_t{1} << (bit % 64);
        }
        out.remainder = next;
    }
    return out;
}

// Only use AFTER integer index/boundary decisions, for interpolation/display.
inline double approximate(UInt256 value) noexcept {
    double result = 0;
    for (unsigned i = 4; i-- != 0;) result = result * 0x1p64 + static_cast<double>(value.words[i]);
    return result;
}

// Preparation-only functions below. Never call from processBlock/render.
struct Ratio128 { UInt128 numerator{}, denominator{1, 0}; bool valid{}; };
inline UInt256 gcdForPreparation(UInt256 a, UInt256 b) noexcept {
    // Euclid is bounded for fixed width, but deliberately excluded from RT.
    for (unsigned iteration = 0; iteration < 512 && !zero(b); ++iteration) {
        const auto remainder = divmod(a, b).remainder;
        a = b;
        b = remainder;
    }
    return a;
}
inline Ratio128 reduceForPreparation(UInt256 n, UInt256 d) noexcept {
    if (zero(d)) return {};
    const auto divisor = gcdForPreparation(n, d);
    const auto nr = divmod(n, divisor).quotient;
    const auto dr = divmod(d, divisor).quotient;
    if (!fits128(nr) || !fits128(dr)) return {};
    return {low128(nr), low128(dr), true};
}
inline Ratio128 decodeForPreparation(double value) noexcept {
    static_assert(sizeof(double) == sizeof(std::uint64_t) &&
                  std::numeric_limits<double>::is_iec559 &&
                  std::numeric_limits<double>::digits == 53);
    const auto bits = std::bit_cast<std::uint64_t>(value);
    const auto exponent = static_cast<unsigned>((bits >> 52) & 2047);
    auto significand = bits & ((std::uint64_t{1} << 52) - 1);
    if (exponent == 2047 || ((bits >> 63) && (bits << 1))) return {};
    if (exponent) significand |= std::uint64_t{1} << 52;
    if (!significand) return {{}, {1, 0}, true};
    int shift = exponent ? static_cast<int>(exponent) - 1023 - 52 : -1074;
    const auto zeros = std::countr_zero(significand);
    significand >>= zeros;
    shift += zeros;
    UInt256 n = wide(significand), d = wide(1), result;
    if (shift >= 0) {
        if (shiftLeft(n, static_cast<unsigned>(shift), result) || !fits128(result)) return {};
        n = result;
    } else {
        if (shiftLeft(d, static_cast<unsigned>(-shift), result) || !fits128(result)) return {};
        d = result;
    }
    return {low128(n), low128(d), true};
}
inline Ratio128 rateRatioForPreparation(double numerator, double denominator) noexcept {
    const auto n = decodeForPreparation(numerator), d = decodeForPreparation(denominator);
    if (!n.valid || !d.valid || zero(wide(n.numerator)) || zero(wide(d.numerator))) return {};
    return reduceForPreparation(multiply(n.numerator, d.denominator),
                                multiply(n.denominator, d.numerator));
}
} // namespace vitadaw::audio::exact
