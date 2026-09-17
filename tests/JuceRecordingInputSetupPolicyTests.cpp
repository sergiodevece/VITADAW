#include "vitadaw/platform/juce/RecordingInputSetupPolicy.h"

#include <cstdlib>
#include <iostream>

namespace {

void check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

juce::AudioDeviceManager::AudioDeviceSetup outputOnlySetup() {
    juce::AudioDeviceManager::AudioDeviceSetup setup;
    setup.outputDeviceName = "MacBook Speakers";
    setup.outputChannels.setRange(0, 2, true);
    setup.useDefaultOutputChannels = false;
    return setup;
}

void lazyDefaultMonoSelection() {
    const auto previous = outputOnlySetup();
    const auto result = vitadaw::platform::juce_adapter::detail::makeRecordingInputSetup(
        previous, {"MacBook Microphone"}, 0, 1);
    check(result.success(), "mono lazy default input selection succeeds");
    check(result.setup.outputDeviceName == previous.outputDeviceName &&
              result.setup.outputChannels == previous.outputChannels &&
              !result.setup.useDefaultOutputChannels,
          "mono selection preserves existing output configuration");
    check(result.setup.inputDeviceName == "MacBook Microphone" &&
              result.setup.inputChannels[0] && !result.setup.inputChannels[1] &&
              !result.setup.useDefaultInputChannels,
          "mono selection opens only input channel one");
}

void stereoAndExistingInput() {
    auto previous = outputOnlySetup();
    previous.inputDeviceName = "Interface Input";
    const auto result = vitadaw::platform::juce_adapter::detail::makeRecordingInputSetup(
        previous, {"MacBook Microphone", "Interface Input"}, 0, 2);
    check(result.success(), "stereo selection succeeds");
    check(result.setup.inputDeviceName == "Interface Input" &&
              result.setup.inputChannels[0] && result.setup.inputChannels[1] &&
              !result.setup.inputChannels[2],
          "stereo retains a valid selected input and requests inputs one and two");
}

void unavailableInputAndChannelValidation() {
    const auto previous = outputOnlySetup();
    const auto unavailable = vitadaw::platform::juce_adapter::detail::makeRecordingInputSetup(
        previous, {}, -1, 1);
    check(!unavailable.success() && unavailable.setup == previous,
          "unavailable input leaves the previous output-only setup untouched");

    juce::BigInteger mono;
    mono.setBit(0);
    check(vitadaw::platform::juce_adapter::detail::recordingInputChannelsAreAvailableAndActive(1, mono, 1),
          "one available and active channel satisfies mono");
    check(!vitadaw::platform::juce_adapter::detail::recordingInputChannelsAreAvailableAndActive(1, mono, 2),
          "insufficient physical input channels rejects stereo");
    check(!vitadaw::platform::juce_adapter::detail::recordingInputChannelsAreAvailableAndActive(2, mono, 2),
          "inactive requested channel rejects stereo");
}

} // namespace

int main() {
    lazyDefaultMonoSelection();
    stereoAndExistingInput();
    unavailableInputAndChannelValidation();
    return 0;
}
