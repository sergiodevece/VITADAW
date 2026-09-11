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

        loadButton_.onClick = [this] { chooseWav(); };
        playButton_.onClick = [this] { dispatch(commands::Play{}); };
        stopButton_.onClick = [this] { dispatch(commands::Stop{}); };
        addAndMakeVisible(loadButton_);
        addAndMakeVisible(playButton_);
        addAndMakeVisible(stopButton_);
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
        auto buttons = bounds.removeFromTop(32);
        loadButton_.setBounds(buttons.removeFromLeft(120));
        buttons.removeFromLeft(8);
        playButton_.setBounds(buttons.removeFromLeft(80));
        buttons.removeFromLeft(8);
        stopButton_.setBounds(buttons.removeFromLeft(80));
        bounds.removeFromTop(12);
        resultLabel_.setBounds(bounds.removeFromTop(32));
    }

private:
    void chooseWav() {
        fileChooser_ = std::make_unique<juce::FileChooser>(
            "Select a WAV file", juce::File{}, "*.wav");
        constexpr auto flags = juce::FileBrowserComponent::openMode |
                               juce::FileBrowserComponent::canSelectFiles;
        fileChooser_->launchAsync(flags, [this](const juce::FileChooser& chooser) {
            const auto file = chooser.getResult();
            if (file.existsAsFile()) {
                const auto fullPath = file.getFullPathName();
#if JUCE_WINDOWS
                const std::filesystem::path nativePath{fullPath.toWideCharPointer()};
#else
                const std::filesystem::path nativePath{fullPath.toStdString()};
#endif
                dispatch(commands::LoadAudioFile{nativePath});
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
    juce::Label resultLabel_;
    juce::TextButton loadButton_{"Load WAV"};
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

} // namespace vitadaw::platform::juce_adapter
