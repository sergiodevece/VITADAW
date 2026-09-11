#include "vitadaw/platform/juce/JuceAudioDeviceAdapter.h"
#include "vitadaw/audio/AudioPreparationPolicy.h"

#include <juce_audio_formats/juce_audio_formats.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <exception>
#include <limits>
#include <new>
#include <utility>
#include <vector>

namespace vitadaw::platform::juce_adapter {

struct JuceAudioDeviceAdapter::PreparedAudio {
    juce::AudioBuffer<float> samples;
    timeline::SampleRate sourceSampleRate;
};

struct JuceAudioDeviceAdapter::PreparedProject {
    struct TrackResource {
        tracks::TrackId id;
        std::shared_ptr<const PreparedAudio> audio;
        mixer::PreparedTrackMixState mix;
    };

    std::vector<TrackResource> resources;
    std::vector<audio::PreparedTrackView> views;
    timeline::ProjectFrameCount duration;
    mixer::PreparedMasterMixState masterMix;
    bool anySolo{};
};

struct JuceAudioDeviceAdapter::PreparedJuceAudioFile final
    : audio::PreparedAudioFile {
    PreparedJuceAudioFile(audio::AudioFileMetadata metadata,
                          tracks::TrackId destination,
                          timeline::SampleRate projectRate,
                          std::unique_ptr<PreparedProject> project) noexcept
        : audio::PreparedAudioFile(metadata), track(destination),
          projectSampleRate(projectRate), preparedProject(std::move(project)) {}

    tracks::TrackId track;
    timeline::SampleRate projectSampleRate;
    std::unique_ptr<PreparedProject> preparedProject;
};

JuceAudioDeviceAdapter::JuceAudioDeviceAdapter() = default;
JuceAudioDeviceAdapter::~JuceAudioDeviceAdapter() { shutdown(); }

bool JuceAudioDeviceAdapter::initialise() {
    closeDevice(false);
    pendingLifecycleEvent_.store(PendingLifecycleEvent::none, std::memory_order_release);
    realtimeEngine_.deviceInitialising();
    deviceManager_.addChangeListener(this);
    changeListenerRegistered_ = true;
    const auto error = deviceManager_.initialiseWithDefaultDevices(0, 2);
    if (error.isNotEmpty()) {
        realtimeEngine_.deviceError();
        stateModel_.markError(error.toStdString());
        publishState();
        return false;
    }
    attachAudioCallback();
    refreshState();
    return stateModel_.state().status == audio::AudioDeviceStatus::active;
}

bool JuceAudioDeviceAdapter::reinitialise() { return initialise(); }
void JuceAudioDeviceAdapter::shutdown() noexcept { closeDevice(true); }

void JuceAudioDeviceAdapter::pollDeviceLifecycle() {
    const auto event = pendingLifecycleEvent_.exchange(PendingLifecycleEvent::none,
                                                        std::memory_order_acq_rel);
    if (event == PendingLifecycleEvent::error) {
        stateModel_.markError("Audio output device reported an error");
        publishState();
    } else if (event == PendingLifecycleEvent::stopped) {
        stateModel_.markError("Audio output device stopped");
        publishState();
    } else if (realtimeEngine_.deviceState() ==
                   audio::DeviceProcessingState::operational &&
               stateModel_.state().status != audio::AudioDeviceStatus::active) {
        refreshState();
    }
}

const audio::AudioDeviceState& JuceAudioDeviceAdapter::state() const noexcept {
    return stateModel_.state();
}

void JuceAudioDeviceAdapter::setStateChangedCallback(StateChangedCallback callback) {
    stateChangedCallback_ = std::move(callback);
    publishState();
}

audio::AudioFilePreparationResult JuceAudioDeviceAdapter::prepareWav(
    const std::filesystem::path& filePath, tracks::TrackId track,
    timeline::SampleRate projectSampleRate,
    mixer::PreparedTrackMixState trackMix) {
    try {
        if (!track.isValid()) {
            return {nullptr, "Invalid audio track identity"};
        }
        if (!projectSampleRate.isValid()) {
            return {nullptr, "Project sample rate must be finite and positive"};
        }
        if (!trackMix.isValid()) {
            return {nullptr, "Invalid prepared track mixer state"};
        }

        const auto nativePath = filePath.wstring();
        const juce::File file{juce::String(nativePath.c_str())};
        if (!file.hasFileExtension("wav")) {
            return {nullptr, "Only WAV files are supported"};
        }
        if (!file.existsAsFile()) {
            return {nullptr, "WAV file does not exist"};
        }
        juce::WavAudioFormat wavFormat;
        auto inputStream = file.createInputStream();
        if (inputStream == nullptr) {
            return {nullptr, "WAV file could not be opened"};
        }
        std::unique_ptr<juce::AudioFormatReader> reader{
            wavFormat.createReaderFor(inputStream.release(), true)};
        if (reader == nullptr || reader->lengthInSamples <= 0) {
            return {nullptr, "WAV is invalid or is not mono/stereo PCM or float"};
        }
        if (reader->lengthInSamples > std::numeric_limits<int>::max()) {
            return {nullptr, "WAV file is too large to prepare safely"};
        }

        const auto frameCount = static_cast<std::uint64_t>(reader->lengthInSamples);
        const auto channelCount = static_cast<std::uint64_t>(reader->numChannels);
        const auto validation = audio::validatePreparationShape(
            timeline::SampleRate{reader->sampleRate}, channelCount, frameCount,
            preparedBytes(), preparationMemoryBudgetBytes);
        if (!validation.isValid()) {
            if (validation.error ==
                audio::PreparationValidationError::memoryBudgetExceeded) {
                return {nullptr,
                        "WAV exceeds the 512 MiB preparation memory budget"};
            }
            if (validation.error ==
                audio::PreparationValidationError::sizeOverflow) {
                return {nullptr,
                        "WAV decoded size overflows the platform size type"};
            }
            return {nullptr, "WAV has invalid sample rate, channels, or length"};
        }

        auto prepared = std::make_shared<PreparedAudio>();
        prepared->sourceSampleRate = timeline::SampleRate{reader->sampleRate};
        prepared->samples.setSize(static_cast<int>(channelCount),
                                  static_cast<int>(frameCount), false, true, false);
        std::vector<float*> destinationChannels(static_cast<std::size_t>(channelCount));
        for (std::size_t channel = 0; channel < destinationChannels.size(); ++channel) {
            destinationChannels[channel] =
                prepared->samples.getWritePointer(static_cast<int>(channel));
        }
        if (!reader->read(destinationChannels.data(), static_cast<int>(channelCount),
                          0, static_cast<int>(frameCount))) {
            return {nullptr, "WAV samples could not be decoded"};
        }
        for (const auto* samples : destinationChannels) {
            if (!audio::containsOnlyFiniteSamples(
                    {samples, static_cast<std::size_t>(frameCount)})) {
                return {nullptr, "WAV contains non-finite float samples"};
            }
        }
        const audio::AudioFileMetadata metadata{
            prepared->sourceSampleRate, static_cast<std::uint32_t>(channelCount),
            {frameCount}, {static_cast<double>(frameCount) / reader->sampleRate}};

        auto candidateProject = std::make_unique<PreparedProject>();
        if (preparedProject_ != nullptr) {
            candidateProject->resources = preparedProject_->resources;
        }
        candidateProject->masterMix = masterMix_;
        candidateProject->anySolo = anySolo_;
        const auto existing = std::find_if(
            candidateProject->resources.begin(), candidateProject->resources.end(),
            [track](const auto& resource) { return resource.id == track; });
        if (existing == candidateProject->resources.end()) {
            if (candidateProject->resources.size() >=
                audio::RealtimeAudioEngine::maximumTrackCount) {
                return {nullptr, "Realtime prepared track capacity exceeded"};
            }
            candidateProject->resources.push_back(
                {track, std::move(prepared), trackMix});
        } else {
            existing->audio = std::move(prepared);
            existing->mix = trackMix;
        }

        candidateProject->views.reserve(candidateProject->resources.size());
        for (const auto& resource : candidateProject->resources) {
            const auto& source = *resource.audio;
            audio::PreparedTrackView view;
            view.id = resource.id;
            view.channelCount = static_cast<std::uint32_t>(
                source.samples.getNumChannels());
            view.frameCount = {static_cast<std::uint64_t>(
                source.samples.getNumSamples())};
            view.sourceSampleRate = source.sourceSampleRate;
            view.clipStart = {0};
            view.clipDuration = timeline::sourceFramesToProjectDuration(
                view.frameCount, view.sourceSampleRate, projectSampleRate);
            view.sourceOffset = {0};
            view.mix = resource.mix;
            for (std::size_t channel = 0; channel < view.channelCount; ++channel) {
                view.channels[channel] =
                    source.samples.getReadPointer(static_cast<int>(channel));
            }
            candidateProject->duration.value = std::max(
                candidateProject->duration.value,
                view.clipStart.value + view.clipDuration.value);
            candidateProject->views.push_back(view);
        }

        return {std::make_unique<PreparedJuceAudioFile>(
                    metadata, track, projectSampleRate,
                    std::move(candidateProject)),
                {}};
    } catch (const std::bad_alloc&) {
        return {nullptr, "Not enough memory to prepare WAV"};
    } catch (const std::exception&) {
        return {nullptr, "Unexpected error while preparing WAV"};
    } catch (...) {
        return {nullptr, "Unknown error while preparing WAV"};
    }
}

bool JuceAudioDeviceAdapter::tryUpdateTrackMix(
    tracks::TrackId track, mixer::PreparedTrackMixState mix,
    bool anySolo) noexcept {
    if (preparedProject_ == nullptr || !track.isValid() || !mix.isValid()) {
        return false;
    }
    const auto resource = std::find_if(
        preparedProject_->resources.begin(), preparedProject_->resources.end(),
        [track](const auto& candidate) { return candidate.id == track; });
    const auto view = std::find_if(
        preparedProject_->views.begin(), preparedProject_->views.end(),
        [track](const auto& candidate) { return candidate.id == track; });
    if (resource == preparedProject_->resources.end() ||
        view == preparedProject_->views.end() ||
        !realtimeEngine_.tryUpdateTrackMix(track, mix, anySolo)) {
        return false;
    }
    resource->mix = mix;
    view->mix = mix;
    anySolo_ = anySolo;
    preparedProject_->anySolo = anySolo;
    return true;
}

bool JuceAudioDeviceAdapter::tryUpdateGlobalSolo(bool anySolo) noexcept {
    if (!realtimeEngine_.tryUpdateGlobalSolo(anySolo)) {
        return false;
    }
    anySolo_ = anySolo;
    if (preparedProject_ != nullptr) {
        preparedProject_->anySolo = anySolo;
    }
    return true;
}

bool JuceAudioDeviceAdapter::tryUpdateMasterMix(
    mixer::PreparedMasterMixState mix) noexcept {
    if (!mix.isValid() || !realtimeEngine_.tryUpdateMasterMix(mix)) {
        return false;
    }
    masterMix_ = mix;
    if (preparedProject_ != nullptr) {
        preparedProject_->masterMix = mix;
    }
    return true;
}

bool JuceAudioDeviceAdapter::commitPreparedWav(
    audio::PreparedAudioFilePtr prepared,
    audio::AudioFileCommitAction modelCommit) noexcept {
    auto* candidate = dynamic_cast<PreparedJuceAudioFile*>(prepared.get());
    if (candidate == nullptr || !modelCommit.isValid() ||
        !candidate->track.isValid() || candidate->preparedProject == nullptr) {
        return false;
    }

    const auto callbackWasRegistered = callbackRegistered_;
    detachAudioCallback();

    // Commit point: from here to modelCommit.execute() every operation is a
    // no-throw scalar assignment, pointer swap or portable no-throw configure.
    // The callback stays quiescent until both engine and ProjectState are new.
    preparedProject_.swap(candidate->preparedProject);
    projectSampleRate_ = candidate->projectSampleRate;
    configureRealtimeEngine();
    modelCommit.execute();

    // candidate now owns the previous project. Its destruction happens on this
    // application thread after the engine no longer exposes that view.
    if (callbackWasRegistered) {
        try {
            attachAudioCallback();
        } catch (...) {
            realtimeEngine_.deviceError();
            pendingLifecycleEvent_.store(PendingLifecycleEvent::error,
                                         std::memory_order_release);
        }
    }
    return true;
}

audio::AudioControlRequestResult JuceAudioDeviceAdapter::tryRequestPlay() noexcept {
    return realtimeEngine_.tryRequestPlay();
}
audio::AudioControlRequestResult JuceAudioDeviceAdapter::tryRequestStop() noexcept {
    return realtimeEngine_.tryRequestStop();
}
audio::RealtimeTransportSnapshot JuceAudioDeviceAdapter::transportSnapshot() const noexcept {
    return realtimeEngine_.transportSnapshot();
}
mixer::MeterSnapshot JuceAudioDeviceAdapter::meterSnapshot() const noexcept {
    return realtimeEngine_.meterSnapshot();
}

void JuceAudioDeviceAdapter::audioDeviceIOCallbackWithContext(
    const float* const*, int, float* const* output, int channels, int frames,
    const juce::AudioIODeviceCallbackContext&) noexcept {
    realtimeEngine_.processBlock(
        {output, static_cast<std::size_t>(std::max(0, channels)),
         static_cast<std::size_t>(std::max(0, frames))},
        deviceSampleRate_);
}

void JuceAudioDeviceAdapter::audioDeviceAboutToStart(juce::AudioIODevice* device) noexcept {
    deviceSampleRate_ = timeline::SampleRate{
        device != nullptr ? device->getCurrentSampleRate() : 0.0};
    if (deviceSampleRate_.isValid()) {
        pendingLifecycleEvent_.store(PendingLifecycleEvent::none, std::memory_order_release);
        // JUCE also invokes this synchronously when merely adding a callback.
        // It only enters "initializing"; actual processing is confirmed by
        // isPlaying() after registration or by callback entry.
        realtimeEngine_.deviceInitialising();
    } else {
        realtimeEngine_.deviceError();
    }
}

void JuceAudioDeviceAdapter::audioDeviceStopped() noexcept {
    realtimeEngine_.deviceStopped();
    if (!suppressLifecycleNotification_.load(std::memory_order_acquire)) {
        pendingLifecycleEvent_.store(PendingLifecycleEvent::stopped,
                                     std::memory_order_release);
    }
}

void JuceAudioDeviceAdapter::audioDeviceError(const juce::String&) {
    realtimeEngine_.deviceError();
    pendingLifecycleEvent_.store(PendingLifecycleEvent::error,
                                 std::memory_order_release);
}

void JuceAudioDeviceAdapter::changeListenerCallback(juce::ChangeBroadcaster* source) {
    if (source == &deviceManager_ && changeListenerRegistered_) {
        refreshState();
    }
}

void JuceAudioDeviceAdapter::closeDevice(bool publishClosedState) noexcept {
    detachAudioCallback();
    if (changeListenerRegistered_) {
        deviceManager_.removeChangeListener(this);
        changeListenerRegistered_ = false;
    }
    suppressLifecycleNotification_.store(true, std::memory_order_release);
    deviceManager_.closeAudioDevice();
    suppressLifecycleNotification_.store(false, std::memory_order_release);
    realtimeEngine_.deviceUnavailable();
    if (publishClosedState) {
        preparedProject_.reset();
        projectSampleRate_ = {};
        configureRealtimeEngine();
        stateModel_.markClosed();
        publishState();
    }
}

void JuceAudioDeviceAdapter::refreshState() {
    auto* device = deviceManager_.getCurrentAudioDevice();
    if (device != nullptr && callbackRegistered_ && device->isPlaying()) {
        realtimeEngine_.deviceConsumerStarted();
    }
    if (device == nullptr || !callbackRegistered_ || !device->isPlaying() ||
        realtimeEngine_.deviceState() !=
            audio::DeviceProcessingState::operational) {
        stateModel_.markError("No operational audio output device");
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

void JuceAudioDeviceAdapter::detachAudioCallback() noexcept {
    if (callbackRegistered_) {
        suppressLifecycleNotification_.store(true, std::memory_order_release);
        deviceManager_.removeAudioCallback(this);
        suppressLifecycleNotification_.store(false, std::memory_order_release);
        callbackRegistered_ = false;
    }
}

void JuceAudioDeviceAdapter::attachAudioCallback() {
    if (!callbackRegistered_ && deviceManager_.getCurrentAudioDevice() != nullptr) {
        deviceManager_.addAudioCallback(this);
        callbackRegistered_ = true;
        auto* device = deviceManager_.getCurrentAudioDevice();
        if (device != nullptr && device->isPlaying() && deviceSampleRate_.isValid()) {
            realtimeEngine_.deviceConsumerStarted();
        }
    }
}

void JuceAudioDeviceAdapter::configureRealtimeEngine() noexcept {
    const auto views = preparedProject_ == nullptr
                           ? std::span<const audio::PreparedTrackView>{}
                           : std::span<const audio::PreparedTrackView>{
                                 preparedProject_->views};
    const auto duration = preparedProject_ == nullptr
                              ? timeline::ProjectFrameCount{}
                              : preparedProject_->duration;
    realtimeEngine_.configure(
        {projectSampleRate_, duration, views, masterMix_, anySolo_});
}

std::size_t JuceAudioDeviceAdapter::preparedBytes() const noexcept {
    std::size_t bytes{};
    if (preparedProject_ == nullptr) {
        return bytes;
    }
    for (const auto& track : preparedProject_->resources) {
        bytes += static_cast<std::size_t>(track.audio->samples.getNumChannels()) *
                 static_cast<std::size_t>(track.audio->samples.getNumSamples()) *
                 sizeof(float);
    }
    return bytes;
}

} // namespace vitadaw::platform::juce_adapter
