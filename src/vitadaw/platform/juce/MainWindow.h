#pragma once

#include "vitadaw/application/DawApplication.h"
#include "vitadaw/audio/AudioDeviceState.h"
#include "vitadaw/commands/CommandDispatcher.h"
#include "vitadaw/mixer/Metering.h"
#include "vitadaw/timeline/Time.h"
#include "vitadaw/transport/TransportState.h"

#include <juce_gui_basics/juce_gui_basics.h>

namespace vitadaw::platform::juce_adapter {

class AudioStatusComponent;

class MainWindow final : public juce::DocumentWindow {
public:
    MainWindow(const audio::AudioDeviceState& initialAudioState,
               commands::ICommandDispatcher& commandDispatcher,
               application::DawApplication& application);
    void closeButtonPressed() override;
    void setAudioDeviceState(const audio::AudioDeviceState& state);
    void setTransportState(const transport::TransportState& state,
                           timeline::SampleRate projectSampleRate);
    void setMeterState(const mixer::MeterSnapshot& meters);

private:
    commands::ICommandDispatcher& dispatcher_;
    application::DawApplication& application_;
    audio::AudioDeviceState audioState_;
    bool rebuildPending_{};
    // Non-owning observer. DocumentWindow exclusively owns the component passed
    // to setContentOwned(); this pointer is replaced synchronously on rebuild.
    AudioStatusComponent* content_{};
};

} // namespace vitadaw::platform::juce_adapter
