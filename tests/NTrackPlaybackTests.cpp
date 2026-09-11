#include "vitadaw/audio/RealtimeAudioEngine.h"
#include "vitadaw/audio/StereoAccumulator.h"

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
    vitadaw::tracks::TrackId id,
    const std::vector<float>& samples,
    vitadaw::timeline::SampleRate sourceRate,
    vitadaw::timeline::SampleRate projectRate,
    std::int64_t clipStart = 0) {
    return {id,
            {{samples.data(), nullptr}},
            1,
            {static_cast<std::uint64_t>(samples.size())},
            sourceRate,
            {clipStart},
            vitadaw::timeline::sourceFramesToProjectDuration(
                {static_cast<std::uint64_t>(samples.size())}, sourceRate,
                projectRate),
            {0}};
}

vitadaw::audio::StereoSample renderProjectSample(
    std::span<const vitadaw::audio::PreparedTrackView> tracks,
    vitadaw::timeline::PreciseProjectFramePosition position,
    vitadaw::timeline::SampleRate projectRate) {
    vitadaw::audio::StereoSample result;
    for (const auto& track : tracks) {
        vitadaw::audio::accumulateTrackContribution(
            result, vitadaw::audio::renderTrackAtProjectPosition(
                        track, position, projectRate));
    }
    return result;
}

float processFirstSample(
    std::span<const vitadaw::audio::PreparedTrackView> tracks,
    vitadaw::timeline::SampleRate projectRate,
    vitadaw::timeline::ProjectFrameCount duration) {
    vitadaw::audio::RealtimeAudioEngine engine;
    engine.configure({projectRate, duration, tracks});
    engine.deviceInitialising();
    std::array<float*, 0> noChannels{};
    engine.processBlock({noChannels.data(), 0, 0}, projectRate);
    check(engine.tryRequestPlay().accepted,
          "prepared N-track project should accept Play");
    float left{}, right{};
    std::array<float*, 2> channels{&left, &right};
    engine.processBlock({channels.data(), channels.size(), 1}, projectRate);
    return left;
}

} // namespace

int main() {
    using namespace vitadaw;
    constexpr timeline::SampleRate projectRate{48000.0};

    const std::vector<audio::PreparedTrackView> zeroTracks;
    check(renderProjectSample(zeroTracks, {0.0}, projectRate) ==
              audio::StereoSample{},
          "zero prepared tracks should accumulate silence");

    const std::vector<float> one(8, 1.0F);
    const std::vector<float> half(8, 0.5F);
    const std::vector<audio::PreparedTrackView> oneTrack{
        monoView({1}, one, projectRate, projectRate)};
    check(std::abs(renderProjectSample(oneTrack, {3.0}, projectRate).left -
                   0.125F) < 1.0e-6F &&
              std::abs(processFirstSample(oneTrack, projectRate, {8}) -
                       0.125F) < 1.0e-6F,
          "one track should use the fixed provisional gain");

    const std::vector<audio::PreparedTrackView> twoTracks{
        monoView({1}, one, projectRate, projectRate),
        monoView({2}, half, projectRate, projectRate)};
    check(std::abs(renderProjectSample(twoTracks, {3.0}, projectRate).left -
                   0.1875F) < 1.0e-6F &&
              std::abs(processFirstSample(twoTracks, projectRate, {8}) -
                       0.1875F) < 1.0e-6F,
          "two tracks should accumulate independent contributions");

    std::vector<std::vector<float>> fourSignals(4, std::vector<float>(8, 0.5F));
    std::vector<audio::PreparedTrackView> fourTracks;
    for (std::size_t index = 0; index < fourSignals.size(); ++index) {
        fourTracks.push_back(monoView({index + 1}, fourSignals[index],
                                      projectRate, projectRate));
    }
    check(std::abs(renderProjectSample(fourTracks, {2.0}, projectRate).left -
                   0.25F) < 1.0e-6F &&
              std::abs(processFirstSample(fourTracks, projectRate, {8}) -
                       0.25F) < 1.0e-6F,
          "four tracks should accumulate without a fixed topology");

    std::vector<std::vector<float>> eightSignals(8, std::vector<float>(8, 0.25F));
    std::vector<audio::PreparedTrackView> tracksWithEmptyGaps;
    for (std::size_t index = 0; index < eightSignals.size(); ++index) {
        tracksWithEmptyGaps.push_back(monoView(
            {index * 2 + 1}, eightSignals[index], projectRate, projectRate));
        tracksWithEmptyGaps.push_back({});
    }
    check(std::abs(renderProjectSample(tracksWithEmptyGaps, {4.0}, projectRate).left -
                   0.25F) < 1.0e-6F &&
              std::abs(processFirstSample(tracksWithEmptyGaps, projectRate, {8}) -
                       0.25F) < 1.0e-6F,
          "eight active tracks should ignore empty entries between them");

    std::vector<float> ramp441(44101);
    std::vector<float> ramp480(48001);
    for (std::size_t frame = 0; frame < ramp441.size(); ++frame) {
        ramp441[frame] = static_cast<float>(frame);
    }
    for (std::size_t frame = 0; frame < ramp480.size(); ++frame) {
        ramp480[frame] = static_cast<float>(frame);
    }
    const std::vector<audio::PreparedTrackView> mixedRates{
        monoView({41}, ramp441, timeline::SampleRate{44100.0}, projectRate),
        monoView({48}, ramp480, projectRate, projectRate)};
    const auto halfSecond = renderProjectSample(
        mixedRates, {24000.0}, projectRate);
    check(std::abs(halfSecond.left - 5756.25F) < 1.0e-3F,
          "mixed sample rates should derive the same project instant");

    const std::vector<float> shortSignal(2, 1.0F);
    const std::vector<float> longSignal(4, 0.4F);
    const std::vector<audio::PreparedTrackView> unequalLengths{
        monoView({1}, shortSignal, timeline::SampleRate{4.0},
                 timeline::SampleRate{4.0}),
        monoView({2}, longSignal, timeline::SampleRate{4.0},
                 timeline::SampleRate{4.0})};
    check(std::abs(renderProjectSample(
                       unequalLengths, {2.0}, timeline::SampleRate{4.0}).left -
                   0.05F) < 1.0e-6F,
          "a finished short track should stop contributing");

    const auto shifted = monoView({99}, one, projectRate, projectRate, 10);
    check(renderProjectSample(std::span{&shifted, 1}, {9.0}, projectRate).left ==
              0.0F &&
              renderProjectSample(std::span{&shifted, 1}, {10.0}, projectRate).left ==
                  0.125F,
          "prepared tracks should support a future non-zero clip start");

    std::vector<std::vector<float>> scaleSignals(
        32, std::vector<float>(16, 1.0F / 32.0F));
    std::vector<audio::PreparedTrackView> scaleTracks;
    scaleTracks.reserve(scaleSignals.size());
    for (std::size_t index = 0; index < scaleSignals.size(); ++index) {
        scaleTracks.push_back(monoView({index + 1}, scaleSignals[index],
                                       projectRate, projectRate));
    }
    audio::RealtimeAudioEngine engine;
    engine.configure({projectRate, {16}, scaleTracks});
    engine.deviceInitialising();
    std::array<float*, 0> noChannels{};
    engine.processBlock({noChannels.data(), 0, 0}, projectRate);
    check(engine.tryRequestPlay().accepted,
          "32-track prepared topology should accept playback");
    std::vector<float> left(16), right(16);
    std::array<float*, 2> channels{left.data(), right.data()};
    engine.processBlock({channels.data(), channels.size(), left.size()}, projectRate);
    check(std::all_of(left.begin(), left.end(), [](float sample) {
              return std::abs(sample - 0.125F) < 1.0e-6F;
          }) &&
              !engine.transportSnapshot().playing &&
              engine.transportSnapshot().position.value == 16,
          "the production processBlock should render 32 tracks to global end");

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
    check(!clock.isPlaying() &&
              clock.publicPosition().value == sixHoursProjectFrames,
          "long N-track playback should retain one exact master clock");

    std::cout << "All N-track playback tests passed\n";
    return EXIT_SUCCESS;
}
