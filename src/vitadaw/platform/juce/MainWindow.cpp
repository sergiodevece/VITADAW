#include "vitadaw/platform/juce/MainWindow.h"

namespace vitadaw::platform::juce_adapter {

class AudioStatusComponent final : public juce::Component {
public:
    explicit AudioStatusComponent(commands::ICommandDispatcher& commandDispatcher)
        : commandDispatcher_(commandDispatcher) {
        statusLabel_.setColour(juce::Label::textColourId, juce::Colours::white);
        statusLabel_.setJustificationType(juce::Justification::centredLeft);
        addAndMakeVisible(statusLabel_);

        resultLabel_.setColour(juce::Label::textColourId, juce::Colours::white);
        addAndMakeVisible(resultLabel_);

        transportLabel_.setColour(juce::Label::textColourId, juce::Colours::white);
        addAndMakeVisible(transportLabel_);

        loadTrack1Button_.onClick = [this] {
            chooseWav(tracks::AudioTrackSlot::first);
        };
        loadTrack2Button_.onClick = [this] {
            chooseWav(tracks::AudioTrackSlot::second);
        };
        playButton_.onClick = [this] { dispatch(commands::Play{}); };
        stopButton_.onClick = [this] { dispatch(commands::Stop{}); };
        addAndMakeVisible(loadTrack1Button_);
        addAndMakeVisible(loadTrack2Button_);
        addAndMakeVisible(playButton_);
        addAndMakeVisible(stopButton_);
    }

    void setTransportState(const transport::TransportState& state,
                           timeline::SampleRate projectSampleRate) {
        const auto position = timeline::projectPositionToSeconds(
            state.position, projectSampleRate);
        const auto duration = timeline::projectFramesToSeconds(
            state.duration, projectSampleRate);
        juce::String text;
        text << "Transport: "
             << (state.playback == transport::PlaybackState::playing ? "Playing" : "Stopped")
             << " | " << juce::String(position.value, 3) << " / "
             << juce::String(duration.value, 3) << " s"
             << " | Project: " << juce::String(projectSampleRate.hertz(), 0) << " Hz";
        transportLabel_.setText(text, juce::NotificationType::dontSendNotification);
    }

    void setAudioDeviceState(const audio::AudioDeviceState& state) {
        juce::String text;

        if (state.status == audio::AudioDeviceStatus::active) {
            text << "Audio device: active\n"
                 << "Output: " << state.info.outputDeviceName << "\n"
                 << "Sample rate: " << juce::String(state.info.sampleRate, 0) << " Hz\n"
                 << "Buffer size: " << static_cast<int>(state.info.bufferSizeFrames)
                 << " frames\n"
                 << "Available input channels: "
                 << static_cast<int>(state.info.availableInputChannels) << "\n"
                 << "Available output channels: "
                 << static_cast<int>(state.info.availableOutputChannels);
        } else if (state.status == audio::AudioDeviceStatus::error) {
            text << "Audio device: error\n" << state.errorMessage;
        } else {
            text << "Audio device: closed";
        }

        statusLabel_.setText(text, juce::NotificationType::dontSendNotification);
    }

    void paint(juce::Graphics& graphics) override {
        graphics.fillAll(juce::Colours::darkgrey);
    }

    void resized() override {
        auto bounds = getLocalBounds().reduced(24);
        statusLabel_.setBounds(bounds.removeFromTop(150));
        bounds.removeFromTop(12);
        transportLabel_.setBounds(bounds.removeFromTop(32));
        bounds.removeFromTop(12);
        auto buttons = bounds.removeFromTop(32);
        loadTrack1Button_.setBounds(buttons.removeFromLeft(150));
        buttons.removeFromLeft(8);
        loadTrack2Button_.setBounds(buttons.removeFromLeft(150));
        buttons.removeFromLeft(8);
        playButton_.setBounds(buttons.removeFromLeft(80));
        buttons.removeFromLeft(8);
        stopButton_.setBounds(buttons.removeFromLeft(80));
        bounds.removeFromTop(12);
        resultLabel_.setBounds(bounds.removeFromTop(32));
    }

private:
    void chooseWav(tracks::AudioTrackSlot track) {
        fileChooser_ = std::make_unique<juce::FileChooser>(
            "Select a WAV file", juce::File{}, "*.wav");
        constexpr auto flags = juce::FileBrowserComponent::openMode |
                               juce::FileBrowserComponent::canSelectFiles;
        fileChooser_->launchAsync(flags, [this, track](const juce::FileChooser& chooser) {
            const auto file = chooser.getResult();
            if (file.existsAsFile()) {
                const auto fullPath = file.getFullPathName();
#if JUCE_WINDOWS
                const std::filesystem::path nativePath{fullPath.toWideCharPointer()};
#else
                const std::filesystem::path nativePath{fullPath.toStdString()};
#endif
                dispatch(commands::LoadAudioFile{nativePath, track});
            }
            fileChooser_.reset();
        });
    }

    void dispatch(const commands::Command& command) {
        const auto result = commandDispatcher_.dispatch(command);
        const auto prefix = result.status == commands::CommandStatus::accepted
                                ? juce::String{"OK: "}
                                : juce::String{"Error: "};
        resultLabel_.setText(prefix + juce::String(result.message),
                             juce::NotificationType::dontSendNotification);
    }

    commands::ICommandDispatcher& commandDispatcher_;
    juce::Label statusLabel_;
    juce::Label transportLabel_;
    juce::Label resultLabel_;
    juce::TextButton loadTrack1Button_{"Load Track 1 WAV"};
    juce::TextButton loadTrack2Button_{"Load Track 2 WAV"};
    juce::TextButton playButton_{"Play"};
    juce::TextButton stopButton_{"Stop"};
    std::unique_ptr<juce::FileChooser> fileChooser_;
};

MainWindow::MainWindow(const audio::AudioDeviceState& initialAudioState,
                       commands::ICommandDispatcher& commandDispatcher)
    : DocumentWindow("VitaDAW",
                     juce::Colours::darkgrey,
                     DocumentWindow::allButtons) {
    setUsingNativeTitleBar(true);
    content_ = new AudioStatusComponent(commandDispatcher);
    content_->setAudioDeviceState(initialAudioState);
    setContentOwned(content_, true);
    centreWithSize(800, 500);
    setResizable(true, false);
    setVisible(true);
}

void MainWindow::closeButtonPressed() {
    juce::JUCEApplicationBase::quit();
}

void MainWindow::setAudioDeviceState(const audio::AudioDeviceState& state) {
    content_->setAudioDeviceState(state);
}

void MainWindow::setTransportState(const transport::TransportState& state,
                                   timeline::SampleRate projectSampleRate) {
    content_->setTransportState(state, projectSampleRate);
}

} // namespace vitadaw::platform::juce_adapter
