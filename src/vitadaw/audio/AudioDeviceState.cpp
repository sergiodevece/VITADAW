#include "vitadaw/audio/AudioDeviceState.h"

#include <cmath>
#include <utility>

namespace vitadaw::audio {

const AudioDeviceState& AudioDeviceStateModel::state() const noexcept {
    return state_;
}

void AudioDeviceStateModel::markActive(AudioDeviceInfo info) {
    if (info.outputDeviceName.empty() || !std::isfinite(info.sampleRate) ||
        info.sampleRate <= 0.0 ||
        info.bufferSizeFrames == 0 || info.availableOutputChannels == 0) {
        markError("Invalid active audio device configuration");
        return;
    }

    state_ = {AudioDeviceStatus::active, std::move(info), {}};
}

void AudioDeviceStateModel::markError(std::string message) {
    if (message.empty()) {
        message = "Unknown audio device error";
    }
    state_ = {AudioDeviceStatus::error, {}, std::move(message)};
}

void AudioDeviceStateModel::markClosed() {
    state_ = {};
}

} // namespace vitadaw::audio
