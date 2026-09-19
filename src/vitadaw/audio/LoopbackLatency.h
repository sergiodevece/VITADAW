#pragma once

#include "vitadaw/audio/AudioBlockView.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace vitadaw::audio {

inline constexpr std::size_t loopbackTrialCount = 5;
inline constexpr std::size_t loopbackMinimumValidTrials = 3;
inline constexpr float loopbackStimulusAmplitude = 0.0630957344F; // -24 dBFS.
inline constexpr std::size_t loopbackStimulusLength = 1023;

enum class LoopbackLatencyStatus : std::uint8_t {
    idle,
    preparing,
    running,
    analysing,
    completed,
    failed,
    cancelled,
    invalidated,
};

enum class LoopbackTrialQuality : std::uint8_t {
    pending,
    valid,
    signalTooLow,
    clipped,
    ambiguous,
    notFound,
    configurationChanged,
    cancelled,
    deviceError,
    capacityExceeded,
};

struct LoopbackLatencyRequest {
    // Zero-based physical channel indices in the logical JUCE device.
    std::uint32_t inputChannel{};
    std::uint32_t outputChannel{};
};

struct LoopbackLatencyConfiguration {
    std::uint64_t generation{};
    std::string deviceName;
    std::string inputDeviceName;
    std::string outputDeviceName;
    std::uint32_t inputChannel{};
    std::uint32_t outputChannel{};
    std::vector<std::uint32_t> activeInputChannels;
    std::vector<std::uint32_t> activeOutputChannels;
    std::uint32_t inputCallbackOrdinal{};
    std::uint32_t outputCallbackOrdinal{};
    std::string inputChannelName;
    std::string outputChannelName;
    double sampleRateHz{};
    std::uint32_t bufferSizeFrames{};
    std::optional<std::uint32_t> reportedInputFrames;
    std::optional<std::uint32_t> reportedOutputFrames;
    std::optional<std::uint64_t> reportedRoundTripFrames;
    std::uint32_t stimulusVersion{2};
};

struct LoopbackLatencyTrial {
    LoopbackTrialQuality quality{LoopbackTrialQuality::pending};
    std::uint64_t emittedStartDeviceFrame{};
    std::optional<std::uint64_t> returnedStartDeviceFrame;
    std::optional<std::uint64_t> measuredFrames;
    double correlation{};
    double secondCorrelation{};
    double ambiguityRatio{};
    double signalToNoiseDb{};
    float peak{};
    bool polarityInverted{};
    bool clipped{};
};

struct LoopbackLatencyReadModel {
    std::uint64_t sessionId{};
    LoopbackLatencyStatus status{LoopbackLatencyStatus::idle};
    LoopbackLatencyConfiguration configuration;
    std::array<LoopbackLatencyTrial, loopbackTrialCount> trials{};
    std::size_t validTrials{};
    std::optional<std::uint64_t> measuredRoundTripFrames;
    std::optional<std::uint64_t> minimumFrames;
    std::optional<std::uint64_t> maximumFrames;
    std::optional<std::uint64_t> jitterFrames;
    std::optional<std::int64_t> residualFrames;
    bool configurationStillCurrent{};
    std::string diagnostic;

    [[nodiscard]] bool busy() const noexcept {
        return status == LoopbackLatencyStatus::preparing ||
               status == LoopbackLatencyStatus::running ||
               status == LoopbackLatencyStatus::analysing;
    }
};

struct LoopbackLatencyControlResult {
    bool success{};
    std::string errorMessage;
};

struct LoopbackProbePreparation {
    double sampleRateHz{};
    std::size_t blockCapacity{};
    std::size_t inputOrdinal{};
    std::size_t outputOrdinal{};
};

enum class RealtimeLoopbackProbeStatus : std::uint8_t {
    idle,
    prepared,
    running,
    captured,
    cancelled,
    configurationChanged,
    inputClipped,
    deviceError,
    capacityExceeded,
};

// A diagnostic-only RT producer. All storage and scheduling are prepared on the
// control thread; processBlock performs only bounded copies, clears and atomics.
class RealtimeLoopbackProbe {
public:
    [[nodiscard]] bool prepare(const LoopbackProbePreparation&);
    void processBlock(ConstAudioBlockView input, AudioBlockView output) noexcept;
    void cancel() noexcept;
    void invalidateConfiguration() noexcept;
    void failDevice() noexcept;
    void reset() noexcept;

    [[nodiscard]] bool active() const noexcept;
    [[nodiscard]] RealtimeLoopbackProbeStatus status() const noexcept;
    [[nodiscard]] std::span<const float> capturedSamples() const noexcept;
    [[nodiscard]] std::span<const float> stimulus() const noexcept;
    [[nodiscard]] const std::array<std::uint64_t, loopbackTrialCount>&
        emittedFrames() const noexcept { return emittedFrames_; }
    [[nodiscard]] std::uint64_t searchWindowFrames() const noexcept {
        return searchWindowFrames_;
    }
    [[nodiscard]] std::uint64_t preRollFrames() const noexcept {
        return preRollFrames_;
    }
    [[nodiscard]] float terminalPeak() const noexcept { return terminalPeak_; }

private:
    friend struct LoopbackProbeTestAccess;
    void fail(RealtimeLoopbackProbeStatus) noexcept;
    void completeIfRunning() noexcept;
    static void clearOutput(AudioBlockView) noexcept;

    std::vector<float> stimulus_;
    std::vector<float> capture_;
    std::array<std::uint64_t, loopbackTrialCount> emittedFrames_{};
    std::size_t blockCapacity_{};
    std::size_t inputOrdinal_{};
    std::size_t outputOrdinal_{};
    std::uint64_t searchWindowFrames_{};
    std::uint64_t preRollFrames_{};
    std::uint64_t frameIndex_{};
    float terminalPeak_{};
    std::atomic<RealtimeLoopbackProbeStatus> status_{RealtimeLoopbackProbeStatus::idle};
};

[[nodiscard]] LoopbackLatencyReadModel analyseLoopbackLatency(
    std::span<const float> captured,
    std::span<const float> stimulus,
    const std::array<std::uint64_t, loopbackTrialCount>& emittedFrames,
    std::uint64_t searchWindowFrames,
    std::uint64_t preRollFrames,
    LoopbackLatencyReadModel seed);

[[nodiscard]] const char* loopbackStatusName(LoopbackLatencyStatus) noexcept;
[[nodiscard]] const char* loopbackQualityName(LoopbackTrialQuality) noexcept;

} // namespace vitadaw::audio
