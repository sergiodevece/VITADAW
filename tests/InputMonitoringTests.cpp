#include "vitadaw/application/DawApplication.h"
#include "vitadaw/audio/RealtimeAudioEngine.h"
#include "vitadaw/commands/CommandDispatcher.h"

#include <array>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <new>
#include <span>
#include <string_view>
#include <thread>
#include <vector>

namespace {
using namespace vitadaw;

thread_local bool realtimeRegion{};
std::atomic<std::size_t> realtimeAllocations{};

void check(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

bool near(float actual, float expected, float tolerance = 1.0e-5F) {
    return std::abs(actual - expected) <= tolerance;
}

audio::PreparedTrackView monoTrack(tracks::TrackId id,
                                   const std::vector<float>& samples) {
    return {id, {{samples.data(), nullptr}}, 1,
            {static_cast<std::uint64_t>(samples.size())},
            timeline::SampleRate{48000.0}, {0},
            {static_cast<std::int64_t>(samples.size())}, {0}};
}

struct Harness {
    audio::RealtimeAudioEngine engine;
    const timeline::SampleRate rate{48000.0};

    explicit Harness(std::span<const audio::PreparedTrackView> tracks = {},
                     std::int64_t duration = 64, bool prepareCapture = false,
                     std::size_t captureCapacity = 512) {
        engine.configure({rate, {duration}, tracks});
        check(engine.prepareLegacyDeviceRate(rate), "prepare portable device rate");
        if (prepareCapture)
            check(engine.prepareRecordingCapture(captureCapacity),
                  "prepare recording capture");
        engine.deviceInitialising();
        process({}, 0, {}, 0, 0);
        check(engine.deviceState() == audio::DeviceProcessingState::operational,
              "portable callback makes device operational");
    }

    void process(const float* const* input, std::size_t inputChannels,
                 float* const* output, std::size_t outputChannels,
                 std::size_t frames) {
        realtimeRegion = true;
        engine.processBlock({input, inputChannels, frames},
                            {output, outputChannels, frames}, rate);
        realtimeRegion = false;
    }

    void settleMonitoring() {
        std::array<float, 240> input{};
        std::array<float, 240> left{};
        std::array<float, 240> right{};
        const std::array<const float*, 1> inputs{input.data()};
        const std::array<float*, 2> outputs{left.data(), right.data()};
        process(inputs.data(), inputs.size(), outputs.data(), outputs.size(),
                input.size());
    }

    void enableAt(audio::MonitorGainDb gain) {
        check(engine.trySetMonitorGain(gain), "valid monitoring gain publishes");
        check(engine.trySetInputMonitoringEnabled(true).accepted,
              "monitor enable schedules");
        settleMonitoring();
    }
};

void processMono(Harness& harness, float input, float& left, float& right) {
    const std::array<const float*, 1> inputs{&input};
    const std::array<float*, 2> outputs{&left, &right};
    harness.process(inputs.data(), inputs.size(), outputs.data(), outputs.size(), 1);
}

void stateAndGainTests() {
    Harness harness;
    const auto initial = harness.engine.transportSnapshot();
    check(!initial.monitoringEnabled, "monitoring starts disabled");
    check(initial.monitorGainDb == -12.0F, "monitoring default gain is -12 dB");
    check(harness.engine.trySetMonitorGain({-100.0F}), "-100 dB is valid");
    check(!harness.engine.trySetMonitorGain({-100.1F}), "gain below -100 dB rejects");
    check(!harness.engine.trySetMonitorGain({0.1F}), "positive monitoring gain rejects");
    check(!harness.engine.trySetMonitorGain(
              {std::numeric_limits<float>::quiet_NaN()}),
          "non-finite monitoring gain rejects");

    float disabledInput{1.0F};
    float disabledLeft{-1.0F};
    float disabledRight{-1.0F};
    processMono(harness, disabledInput, disabledLeft, disabledRight);
    check(disabledLeft == 0.0F && disabledRight == 0.0F,
          "disabled monitoring with stopped playback remains silent");

    check(harness.engine.trySetMonitorGain({0.0F}), "unity gain publishes");
    check(harness.engine.trySetInputMonitoringEnabled(true).accepted,
          "enable schedules at unity");
    float sample{1.0F};
    float left{};
    float right{};
    processMono(harness, sample, left, right);
    check(left > 0.0F && left < 1.0F && near(left, right),
          "enable starts a bounded non-discontinuous gain ramp");
    harness.settleMonitoring();
    processMono(harness, sample, left, right);
    check(near(left, 1.0F) && near(right, 1.0F),
          "ramp reaches the requested unity gain");

    Harness muted;
    muted.enableAt({-100.0F});
    processMono(muted, sample, left, right);
    check(left == 0.0F && right == 0.0F,
          "-100 dB monitoring gain produces silence");
}

void inputMeterPreGainTests() {
    Harness harness;
    check(harness.engine.trySetMonitorGain({-100.0F}), "mute monitoring gain for meter test");
    float leftInput{-0.75F};
    float rightInput{0.25F};
    float leftOutput{};
    float rightOutput{};
    const std::array<const float*, 2> inputs{&leftInput, &rightInput};
    const std::array<float*, 2> outputs{&leftOutput, &rightOutput};
    const auto before = realtimeAllocations.load(std::memory_order_acquire);
    harness.process(inputs.data(), inputs.size(), outputs.data(), outputs.size(), 1);
    const auto muted = harness.engine.meterSnapshot();
    check(muted.inputAvailable && near(muted.input.left, 0.75F) &&
              near(muted.input.right, 0.25F) &&
              realtimeAllocations.load(std::memory_order_acquire) == before,
          "input meter publishes raw pre-gain peaks without RT allocation");

    check(harness.engine.trySetMonitorGain({0.0F}) &&
              harness.engine.trySetInputMonitoringEnabled(true).accepted,
          "enable monitoring after raw meter baseline");
    harness.process(inputs.data(), inputs.size(), outputs.data(), outputs.size(), 1);
    const auto monitored = harness.engine.meterSnapshot();
    check(near(monitored.input.left, 0.75F) && near(monitored.input.right, 0.25F),
          "monitor gain and route state do not alter pre-gain input meter");
}

void mappingAndLayoutTests() {
    {
        Harness harness;
        harness.enableAt({0.0F});
        float source{0.25F};
        float left{};
        float right{};
        processMono(harness, source, left, right);
        check(near(left, 0.25F) && near(right, 0.25F),
              "mono input duplicates to stereo output");
    }
    {
        Harness harness;
        harness.enableAt({0.0F});
        float leftInput{0.25F};
        float rightInput{-0.5F};
        float left{};
        float right{};
        const std::array<const float*, 2> inputs{&leftInput, &rightInput};
        const std::array<float*, 2> outputs{&left, &right};
        harness.process(inputs.data(), inputs.size(), outputs.data(), outputs.size(), 1);
        check(near(left, leftInput) && near(right, rightInput),
              "stereo input maps directly to stereo output");
    }
    {
        Harness harness;
        harness.enableAt({0.0F});
        float leftInput{0.75F};
        float rightInput{-0.25F};
        float output{};
        const std::array<const float*, 2> inputs{&leftInput, &rightInput};
        const std::array<float*, 1> outputs{&output};
        harness.process(inputs.data(), inputs.size(), outputs.data(), outputs.size(), 1);
        check(near(output, 0.25F), "stereo folds to mono with 0.5 * (L + R)");
    }
    {
        Harness harness;
        harness.enableAt({0.0F});
        float inputLeft{0.25F};
        float inputRight{-0.5F};
        float extraInput{0.75F};
        float outputLeft{};
        float outputRight{};
        float extraOutput{99.0F};
        const std::array<const float*, 3> inputs{
            &inputLeft, &inputRight, &extraInput};
        const std::array<float*, 3> outputs{
            &outputLeft, &outputRight, &extraOutput};
        harness.process(inputs.data(), inputs.size(), outputs.data(), outputs.size(), 1);
        const auto snapshot = harness.engine.transportSnapshot();
        check(near(outputLeft, inputLeft) && near(outputRight, inputRight) &&
                  extraOutput == 0.0F && !snapshot.monitoringRouteSupported,
              "higher layouts use only the diagnosed primary pair safely");
    }
}

void playbackIndependenceTests() {
    const std::vector<float> source{1.0F, 1.0F, 1.0F};
    const std::array tracks{monoTrack({1}, source)};
    {
        Harness harness{tracks, 3};
        check(harness.engine.tryRequestPlay().accepted, "playback schedules");
        float input{0.25F};
        float left{};
        float right{};
        processMono(harness, input, left, right);
        check(near(left, 0.70710678F) && near(right, 0.70710678F),
              "disabled monitoring preserves prior playback output");
    }
    {
        Harness harness{tracks, 3};
        harness.enableAt({0.0F});
        check(harness.engine.tryRequestPlay().accepted, "playback with monitoring schedules");
        float input{0.25F};
        float left{};
        float right{};
        processMono(harness, input, left, right);
        check(near(left, 0.95710678F) && near(right, 0.95710678F),
              "playback and monitoring sum mathematically without limiter");

        check(harness.engine.tryRequestStop().accepted, "stop schedules independently");
        processMono(harness, input, left, right);
        const auto stopped = harness.engine.transportSnapshot();
        check(near(left, input) && near(right, input) && stopped.monitoringEnabled &&
                  stopped.playback == transport::PlaybackState::stopped,
              "stop does not disable monitoring");

        check(harness.engine.tryRequestSeek({1}).accepted, "seek remains available");
        processMono(harness, input, left, right);
        check(harness.engine.transportSnapshot().monitoringEnabled &&
                  near(left, input),
              "seek does not change monitoring state");
    }
    {
        const std::vector<float> shortSource{1.0F};
        const std::array shortTracks{monoTrack({2}, shortSource)};
        Harness harness{shortTracks, 1};
        harness.enableAt({0.0F});
        check(harness.engine.tryRequestPlay().accepted, "short playback schedules");
        std::array<float, 2> input{0.25F, 0.25F};
        std::array<float, 2> left{};
        std::array<float, 2> right{};
        const std::array<const float*, 1> inputs{input.data()};
        const std::array<float*, 2> outputs{left.data(), right.data()};
        harness.process(inputs.data(), inputs.size(), outputs.data(), outputs.size(),
                        input.size());
        const auto ended = harness.engine.transportSnapshot();
        check(near(left[0], 0.95710678F) && near(left[1], 0.25F) &&
                  ended.playback == transport::PlaybackState::stopped &&
                  ended.monitoringEnabled,
              "monitoring continues through natural playback end");
    }
}

void recordingIndependenceAndRealtimeTests() {
    {
        Harness disabled{{}, 64, true};
        check(disabled.engine.tryRequestRecord(
                  {41, {9}, media::AudioChannelLayout::mono}).accepted,
              "recording schedules with monitoring disabled");
        std::array<float, 2> raw{0.125F, -0.5F};
        std::array<float, 2> outputLeft{};
        std::array<float, 2> outputRight{};
        const std::array<const float*, 1> inputs{raw.data()};
        const std::array<float*, 2> outputs{outputLeft.data(), outputRight.data()};
        disabled.process(inputs.data(), inputs.size(), outputs.data(), outputs.size(),
                         raw.size());
        check(disabled.engine.tryRequestStop().accepted, "disabled-monitor recording stop");
        disabled.process(nullptr, 0, nullptr, 0, 0);
        std::array<float, 4> drained{};
        const std::array<float*, 1> drainOutput{drained.data()};
        const auto count = disabled.engine.drainRecording(
            {drainOutput.data(), drainOutput.size(), drained.size()});
        check(count == raw.size() && drained[0] == raw[0] && drained[1] == raw[1],
              "recording captures raw input normally while monitoring is disabled");
    }

    Harness harness{{}, 64, true};
    harness.enableAt({0.0F});
    check(harness.engine.tryRequestRecord(
              {1, {9}, media::AudioChannelLayout::mono}).accepted,
          "recording schedules with monitoring enabled");
    std::array<float, 3> first{0.1F, -0.25F, 0.5F};
    std::array<float, 3> outputLeft{};
    std::array<float, 3> outputRight{};
    const std::array<const float*, 1> firstInput{first.data()};
    const std::array<float*, 2> firstOutput{outputLeft.data(), outputRight.data()};
    const auto before = realtimeAllocations.load(std::memory_order_acquire);
    harness.process(firstInput.data(), firstInput.size(), firstOutput.data(),
                    firstOutput.size(), first.size());
    check(realtimeAllocations.load(std::memory_order_acquire) == before,
          "monitoring callback performs no allocation");
    check(outputLeft == first && outputRight == first,
          "monitor gain applies only to output contribution");
    const auto recordingMeter = harness.engine.meterSnapshot();
    check(recordingMeter.inputAvailable && near(recordingMeter.input.left, 0.5F) &&
              near(recordingMeter.input.right, 0.5F),
          "input meter observes raw capture input independently of recording and monitor gain");

    check(harness.engine.trySetMonitorGain({-24.0F}),
          "gain updates while recording publish latest value");
    std::array<float, 2> second{-0.75F, 0.375F};
    std::array<float, 2> secondLeft{};
    std::array<float, 2> secondRight{};
    const std::array<const float*, 1> secondInput{second.data()};
    const std::array<float*, 2> secondOutput{secondLeft.data(), secondRight.data()};
    harness.process(secondInput.data(), secondInput.size(), secondOutput.data(),
                    secondOutput.size(), second.size());
    check(harness.engine.trySetInputMonitoringEnabled(false).accepted,
          "disable monitoring schedules during recording");
    harness.process(secondInput.data(), secondInput.size(), secondOutput.data(),
                    secondOutput.size(), second.size());
    check(harness.engine.recordingSnapshot().phase == audio::RecordingPhase::capturing,
          "monitor disable does not change recording lifecycle");
    check(harness.engine.trySetInputMonitoringEnabled(true).accepted,
          "enable monitoring schedules during recording when input exists");
    harness.process(secondInput.data(), secondInput.size(), secondOutput.data(),
                    secondOutput.size(), second.size());
    check(harness.engine.recordingSnapshot().phase == audio::RecordingPhase::capturing,
          "monitor enable does not change recording lifecycle");

    check(harness.engine.tryRequestStop().accepted, "recording stop schedules");
    harness.process(nullptr, 0, nullptr, 0, 0);
    std::array<float, 16> drained{};
    const std::array<float*, 1> drainOutput{drained.data()};
    const auto count = harness.engine.drainRecording(
        {drainOutput.data(), drainOutput.size(), drained.size()});
    const std::vector<float> captured{drained.begin(), drained.begin() + count};
    const std::vector<float> expected{0.1F, -0.25F, 0.5F, -0.75F, 0.375F,
                                      -0.75F, 0.375F, -0.75F, 0.375F};
    check(captured == expected,
          "capture remains bit-identical raw input across monitoring and gain changes");
}

void aliasSafetyTests() {
    {
        Harness harness{{}, 64, true};
        check(harness.engine.tryRequestRecord(
                  {71, {9}, media::AudioChannelLayout::mono}).accepted,
              "alias-off mono recording schedules");
        std::array<float, 3> shared{0.125F, -0.5F, 0.75F};
        const auto original = shared;
        const std::array<const float*, 1> inputs{shared.data()};
        const std::array<float*, 1> outputs{shared.data()};
        harness.process(inputs.data(), inputs.size(), outputs.data(), outputs.size(),
                        shared.size());
        check(harness.engine.tryRequestStop().accepted, "alias-off mono stop schedules");
        harness.process(nullptr, 0, nullptr, 0, 0);
        std::array<float, 3> drained{};
        const std::array<float*, 1> drain{drained.data()};
        check(harness.engine.drainRecording({drain.data(), drain.size(), drained.size()}) ==
                  original.size() && drained == original,
              "monitoring-off capture remains raw with total mono alias");
    }
    {
        Harness harness{{}, 64, true};
        harness.enableAt({0.0F});
        check(harness.engine.tryRequestRecord(
                  {72, {9}, media::AudioChannelLayout::mono}).accepted,
              "alias-on mono recording schedules");
        std::array<float, 3> shared{0.125F, -0.5F, 0.75F};
        const auto original = shared;
        const std::array<const float*, 1> inputs{shared.data()};
        const std::array<float*, 1> outputs{shared.data()};
        const auto before = realtimeAllocations.load(std::memory_order_acquire);
        harness.process(inputs.data(), inputs.size(), outputs.data(), outputs.size(),
                        shared.size());
        check(realtimeAllocations.load(std::memory_order_acquire) == before &&
                  shared == original,
              "total mono alias stages raw monitoring input without RT allocation");
        check(harness.engine.tryRequestStop().accepted, "alias-on mono stop schedules");
        harness.process(nullptr, 0, nullptr, 0, 0);
        std::array<float, 3> drained{};
        const std::array<float*, 1> drain{drained.data()};
        check(harness.engine.drainRecording({drain.data(), drain.size(), drained.size()}) ==
                  original.size() && drained == original,
              "total mono alias leaves captured PCM raw");
    }
    {
        Harness harness{{}, 64, true};
        harness.enableAt({0.0F});
        check(harness.engine.tryRequestRecord(
                  {73, {9}, media::AudioChannelLayout::stereo}).accepted,
              "alias-on stereo recording schedules");
        std::array<float, 3> left{0.25F, -0.5F, 0.75F};
        std::array<float, 3> right{-0.125F, 0.375F, -0.625F};
        const auto originalLeft = left;
        const auto originalRight = right;
        const std::array<const float*, 2> inputs{left.data(), right.data()};
        const std::array<float*, 2> outputs{left.data(), right.data()};
        harness.process(inputs.data(), inputs.size(), outputs.data(), outputs.size(),
                        left.size());
        check(left == originalLeft && right == originalRight,
              "total stereo alias preserves each monitor channel");
        check(harness.engine.tryRequestStop().accepted, "alias-on stereo stop schedules");
        harness.process(nullptr, 0, nullptr, 0, 0);
        std::array<float, 3> drainedLeft{};
        std::array<float, 3> drainedRight{};
        const std::array<float*, 2> drain{drainedLeft.data(), drainedRight.data()};
        check(harness.engine.drainRecording({drain.data(), drain.size(),
                                             drainedLeft.size()}) == originalLeft.size() &&
                  drainedLeft == originalLeft && drainedRight == originalRight,
              "total stereo alias leaves both captured channels raw");
    }
    {
        const std::vector<float> source{1.0F};
        const std::array tracks{monoTrack({81}, source)};
        Harness harness{tracks, 1, true};
        harness.enableAt({0.0F});
        check(harness.engine.tryRequestRecord(
                  {74, {9}, media::AudioChannelLayout::mono}).accepted &&
                  harness.engine.tryRequestPlay().accepted,
              "alias playback and recording schedule");
        std::array<float, 1> shared{0.25F};
        std::array<float, 1> right{};
        const std::array<const float*, 1> inputs{shared.data()};
        const std::array<float*, 2> outputs{shared.data(), right.data()};
        harness.process(inputs.data(), inputs.size(), outputs.data(), outputs.size(), 1);
        check(near(shared[0], 0.95710678F) && near(right[0], 0.95710678F),
              "playback sums with staged raw monitoring input under alias");
        check(harness.engine.tryRequestStop().accepted, "alias playback stop schedules");
        harness.process(nullptr, 0, nullptr, 0, 0);
        std::array<float, 1> drained{};
        const std::array<float*, 1> drain{drained.data()};
        check(harness.engine.drainRecording({drain.data(), drain.size(), drained.size()}) == 1 &&
                  near(drained[0], 0.25F),
              "playback does not replace aliased captured raw input");
    }
    {
        Harness harness{{}, 64, true};
        harness.enableAt({0.0F});
        std::array<float, 4> shared{0.1F, -0.25F, 0.5F, 99.0F};
        const std::array<float, 3> original{shared[0], shared[1], shared[2]};
        check(harness.engine.tryRequestRecord(
                  {75, {9}, media::AudioChannelLayout::mono}).accepted,
              "partial-alias recording schedules");
        const std::array<const float*, 1> inputs{shared.data()};
        const std::array<float*, 1> outputs{shared.data() + 1};
        harness.process(inputs.data(), inputs.size(), outputs.data(), outputs.size(), 3);
        check(near(shared[1], 0.1F) && near(shared[2], -0.25F) &&
                  near(shared[3], 0.5F) &&
                  harness.engine.recordingSnapshot().phase == audio::RecordingPhase::capturing,
              "partial alias monitors from staged raw input without changing capture lifecycle");
        check(harness.engine.tryRequestStop().accepted, "partial-alias recording stop schedules");
        harness.process(nullptr, 0, nullptr, 0, 0);
        std::array<float, 3> drained{};
        const std::array<float*, 1> drain{drained.data()};
        check(harness.engine.drainRecording({drain.data(), drain.size(), drained.size()}) ==
                  original.size() && drained == original,
              "partial alias leaves captured PCM bit-identical to raw input");
    }
    {
        Harness harness{{}, 2048, true, 1024};
        harness.enableAt({0.0F});
        check(harness.engine.tryRequestRecord(
                  {76, {9}, media::AudioChannelLayout::mono}).accepted,
              "overflow recording schedules with an independent larger capture ring");
        std::vector<float> shared(513, 0.25F);
        const std::array<const float*, 1> inputs{shared.data()};
        const std::array<float*, 1> outputs{shared.data()};
        const auto before = realtimeAllocations.load(std::memory_order_acquire);
        harness.process(inputs.data(), inputs.size(), outputs.data(), outputs.size(),
                        shared.size());
        const auto oversized = harness.engine.transportSnapshot();
        std::array<float, 2> resumed{0.5F, -0.25F};
        const auto resumedRaw = resumed;
        const std::array<const float*, 1> resumedInput{resumed.data()};
        const std::array<float*, 1> resumedOutput{resumed.data()};
        harness.process(resumedInput.data(), resumedInput.size(), resumedOutput.data(),
                        resumedOutput.size(), resumed.size());
        const auto recovered = harness.engine.transportSnapshot();
        check(realtimeAllocations.load(std::memory_order_acquire) == before &&
                  !oversized.monitoringRouteSupported &&
                  std::all_of(shared.begin(), shared.end(), [](float value) {
                      return value == 0.0F;
                  }) && recovered.monitoringRouteSupported && resumed == resumedRaw &&
                  std::all_of(resumed.begin(), resumed.end(), [](float value) {
                      return std::isfinite(value);
                  }) &&
                  harness.engine.recordingSnapshot().phase == audio::RecordingPhase::capturing,
              "oversized alias block is bounded and the next valid block recovers monitoring");
        check(harness.engine.tryRequestStop().accepted, "overflow recording stop schedules");
        harness.process(nullptr, 0, nullptr, 0, 0);
        std::vector<float> drained(shared.size() + resumedRaw.size());
        const std::array<float*, 1> drain{drained.data()};
        check(harness.engine.drainRecording({drain.data(), drain.size(), drained.size()}) ==
                  drained.size() &&
                  std::all_of(drained.begin(), drained.begin() + shared.size(),
                              [](float value) { return value == 0.25F; }) &&
                  drained[shared.size()] == resumedRaw[0] &&
                  drained[shared.size() + 1] == resumedRaw[1],
              "staging overflow does not alter capture or leave a zombie recording state");
    }
}

void latestValueWinsTests() {
    Harness harness;
    bool published = true;
    for (int index = 0; index < 1000; ++index) {
        published = published && harness.engine.trySetMonitorGain(
            {index % 2 == 0 ? -24.0F : -6.0F});
    }
    check(published && harness.engine.trySetMonitorGain({-24.0F}),
          "gain burst publishes without FIFO admission");
    check(harness.engine.trySetInputMonitoringEnabled(true).accepted,
          "enable after gain burst schedules");
    harness.settleMonitoring();
    float input{1.0F};
    float left{};
    float right{};
    processMono(harness, input, left, right);
    const auto expected = audio::prepareMonitorGain({-24.0F});
    const auto snapshot = harness.engine.transportSnapshot();
    check(near(left, expected) && near(right, expected) &&
              snapshot.monitorGainDb == -24.0F,
          "A to B to A gain burst consumes only the final payload");
}

class ApplicationControl final : public audio::IAudioEngineControl {
public:
    ApplicationControl() {
        engine_.configure({rate_, {16}, {}});
        check(engine_.prepareLegacyDeviceRate(rate_), "application control prepares rate");
        engine_.deviceInitialising();
        engine_.processBlock({nullptr, 0, 0}, rate_);
    }

    audio::AudioFilePreparationResult prepareWav(const std::filesystem::path&) override {
        return {};
    }
    audio::StructuralPlanPreparationResult prepareProcessingPlan(
        const audio::ProcessingPlanSpecification&) override {
        return {nullptr, "not used by monitoring command tests"};
    }
    bool commitPreparedProcessingPlan(audio::PreparedProcessingPlanChangePtr,
                                      audio::AudioFileCommitAction) noexcept override {
        return false;
    }
    bool tryUpdateTrackMix(tracks::TrackId, mixer::PreparedTrackMixState,
                           audio::PreparedAudibilityState) noexcept override { return false; }
    bool tryUpdateBusMix(routing::BusId, mixer::PreparedBusMixState,
                         audio::PreparedAudibilityState) noexcept override { return false; }
    bool tryUpdateSendMix(routing::SendId, mixer::PreparedSendMixState) noexcept override {
        return false;
    }
    bool tryUpdateMasterMix(mixer::PreparedMasterMixState) noexcept override { return false; }
    audio::AudioControlRequestResult tryRequestPlay() noexcept override {
        return engine_.tryRequestPlay();
    }
    audio::AudioControlRequestResult tryRequestStop() noexcept override {
        return engine_.tryRequestStop();
    }
    audio::AudioControlRequestResult trySetInputMonitoringEnabled(
        bool enabled) noexcept override {
        return engine_.trySetInputMonitoringEnabled(enabled);
    }
    bool trySetMonitorGain(audio::MonitorGainDb gain) noexcept override {
        return engine_.trySetMonitorGain(gain);
    }
    audio::RealtimeTransportSnapshot transportSnapshot() const noexcept override {
        return engine_.transportSnapshot();
    }
    audio::RealtimeTransportSnapshot projectedTransportSnapshot() noexcept override {
        return engine_.projectedTransportSnapshot();
    }
    mixer::MeterSnapshot meterSnapshot() const noexcept override { return {}; }

    void callback() {
        std::array<float, 240> input{};
        std::array<float, 240> left{};
        std::array<float, 240> right{};
        const std::array<const float*, 1> inputs{input.data()};
        const std::array<float*, 2> outputs{left.data(), right.data()};
        engine_.processBlock({inputs.data(), inputs.size(), input.size()},
                             {outputs.data(), outputs.size(), input.size()}, rate_);
    }

private:
    audio::RealtimeAudioEngine engine_;
    const timeline::SampleRate rate_{48000.0};
};

void commandAndDocumentIndependenceTests() {
    ApplicationControl control;
    application::DawApplication app{control, timeline::SampleRate{48000.0}};
    commands::CommandDispatcher dispatcher{app};
    const auto token = app.history().currentStateToken();
    const auto revision = app.timelineRevision();
    check(!app.session().dirty(), "new application starts clean");
    const auto initialMonitoring = app.inputMonitoringReadModel();
    check(!initialMonitoring.enabled && initialMonitoring.gain.value == -12.0F,
          "application monitoring read model starts confirmed off at default gain");
    check(dispatcher.dispatch(commands::SetMonitorGain{{0.1F}}).status ==
              commands::CommandStatus::rejected,
          "command system validates positive monitoring gain");
    check(dispatcher.dispatch(commands::SetMonitorGain{{-6.0F}}).status ==
              commands::CommandStatus::accepted &&
              dispatcher.dispatch(commands::EnableInputMonitoring{}).status ==
                  commands::CommandStatus::accepted,
          "ephemeral monitoring commands route through dispatcher");
    control.callback();
    app.synchroniseTransport();
    check(app.inputMonitoringEnabled() && app.monitorGain().value == -6.0F,
          "application observes confirmed RT monitoring state");
    const auto confirmedMonitoring = app.inputMonitoringReadModel();
    check(confirmedMonitoring.enabled && confirmedMonitoring.gain.value == -6.0F,
          "UI read model exposes confirmed monitoring state without document state");
    check(dispatcher.dispatch(commands::ToggleInputMonitoring{}).status ==
              commands::CommandStatus::accepted,
          "toggle resolves from ephemeral requested state");
    control.callback();
    app.synchroniseTransport();
    check(!app.inputMonitoringEnabled(), "toggle disables monitoring through command path");
    check(app.history().currentStateToken() == token &&
              app.timelineRevision() == revision && !app.session().dirty(),
          "monitoring commands do not mutate ProjectState history or dirty state");
}

void lifecycleOffMailboxConcurrencyTest() {
    Harness harness;
    harness.enableAt({0.0F});
    std::array<float, 64> input{};
    input.fill(0.5F);
    std::array<float, 64> left{};
    std::array<float, 64> right{};
    const std::array<const float*, 1> inputs{input.data()};
    const std::array<float*, 2> outputs{left.data(), right.data()};
    std::atomic<bool> start{};
    std::thread lifecycle([&] {
        while (!start.load(std::memory_order_acquire)) {}
        for (int index = 0; index < 2000; ++index)
            harness.engine.requestInputMonitoringLifecycleOff();
    });
    start.store(true, std::memory_order_release);
    for (int index = 0; index < 2000; ++index)
        harness.process(inputs.data(), inputs.size(), outputs.data(), outputs.size(), input.size());
    lifecycle.join();
    harness.process(inputs.data(), inputs.size(), outputs.data(), outputs.size(), input.size());
    const auto state = harness.engine.transportSnapshot();
    check(!state.monitoringEnabled && state.monitoringLifecycleForcedOff &&
              !state.monitoringRouteSupported,
          "concurrent lifecycle-off mailbox is consumed solely by the RT writer");
}

} // namespace

void* operator new(std::size_t size) {
    if (realtimeRegion) realtimeAllocations.fetch_add(1, std::memory_order_relaxed);
    if (void* memory = std::malloc(size)) return memory;
    throw std::bad_alloc{};
}
void* operator new[](std::size_t size) {
    if (realtimeRegion) realtimeAllocations.fetch_add(1, std::memory_order_relaxed);
    if (void* memory = std::malloc(size)) return memory;
    throw std::bad_alloc{};
}
void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete[](void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }
void operator delete[](void* memory, std::size_t) noexcept { std::free(memory); }

int main() {
    stateAndGainTests();
    inputMeterPreGainTests();
    mappingAndLayoutTests();
    playbackIndependenceTests();
    recordingIndependenceAndRealtimeTests();
    aliasSafetyTests();
    latestValueWinsTests();
    commandAndDocumentIndependenceTests();
    lifecycleOffMailboxConcurrencyTest();
}
