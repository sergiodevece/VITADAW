#pragma once

#include "vitadaw/audio/AudioDeviceState.h"
#include "vitadaw/commands/CommandDispatcher.h"

#include <juce_gui_basics/juce_gui_basics.h>

namespace vitadaw::platform::juce_adapter {

class AudioStatusComponent;

class MainWindow final : public juce::DocumentWindow {
public:
    MainWindow(const audio::AudioDeviceState& initialAudioState,
               commands::ICommandDispatcher& commandDispatcher);
    void closeButtonPressed() override;
    void setAudioDeviceState(const audio::AudioDeviceState& state);

private:
    AudioStatusComponent* content_{};
};

} // namespace vitadaw::platform::juce_adapter
