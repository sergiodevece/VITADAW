#include "vitadaw/platform/juce/MainWindow.h"

namespace vitadaw::platform::juce_adapter {

class AudioStatusComponent final : public juce::Component {
public:
    AudioStatusComponent(commands::ICommandDispatcher& commandDispatcher,
                         std::vector<tracks::TrackId> audioTracks)
        : commandDispatcher_(commandDispatcher) {
        statusLabel_.setColour(juce::Label::textColourId, juce::Colours::white);
        statusLabel_.setJustificationType(juce::Justification::centredLeft);
        addAndMakeVisible(statusLabel_);

        resultLabel_.setColour(juce::Label::textColourId, juce::Colours::white);
        addAndMakeVisible(resultLabel_);

        transportLabel_.setColour(juce::Label::textColourId, juce::Colours::white);
        addAndMakeVisible(transportLabel_);

        for (std::size_t index = 0; index < audioTracks.size(); ++index) {
            auto controls = std::make_unique<TrackControls>();
            controls->track = audioTracks[index];
            controls->name.setText(
                "Track " + juce::String(static_cast<int>(index + 1)),
                juce::NotificationType::dontSendNotification);
            controls->name.setColour(juce::Label::textColourId,
                                     juce::Colours::white);
            controls->gain.setRange(mixer::GainDb::silence,
                                    mixer::GainDb::maximum, 0.1);
            controls->gain.setValue(0.0,
                                    juce::NotificationType::dontSendNotification);
            controls->gain.setTextValueSuffix(" dB");
            controls->pan.setRange(mixer::Pan::left, mixer::Pan::right, 0.01);
            controls->pan.setValue(0.0,
                                   juce::NotificationType::dontSendNotification);
            controls->gain.onValueChange = [this, raw = controls.get()] {
                dispatch(commands::SetTrackGain{
                    raw->track,
                    mixer::GainDb{static_cast<float>(raw->gain.getValue())}});
            };
            controls->pan.onValueChange = [this, raw = controls.get()] {
                dispatch(commands::SetTrackPan{
                    raw->track,
                    mixer::Pan{static_cast<float>(raw->pan.getValue())}});
            };
            controls->mute.onClick = [this, raw = controls.get()] {
                dispatch(commands::SetTrackMute{raw->track,
                                                raw->mute.getToggleState()});
            };
            controls->solo.onClick = [this, raw = controls.get()] {
                dispatch(commands::SetTrackSolo{raw->track,
                                                raw->solo.getToggleState()});
            };
            auto button = std::make_unique<juce::TextButton>(
                "Load Audio " + juce::String(static_cast<int>(index + 1)));
            const auto track = audioTracks[index];
            button->onClick = [this, track] { chooseWav(track); };
            addAndMakeVisible(controls->name);
            addAndMakeVisible(controls->gain);
            addAndMakeVisible(controls->pan);
            addAndMakeVisible(controls->mute);
            addAndMakeVisible(controls->solo);
            addAndMakeVisible(*button);
            trackControls_.push_back(std::move(controls));
            loadButtons_.push_back(std::move(button));
        }
        masterGain_.setRange(mixer::GainDb::silence,
                             mixer::GainDb::maximum, 0.1);
        masterGain_.setValue(0.0, juce::NotificationType::dontSendNotification);
        masterGain_.setTextValueSuffix(" dB master");
        masterGain_.onValueChange = [this] {
            dispatch(commands::SetMasterGain{
                mixer::GainDb{static_cast<float>(masterGain_.getValue())}});
        };
        addAndMakeVisible(masterGain_);
        playButton_.onClick = [this] { dispatch(commands::Play{}); };
        stopButton_.onClick = [this] { dispatch(commands::Stop{}); };
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
        for (std::size_t index = 0; index < loadButtons_.size(); ++index) {
            auto row = bounds.removeFromTop(42);
            auto& controls = *trackControls_[index];
            controls.name.setBounds(row.removeFromLeft(64));
            loadButtons_[index]->setBounds(row.removeFromLeft(112));
            row.removeFromLeft(8);
            controls.gain.setBounds(row.removeFromLeft(180));
            controls.pan.setBounds(row.removeFromLeft(150));
            controls.mute.setBounds(row.removeFromLeft(64));
            controls.solo.setBounds(row.removeFromLeft(64));
        }
        bounds.removeFromTop(8);
        masterGain_.setBounds(bounds.removeFromTop(36).removeFromLeft(240));
        bounds.removeFromTop(8);
        auto transportButtons = bounds.removeFromTop(32);
        playButton_.setBounds(transportButtons.removeFromLeft(80));
        transportButtons.removeFromLeft(8);
        stopButton_.setBounds(transportButtons.removeFromLeft(80));
        bounds.removeFromTop(12);
        resultLabel_.setBounds(bounds.removeFromTop(32));
    }

private:
    struct TrackControls {
        tracks::TrackId track;
        juce::Label name;
        juce::Slider gain{juce::Slider::LinearHorizontal,
                          juce::Slider::TextBoxRight};
        juce::Slider pan{juce::Slider::LinearHorizontal,
                         juce::Slider::TextBoxRight};
        juce::ToggleButton mute{"Mute"};
        juce::ToggleButton solo{"Solo"};
    };

    void chooseWav(tracks::TrackId track) {
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
    std::vector<std::unique_ptr<juce::TextButton>> loadButtons_;
    std::vector<std::unique_ptr<TrackControls>> trackControls_;
    juce::Slider masterGain_{juce::Slider::LinearHorizontal,
                             juce::Slider::TextBoxRight};
    juce::TextButton playButton_{"Play"};
    juce::TextButton stopButton_{"Stop"};
    std::unique_ptr<juce::FileChooser> fileChooser_;
};

MainWindow::MainWindow(const audio::AudioDeviceState& initialAudioState,
                       commands::ICommandDispatcher& commandDispatcher,
                       std::vector<tracks::TrackId> audioTracks)
    : DocumentWindow("VitaDAW",
                     juce::Colours::darkgrey,
                     DocumentWindow::allButtons) {
    setUsingNativeTitleBar(true);
    content_ = new AudioStatusComponent(commandDispatcher,
                                        std::move(audioTracks));
    content_->setAudioDeviceState(initialAudioState);
    setContentOwned(content_, true);
    centreWithSize(900, 620);
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
