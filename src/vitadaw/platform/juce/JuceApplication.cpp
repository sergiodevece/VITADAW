#include "vitadaw/application/DawApplication.h"
#include "vitadaw/commands/CommandDispatcher.h"
#include "vitadaw/platform/juce/JuceAudioDeviceAdapter.h"
#include "vitadaw/platform/juce/MainWindow.h"
#include "vitadaw/platform/lifecycle/ApplicationShutdown.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <exception>
#include <memory>
#include <string>
#include <vector>

namespace vitadaw::platform::juce_adapter {

namespace {

enum class StartupPhase {
    notStarted,
    creatingAudioAdapter,
    audioAdapterCreated,
    applicationCreated,
    windowCreated,
    running,
    failed,
    shutDown,
};

[[nodiscard]] const char* phaseName(StartupPhase phase) noexcept {
    switch (phase) {
    case StartupPhase::notStarted:
        return "not started";
    case StartupPhase::creatingAudioAdapter:
        return "creating audio adapter";
    case StartupPhase::audioAdapterCreated:
        return "audio adapter created";
    case StartupPhase::applicationCreated:
        return "application/session created";
    case StartupPhase::windowCreated:
        return "main window created";
    case StartupPhase::running:
        return "running";
    case StartupPhase::failed:
        return "startup failed";
    case StartupPhase::shutDown:
        return "shut down";
    }
    return "unknown";
}

void logLifecycle(const juce::String& message) {
    juce::Logger::writeToLog("[VitaDAW lifecycle] " + message);
}

} // namespace

class VitaDawJuceApplication final : public juce::JUCEApplication,
                                     private juce::Timer {
public:
    [[nodiscard]] const juce::String getApplicationName() override {
        return "VitaDAW";
    }

    [[nodiscard]] const juce::String getApplicationVersion() override {
        return "0.6.6";
    }

    [[nodiscard]] bool moreThanOneInstanceAllowed() override {
        return false;
    }

    void initialise(const juce::String& commandLine) override {
        static_cast<void>(commandLine);

        phase_ = StartupPhase::creatingAudioAdapter;
        logLifecycle("startup begun");
        try {
            audioDevice_ = std::make_unique<JuceAudioDeviceAdapter>();
            phase_ = StartupPhase::audioAdapterCreated;
            const auto audioInitialised = audioDevice_->initialise();
            const auto deviceState = audioDevice_->state();
            if (audioInitialised) {
                logLifecycle("audio device initialised: " +
                             juce::String(deviceState.info.outputDeviceName));
            } else {
                logLifecycle("audio device unavailable: " +
                             juce::String(deviceState.errorMessage));
            }
            const auto projectSampleRate = timeline::SampleRate{
                deviceState.status == audio::AudioDeviceStatus::active
                    ? deviceState.info.sampleRate
                    : 48000.0};

            dawApplication_ = std::make_unique<application::DawApplication>(
                *audioDevice_, projectSampleRate);
            commandDispatcher_ =
                std::make_unique<commands::CommandDispatcher>(*dawApplication_);
            phase_ = StartupPhase::applicationCreated;
            mainWindow_ = std::make_unique<MainWindow>(
                audioDevice_->state(), *commandDispatcher_, *dawApplication_);
            phase_ = StartupPhase::windowCreated;
            mainWindow_->setTransportState(
                dawApplication_->transport(),
                dawApplication_->project().sampleRate());
            mainWindow_->setMeterState(dawApplication_->meterSnapshot());
            audioDevice_->setStateChangedCallback(
                [this](const audio::AudioDeviceState& state) {
                    if (mainWindow_ != nullptr) {
                        mainWindow_->setAudioDeviceState(state);
                    }
                });
            startTimerHz(30);
            phase_ = StartupPhase::running;
            logLifecycle("startup completed");

            if (!audioInitialised) {
                juce::AlertWindow::showMessageBoxAsync(
                    juce::MessageBoxIconType::WarningIcon,
                    "Audio device unavailable",
                    "VitaDAW could not initialise the default audio output device:\n" +
                        juce::String(audioDevice_->state().errorMessage));
            }
        } catch (const std::exception& exception) {
            const auto failedDuring = phase_;
            phase_ = StartupPhase::failed;
            logLifecycle("startup failed during " +
                         juce::String(phaseName(failedDuring)) + ": " +
                         juce::String(exception.what()));
            setApplicationReturnValue(1);
            quit();
        } catch (...) {
            const auto failedDuring = phase_;
            phase_ = StartupPhase::failed;
            logLifecycle("startup failed during " +
                         juce::String(phaseName(failedDuring)) +
                         ": unknown exception");
            setApplicationReturnValue(1);
            quit();
        }
    }

    void shutdown() override {
        const auto phaseAtEntry = phase_;
        logLifecycle("shutdown entered from phase: " +
                     juce::String(phaseName(phaseAtEntry)));
        lifecycle::shutdownApplicationOwners(
            audioDevice_, mainWindow_, commandDispatcher_, dawApplication_,
            [this] { stopTimer(); });
        phase_ = StartupPhase::shutDown;
        logLifecycle(phaseAtEntry == StartupPhase::running
                         ? "normal shutdown completed"
                         : "partial/idempotent shutdown completed");
    }

    void systemRequestedQuit() override {
        quit();
    }

private:
    void timerCallback() override {
        if (audioDevice_ == nullptr || dawApplication_ == nullptr ||
            mainWindow_ == nullptr) {
            return;
        }

        audioDevice_->pollDeviceLifecycle();
        dawApplication_->synchroniseTransport();
        mainWindow_->setTransportState(dawApplication_->transport(),
                                       dawApplication_->project().sampleRate());
        mainWindow_->setMeterState(dawApplication_->meterSnapshot());
    }

    std::unique_ptr<JuceAudioDeviceAdapter> audioDevice_;
    std::unique_ptr<application::DawApplication> dawApplication_;
    std::unique_ptr<commands::CommandDispatcher> commandDispatcher_;
    std::unique_ptr<MainWindow> mainWindow_;
    StartupPhase phase_{StartupPhase::notStarted};
};

} // namespace vitadaw::platform::juce_adapter

START_JUCE_APPLICATION(vitadaw::platform::juce_adapter::VitaDawJuceApplication)
