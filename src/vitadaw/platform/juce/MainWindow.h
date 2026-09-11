#pragma once

#include "vitadaw/audio/AudioDeviceState.h"
#include "vitadaw/commands/CommandDispatcher.h"
#include "vitadaw/timeline/Time.h"
#include "vitadaw/transport/TransportState.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <vector>

namespace vitadaw::platform::juce_adapter {

class AudioStatusComponent;

class MainWindow final : public juce::DocumentWindow {
public:
    MainWindow(const audio::AudioDeviceState& initialAudioState,
               commands::ICommandDispatcher& commandDispatcher,
               std::vector<tracks::TrackId> audioTracks);
    void closeButtonPressed() override;
    void setAudioDeviceState(const audio::AudioDeviceState& state);
    void setTransportState(const transport::TransportState& state,
                           timeline::SampleRate projectSampleRate);

private:
    AudioStatusComponent* content_{};
};

} // namespace vitadaw::platform::juce_adapter
