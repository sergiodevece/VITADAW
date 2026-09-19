#include "vitadaw/audio/LoopbackLatency.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <new>
#include <utility>

namespace vitadaw::audio {
namespace {

constexpr std::size_t maximumLoopbackCaptureFrames = 4U * 1024U * 1024U;
constexpr double minimumNormalizedMatchedPeak = 0.08;
constexpr double maximumAmbiguousRatio = 0.70;
constexpr double minimumSignalToNoiseDb = 6.0;
constexpr float clippingThreshold = 0.999F;
constexpr float absoluteMinimumSignal = 1.0e-6F;
constexpr std::uint64_t correlationExclusionRadius = 64;

std::vector<float> makeMaximumLengthStimulus() {
    std::vector<float> stimulus(loopbackStimulusLength);
    // Ten-bit maximal LFSR, primitive polynomial x^10 + x^3 + 1. The all-ones
    // seed produces all 2^10 - 1 non-zero states before repeating.
    std::uint16_t state = 0x03ffU;
    for (auto& sample : stimulus) {
        sample = (state & 1U) != 0U ? loopbackStimulusAmplitude
                                    : -loopbackStimulusAmplitude;
        const auto feedback = static_cast<std::uint16_t>(
            ((state >> 0U) ^ (state >> 3U)) & 1U);
        state = static_cast<std::uint16_t>((state >> 1U) | (feedback << 9U));
    }
    return stimulus;
}

double rms(std::span<const float> samples) noexcept {
    if (samples.empty()) return 0.0;
    long double sum{};
    for (const auto sample : samples) {
        const auto value = static_cast<long double>(sample);
        sum += value * value;
    }
    return std::sqrt(static_cast<double>(sum / samples.size()));
}

std::optional<std::uint64_t> checkedAdd(std::uint64_t left,
                                        std::uint64_t right) noexcept {
    if (right > std::numeric_limits<std::uint64_t>::max() - left)
        return std::nullopt;
    return left + right;
}

} // namespace

bool RealtimeLoopbackProbe::prepare(const LoopbackProbePreparation& preparation) {
    if (!std::isfinite(preparation.sampleRateHz) || preparation.sampleRateHz <= 0.0 ||
        preparation.blockCapacity == 0 || active()) return false;
    const auto roundedRate = std::ceil(preparation.sampleRateHz);
    if (!std::isfinite(roundedRate) || roundedRate <= 0.0 ||
        roundedRate > static_cast<double>(maximumLoopbackCaptureFrames)) return false;
    const auto search = static_cast<std::uint64_t>(roundedRate);
    const auto blockCapacity = static_cast<std::uint64_t>(preparation.blockCapacity);
    const auto doubledBlockCapacity = checkedAdd(blockCapacity, blockCapacity);
    if (!doubledBlockCapacity) return false;
    constexpr auto stimulusFrames = static_cast<std::uint64_t>(loopbackStimulusLength);
    const auto doubledStimulus = checkedAdd(stimulusFrames, stimulusFrames);
    if (!doubledStimulus) return false;
    const auto guard = std::max<std::uint64_t>(*doubledBlockCapacity, *doubledStimulus);
    const auto stimulusAndGuard = checkedAdd(stimulusFrames, guard);
    const auto stride = stimulusAndGuard ? checkedAdd(search, *stimulusAndGuard)
                                         : std::nullopt;
    if (!stride) return false;
    std::array<std::uint64_t, loopbackTrialCount> emitted{};
    for (std::size_t index = 0; index < emitted.size(); ++index) {
        if (index == 0) {
            emitted[index] = guard;
            continue;
        }
        const auto next = checkedAdd(emitted[index - 1], *stride);
        if (!next) return false;
        emitted[index] = *next;
    }
    const auto afterSearch = checkedAdd(emitted.back(), search);
    const auto afterStimulus = afterSearch ? checkedAdd(*afterSearch, stimulusFrames)
                                          : std::nullopt;
    const auto total = afterStimulus ? checkedAdd(*afterStimulus, guard) : std::nullopt;
    if (!total || *total == 0 || *total > maximumLoopbackCaptureFrames) return false;
    try {
        auto nextStimulus = makeMaximumLengthStimulus();
        std::vector<float> nextCapture(static_cast<std::size_t>(*total), 0.0F);
        stimulus_.swap(nextStimulus);
        capture_.swap(nextCapture);
    } catch (const std::bad_alloc&) {
        return false;
    }
    emittedFrames_ = emitted;
    blockCapacity_ = preparation.blockCapacity;
    inputOrdinal_ = preparation.inputOrdinal;
    outputOrdinal_ = preparation.outputOrdinal;
    searchWindowFrames_ = search;
    preRollFrames_ = guard;
    frameIndex_ = 0;
    terminalPeak_ = 0.0F;
    status_.store(RealtimeLoopbackProbeStatus::prepared, std::memory_order_release);
    return true;
}

void RealtimeLoopbackProbe::clearOutput(AudioBlockView output) noexcept {
    if (output.channels == nullptr) return;
    for (std::size_t channel = 0; channel < output.channelCount; ++channel) {
        if (output.channels[channel] != nullptr)
            std::fill_n(output.channels[channel], output.frameCount, 0.0F);
    }
}

void RealtimeLoopbackProbe::fail(RealtimeLoopbackProbeStatus reason) noexcept {
    auto state = status_.load(std::memory_order_acquire);
    while (state == RealtimeLoopbackProbeStatus::prepared ||
           state == RealtimeLoopbackProbeStatus::running) {
        if (status_.compare_exchange_weak(state, reason, std::memory_order_release,
                                          std::memory_order_acquire)) return;
    }
}

void RealtimeLoopbackProbe::completeIfRunning() noexcept {
    auto expected = RealtimeLoopbackProbeStatus::running;
    static_cast<void>(status_.compare_exchange_strong(
        expected, RealtimeLoopbackProbeStatus::captured,
        std::memory_order_release, std::memory_order_acquire));
}

void RealtimeLoopbackProbe::processBlock(ConstAudioBlockView input,
                                         AudioBlockView output) noexcept {
    auto state = status_.load(std::memory_order_acquire);
    if (state == RealtimeLoopbackProbeStatus::prepared) {
        auto expected = state;
        if (status_.compare_exchange_strong(expected, RealtimeLoopbackProbeStatus::running,
                                            std::memory_order_acq_rel,
                                            std::memory_order_acquire)) {
            state = RealtimeLoopbackProbeStatus::running;
        } else {
            state = expected;
        }
    }
    if (state != RealtimeLoopbackProbeStatus::running) return;
    if (output.frameCount > blockCapacity_) {
        clearOutput(output);
        fail(RealtimeLoopbackProbeStatus::capacityExceeded);
        return;
    }
    if (input.frameCount != output.frameCount || input.channels == nullptr ||
        output.channels == nullptr || inputOrdinal_ >= input.channelCount ||
        outputOrdinal_ >= output.channelCount || input.channels[inputOrdinal_] == nullptr ||
        output.channels[outputOrdinal_] == nullptr) {
        clearOutput(output);
        fail(RealtimeLoopbackProbeStatus::deviceError);
        return;
    }

    const auto remaining = frameIndex_ < capture_.size()
        ? capture_.size() - static_cast<std::size_t>(frameIndex_) : 0U;
    const auto count = std::min(output.frameCount, remaining);
    bool clipped{};
    float blockPeak{};
    for (std::size_t frame = 0; frame < count; ++frame) {
        const auto sample = input.channels[inputOrdinal_][frame];
        capture_[static_cast<std::size_t>(frameIndex_) + frame] = sample;
        if (std::isfinite(sample)) blockPeak = std::max(blockPeak, std::abs(sample));
        clipped = clipped || !std::isfinite(sample) || std::abs(sample) >= clippingThreshold;
    }
    clearOutput(output);
    if (clipped) {
        terminalPeak_ = blockPeak;
        fail(RealtimeLoopbackProbeStatus::inputClipped);
        return;
    }

    auto* selectedOutput = output.channels[outputOrdinal_];
    const auto blockEnd = frameIndex_ + count;
    for (const auto emitted : emittedFrames_) {
        const auto stimulusEnd = emitted + stimulus_.size();
        const auto overlapStart = std::max(frameIndex_, emitted);
        const auto overlapEnd = std::min(blockEnd, stimulusEnd);
        if (overlapStart >= overlapEnd) continue;
        const auto outputOffset = static_cast<std::size_t>(overlapStart - frameIndex_);
        const auto stimulusOffset = static_cast<std::size_t>(overlapStart - emitted);
        const auto overlapCount = static_cast<std::size_t>(overlapEnd - overlapStart);
        std::copy_n(stimulus_.data() + stimulusOffset, overlapCount,
                    selectedOutput + outputOffset);
    }
    frameIndex_ += count;
    if (frameIndex_ >= capture_.size()) completeIfRunning();
}

void RealtimeLoopbackProbe::cancel() noexcept {
    fail(RealtimeLoopbackProbeStatus::cancelled);
}

void RealtimeLoopbackProbe::invalidateConfiguration() noexcept {
    fail(RealtimeLoopbackProbeStatus::configurationChanged);
}

void RealtimeLoopbackProbe::failDevice() noexcept {
    fail(RealtimeLoopbackProbeStatus::deviceError);
}

void RealtimeLoopbackProbe::reset() noexcept {
    if (active()) return;
    frameIndex_ = 0;
    terminalPeak_ = 0.0F;
    status_.store(RealtimeLoopbackProbeStatus::idle, std::memory_order_release);
}

bool RealtimeLoopbackProbe::active() const noexcept {
    const auto value = status();
    return value == RealtimeLoopbackProbeStatus::prepared ||
           value == RealtimeLoopbackProbeStatus::running;
}

RealtimeLoopbackProbeStatus RealtimeLoopbackProbe::status() const noexcept {
    return status_.load(std::memory_order_acquire);
}

std::span<const float> RealtimeLoopbackProbe::capturedSamples() const noexcept {
    return capture_;
}

std::span<const float> RealtimeLoopbackProbe::stimulus() const noexcept {
    return stimulus_;
}

LoopbackLatencyReadModel analyseLoopbackLatency(
    std::span<const float> captured, std::span<const float> stimulus,
    const std::array<std::uint64_t, loopbackTrialCount>& emittedFrames,
    std::uint64_t searchWindowFrames, std::uint64_t preRollFrames,
    LoopbackLatencyReadModel result) {
    result.status = LoopbackLatencyStatus::analysing;
    result.validTrials = 0;
    result.measuredRoundTripFrames.reset();
    result.minimumFrames.reset();
    result.maximumFrames.reset();
    result.jitterFrames.reset();
    result.residualFrames.reset();

    double templateEnergy{};
    for (const auto sample : stimulus)
        templateEnergy += static_cast<double>(sample) * sample;
    if (stimulus.empty() || templateEnergy <= 0.0 || captured.empty()) {
        result.status = LoopbackLatencyStatus::failed;
        result.diagnostic = "Loopback stimulus or capture is empty";
        return result;
    }
    const auto baselineCount = static_cast<std::size_t>(
        std::min<std::uint64_t>(preRollFrames, captured.size()));
    const auto noiseRms = rms(captured.first(baselineCount));
    std::vector<double> captureEnergyPrefix(captured.size() + 1U, 0.0);
    for (std::size_t index = 0; index < captured.size(); ++index) {
        const auto sample = static_cast<double>(captured[index]);
        captureEnergyPrefix[index + 1U] = captureEnergyPrefix[index] + sample * sample;
    }
    std::vector<std::uint64_t> valid;
    valid.reserve(loopbackTrialCount);

    std::uint64_t minimumLag{};
    auto maximumLag = searchWindowFrames;
    if (result.configuration.reportedRoundTripFrames &&
        std::isfinite(result.configuration.sampleRateHz) &&
        result.configuration.sampleRateHz > 0.0) {
        const auto quarterSecond = static_cast<std::uint64_t>(
            std::ceil(result.configuration.sampleRateHz * 0.25));
        const auto broadMargin = std::max<std::uint64_t>(4096U, quarterSecond);
        const auto reported = *result.configuration.reportedRoundTripFrames;
        minimumLag = reported > broadMargin ? reported - broadMargin : 0U;
        const auto upper = checkedAdd(reported, broadMargin);
        const auto exclusiveUpper = upper ? checkedAdd(*upper, 1U) : std::nullopt;
        maximumLag = exclusiveUpper
            ? std::min(searchWindowFrames, *exclusiveUpper) : searchWindowFrames;
        if (minimumLag >= maximumLag) {
            minimumLag = 0;
            maximumLag = searchWindowFrames;
        }
    }

    for (std::size_t trialIndex = 0; trialIndex < loopbackTrialCount; ++trialIndex) {
        auto& trial = result.trials[trialIndex];
        trial = {};
        trial.emittedStartDeviceFrame = emittedFrames[trialIndex];
        const auto start = emittedFrames[trialIndex];
        const auto searchStart = checkedAdd(start, minimumLag);
        const auto desiredEnd = checkedAdd(start, maximumLag);
        if (!searchStart || !desiredEnd || *searchStart >= captured.size()) {
            trial.quality = LoopbackTrialQuality::capacityExceeded;
            continue;
        }
        const auto maximumCandidate = std::min<std::uint64_t>(
            *desiredEnd, captured.size() >= stimulus.size()
                ? captured.size() - stimulus.size() + 1U : 0U);
        if (maximumCandidate <= *searchStart) {
            trial.quality = LoopbackTrialQuality::capacityExceeded;
            continue;
        }
        bool clipped{};
        float windowPeak{};
        const auto extendedEnd = checkedAdd(*desiredEnd, stimulus.size());
        if (!extendedEnd) {
            trial.quality = LoopbackTrialQuality::capacityExceeded;
            continue;
        }
        const auto windowEnd = static_cast<std::size_t>(std::min<std::uint64_t>(
            *extendedEnd, captured.size()));
        for (std::size_t index = static_cast<std::size_t>(start);
             index < windowEnd; ++index) {
            const auto sample = captured[index];
            windowPeak = std::max(windowPeak, std::abs(sample));
            clipped = clipped || !std::isfinite(sample) ||
                std::abs(sample) >= clippingThreshold;
        }
        trial.peak = windowPeak;
        trial.clipped = clipped;
        if (clipped) {
            trial.quality = LoopbackTrialQuality::clipped;
            continue;
        }

        double bestMagnitude{};
        double bestScore{};
        double bestDot{};
        std::uint64_t bestIndex{};
        const auto correlationAt = [&](std::uint64_t candidate) {
            double dot{};
            const auto candidateStart = static_cast<std::size_t>(candidate);
            for (std::size_t frame = 0; frame < stimulus.size(); ++frame) {
                dot += static_cast<double>(captured[candidateStart + frame]) *
                       static_cast<double>(stimulus[frame]);
            }
            const auto candidateEnergy =
                captureEnergyPrefix[candidateStart + stimulus.size()] -
                captureEnergyPrefix[candidateStart];
            const auto denominator = std::sqrt(templateEnergy * candidateEnergy);
            const auto score = denominator > 0.0
                ? std::abs(dot) / denominator : 0.0;
            return std::array<double, 3>{std::abs(dot), score, dot};
        };
        for (auto candidate = *searchStart; candidate < maximumCandidate; ++candidate) {
            const auto point = correlationAt(candidate);
            if (point[0] > bestMagnitude) {
                bestMagnitude = point[0];
                bestScore = point[1];
                bestDot = point[2];
                bestIndex = candidate;
            }
        }
        double secondMagnitude{};
        double secondScore{};
        for (auto candidate = *searchStart; candidate < maximumCandidate; ++candidate) {
            const auto distance = bestIndex > candidate ? bestIndex - candidate
                                                        : candidate - bestIndex;
            if (distance <= correlationExclusionRadius) continue;
            const auto point = correlationAt(candidate);
            if (point[0] > secondMagnitude) {
                secondMagnitude = point[0];
                secondScore = point[1];
            }
        }
        trial.correlation = bestScore;
        trial.secondCorrelation = secondScore;
        trial.ambiguityRatio = bestMagnitude > 0.0
            ? secondMagnitude / bestMagnitude : 0.0;
        trial.polarityInverted = bestDot < 0.0;
        const auto bestStart = static_cast<std::size_t>(bestIndex);
        const auto candidateEnergy = captureEnergyPrefix[bestStart + stimulus.size()] -
                                     captureEnergyPrefix[bestStart];
        const auto candidatePower = candidateEnergy /
                                    static_cast<double>(stimulus.size());
        const auto noisePower = noiseRms * noiseRms;
        const auto signalPower = std::max(0.0, candidatePower - noisePower);
        if (noisePower > 0.0 && signalPower > 0.0)
            trial.signalToNoiseDb = 10.0 * std::log10(signalPower / noisePower);
        else if (signalPower > 0.0)
            trial.signalToNoiseDb = std::numeric_limits<double>::infinity();
        else
            trial.signalToNoiseDb = -std::numeric_limits<double>::infinity();
        if (windowPeak < absoluteMinimumSignal ||
            trial.signalToNoiseDb < minimumSignalToNoiseDb) {
            trial.quality = LoopbackTrialQuality::signalTooLow;
            continue;
        }
        if (bestScore < minimumNormalizedMatchedPeak) {
            trial.quality = LoopbackTrialQuality::notFound;
            continue;
        }
        if (trial.ambiguityRatio >= maximumAmbiguousRatio) {
            trial.quality = LoopbackTrialQuality::ambiguous;
            continue;
        }
        if (bestIndex < start) {
            trial.quality = LoopbackTrialQuality::notFound;
            continue;
        }
        trial.returnedStartDeviceFrame = bestIndex;
        trial.measuredFrames = bestIndex - start;
        trial.quality = LoopbackTrialQuality::valid;
        valid.push_back(*trial.measuredFrames);
    }

    result.validTrials = valid.size();
    if (valid.size() < loopbackMinimumValidTrials) {
        result.status = LoopbackLatencyStatus::failed;
        result.diagnostic = "Fewer than three loopback trials were valid";
        return result;
    }
    std::sort(valid.begin(), valid.end());
    result.minimumFrames = valid.front();
    result.maximumFrames = valid.back();
    result.jitterFrames = valid.back() - valid.front();
    result.measuredRoundTripFrames = valid[valid.size() / 2U];
    if (result.configuration.reportedRoundTripFrames &&
        *result.measuredRoundTripFrames <=
            static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) &&
        *result.configuration.reportedRoundTripFrames <=
            static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
        result.residualFrames =
            static_cast<std::int64_t>(*result.measuredRoundTripFrames) -
            static_cast<std::int64_t>(*result.configuration.reportedRoundTripFrames);
    }
    result.status = LoopbackLatencyStatus::completed;
    result.diagnostic = "Physical loopback measurement completed";
    return result;
}

const char* loopbackStatusName(LoopbackLatencyStatus status) noexcept {
    switch (status) {
    case LoopbackLatencyStatus::idle: return "Idle";
    case LoopbackLatencyStatus::preparing: return "Preparing";
    case LoopbackLatencyStatus::running: return "Running";
    case LoopbackLatencyStatus::analysing: return "Analysing";
    case LoopbackLatencyStatus::completed: return "Completed";
    case LoopbackLatencyStatus::failed: return "Failed";
    case LoopbackLatencyStatus::cancelled: return "Cancelled";
    case LoopbackLatencyStatus::invalidated: return "Configuration Changed";
    }
    return "Unknown";
}

const char* loopbackQualityName(LoopbackTrialQuality quality) noexcept {
    switch (quality) {
    case LoopbackTrialQuality::pending: return "Pending";
    case LoopbackTrialQuality::valid: return "Valid";
    case LoopbackTrialQuality::signalTooLow: return "Signal Too Low";
    case LoopbackTrialQuality::clipped: return "Clipped";
    case LoopbackTrialQuality::ambiguous: return "Ambiguous";
    case LoopbackTrialQuality::notFound: return "Not Found";
    case LoopbackTrialQuality::configurationChanged: return "Configuration Changed";
    case LoopbackTrialQuality::cancelled: return "Cancelled";
    case LoopbackTrialQuality::deviceError: return "Device Error";
    case LoopbackTrialQuality::capacityExceeded: return "Capacity Exceeded";
    }
    return "Unknown";
}

} // namespace vitadaw::audio
