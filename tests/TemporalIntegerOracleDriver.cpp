#include "vitadaw/audio/TemporalInteger.h"
#include "vitadaw/audio/ExactProjectPhase.h"
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <string>

using namespace vitadaw::audio::exact;
UInt256 readValue() {
    UInt256 value;
    for (unsigned i = 4; i-- != 0;) std::cin >> std::hex >> value.words[i];
    return value;
}
void print(UInt256 value) {
    for (unsigned i = 4; i-- != 0;)
        std::cout << std::hex << std::setfill('0') << std::setw(16) << value.words[i];
    std::cout << ' ';
}
int main() {
    // Failure atomicity and exact context conversion, outside RT.
    ProjectPhase checkpointClock;
    if (!checkpointClock.installPreparedFormat(clockForPreparation(48000, 96000))) return 3;
    checkpointClock.locate(17);
    checkpointClock.advance(std::nullopt);
    const auto original = checkpointClock.checkpoint();
    if (!checkpointClock.installPreparedFormat(clockForPreparation(48000, 44100))) return 4;
    checkpointClock.advance(std::nullopt);
    const auto beforeFailure = checkpointClock.position();
    if (checkpointClock.installPreparedFormat(clockForPreparation(48000, 96000))) return 5;
    if (checkpointClock.position().frame != beforeFailure.frame ||
        checkpointClock.position().phase.magnitude != beforeFailure.phase.magnitude ||
        checkpointClock.position().phase.denominator != beforeFailure.phase.denominator) return 6;
    if (!checkpointClock.restore(original)) return 7;
    auto malformed = original;
    malformed.position.phase.denominator = {};
    if (checkpointClock.restore(malformed)) return 8;
    if (clockForPreparation(1e-250, 48000).valid ||
        boundaryForPreparation(0x1p-200, 84).valid) return 9;
    std::string operation;
    unsigned backend;
    while (std::cin >> operation >> std::dec >> backend) {
        const auto selected = backend ? Backend::accelerated : Backend::portable;
        if (operation == "source") {
            std::string p, d, s, length, offset, phaseText;
            std::int64_t frame;
            std::uint64_t frameCount;
            std::cin >> p >> d >> s >> length >> offset >> frame >> phaseText >> frameCount;
            const auto number = [](const std::string& text) { return std::strtod(text.c_str(), nullptr); };
            const auto clock = clockForPreparation(number(p), number(d));
            const auto mapping = sourceForPreparation(number(s), number(p), number(length), number(offset), clock);
            const bool negative = phaseText.front() == '-';
            const auto fraction = decodeForPreparation(number(negative ? phaseText.substr(1) : phaseText));
            const auto scaled = divmod(multiply(fraction.numerator, clock.denominator), wide(fraction.denominator));
            const Position position{frame, {low128(scaled.quotient), clock.denominator, negative}};
            const auto result = sourceIndex(mapping, position, frameCount, selected);
            std::cout << std::hex << mapping.valid << ' ' << result.inside << ' ' << result.index << ' ';
            print(result.remainder); print(result.denominator); std::cout << '\n';
            continue;
        }
        if (operation == "clock") {
            std::string p, d, start, end, ls, le;
            std::uint64_t count;
            std::cin >> p >> d >> start >> count >> end >> ls >> le;
            const auto number = [](const std::string& text) { return std::strtod(text.c_str(), nullptr); };
            ProjectPhase clock;
            const bool valid = clock.installPreparedFormat(clockForPreparation(number(p), number(d)));
            const auto initial = boundaryForPreparation(number(start), 72);
            const auto boundary = boundaryForPreparation(number(end), 72);
            const auto loopEnd = rationalBoundaryForPreparation(number(le));
            std::optional<ProjectPhase::Loop> loop;
            if (number(le) != 0) loop = ProjectPhase::Loop{
                rationalBoundaryForPreparation(number(ls)), loopEnd};
            clock.locate(initial);
            std::uint64_t firstEnd = 0, wraps = 0;
            for (std::uint64_t i = 0; i < count; ++i) {
                if (clock.advance(loop)) ++wraps;
                if (!firstEnd && !clock.before(boundary)) firstEnd = i + 1;
            }
            const auto pos = clock.position();
            std::cout << std::hex << valid << ' ' << pos.frame << ' ' << pos.phase.negative << ' ';
            print(wide(pos.phase.magnitude)); print(wide(pos.phase.denominator));
            std::cout << firstEnd << ' ' << wraps << '\n';
            continue;
        }
        if (operation == "decode" || operation == "ratio") {
            std::string input, other;
            std::cin >> input;
            Ratio128 result;
            if (operation == "decode") result = decodeForPreparation(std::strtod(input.c_str(), nullptr));
            else {
                std::cin >> other;
                result = rateRatioForPreparation(std::strtod(input.c_str(), nullptr), std::strtod(other.c_str(), nullptr));
            }
            print(wide(result.numerator)); print(wide(result.denominator));
            std::cout << result.valid << '\n';
            continue;
        }
        const auto a = readValue(), b = readValue();
        UInt256 result;
        if (operation == "mul") {
            print(multiply(low128(a), low128(b), selected));
        } else if (operation == "div") {
            const auto division = divmod(a, b, selected);
            print(division.quotient); print(division.remainder);
            std::cout << division.valid;
        } else if (operation == "add") {
            const auto carry = add(a, b, result); print(result); std::cout << carry;
        } else if (operation == "sub") {
            const auto borrow = subtract(a, b, result); print(result); std::cout << borrow;
        } else if (operation == "left") {
            const auto carry = shiftLeft(a, static_cast<unsigned>(b.words[0]), result);
            print(result); std::cout << carry;
        } else if (operation == "right") {
            print(shiftRight(a, static_cast<unsigned>(b.words[0])));
        } else return 2;
        std::cout << '\n';
    }
}
