#pragma once

#include "vitadaw/audio/LinearSmoother.h"
#include "vitadaw/processors/IAudioProcessor.h"

namespace vitadaw::processors {

class GainProcessor final : public IAudioProcessor {
public:
    explicit GainProcessor(float gainDb = 0.0F) noexcept;

    [[nodiscard]] bool prepare(const ProcessingFormat& format) override;
    void reset() noexcept override;
    void applyParameter(PreparedParameterEvent event) noexcept override;
    [[nodiscard]] ProcessStatus processBlock(
        const ProcessorProcessContext& context,
        audio::ConstAudioBlockView input,
        audio::AudioBlockView output) noexcept override;
    [[nodiscard]] ProcessingFrameCount latency() const noexcept override;
    [[nodiscard]] TailInfo tail() const noexcept override;
    [[nodiscard]] ProcessorCapabilities capabilities() const noexcept override;
    [[nodiscard]] std::size_t runtimeMemoryBytes() const noexcept override;

    [[nodiscard]] static bool isValidGainDb(float gainDb) noexcept;
    [[nodiscard]] static bool isValidPreparedLinearGain(float gain) noexcept;
    [[nodiscard]] static float gainDbToLinear(float gainDb) noexcept;

private:
    timeline::SampleRate sampleRate_;
    float desiredGainDb_{};
    float preparedLinearGain_{1.0F};
    audio::LinearSmoother gain_;
};

} // namespace vitadaw::processors
