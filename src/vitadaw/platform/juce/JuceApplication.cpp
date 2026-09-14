#include "vitadaw/application/DawApplication.h"
#include "vitadaw/commands/CommandDispatcher.h"
#include "vitadaw/platform/juce/JuceAudioDeviceAdapter.h"
#include "vitadaw/platform/juce/MainWindow.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <array>
#include <memory>
#include <string>
#include <vector>

namespace vitadaw::platform::juce_adapter {

class VitaDawJuceApplication final : public juce::JUCEApplication,
                                     private juce::Timer {
public:
    [[nodiscard]] const juce::String getApplicationName() override {
        return "VitaDAW";
    }

    [[nodiscard]] const juce::String getApplicationVersion() override {
        return "0.5.0";
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
        const std::array trackNames{"Snare", "Kick", "Guitar", "Stereo 4"};
        for (std::size_t index = 0; index < trackNames.size(); ++index) {
            static_cast<void>(commandDispatcher_->dispatch(
                commands::AddAudioTrack{
                    trackNames[index],
                    index == 3 ? media::AudioChannelLayout::stereo
                               : media::AudioChannelLayout::mono}));
        }
        static_cast<void>(commandDispatcher_->dispatch(
            commands::AddBus{"Drum Bus"}));
        static_cast<void>(commandDispatcher_->dispatch(
            commands::AddBus{"Music Bus"}));
        static_cast<void>(commandDispatcher_->dispatch(
            commands::AddBus{"Plate Bus"}));
        static_cast<void>(commandDispatcher_->dispatch(
            commands::AddBus{"Parallel Bus"}));
        static_cast<void>(commandDispatcher_->dispatch(
            commands::AddBus{"Room Bus"}));
        const auto& tracks = dawApplication_->project().tracks();
        const auto& buses = dawApplication_->project().routing().buses();
        if (tracks.size() >= 3 && buses.size() >= 5) {
            static_cast<void>(commandDispatcher_->dispatch(
                commands::SetTrackOutputDestination{
                    tracks[0].id,
                    routing::TrackOutputDestination::toBus(buses[0].id)}));
            static_cast<void>(commandDispatcher_->dispatch(
                commands::SetTrackOutputDestination{
                    tracks[1].id,
                    routing::TrackOutputDestination::toBus(buses[0].id)}));
            static_cast<void>(commandDispatcher_->dispatch(
                commands::SetTrackOutputDestination{
                    tracks[2].id,
                    routing::TrackOutputDestination::toBus(buses[1].id)}));
            static_cast<void>(commandDispatcher_->dispatch(
                commands::SetBusOutputDestination{
                    buses[0].id,
                    routing::OutputDestination::toBus(buses[1].id)}));
            static_cast<void>(commandDispatcher_->dispatch(
                commands::AddTrackSend{
                    tracks[0].id, buses[2].id,
                    routing::SendTapPoint::preFaderPrePan,
                    mixer::GainDb{-6.0F}}));
            static_cast<void>(commandDispatcher_->dispatch(
                commands::AddBusSend{
                    buses[0].id, buses[3].id,
                    routing::SendTapPoint::preFaderPrePan,
                    mixer::GainDb{-6.0F}}));
            static_cast<void>(commandDispatcher_->dispatch(
                commands::AddBusSend{
                    buses[1].id, buses[4].id,
                    routing::SendTapPoint::postFaderPostPan,
                    mixer::GainDb{-6.0F}}));
            static_cast<void>(commandDispatcher_->dispatch(
                commands::AddProcessor{
                    tracks[0].id,
                    {processors::internalGainProcessorType}}));
            static_cast<void>(commandDispatcher_->dispatch(
                commands::AddProcessor{
                    buses[0].id,
                    {processors::internalGainProcessorType}}));
            static_cast<void>(commandDispatcher_->dispatch(
                commands::AddProcessor{
                    processors::MasterTarget{},
                    {processors::internalGainProcessorType}}));
        }
        mainWindow_ = std::make_unique<MainWindow>(
            audioDevice_->state(), *commandDispatcher_, *dawApplication_);
        mainWindow_->setTransportState(dawApplication_->transport(),
                                       dawApplication_->project().sampleRate());
        mainWindow_->setMeterState(dawApplication_->meterSnapshot());
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
};

} // namespace vitadaw::platform::juce_adapter

START_JUCE_APPLICATION(vitadaw::platform::juce_adapter::VitaDawJuceApplication)
