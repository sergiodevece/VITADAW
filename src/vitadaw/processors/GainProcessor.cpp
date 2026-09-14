#include "vitadaw/processors/GainProcessor.h"

#include <algorithm>
#include <cmath>

namespace vitadaw::processors {
namespace {

constexpr double smoothingSeconds = 0.005;
constexpr float maximumPreparedLinearGain = 3.98107171F;

class InternalAudioProcessorFactory final : public IAudioProcessorFactory {
public:
    std::unique_ptr<IAudioProcessor> create(
        const ProcessorState& state) const override {
        if (state.type.identifier != internalGainProcessorType ||
            state.parameters.size() != 1 ||
            state.parameters.front().id != gainParameterId) {
            return {};
        }
        const auto gainDb = state.parameters.front().value;
        if (!GainProcessor::isValidGainDb(gainDb)) {
            return {};
        }
        return std::make_unique<GainProcessor>(gainDb);
    }
};

} // namespace

GainProcessor::GainProcessor(float gainDb) noexcept
    : desiredGainDb_(gainDb), preparedLinearGain_(gainDbToLinear(gainDb)) {
    gain_.reset(preparedLinearGain_);
}

bool GainProcessor::prepare(const ProcessingFormat& format) {
    if (!format.isValid() || !isValidGainDb(desiredGainDb_)) {
        return false;
    }
    sampleRate_ = format.sampleRate;
    preparedLinearGain_ = gainDbToLinear(desiredGainDb_);
    gain_.reset(preparedLinearGain_);
    return true;
}

void GainProcessor::reset() noexcept {
    gain_.reset(preparedLinearGain_);
}

void GainProcessor::applyParameter(PreparedParameterEvent event) noexcept {
    if (event.parameter != gainParameterId || event.frameOffset != 0 ||
        !isValidPreparedLinearGain(event.preparedValue)) {
        return;
    }
    preparedLinearGain_ = event.preparedValue;
    gain_.setTarget(preparedLinearGain_, sampleRate_, smoothingSeconds);
}

ProcessStatus GainProcessor::processBlock(
    const ProcessorProcessContext& context,
    audio::ConstAudioBlockView input,
    audio::AudioBlockView output) noexcept {
    const auto channels = std::min(input.channelCount, output.channelCount);
    const auto frames = std::min({input.frameCount, output.frameCount,
                                  context.frameCount});
    if (input.channels == nullptr || output.channels == nullptr ||
        channels == 0 || frames == 0) {
        return ProcessStatus::failed;
    }
    bool silent = true;
    for (std::size_t frame = 0; frame < frames; ++frame) {
        const auto gain = gain_.next();
        for (std::size_t channel = 0; channel < channels; ++channel) {
            const auto sample = input.channels[channel][frame] * gain;
            output.channels[channel][frame] = sample;
            silent = silent && sample == 0.0F;
        }
    }
    return silent ? ProcessStatus::silent : ProcessStatus::processed;
}

ProcessingFrameCount GainProcessor::latency() const noexcept { return {}; }

TailInfo GainProcessor::tail() const noexcept {
    return {TailKind::none, {}};
}

ProcessorCapabilities GainProcessor::capabilities() const noexcept {
    return {true, true, true, true};
}

std::size_t GainProcessor::runtimeMemoryBytes() const noexcept { return 0; }

bool GainProcessor::isValidGainDb(float gainDb) noexcept {
    return std::isfinite(gainDb) && gainDb >= -100.0F && gainDb <= 12.0F;
}

bool GainProcessor::isValidPreparedLinearGain(float gain) noexcept {
    return std::isfinite(gain) && gain >= 0.0F &&
           gain <= maximumPreparedLinearGain;
}

float GainProcessor::gainDbToLinear(float gainDb) noexcept {
    if (!isValidGainDb(gainDb) || gainDb <= -100.0F) {
        return 0.0F;
    }
    return std::pow(10.0F, gainDb / 20.0F);
}

const IAudioProcessorFactory& internalAudioProcessorFactory() noexcept {
    static const InternalAudioProcessorFactory factory;
    return factory;
}

} // namespace vitadaw::processors
