#pragma once

#include "vitadaw/audio/AudioDeviceState.h"
#include "vitadaw/audio/IAudioEngineControl.h"
#include "vitadaw/audio/RealtimeAudioEngine.h"

#include <juce_audio_devices/juce_audio_devices.h>

#include <atomic>
#include <cstddef>
#include <functional>
#include <memory>

namespace vitadaw::platform::juce_adapter {

class JuceAudioDeviceAdapter final : public audio::IAudioEngineControl,
                                     private juce::AudioIODeviceCallback,
                                     private juce::ChangeListener {
public:
    using StateChangedCallback = std::function<void(const audio::AudioDeviceState&)>;
    static constexpr std::size_t preparationMemoryBudgetBytes = 512U * 1024U * 1024U;

    JuceAudioDeviceAdapter();
    ~JuceAudioDeviceAdapter() override;
    JuceAudioDeviceAdapter(const JuceAudioDeviceAdapter&) = delete;
    JuceAudioDeviceAdapter& operator=(const JuceAudioDeviceAdapter&) = delete;

    [[nodiscard]] bool initialise();
    [[nodiscard]] bool reinitialise();
    void shutdown() noexcept;
    void pollDeviceLifecycle();
    [[nodiscard]] const audio::AudioDeviceState& state() const noexcept;
    void setStateChangedCallback(StateChangedCallback callback);

    [[nodiscard]] audio::AudioFilePreparationResult prepareWav(
        const std::filesystem::path& file, tracks::TrackId track,
        timeline::SampleRate projectSampleRate,
        mixer::PreparedTrackMixState trackMix) override;
    [[nodiscard]] bool commitPreparedWav(
        audio::PreparedAudioFilePtr prepared,
        audio::AudioFileCommitAction modelCommit) noexcept override;
    [[nodiscard]] audio::StructuralPlanPreparationResult prepareProcessingPlan(
        const audio::ProcessingPlanSpecification& specification) override;
    [[nodiscard]] bool commitPreparedProcessingPlan(
        audio::PreparedProcessingPlanChangePtr prepared,
        audio::AudioFileCommitAction modelCommit) noexcept override;
    [[nodiscard]] bool tryUpdateTrackMix(
        tracks::TrackId track,
        mixer::PreparedTrackMixState mix,
        audio::PreparedAudibilityState audibility) noexcept override;
    [[nodiscard]] bool tryUpdateBusMix(
        routing::BusId bus, mixer::PreparedBusMixState mix,
        audio::PreparedAudibilityState audibility) noexcept override;
    [[nodiscard]] bool tryUpdateSendMix(
        routing::SendId send,
        mixer::PreparedSendMixState mix) noexcept override;
    [[nodiscard]] bool tryUpdateMasterMix(
        mixer::PreparedMasterMixState mix) noexcept override;
    [[nodiscard]] bool tryUpdateProcessorBypass(
        processors::ProcessorInstanceId processor,
        bool bypassed) noexcept override;
    [[nodiscard]] bool tryUpdateProcessorParameter(
        processors::ProcessorInstanceId processor,
        processors::ParameterId parameter,
        float desiredValue,
        float preparedValue) noexcept override;
    [[nodiscard]] audio::AudioControlRequestResult tryRequestPlay() noexcept override;
    [[nodiscard]] audio::AudioControlRequestResult tryRequestStop() noexcept override;
    [[nodiscard]] audio::RealtimeTransportSnapshot transportSnapshot() const noexcept override;
    [[nodiscard]] mixer::MeterSnapshot meterSnapshot() const noexcept override;

private:
    void audioDeviceIOCallbackWithContext(
        const float* const*, int, float* const*, int, int,
        const juce::AudioIODeviceCallbackContext&) noexcept override;
    void audioDeviceAboutToStart(juce::AudioIODevice*) noexcept override;
    void audioDeviceStopped() noexcept override;
    void audioDeviceError(const juce::String&) override;
    void changeListenerCallback(juce::ChangeBroadcaster*) override;

    enum class PendingLifecycleEvent : std::uint8_t { none, stopped, error };
    struct PreparedAudio;
    struct PreparedProject;
    struct PreparedJuceAudioFile;
    struct PreparedJuceProcessingPlan;

    void closeDevice(bool publishClosedState) noexcept;
    void refreshState();
    void publishState();
    void detachAudioCallback() noexcept;
    void attachAudioCallback();
    void configureRealtimeEngine() noexcept;
    [[nodiscard]] bool prepareProjectPlan(
        PreparedProject& candidate,
        const audio::ProcessingPlanSpecification& specification,
        std::string& errorMessage);
    [[nodiscard]] bool reprepareForCurrentDevice(std::string& errorMessage);
    [[nodiscard]] bool commitPreparedProject(
        std::unique_ptr<PreparedProject>& candidate,
        audio::AudioFileCommitAction modelCommit) noexcept;
    [[nodiscard]] std::size_t preparedBytes() const noexcept;

    juce::AudioDeviceManager deviceManager_;
    audio::AudioDeviceStateModel stateModel_;
    StateChangedCallback stateChangedCallback_;
    std::unique_ptr<PreparedProject> preparedProject_;
    audio::RealtimeAudioEngine realtimeEngine_;
    timeline::SampleRate projectSampleRate_;
    timeline::SampleRate deviceSampleRate_;
    mixer::PreparedMasterMixState masterMix_;
    std::atomic<PendingLifecycleEvent> pendingLifecycleEvent_{};
    std::atomic<bool> suppressLifecycleNotification_{};
    bool callbackRegistered_{};
    bool changeListenerRegistered_{};
};

} // namespace vitadaw::platform::juce_adapter
