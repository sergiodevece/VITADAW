#pragma once

#include <juce_audio_devices/juce_audio_devices.h>

#include <string>

namespace vitadaw::platform::juce_adapter::detail {

struct RecordingInputSetupResult {
    juce::AudioDeviceManager::AudioDeviceSetup setup;
    std::string errorMessage;

    [[nodiscard]] bool success() const noexcept { return errorMessage.empty(); }
};

// Builds the non-RT input side of a recording setup. The caller supplies the
// already-scanned names from its current JUCE device type, so the existing
// output selection remains untouched and no global physical-device inference
// is made from the currently-open AudioIODevice.
[[nodiscard]] inline RecordingInputSetupResult makeRecordingInputSetup(
    const juce::AudioDeviceManager::AudioDeviceSetup& previous,
    const juce::StringArray& inputDeviceNames, int defaultInputIndex,
    int requestedChannels) {
    RecordingInputSetupResult result{previous, {}};
    if (requestedChannels < 1 || requestedChannels > 2) {
        result.errorMessage = "Only mono and stereo recording are supported";
        return result;
    }

    if (inputDeviceNames.indexOf(previous.inputDeviceName) < 0) {
        if (defaultInputIndex < 0 || defaultInputIndex >= inputDeviceNames.size()) {
            result.errorMessage = "No audio input device is available";
            return result;
        }
        result.setup.inputDeviceName = inputDeviceNames[defaultInputIndex];
    }

    result.setup.inputChannels.clear();
    result.setup.inputChannels.setRange(0, requestedChannels, true);
    result.setup.useDefaultInputChannels = false;
    return result;
}

[[nodiscard]] inline bool recordingInputChannelsAreAvailableAndActive(
    std::size_t availableChannels, const juce::BigInteger& activeChannels,
    int requestedChannels) noexcept {
    if (requestedChannels < 1 || availableChannels < static_cast<std::size_t>(requestedChannels))
        return false;
    for (int channel = 0; channel < requestedChannels; ++channel) {
        if (!activeChannels[channel]) return false;
    }
    return true;
}

} // namespace vitadaw::platform::juce_adapter::detail
