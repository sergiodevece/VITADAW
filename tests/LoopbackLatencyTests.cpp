#include "vitadaw/audio/LoopbackLatency.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <new>
#include <optional>
#include <vector>

namespace vitadaw::audio {

struct LoopbackProbeTestAccess {
    static void publishLateCompletion(RealtimeLoopbackProbe& probe) noexcept {
        probe.completeIfRunning();
    }
};

} // namespace vitadaw::audio

namespace {
using namespace vitadaw;

std::atomic<std::size_t> realtimeAllocations{};
thread_local bool realtimeRegion{};
constexpr double testSampleRateHz = 8192.0;

void check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

void prepareProbe(audio::RealtimeLoopbackProbe& probe) {
    check(probe.prepare({testSampleRateHz, 256, 0, 0}), "loopback probe prepares");
}

audio::LoopbackLatencyReadModel seed() {
    audio::LoopbackLatencyReadModel value;
    value.sessionId = 1;
    value.status = audio::LoopbackLatencyStatus::analysing;
    value.configuration.sampleRateHz = testSampleRateHz;
    value.configuration.reportedInputFrames = 32;
    value.configuration.reportedOutputFrames = 32;
    value.configuration.reportedRoundTripFrames = 64;
    return value;
}

std::vector<float> syntheticCapture(
    const audio::RealtimeLoopbackProbe& probe,
    const std::array<std::optional<std::uint64_t>, audio::loopbackTrialCount>& delays,
    float scale = 1.0F, bool inverted = false) {
    std::vector<float> capture(probe.capturedSamples().size(), 0.0F);
    const auto stimulus = probe.stimulus();
    const auto sign = inverted ? -scale : scale;
    for (std::size_t trial = 0; trial < delays.size(); ++trial) {
        if (!delays[trial]) continue;
        const auto start = probe.emittedFrames()[trial] + *delays[trial];
        check(start + stimulus.size() <= capture.size(), "synthetic response fits");
        for (std::size_t frame = 0; frame < stimulus.size(); ++frame)
            capture[static_cast<std::size_t>(start) + frame] += stimulus[frame] * sign;
    }
    return capture;
}

std::vector<float> syntheticFilteredCapture(
    const audio::RealtimeLoopbackProbe& probe,
    const std::array<std::optional<std::uint64_t>, audio::loopbackTrialCount>& delays,
    std::span<const float> impulseResponse, float scale = 1.0F,
    bool inverted = false) {
    std::vector<float> capture(probe.capturedSamples().size(), 0.0F);
    const auto stimulus = probe.stimulus();
    const auto sign = inverted ? -scale : scale;
    for (std::size_t trial = 0; trial < delays.size(); ++trial) {
        if (!delays[trial]) continue;
        const auto start = probe.emittedFrames()[trial] + *delays[trial];
        check(start + stimulus.size() + impulseResponse.size() <= capture.size(),
              "filtered synthetic response fits");
        for (std::size_t frame = 0; frame < stimulus.size(); ++frame) {
            for (std::size_t tap = 0; tap < impulseResponse.size(); ++tap) {
                capture[static_cast<std::size_t>(start) + frame + tap] +=
                    stimulus[frame] * impulseResponse[tap] * sign;
            }
        }
    }
    return capture;
}

float dbToGain(float db) {
    return std::pow(10.0F, db / 20.0F);
}

void addDeterministicNoise(std::vector<float>& samples, float rmsLevel,
                           std::uint32_t seedValue = 1U) {
    auto state = seedValue;
    const auto uniformScale = rmsLevel * std::sqrt(3.0F);
    for (auto& sample : samples) {
        state = state * 1664525U + 1013904223U;
        const auto uniform = static_cast<float>((state >> 8U) & 0xffffU) /
                                 65535.0F * 2.0F - 1.0F;
        sample += uniform * uniformScale;
    }
}

audio::LoopbackLatencyReadModel analyse(
    const audio::RealtimeLoopbackProbe& probe, std::span<const float> capture) {
    return audio::analyseLoopbackLatency(
        capture, probe.stimulus(), probe.emittedFrames(),
        probe.searchWindowFrames(), probe.preRollFrames(), seed());
}

void exactDelayAndPolarityTests() {
    audio::RealtimeLoopbackProbe probe;
    prepareProbe(probe);
    check(probe.stimulus().size() == audio::loopbackStimulusLength,
          "probe uses the documented 1023-sample MLS");
    check(std::all_of(probe.stimulus().begin(), probe.stimulus().end(),
                      [](float sample) {
                          return std::abs(sample) == audio::loopbackStimulusAmplitude;
                      }),
          "MLS is bounded at -24 dBFS and contains no runtime-generated gaps");
    for (const auto delay : {0ULL, 16ULL, 64ULL, 128ULL, 513ULL, 1200ULL, 4096ULL}) {
        const std::array<std::optional<std::uint64_t>, audio::loopbackTrialCount> delays{
            delay, delay, delay, delay, delay};
        const auto result = analyse(probe, syntheticCapture(probe, delays));
        check(result.status == audio::LoopbackLatencyStatus::completed &&
                  result.validTrials == 5 && result.measuredRoundTripFrames == delay &&
                  result.minimumFrames == delay && result.maximumFrames == delay &&
                  result.jitterFrames == 0,
              "exact synthetic delay is recovered");
    }

    for (const auto gainDb : {-6.0F, -24.0F, -50.0F}) {
        const std::array<std::optional<std::uint64_t>, audio::loopbackTrialCount> delays{
            1200, 1200, 1200, 1200, 1200};
        const auto gainResult = analyse(
            probe, syntheticCapture(probe, delays, dbToGain(gainDb)));
        check(gainResult.status == audio::LoopbackLatencyStatus::completed &&
                  gainResult.measuredRoundTripFrames == 1200 &&
                  gainResult.trials[0].correlation > 0.99,
              "matched detector is gain-invariant from -6 through -50 dB");
    }

    const std::array<std::optional<std::uint64_t>, audio::loopbackTrialCount> inverted{
        64, 64, 64, 64, 64};
    const auto result = analyse(probe, syntheticCapture(probe, inverted, 0.5F, true));
    check(result.status == audio::LoopbackLatencyStatus::completed &&
              result.measuredRoundTripFrames == 64 && result.trials[0].polarityInverted &&
              result.trials[0].measuredFrames == std::optional<std::uint64_t>{64} &&
              result.trials[0].correlation > 0.99 &&
              result.trials[0].secondCorrelation >= 0.0 &&
              std::isfinite(result.trials[0].ambiguityRatio) &&
              !result.trials[0].clipped,
          "valid trial publishes delay, polarity, correlation, SNR and clipping diagnostics");
}

void filteredAndFractionalDelayTests() {
    audio::RealtimeLoopbackProbe probe;
    prepareProbe(probe);
    const std::array<std::optional<std::uint64_t>, audio::loopbackTrialCount> delays{
        1200, 1200, 1200, 1200, 1200};
    const std::array<std::array<float, 7>, 3> analogueResponses{{
        {0.72F, 0.18F, 0.07F, 0.03F, 0.0F, 0.0F, 0.0F},
        {0.10F, 0.20F, 0.40F, 0.20F, 0.10F, 0.0F, 0.0F},
        {0.72F, -0.31F, 0.19F, -0.11F, 0.07F, -0.04F, 0.02F},
    }};
    const std::array<std::uint64_t, 3> expected{1200, 1202, 1200};
    for (std::size_t index = 0; index < analogueResponses.size(); ++index) {
        const auto result = analyse(
            probe, syntheticFilteredCapture(probe, delays, analogueResponses[index]));
        check(result.status == audio::LoopbackLatencyStatus::completed &&
                  result.measuredRoundTripFrames == expected[index] &&
                  result.trials[0].ambiguityRatio < 0.70,
              "MLS survives low-pass rolloff, short FIR dispersion and ringing");
    }

    const std::array<float, 2> fractionalEarly{0.60F, 0.40F};
    const auto early = analyse(
        probe, syntheticFilteredCapture(probe, delays, fractionalEarly));
    check(early.status == audio::LoopbackLatencyStatus::completed &&
              early.measuredRoundTripFrames == 1200,
          "fractional energy split keeps the nearest earlier integer lag");
    const std::array<float, 2> fractionalLate{0.40F, 0.60F};
    const auto late = analyse(
        probe, syntheticFilteredCapture(probe, delays, fractionalLate));
    check(late.status == audio::LoopbackLatencyStatus::completed &&
              late.measuredRoundTripFrames == 1201,
          "fractional energy split keeps the nearest later integer lag");
}

void noiseQualityAndFailureTests() {
    audio::RealtimeLoopbackProbe probe;
    prepareProbe(probe);
    const std::array<std::optional<std::uint64_t>, audio::loopbackTrialCount> delays{
        128, 128, 128, 128, 128};
    for (const auto snrDb : {30.0F, 12.0F, 7.0F}) {
        auto noisy = syntheticCapture(probe, delays);
        addDeterministicNoise(
            noisy, audio::loopbackStimulusAmplitude / dbToGain(snrDb));
        const auto noisyResult = analyse(probe, noisy);
        check(noisyResult.status == audio::LoopbackLatencyStatus::completed &&
                  noisyResult.measuredRoundTripFrames == 128 &&
                  noisyResult.trials[0].signalToNoiseDb >= 6.0,
              "controlled noise above the conservative SNR gate preserves detection");
    }

    auto belowSnr = syntheticCapture(probe, delays);
    addDeterministicNoise(
        belowSnr, audio::loopbackStimulusAmplitude / dbToGain(3.0F));
    const auto belowSnrResult = analyse(probe, belowSnr);
    check(belowSnrResult.status == audio::LoopbackLatencyStatus::failed &&
              belowSnrResult.trials[0].quality ==
                  audio::LoopbackTrialQuality::signalTooLow,
          "signal below the six-decibel SNR gate is rejected");

    const auto low = analyse(probe, syntheticCapture(probe, delays, 1.0e-8F));
    check(low.status == audio::LoopbackLatencyStatus::failed &&
              low.trials[0].quality == audio::LoopbackTrialQuality::signalTooLow &&
              low.trials[0].peak > 0.0F && low.trials[0].peak < 1.0e-6F &&
              low.trials[0].correlation > 0.99 && !low.trials[0].clipped,
          "very low signal publishes its classification, peak and correlation");

    std::vector<float> absent(probe.capturedSamples().size(), 0.001F);
    const auto notFound = analyse(probe, absent);
    check(notFound.status == audio::LoopbackLatencyStatus::failed &&
              notFound.trials[0].quality == audio::LoopbackTrialQuality::signalTooLow &&
              notFound.trials[0].peak == 0.001F &&
              notFound.trials[0].correlation < 0.08 &&
              notFound.trials[0].secondCorrelation >= 0.0 &&
              !std::isfinite(notFound.trials[0].signalToNoiseDb),
          "steady uncorrelated input is rejected by signal-over-baseline SNR");

    auto clipped = syntheticCapture(probe, delays);
    clipped[static_cast<std::size_t>(probe.emittedFrames()[0] + 4U)] = 1.0F;
    const auto clippedResult = analyse(probe, clipped);
    check(clippedResult.status == audio::LoopbackLatencyStatus::completed &&
              clippedResult.trials[0].quality == audio::LoopbackTrialQuality::clipped &&
              clippedResult.trials[0].clipped &&
              clippedResult.trials[0].peak == 1.0F &&
              clippedResult.validTrials == 4,
          "clipped trial publishes clipping and peak while remaining trials aggregate");

    auto ambiguous = syntheticCapture(probe, delays);
    const auto stimulus = probe.stimulus();
    for (std::size_t trial = 0; trial < audio::loopbackTrialCount; ++trial) {
        const auto duplicate = probe.emittedFrames()[trial] + 2048U;
        for (std::size_t frame = 0; frame < stimulus.size(); ++frame)
            ambiguous[static_cast<std::size_t>(duplicate) + frame] += stimulus[frame];
    }
    const auto ambiguousResult = analyse(probe, ambiguous);
    check(ambiguousResult.status == audio::LoopbackLatencyStatus::failed &&
              ambiguousResult.trials[0].quality == audio::LoopbackTrialQuality::ambiguous &&
              ambiguousResult.trials[0].correlation > 0.99 &&
              ambiguousResult.trials[0].secondCorrelation > 0.99 &&
              ambiguousResult.trials[0].ambiguityRatio >= 0.70,
          "ambiguous trial publishes best, second and the ratio that rejects it");
}

void falsePositiveTests() {
    audio::RealtimeLoopbackProbe probe;
    prepareProbe(probe);

    const auto rejected = [&](std::span<const float> capture, const char* message) {
        const auto result = analyse(probe, capture);
        check(result.status == audio::LoopbackLatencyStatus::failed &&
                  !result.measuredRoundTripFrames && result.validTrials < 3,
              message);
    };

    std::vector<float> silence(probe.capturedSamples().size(), 0.0F);
    rejected(silence, "silence cannot produce a loopback result");

    std::vector<float> noise(probe.capturedSamples().size(), 0.0F);
    addDeterministicNoise(noise, 0.01F, 17U);
    rejected(noise, "pure noise cannot produce a loopback result");

    std::vector<float> sine(probe.capturedSamples().size(), 0.0F);
    for (std::size_t frame = static_cast<std::size_t>(probe.preRollFrames());
         frame < sine.size(); ++frame) {
        sine[frame] = 0.02F * std::sin(
            2.0 * 3.14159265358979323846 * 431.0 *
            static_cast<double>(frame) / testSampleRateHz);
    }
    rejected(sine, "stable sine cannot produce a loopback result");

    std::vector<float> musicLike(probe.capturedSamples().size(), 0.0F);
    for (std::size_t frame = static_cast<std::size_t>(probe.preRollFrames());
         frame < musicLike.size(); ++frame) {
        const auto time = static_cast<double>(frame) / testSampleRateHz;
        const auto envelope = 0.45 + 0.35 * std::sin(2.0 * 3.14159265358979323846 * 2.1 * time);
        musicLike[frame] = static_cast<float>(0.018 * envelope *
            (std::sin(2.0 * 3.14159265358979323846 * 196.0 * time) +
             0.6 * std::sin(2.0 * 3.14159265358979323846 * 293.7 * time) +
             0.35 * std::sin(2.0 * 3.14159265358979323846 * 659.3 * time)));
    }
    rejected(musicLike, "music-like programme audio cannot produce a loopback result");

    std::vector<float> wrongPattern(probe.capturedSamples().size(), 0.0F);
    const auto stimulus = probe.stimulus();
    for (std::size_t trial = 0; trial < audio::loopbackTrialCount; ++trial) {
        const auto start = static_cast<std::size_t>(probe.emittedFrames()[trial] + 128U);
        for (std::size_t frame = 0; frame < stimulus.size(); ++frame)
            wrongPattern[start + frame] = stimulus[stimulus.size() - 1U - frame];
    }
    rejected(wrongPattern, "a deterministic but incorrect binary pattern is rejected");

    const std::array<std::optional<std::uint64_t>, audio::loopbackTrialCount> delays{
        128, 128, 128, 128, 128};
    rejected(syntheticCapture(probe, delays, 1.0e-8F),
             "correct pattern below the absolute floor is rejected");
}

void aggregationAndCheckedArithmeticTests() {
    audio::RealtimeLoopbackProbe probe;
    prepareProbe(probe);
    const std::array<std::optional<std::uint64_t>, audio::loopbackTrialCount> spread{
        64, 60, 68, 63, 65};
    const auto result = analyse(probe, syntheticCapture(probe, spread));
    check(result.status == audio::LoopbackLatencyStatus::completed &&
              result.measuredRoundTripFrames == 64 && result.minimumFrames == 60 &&
              result.maximumFrames == 68 && result.jitterFrames == 8 &&
              result.residualFrames == 0,
          "median, min, max, jitter and signed residual are exact");

    const std::array<std::optional<std::uint64_t>, audio::loopbackTrialCount> three{
        17, std::nullopt, 15, std::nullopt, 16};
    const auto threeResult = analyse(probe, syntheticCapture(probe, three));
    check(threeResult.status == audio::LoopbackLatencyStatus::completed &&
              threeResult.validTrials == 3 && threeResult.measuredRoundTripFrames == 16,
          "three valid trials produce an authoritative odd median");

    const std::array<std::optional<std::uint64_t>, audio::loopbackTrialCount> two{
        17, std::nullopt, std::nullopt, std::nullopt, 16};
    const auto twoResult = analyse(probe, syntheticCapture(probe, two));
    check(twoResult.status == audio::LoopbackLatencyStatus::failed &&
              !twoResult.measuredRoundTripFrames,
          "fewer than three trials produces no authoritative result");

    auto impossible = probe.emittedFrames();
    impossible[0] = std::numeric_limits<std::uint64_t>::max();
    auto checked = audio::analyseLoopbackLatency(
        syntheticCapture(probe, spread), probe.stimulus(), impossible,
        probe.searchWindowFrames(), probe.preRollFrames(), seed());
    check(checked.trials[0].quality == audio::LoopbackTrialQuality::capacityExceeded,
          "checked analysis arithmetic rejects impossible indices");
}

void realtimeProbeTests() {
    audio::RealtimeLoopbackProbe probe;
    prepareProbe(probe);
    std::array<float, 256> input{};
    std::array<float, 256> output{};
    const std::array<const float*, 1> inputs{input.data()};
    const std::array<float*, 1> outputs{output.data()};
    const auto before = realtimeAllocations.load(std::memory_order_acquire);
    realtimeRegion = true;
    probe.processBlock({inputs.data(), 1, input.size()},
                       {outputs.data(), 1, output.size()});
    realtimeRegion = false;
    check(probe.status() == audio::RealtimeLoopbackProbeStatus::running &&
              realtimeAllocations.load(std::memory_order_acquire) == before,
          "probe callback starts without RT allocation");

    bool emitted{};
    for (std::size_t block = 0; block < 1200 && probe.active(); ++block) {
        input.fill(0.0F);
        output.fill(0.0F);
        realtimeRegion = true;
        probe.processBlock({inputs.data(), 1, input.size()},
                           {outputs.data(), 1, output.size()});
        realtimeRegion = false;
        emitted = emitted || std::any_of(output.begin(), output.end(),
                                         [](float value) { return value != 0.0F; });
    }
    check(probe.status() == audio::RealtimeLoopbackProbeStatus::captured && emitted &&
              realtimeAllocations.load(std::memory_order_acquire) == before,
          "full capture and cross-callback stimulus emission allocate nothing in RT");

    audio::RealtimeLoopbackProbe oversizedProbe;
    prepareProbe(oversizedProbe);
    std::array<float, 257> oversizedInput{};
    std::array<float, 257> oversizedOutput{};
    const std::array<const float*, 1> oversizedInputs{oversizedInput.data()};
    const std::array<float*, 1> oversizedOutputs{oversizedOutput.data()};
    realtimeRegion = true;
    oversizedProbe.processBlock({oversizedInputs.data(), 1, oversizedInput.size()},
                                {oversizedOutputs.data(), 1, oversizedOutput.size()});
    realtimeRegion = false;
    check(oversizedProbe.status() == audio::RealtimeLoopbackProbeStatus::capacityExceeded &&
              std::all_of(oversizedOutput.begin(), oversizedOutput.end(),
                          [](float value) { return value == 0.0F; }) &&
              realtimeAllocations.load(std::memory_order_acquire) == before,
          "oversized callback fails bounded and silent without allocation");

    audio::RealtimeLoopbackProbe clippedProbe;
    prepareProbe(clippedProbe);
    std::array<float, 256> clippedInput{};
    std::array<float, 256> clippedOutput{};
    clippedInput[17] = 1.0F;
    const std::array<const float*, 1> clippedInputs{clippedInput.data()};
    const std::array<float*, 1> clippedOutputs{clippedOutput.data()};
    realtimeRegion = true;
    clippedProbe.processBlock({clippedInputs.data(), 1, clippedInput.size()},
                              {clippedOutputs.data(), 1, clippedOutput.size()});
    realtimeRegion = false;
    check(clippedProbe.status() == audio::RealtimeLoopbackProbeStatus::inputClipped &&
              clippedProbe.terminalPeak() == 1.0F &&
              std::all_of(clippedOutput.begin(), clippedOutput.end(),
                          [](float value) { return value == 0.0F; }) &&
              realtimeAllocations.load(std::memory_order_acquire) == before,
          "clipping abort is explicit, silent and allocation-free");

    audio::RealtimeLoopbackProbe totalAliasProbe;
    prepareProbe(totalAliasProbe);
    std::array<float, 256> totalAlias{};
    std::array<float, 256> totalAliasExpected{};
    for (std::size_t frame = 0; frame < totalAlias.size(); ++frame)
        totalAlias[frame] = totalAliasExpected[frame] =
            static_cast<float>(frame + 1U) / 512.0F;
    const std::array<const float*, 1> totalAliasInputs{totalAlias.data()};
    const std::array<float*, 1> totalAliasOutputs{totalAlias.data()};
    realtimeRegion = true;
    totalAliasProbe.processBlock({totalAliasInputs.data(), 1, totalAlias.size()},
                                 {totalAliasOutputs.data(), 1, totalAlias.size()});
    realtimeRegion = false;
    check(std::equal(totalAliasExpected.begin(), totalAliasExpected.end(),
                     totalAliasProbe.capturedSamples().begin()) &&
              realtimeAllocations.load(std::memory_order_acquire) == before,
          "total input/output alias preserves raw capture before output clear");

    audio::RealtimeLoopbackProbe partialAliasProbe;
    prepareProbe(partialAliasProbe);
    std::array<float, 257> partialAlias{};
    std::array<float, 256> partialAliasExpected{};
    for (std::size_t frame = 0; frame < partialAliasExpected.size(); ++frame)
        partialAlias[frame] = partialAliasExpected[frame] =
            -static_cast<float>(frame + 1U) / 512.0F;
    const std::array<const float*, 1> partialAliasInputs{partialAlias.data()};
    const std::array<float*, 1> partialAliasOutputs{partialAlias.data() + 1U};
    realtimeRegion = true;
    partialAliasProbe.processBlock({partialAliasInputs.data(), 1, partialAliasExpected.size()},
                                   {partialAliasOutputs.data(), 1, partialAliasExpected.size()});
    realtimeRegion = false;
    check(std::equal(partialAliasExpected.begin(), partialAliasExpected.end(),
                     partialAliasProbe.capturedSamples().begin()) &&
              realtimeAllocations.load(std::memory_order_acquire) == before,
          "partial input/output overlap preserves raw capture before output clear");

    const auto terminalWins = [](auto terminate,
                                 audio::RealtimeLoopbackProbeStatus expected,
                                 const char* message) {
        audio::RealtimeLoopbackProbe probe;
        prepareProbe(probe);
        std::array<float, 256> input{};
        std::array<float, 256> output{};
        const std::array<const float*, 1> inputs{input.data()};
        const std::array<float*, 1> outputs{output.data()};
        probe.processBlock({inputs.data(), 1, input.size()},
                           {outputs.data(), 1, output.size()});
        terminate(probe);
        audio::LoopbackProbeTestAccess::publishLateCompletion(probe);
        check(probe.status() == expected, message);
    };
    terminalWins([](auto& probe) { probe.cancel(); },
                 audio::RealtimeLoopbackProbeStatus::cancelled,
                 "late callback completion cannot overwrite cancellation");
    terminalWins([](auto& probe) { probe.invalidateConfiguration(); },
                 audio::RealtimeLoopbackProbeStatus::configurationChanged,
                 "late callback completion cannot overwrite configuration invalidation");
    terminalWins([](auto& probe) { probe.failDevice(); },
                 audio::RealtimeLoopbackProbeStatus::deviceError,
                 "late callback completion cannot overwrite device loss");
}

} // namespace

void* operator new(std::size_t size) {
    if (realtimeRegion) realtimeAllocations.fetch_add(1, std::memory_order_relaxed);
    if (void* pointer = std::malloc(size)) return pointer;
    throw std::bad_alloc{};
}

void operator delete(void* pointer) noexcept { std::free(pointer); }
void operator delete(void* pointer, std::size_t) noexcept { std::free(pointer); }

int main() {
    exactDelayAndPolarityTests();
    filteredAndFractionalDelayTests();
    noiseQualityAndFailureTests();
    falsePositiveTests();
    aggregationAndCheckedArithmeticTests();
    realtimeProbeTests();
    std::cout << "Loopback latency tests passed\n";
    return EXIT_SUCCESS;
}
