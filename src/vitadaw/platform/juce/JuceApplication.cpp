#include "vitadaw/application/DawApplication.h"
#include "vitadaw/commands/CommandDispatcher.h"
#include "vitadaw/platform/juce/JuceAudioDeviceAdapter.h"
#include "vitadaw/platform/juce/MainWindow.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <memory>

namespace vitadaw::platform::juce_adapter {

class VitaDawJuceApplication final : public juce::JUCEApplication,
                                     private juce::Timer {
public:
    [[nodiscard]] const juce::String getApplicationName() override {
        return "VitaDAW";
    }

    [[nodiscard]] const juce::String getApplicationVersion() override {
        return "0.0.4";
    }

    [[nodiscard]] bool moreThanOneInstanceAllowed() override {
        return false;
    }

    void initialise(const juce::String& commandLine) override {
        static_cast<void>(commandLine);

        audioDevice_ = std::make_unique<JuceAudioDeviceAdapter>();
        const auto audioInitialised = audioDevice_->initialise();
        const auto deviceState = audioDevice_->state();
        const auto projectSampleRate = timeline::SampleRate{
            deviceState.status == audio::AudioDeviceStatus::active
                ? deviceState.info.sampleRate
                : 48000.0};

        dawApplication_ = std::make_unique<application::DawApplication>(
            *audioDevice_, projectSampleRate);
        commandDispatcher_ =
            std::make_unique<commands::CommandDispatcher>(*dawApplication_);
        mainWindow_ =
            std::make_unique<MainWindow>(audioDevice_->state(), *commandDispatcher_);
        mainWindow_->setTransportState(dawApplication_->transport(),
                                       dawApplication_->project().sampleRate());
        audioDevice_->setStateChangedCallback(
            [this](const audio::AudioDeviceState& state) {
                if (mainWindow_ != nullptr) {
                    mainWindow_->setAudioDeviceState(state);
                }
            });
        startTimerHz(30);

        if (!audioInitialised) {
            juce::AlertWindow::showMessageBoxAsync(
                juce::MessageBoxIconType::WarningIcon,
                "Audio device unavailable",
                "VitaDAW could not initialise the default audio output device:\n" +
                    juce::String(audioDevice_->state().errorMessage));
        }
    }

    void shutdown() override {
        stopTimer();
        mainWindow_.reset();
        commandDispatcher_.reset();
        dawApplication_.reset();
        audioDevice_->setStateChangedCallback({});
        audioDevice_.reset();
    }

    void systemRequestedQuit() override {
        quit();
    }

private:
    void timerCallback() override {
        if (dawApplication_ == nullptr || mainWindow_ == nullptr) {
            return;
        }

        dawApplication_->synchroniseTransport();
        mainWindow_->setTransportState(dawApplication_->transport(),
                                       dawApplication_->project().sampleRate());
    }

    std::unique_ptr<JuceAudioDeviceAdapter> audioDevice_;
    std::unique_ptr<application::DawApplication> dawApplication_;
    std::unique_ptr<commands::CommandDispatcher> commandDispatcher_;
    std::unique_ptr<MainWindow> mainWindow_;
};

} // namespace vitadaw::platform::juce_adapter

START_JUCE_APPLICATION(vitadaw::platform::juce_adapter::VitaDawJuceApplication)
