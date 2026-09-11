#include "vitadaw/timeline/Time.h"

#include <cmath>
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

    check(sourceFramesToProjectDuration({1}, SampleRate{96000.0},
                                        SampleRate{44100.0}) == ProjectFrameCount{1},
          "one source frame must never become a zero project duration");
    check(sourceFramesToProjectDuration({2}, SampleRate{96000.0},
                                        SampleRate{44100.0}) == ProjectFrameCount{1},
          "two 96 kHz frames should be covered at 44.1 kHz");
    check(sourceFramesToProjectDuration({3}, SampleRate{96000.0},
                                        SampleRate{44100.0}) == ProjectFrameCount{2},
          "exclusive duration should round a fractional project frame upward");
    check(sourceFramesToProjectDuration({1}, sourceRate, projectRate) ==
              ProjectFrameCount{2},
          "44.1 to 48 kHz duration should cover its fractional second frame");
    check(sourceFramesToProjectDuration({1}, projectRate, sourceRate) ==
              ProjectFrameCount{1},
          "48 to 44.1 kHz duration should retain a one-frame resource");
    check(!SampleRate{std::numeric_limits<double>::infinity()}.isValid() &&
              !SampleRate{std::numeric_limits<double>::quiet_NaN()}.isValid(),
          "sample rates must be finite as well as positive");

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
