#include "vitadaw/audio/RealtimeProjectClock.h"
#include "vitadaw/audio/TwoTrackMixer.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <string_view>
#include <vector>

namespace {

void check(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

vitadaw::audio::PreparedTrackView monoView(
    const std::vector<float>& samples,
    vitadaw::timeline::SampleRate sampleRate) {
    return {{{samples.data(), nullptr}},
            1,
            {static_cast<std::uint64_t>(samples.size())},
            sampleRate};
}

} // namespace

int main() {
    using namespace vitadaw;
    constexpr timeline::SampleRate projectRate{48000.0};

    const std::vector<float> one(8, 1.0F);
    const std::vector<float> half(8, 0.5F);
    const std::array<audio::PreparedTrackView, 2> knownSignals{
        monoView(one, projectRate), monoView(half, projectRate)};
    const auto mixed = audio::mixTwoTracksAtProjectPosition(
        knownSignals, {3.0}, projectRate);
    check(std::abs(mixed.left - 0.75F) < 1.0e-6F &&
              std::abs(mixed.right - 0.75F) < 1.0e-6F,
          "two known signals should mix with fixed 0.5 gain per track");

    const std::vector<float> shortSignal(2, 1.0F);
    const std::vector<float> longSignal(4, 0.4F);
    const std::array<audio::PreparedTrackView, 2> unequalLengths{
        monoView(shortSignal, projectRate), monoView(longSignal, projectRate)};
    const auto afterShortEnd = audio::mixTwoTracksAtProjectPosition(
        unequalLengths, {2.5}, projectRate);
    check(std::abs(afterShortEnd.left - 0.2F) < 1.0e-6F,
          "finished short track should be silent while long track continues");

    const std::vector<float> ramp441(44101, 0.0F);
    const std::vector<float> ramp480(48001, 0.0F);
    auto mutableRamp441 = ramp441;
    auto mutableRamp480 = ramp480;
    for (std::size_t frame = 0; frame < mutableRamp441.size(); ++frame) {
        mutableRamp441[frame] = static_cast<float>(frame);
    }
    for (std::size_t frame = 0; frame < mutableRamp480.size(); ++frame) {
        mutableRamp480[frame] = static_cast<float>(frame);
    }
    const std::array<audio::PreparedTrackView, 2> differentRates{
        monoView(mutableRamp441, timeline::SampleRate{44100.0}),
        monoView(mutableRamp480, projectRate)};
    const auto halfSecond = audio::mixTwoTracksAtProjectPosition(
        differentRates, {24000.0}, projectRate);
    check(std::abs(halfSecond.left - 23025.0F) < 1.0e-3F,
          "44.1 and 48 kHz tracks should derive the same half-second instant");

    audio::RealtimeProjectClock clock;
    constexpr auto sixHoursProjectFrames = 48000LL * 60LL * 60LL * 6LL;
    constexpr auto sixHoursDeviceFrames = 44100ULL * 60ULL * 60ULL * 6ULL;
    clock.prepare({sixHoursProjectFrames});
    check(clock.play(), "prepared master clock should play");
    constexpr std::uint64_t blockSize = 512;
    std::uint64_t remaining = sixHoursDeviceFrames;
    while (remaining > 0) {
        const auto block = std::min(blockSize, remaining);
        clock.advance(timeline::projectFramesForDeviceFrames(
            {block}, projectRate, timeline::SampleRate{44100.0}));
        remaining -= block;
    }
    check(!clock.isPlaying() && clock.publicPosition().value == sixHoursProjectFrames,
          "six-hour render should end exactly on the shared project duration");

    const auto source441Position = timeline::projectPositionToSourcePosition(
        clock.position(), projectRate, timeline::SampleRate{44100.0});
    const auto source480Position = timeline::projectPositionToSourcePosition(
        clock.position(), projectRate, projectRate);
    const auto seconds441 = source441Position.value / 44100.0;
    const auto seconds480 = source480Position.value / 48000.0;
    check(std::abs(seconds441 - seconds480) < 1.0e-12,
          "tracks derived from one master clock should show no relative drift");

    check(clock.play() && clock.publicPosition().value == 0,
          "Play after natural end should restart master clock at zero");
    clock.advance({1024.0});
    clock.stopAndRewind();
    check(!clock.isPlaying() && clock.publicPosition().value == 0,
          "Stop should rewind the only clock used by both tracks");

    std::cout << "All two-track playback tests passed\n";
    return EXIT_SUCCESS;
}
