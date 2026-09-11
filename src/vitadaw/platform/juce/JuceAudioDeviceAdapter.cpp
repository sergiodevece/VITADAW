#include "vitadaw/platform/juce/JuceAudioDeviceAdapter.h"

#include <juce_audio_formats/juce_audio_formats.h>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <utility>
#include <vector>

namespace vitadaw::platform::juce_adapter {

struct JuceAudioDeviceAdapter::PreparedAudio {
    juce::AudioBuffer<float> samples;
    timeline::SampleRate sourceSampleRate;
    timeline::SampleRate projectSampleRate;
    timeline::ProjectFrameCount projectDuration;
};

JuceAudioDeviceAdapter::JuceAudioDeviceAdapter() = default;

JuceAudioDeviceAdapter::~JuceAudioDeviceAdapter() {
    shutdown();
}

bool JuceAudioDeviceAdapter::initialise() {
    closeDevice(false);
    playbackCursor_.stopAndRewind();
    clearRealtimeCommands();

    deviceManager_.addChangeListener(this);
    changeListenerRegistered_ = true;

    const auto error = deviceManager_.initialiseWithDefaultDevices(0, 2);
    if (error.isNotEmpty()) {
        stateModel_.markError(error.toStdString());
        publishState();
        return false;
    }

    attachAudioCallback();
    refreshState();
    return stateModel_.state().status == audio::AudioDeviceStatus::active;
}

bool JuceAudioDeviceAdapter::reinitialise() {
    return initialise();
}

void JuceAudioDeviceAdapter::shutdown() noexcept {
    closeDevice(true);
}

const audio::AudioDeviceState& JuceAudioDeviceAdapter::state() const noexcept {
    return stateModel_.state();
}

void JuceAudioDeviceAdapter::setStateChangedCallback(StateChangedCallback callback) {
    stateChangedCallback_ = std::move(callback);
    publishState();
}

audio::AudioFileLoadResult JuceAudioDeviceAdapter::loadWav(
    const std::filesystem::path& filePath,
    timeline::SampleRate projectSampleRate) {
    const auto nativePath = filePath.wstring();
    const juce::File file{juce::String(nativePath.c_str())};
    if (!file.hasFileExtension("wav")) {
        return {false, {}, "Only WAV files are supported"};
    }
    if (!file.existsAsFile()) {
        return {false, {}, "WAV file does not exist"};
    }

    juce::WavAudioFormat wavFormat;
    auto inputStream = file.createInputStream();
    if (inputStream == nullptr) {
        return {false, {}, "WAV file could not be opened"};
    }

    std::unique_ptr<juce::AudioFormatReader> reader{
        wavFormat.createReaderFor(inputStream.release(), true)};
    if (reader == nullptr || reader->sampleRate <= 0.0 || reader->numChannels == 0 ||
        reader->lengthInSamples <= 0) {
        return {false, {}, "WAV file is invalid or uses an unsupported encoding"};
    }

    if (reader->lengthInSamples > std::numeric_limits<int>::max() ||
        reader->numChannels > static_cast<unsigned int>(std::numeric_limits<int>::max())) {
        return {false, {}, "WAV file is too large to prepare safely"};
    }

    const auto frameCount = static_cast<int>(reader->lengthInSamples);
    const auto channelCount = static_cast<int>(reader->numChannels);
    auto prepared = std::make_unique<PreparedAudio>();
    prepared->sourceSampleRate = timeline::SampleRate{reader->sampleRate};
    prepared->projectSampleRate = projectSampleRate;
    prepared->projectDuration = timeline::sourceFramesToProjectFrames(
        {static_cast<std::uint64_t>(reader->lengthInSamples)},
        prepared->sourceSampleRate,
        projectSampleRate);
    prepared->samples.setSize(channelCount, frameCount, false, true, false);

    std::vector<float*> destinationChannels(static_cast<std::size_t>(channelCount));
    for (int channel = 0; channel < channelCount; ++channel) {
        destinationChannels[static_cast<std::size_t>(channel)] =
            prepared->samples.getWritePointer(channel);
    }

    if (!reader->read(destinationChannels.data(), channelCount, 0, frameCount)) {
        return {false, {}, "WAV samples could not be decoded"};
    }

    const audio::AudioFileMetadata metadata{
        timeline::SampleRate{reader->sampleRate},
        static_cast<std::uint32_t>(reader->numChannels),
        {static_cast<std::uint64_t>(reader->lengthInSamples)},
        {static_cast<double>(reader->lengthInSamples) / reader->sampleRate}};

    const auto callbackWasRegistered = callbackRegistered_;
    detachAudioCallback();
    preparedAudio_ = std::move(prepared);
    preparedAudioAvailable_.store(true, std::memory_order_release);
    playbackCursor_.prepare({static_cast<std::uint64_t>(frameCount)});
    clearRealtimeCommands();
    publishRealtimeTransport();
    if (callbackWasRegistered) {
        attachAudioCallback();
    }

    return {true, metadata, {}};
}

void JuceAudioDeviceAdapter::closeDevice(bool publishClosedState) noexcept {
    detachAudioCallback();

    if (changeListenerRegistered_) {
        deviceManager_.removeChangeListener(this);
        changeListenerRegistered_ = false;
    }

    deviceManager_.closeAudioDevice();
    playbackCursor_.stopAndRewind();
    clearRealtimeCommands();

    if (publishClosedState) {
        preparedAudioAvailable_.store(false, std::memory_order_release);
        preparedAudio_.reset();
        playbackCursor_.prepare({0});
        publishRealtimeTransport();
    }

    if (publishClosedState) {
        stateModel_.markClosed();
        publishState();
    }
}

audio::AudioControlRequestResult JuceAudioDeviceAdapter::tryRequestPlay() noexcept {
    if (!preparedAudioAvailable_.load(std::memory_order_acquire) ||
        !callbackRegistered_) {
        return {};
    }
    return enqueueRealtimeCommand(RealtimeCommand::play);
}

audio::AudioControlRequestResult JuceAudioDeviceAdapter::tryRequestStop() noexcept {
    if (!callbackRegistered_) {
        const auto sequence = nextCommandSequence_++;
        playbackCursor_.stopAndRewind();
        lastProcessedCommandSequence_ = sequence;
        clearRealtimeCommands();
        publishRealtimeTransport();
        return {true, sequence};
    }
    return enqueueRealtimeCommand(RealtimeCommand::stop);
}

audio::RealtimeTransportSnapshot JuceAudioDeviceAdapter::transportSnapshot() const noexcept {
    return transportExchange_.snapshot();
}

void JuceAudioDeviceAdapter::audioDeviceIOCallbackWithContext(
    const float* const* inputChannelData,
    int numInputChannels,
    float* const* outputChannelData,
    int numOutputChannels,
    int numSamples,
    const juce::AudioIODeviceCallbackContext& context) noexcept {
    static_cast<void>(inputChannelData);
    static_cast<void>(numInputChannels);
    static_cast<void>(context);

    for (int channel = 0; channel < numOutputChannels; ++channel) {
        if (auto* output = outputChannelData[channel]; output != nullptr) {
            std::fill_n(output, numSamples, 0.0F);
        }
    }

    consumeRealtimeCommands();

    const auto* prepared = preparedAudio_.get();
    if (!playbackCursor_.isPlaying() || prepared == nullptr ||
        !deviceSampleRate_.isValid()) {
        return;
    }

    const auto sourceFrameCount = prepared->samples.getNumSamples();
    const auto sourceChannelCount = prepared->samples.getNumChannels();
    const auto sourceFramesPerDeviceFrame = timeline::sourceFramesForDeviceFrames(
        {1}, prepared->sourceSampleRate, deviceSampleRate_);

    for (int frame = 0; frame < numSamples; ++frame) {
        if (!playbackCursor_.isPlaying()) {
            break;
        }

        const auto sourcePosition = playbackCursor_.position();
        const auto sourceFrame = static_cast<int>(sourcePosition.value);
        const auto nextSourceFrame = std::min(sourceFrame + 1, sourceFrameCount - 1);
        const auto fraction = static_cast<float>(sourcePosition.value - sourceFrame);

        for (int channel = 0; channel < numOutputChannels; ++channel) {
            auto* output = outputChannelData[channel];
            if (output == nullptr) {
                continue;
            }

            const auto sourceChannel = std::min(channel, sourceChannelCount - 1);
            const auto* source = prepared->samples.getReadPointer(sourceChannel);
            output[frame] = source[sourceFrame] +
                            fraction * (source[nextSourceFrame] - source[sourceFrame]);
        }

        playbackCursor_.advance(sourceFramesPerDeviceFrame);
    }

    publishRealtimeTransport();
}

void JuceAudioDeviceAdapter::audioDeviceAboutToStart(juce::AudioIODevice* device) noexcept {
    deviceSampleRate_ =
        timeline::SampleRate{device != nullptr ? device->getCurrentSampleRate() : 0.0};
}

void JuceAudioDeviceAdapter::audioDeviceStopped() noexcept {}

void JuceAudioDeviceAdapter::changeListenerCallback(juce::ChangeBroadcaster* source) {
    if (source == &deviceManager_ && changeListenerRegistered_) {
        refreshState();
    }
}

void JuceAudioDeviceAdapter::refreshState() {
    auto* device = deviceManager_.getCurrentAudioDevice();
    if (device == nullptr) {
        stateModel_.markError("No active audio output device");
        publishState();
        return;
    }

    audio::AudioDeviceInfo info;
    info.outputDeviceName = device->getName().toStdString();
    info.sampleRate = device->getCurrentSampleRate();
    info.bufferSizeFrames =
        static_cast<std::uint32_t>(device->getCurrentBufferSizeSamples());
    info.availableInputChannels =
        static_cast<std::uint32_t>(device->getInputChannelNames().size());
    info.availableOutputChannels =
        static_cast<std::uint32_t>(device->getOutputChannelNames().size());

    stateModel_.markActive(std::move(info));
    publishState();
}

void JuceAudioDeviceAdapter::publishState() {
    if (stateChangedCallback_) {
        stateChangedCallback_(stateModel_.state());
    }
}

audio::AudioControlRequestResult JuceAudioDeviceAdapter::enqueueRealtimeCommand(
    RealtimeCommand command) noexcept {
    const auto write = commandWriteIndex_.load(std::memory_order_relaxed);
    const auto next = (write + 1) % realtimeCommandCapacity;
    if (next == commandReadIndex_.load(std::memory_order_acquire)) {
        return {};
    }

    const auto sequence = nextCommandSequence_++;
    realtimeCommands_[write] = {command, sequence};
    commandWriteIndex_.store(next, std::memory_order_release);
    return {true, sequence};
}

void JuceAudioDeviceAdapter::consumeRealtimeCommands() noexcept {
    auto read = commandReadIndex_.load(std::memory_order_relaxed);
    const auto write = commandWriteIndex_.load(std::memory_order_acquire);

    while (read != write) {
        const auto queued = realtimeCommands_[read];
        read = (read + 1) % realtimeCommandCapacity;

        if (queued.command == RealtimeCommand::stop) {
            playbackCursor_.stopAndRewind();
        } else {
            static_cast<void>(playbackCursor_.play());
        }
        lastProcessedCommandSequence_ = queued.sequence;
    }

    commandReadIndex_.store(read, std::memory_order_release);
    publishRealtimeTransport();
}

void JuceAudioDeviceAdapter::publishRealtimeTransport() noexcept {
    const auto* prepared = preparedAudio_.get();
    if (prepared == nullptr) {
        transportExchange_.publish(
            {false, {0}, {0}, lastProcessedCommandSequence_});
        return;
    }

    auto projectPosition = timeline::sourcePositionToProjectPosition(
        playbackCursor_.position(),
        prepared->sourceSampleRate,
        prepared->projectSampleRate);
    if (projectPosition.value > prepared->projectDuration.value) {
        projectPosition = {prepared->projectDuration.value};
    }

    transportExchange_.publish({playbackCursor_.isPlaying(),
                                projectPosition,
                                prepared->projectDuration,
                                lastProcessedCommandSequence_});
}

void JuceAudioDeviceAdapter::clearRealtimeCommands() noexcept {
    commandReadIndex_.store(0, std::memory_order_relaxed);
    commandWriteIndex_.store(0, std::memory_order_relaxed);
}

void JuceAudioDeviceAdapter::detachAudioCallback() noexcept {
    if (callbackRegistered_) {
        deviceManager_.removeAudioCallback(this);
        callbackRegistered_ = false;
    }
}

void JuceAudioDeviceAdapter::attachAudioCallback() {
    if (!callbackRegistered_ && deviceManager_.getCurrentAudioDevice() != nullptr) {
        deviceManager_.addAudioCallback(this);
        callbackRegistered_ = true;
    }
}

} // namespace vitadaw::platform::juce_adapter
