#pragma once

#include "vitadaw/audio/AudioBlockView.h"
#include "vitadaw/processors/ProcessorState.h"
#include "vitadaw/timeline/Time.h"

#include <cstddef>
#include <cstdint>
#include <memory>

namespace vitadaw::processors {

enum class ChannelLayout : std::uint8_t { mono = 1, stereo = 2 };
enum class ProcessingMode : std::uint8_t { realtime, offline };
enum class TemporalDiscontinuity : std::uint8_t {
    continuous,
    hardDiscontinuity,
    loopWrap,
};

struct ProcessingFrameCount {
    std::uint64_t value{};

    bool operator==(const ProcessingFrameCount&) const = default;
};

struct ProcessingFormat {
    timeline::SampleRate sampleRate;
    std::size_t maximumBlockSize{};
    ChannelLayout channelLayout{ChannelLayout::stereo};
    ProcessingMode mode{ProcessingMode::realtime};

    [[nodiscard]] std::size_t channelCount() const noexcept {
        return static_cast<std::size_t>(channelLayout);
    }
    [[nodiscard]] bool isValid() const noexcept {
        return sampleRate.isValid() && maximumBlockSize > 0 &&
               (channelLayout == ChannelLayout::mono ||
                channelLayout == ChannelLayout::stereo);
    }
};

enum class TailKind : std::uint8_t { none, finite, infinite, unknown };

struct TailInfo {
    TailKind kind{TailKind::none};
    ProcessingFrameCount finiteLength;

    bool operator==(const TailInfo&) const = default;
};

struct ProcessorCapabilities {
    bool supportsInPlace{};
    bool supportsOutOfPlace{true};
    bool supportsMono{true};
    bool supportsStereo{true};
};

struct PreparedParameterEvent {
    ParameterId parameter;
    float preparedValue{};
    std::uint32_t frameOffset{};
};

struct ProcessorProcessContext {
    timeline::PreciseProjectFramePosition projectFrameStart;
    timeline::SampleRate processingSampleRate;
    std::size_t frameCount{};
    ProcessingMode mode{ProcessingMode::realtime};
    bool transportPlaying{};
    TemporalDiscontinuity discontinuity{TemporalDiscontinuity::continuous};
};

enum class ProcessStatus : std::uint8_t { processed, silent, failed };

class IAudioProcessor {
public:
    virtual ~IAudioProcessor() = default;

    // Called only while processing is quiescent. It may allocate or fail.
    [[nodiscard]] virtual bool prepare(const ProcessingFormat& format) = 0;
    virtual void reset() noexcept = 0;
    virtual void applyParameter(PreparedParameterEvent event) noexcept = 0;
    [[nodiscard]] virtual ProcessStatus processBlock(
        const ProcessorProcessContext& context,
        audio::ConstAudioBlockView input,
        audio::AudioBlockView output) noexcept = 0;
    [[nodiscard]] virtual ProcessingFrameCount latency() const noexcept = 0;
    [[nodiscard]] virtual TailInfo tail() const noexcept = 0;
    [[nodiscard]] virtual ProcessorCapabilities capabilities() const noexcept = 0;
    [[nodiscard]] virtual std::size_t runtimeMemoryBytes() const noexcept = 0;
};

class IAudioProcessorFactory {
public:
    virtual ~IAudioProcessorFactory() = default;
    [[nodiscard]] virtual std::unique_ptr<IAudioProcessor> create(
        const ProcessorState& state) const = 0;
};

[[nodiscard]] const IAudioProcessorFactory& internalAudioProcessorFactory()
    noexcept;

} // namespace vitadaw::processors
