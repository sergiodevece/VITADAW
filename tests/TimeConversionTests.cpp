#include "vitadaw/timeline/Time.h"

#include <cmath>
#include <cstdlib>
#include <iostream>
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
    using namespace vitadaw::timeline;
    constexpr SampleRate sourceRate{44100.0};
    constexpr SampleRate projectRate{48000.0};
    check(sourceFramesToSeconds({44100}, sourceRate) == Seconds{1.0},
          "44100 source frames should equal one second");
    check(sourceFramesToProjectFrames({44100}, sourceRate, projectRate) ==
              ProjectFrameCount{48000},
          "44100 source frames should map to 48000 project frames");
    check(secondsToProjectFrames({2.5}, projectRate) == ProjectFrameCount{120000},
          "seconds should map to project frames");
    check(projectFramesToSeconds({120000}, projectRate) == Seconds{2.5},
          "project frames should map back to seconds");
    check(sourceFramesForDeviceFrames({48000}, sourceRate, projectRate) ==
              SourceFrameDuration{44100.0},
          "48000 device frames should advance 44100 source frames");

    constexpr std::uint64_t sourceFrames = 44100ULL * 60ULL * 60ULL * 6ULL;
    constexpr std::int64_t projectFrames = 48000LL * 60LL * 60LL * 6LL;
    const auto longDuration = sourceFramesToProjectFrames(
        {sourceFrames}, sourceRate, projectRate);
    check(longDuration.value == projectFrames,
          "six-hour conversion should not accumulate frame drift");
    check(std::abs(projectFramesToSeconds(longDuration, projectRate).value - 21600.0) <
              1.0e-12,
          "six-hour duration should round-trip without evident drift");
    std::cout << "All time-conversion tests passed\n";
    return EXIT_SUCCESS;
}
