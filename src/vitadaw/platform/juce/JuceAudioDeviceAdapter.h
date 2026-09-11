#pragma once

#include "vitadaw/audio/AudioDeviceState.h"
#include "vitadaw/audio/IAudioEngineControl.h"
#include "vitadaw/audio/RealtimePlaybackCursor.h"

#include <juce_audio_devices/juce_audio_devices.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <functional>
#include <memory>

namespace vitadaw::platform::juce_adapter {

// Owns the JUCE audio-device lifecycle. Playback control remains deliberately
// unavailable until the realtime engine is implemented in a later increment.
class JuceAudioDeviceAdapter final : public audio::IAudioEngineControl,
                                     private juce::AudioIODeviceCallback,
                                     private juce::ChangeListener {
public:
    using StateChangedCallback = std::function<void(const audio::AudioDeviceState&)>;

    JuceAudioDeviceAdapter();
    ~JuceAudioDeviceAdapter() override;

    JuceAudioDeviceAdapter(const JuceAudioDeviceAdapter&) = delete;
    JuceAudioDeviceAdapter& operator=(const JuceAudioDeviceAdapter&) = delete;

    [[nodiscard]] bool initialise();
    [[nodiscard]] bool reinitialise();
    void shutdown() noexcept;

    [[nodiscard]] const audio::AudioDeviceState& state() const noexcept;
    void setStateChangedCallback(StateChangedCallback callback);

    [[nodiscard]] audio::AudioFileLoadResult loadWav(
        const std::filesystem::path& file,
        timeline::SampleRate projectSampleRate) override;
    [[nodiscard]] audio::AudioControlRequestResult tryRequestPlay() noexcept override;
    [[nodiscard]] audio::AudioControlRequestResult tryRequestStop() noexcept override;
    [[nodiscard]] audio::RealtimeTransportSnapshot transportSnapshot() const noexcept override;

private:
    void audioDeviceIOCallbackWithContext(
        const float* const* inputChannelData,
        int numInputChannels,
        float* const* outputChannelData,
        int numOutputChannels,
        int numSamples,
        const juce::AudioIODeviceCallbackContext& context) noexcept override;
    void audioDeviceAboutToStart(juce::AudioIODevice* device) noexcept override;
    void audioDeviceStopped() noexcept override;
    void changeListenerCallback(juce::ChangeBroadcaster* source) override;

    void closeDevice(bool publishClosedState) noexcept;
    void refreshState();
    void publishState();

    enum class RealtimeCommand {
        play,
        stop,
    };

    struct QueuedRealtimeCommand {
        RealtimeCommand command{RealtimeCommand::stop};
        audio::AudioCommandSequence sequence{};
    };

    static constexpr std::size_t realtimeCommandCapacity = 8;

    [[nodiscard]] audio::AudioControlRequestResult enqueueRealtimeCommand(
        RealtimeCommand command) noexcept;
    void consumeRealtimeCommands() noexcept;
    void publishRealtimeTransport() noexcept;
    void clearRealtimeCommands() noexcept;
    void detachAudioCallback() noexcept;
    void attachAudioCallback();

    struct PreparedAudio;

    juce::AudioDeviceManager deviceManager_;
    audio::AudioDeviceStateModel stateModel_;
    StateChangedCallback stateChangedCallback_;
    std::unique_ptr<PreparedAudio> preparedAudio_;
    std::array<QueuedRealtimeCommand, realtimeCommandCapacity> realtimeCommands_{};
    std::atomic<std::size_t> commandWriteIndex_{};
    std::atomic<std::size_t> commandReadIndex_{};
    std::atomic<bool> preparedAudioAvailable_{};
    audio::RealtimePlaybackCursor playbackCursor_;
    audio::RealtimeTransportExchange transportExchange_;
    timeline::SampleRate deviceSampleRate_;
    audio::AudioCommandSequence nextCommandSequence_{1};
    audio::AudioCommandSequence lastProcessedCommandSequence_{};
    bool callbackRegistered_{};
    bool changeListenerRegistered_{};
};

} // namespace vitadaw::platform::juce_adapter
