#include "vitadaw/audio/AudioPreparationPolicy.h"

#include <array>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <string_view>

namespace {
void check(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}
} // namespace

int main() {
    using namespace vitadaw;
    constexpr std::size_t budget = 1024;
    check(audio::validatePreparationShape(timeline::SampleRate{48000.0}, 2, 100,
                                          0, budget).isValid(),
          "valid stereo preparation should fit its budget");
    check(audio::validatePreparationShape(timeline::SampleRate{48000.0}, 3, 100,
                                          0, budget).error ==
              audio::PreparationValidationError::invalidChannelCount,
          "more than two channels should be rejected in this increment");
    check(audio::validatePreparationShape(timeline::SampleRate{48000.0}, 2, 100,
                                          300, budget).error ==
              audio::PreparationValidationError::memoryBudgetExceeded,
          "existing and new buffers should share the preparation budget");
    check(audio::validatePreparationShape(timeline::SampleRate{48000.0}, 2,
                                          std::numeric_limits<std::uint64_t>::max(),
                                          0, budget).error ==
              audio::PreparationValidationError::sizeOverflow,
          "decoded size multiplication must reject overflow");
    const std::array<float, 3> finite{0.0F, -1.0F, 1.0F};
    const std::array<float, 2> nonFinite{
        0.0F, std::numeric_limits<float>::quiet_NaN()};
    check(audio::containsOnlyFiniteSamples(finite) &&
              !audio::containsOnlyFiniteSamples(nonFinite),
          "non-finite decoded samples should be rejected before publication");
    std::cout << "All audio preparation tests passed\n";
    return EXIT_SUCCESS;
}
