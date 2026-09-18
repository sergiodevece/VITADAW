#include "vitadaw/platform/juce/JuceAudioDeviceAdapter.h"
#include "vitadaw/audio/AudioPreparationPolicy.h"
#include "vitadaw/audio/RecordingRecovery.h"
#include "vitadaw/platform/files/ProjectFileIO.h"
#include "vitadaw/platform/juce/RecordingInputSetupPolicy.h"

#include <juce_audio_formats/juce_audio_formats.h>

#include <algorithm>
#include <cmath>
#include <climits>
#include <cstdint>
#include <exception>
#include <cerrno>
#include <filesystem>
#include <fcntl.h>
#include <iomanip>
#include <limits>
#include <new>
#include <sstream>
#include <system_error>
#include <utility>
#include <vector>

#if defined(_WIN32)
#include <io.h>
#include <sys/stat.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace vitadaw::platform::juce_adapter {

namespace {
void closeDescriptor(int descriptor) noexcept {
#if defined(_WIN32)
    if (descriptor >= 0) static_cast<void>(::_close(descriptor));
#else
    if (descriptor >= 0) static_cast<void>(::close(descriptor));
#endif
}

} // namespace

namespace {

class OwnedDescriptorOutputStream final : public juce::OutputStream {
public:
    explicit OwnedDescriptorOutputStream(files::RecordingFileHandle& file) noexcept
        : file_(file) {}
    ~OwnedDescriptorOutputStream() override = default;

    void flush() override {
#if !defined(_WIN32)
        // The adapter calls fsync after the writer destructor has rewritten the
        // final header.  Calling it here would sync an earlier header instead.
#endif
    }
    bool setPosition(juce::int64 position) override {
        if (!file_.setPosition(position, file_.finalizing())) return false;
        position_ = position;
        return true;
    }
    juce::int64 getPosition() override { return position_; }
    bool write(const void* bytes, size_t count) override {
        if (!file_.write(bytes, count, file_.finalizing())) return false;
        position_ += static_cast<juce::int64>(count);
        return true;
    }

private:
    files::RecordingFileHandle& file_;
    juce::int64 position_{};
};

[[nodiscard]] std::optional<JuceAudioDeviceAdapter::FileIdentity> descriptorIdentity(
    int descriptor) noexcept {
    struct stat state {};
#if defined(_WIN32)
    if (::_fstat64(descriptor, &state) != 0) return std::nullopt;
#else
    if (::fstat(descriptor, &state) != 0) return std::nullopt;
#endif
    return JuceAudioDeviceAdapter::FileIdentity{
        static_cast<std::uint64_t>(state.st_dev), static_cast<std::uint64_t>(state.st_ino)};
}

[[nodiscard]] std::optional<JuceAudioDeviceAdapter::FileIdentity> pathnameIdentity(
    const std::filesystem::path& path) noexcept {
    struct stat state {};
#if defined(_WIN32)
    if (::_stat64(path.string().c_str(), &state) != 0) return std::nullopt;
#else
    if (::lstat(path.c_str(), &state) != 0) return std::nullopt;
#endif
    return JuceAudioDeviceAdapter::FileIdentity{
        static_cast<std::uint64_t>(state.st_dev), static_cast<std::uint64_t>(state.st_ino)};
}

} // namespace

JuceAudioDeviceAdapter::ExclusiveTemporaryFile::~ExclusiveTemporaryFile() noexcept {
    closeDescriptor(descriptor_);
}

JuceAudioDeviceAdapter::ExclusiveTemporaryFile::ExclusiveTemporaryFile(
    ExclusiveTemporaryFile&& other) noexcept
    : identity(other.identity), descriptor_(std::exchange(other.descriptor_, -1)) {}

JuceAudioDeviceAdapter::ExclusiveTemporaryFile&
JuceAudioDeviceAdapter::ExclusiveTemporaryFile::operator=(ExclusiveTemporaryFile&& other) noexcept {
    if (this == &other) return *this;
    closeDescriptor(descriptor_);
    descriptor_ = std::exchange(other.descriptor_, -1);
    identity = other.identity;
    return *this;
}

int JuceAudioDeviceAdapter::ExclusiveTemporaryFile::release() noexcept {
    return std::exchange(descriptor_, -1);
}

std::optional<JuceAudioDeviceAdapter::ExclusiveTemporaryFile>
JuceAudioDeviceAdapter::createExclusiveTemporaryFile(
    const std::filesystem::path& path, std::error_code& error) noexcept {
#if defined(_WIN32)
    const auto descriptor = ::_open(path.string().c_str(),
                                   _O_WRONLY | _O_CREAT | _O_EXCL | _O_BINARY,
                                   _S_IREAD | _S_IWRITE);
#else
    const auto descriptor = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
#endif
    if (descriptor < 0) {
        error = {errno, std::generic_category()};
        return std::nullopt;
    }
    const auto identity = descriptorIdentity(descriptor);
    if (!identity || !identity->valid()) {
        error = {errno != 0 ? errno : EIO, std::generic_category()};
        closeDescriptor(descriptor);
        return std::nullopt;
    }
    return ExclusiveTemporaryFile{descriptor, *identity};
}

bool JuceAudioDeviceAdapter::pathHasIdentity(
    const std::filesystem::path& path, FileIdentity identity) noexcept {
    const auto actual = pathnameIdentity(path);
    return actual && *actual == identity;
}

JuceAudioDeviceAdapter::RecoveryMetadataStatus
JuceAudioDeviceAdapter::persistInitialRecoveryMetadata(
    const std::filesystem::path& directory, const audio::RecordingRecoveryMarker& marker,
    audio::RecordingRecoveryMarkerWriteOptions options) {
    std::string error;
    if (audio::writeRecordingRecoveryMarker(directory, marker, error, options)) return {true, {}};
    return {false, "Recording started, but crash-recovery metadata could not be persisted: " + error};
}

bool JuceAudioDeviceAdapter::finalizeRecordingFile(std::string& error) noexcept {
    if (!recordingWriter_ || !recordingFile_) {
        error = "Recording writer is unavailable for finalization";
        return false;
    }
    // JUCE rewrites the WAV header in its writer destructor. Retain the FD in
    // RecordingFileHandle so its final bytes can be synced and close observed.
    recordingFile_->setFinalizing();
    recordingWriter_.reset();
    if (recordingFile_->error() != nullptr) {
        error = recordingFile_->error();
        return false;
    }
    if (!recordingFile_->sync()) {
        error = recordingFile_->error() == nullptr
                    ? "Recording file fsync failed" : recordingFile_->error();
        return false;
    }
    if (!recordingFile_->close()) {
        error = recordingFile_->error() == nullptr
                    ? "Recording file close failed" : recordingFile_->error();
        return false;
    }
    return true;
}

struct JuceAudioDeviceAdapter::PreparedAudio {
    juce::AudioBuffer<float> samples;
    timeline::SampleRate sourceSampleRate;
};

struct JuceAudioDeviceAdapter::PreparedProject {
    struct TrackResource {
        media::SourceId id;
        std::shared_ptr<const PreparedAudio> audio;
    };

    std::vector<TrackResource> resources;
    audio::ProcessingPlanSpecification specification;
    std::unique_ptr<audio::PreparedProcessingBundle> processing;
};

struct JuceAudioDeviceAdapter::PreparedJuceAudioFile final
    : audio::PreparedAudioFile {
    PreparedJuceAudioFile(audio::AudioFileMetadata metadata,
                          std::shared_ptr<const PreparedAudio> resource) noexcept
        : audio::PreparedAudioFile(metadata), audio(std::move(resource)) {}

    std::shared_ptr<const PreparedAudio> audio;
};

struct JuceAudioDeviceAdapter::PreparedJuceProcessingPlan final
    : audio::PreparedProcessingPlanChange {
    explicit PreparedJuceProcessingPlan(
        std::unique_ptr<PreparedProject> project) noexcept
        : preparedProject(std::move(project)) {}

    std::unique_ptr<PreparedProject> preparedProject;
};

JuceAudioDeviceAdapter::JuceAudioDeviceAdapter() = default;
JuceAudioDeviceAdapter::~JuceAudioDeviceAdapter() { shutdown(); }

void JuceAudioDeviceAdapter::beginDeviceReinitialisation() noexcept {
    detachAudioCallback(true);
    const auto checkpoint = realtimeEngine_.temporalCheckpoint();
    closeDevice(false);
    static_cast<void>(realtimeEngine_.restoreTemporalCheckpoint(checkpoint));
    pendingLifecycleEvent_.store(PendingLifecycleEvent::none, std::memory_order_release);
    realtimeEngine_.deviceInitialisingPreservingTransport();
}

bool JuceAudioDeviceAdapter::initialise() {
    beginDeviceReinitialisation();
    deviceManager_.addChangeListener(this);
    changeListenerRegistered_ = true;
    const auto error = deviceManager_.initialiseWithDefaultDevices(0, 2);
    if (error.isNotEmpty()) {
        realtimeEngine_.deviceErrorPreservingTransport();
        stateModel_.markError(error.toStdString());
        publishState();
        return false;
    }
    if (auto* device = deviceManager_.getCurrentAudioDevice()) {
        deviceSampleRate_ = timeline::SampleRate{
            device->getCurrentSampleRate()};
    }
    std::string preparationError;
    if (!reprepareForCurrentDevice(preparationError)) {
        realtimeEngine_.deviceErrorPreservingTransport();
        stateModel_.markError(std::move(preparationError));
        publishState();
        return false;
    }
    attachAudioCallback(true);
    refreshState();
    return stateModel_.state().status == audio::AudioDeviceStatus::active;
}

bool JuceAudioDeviceAdapter::reinitialise() { return initialise(); }
void JuceAudioDeviceAdapter::shutdown() noexcept {
    static_cast<void>(shutdownRecording());
    closeDevice(true);
}

void JuceAudioDeviceAdapter::pollDeviceLifecycle() {
    const auto event = pendingLifecycleEvent_.exchange(PendingLifecycleEvent::none,
                                                        std::memory_order_acq_rel);
    if (event == PendingLifecycleEvent::error) {
        // audioDeviceError may arrive on an unspecified JUCE thread. It only
        // publishes the event; serialised control-side cleanup first retires
        // the callback, proving the direct Core transition is quiescent.
        detachAudioCallback(true);
        clearMonitoringDemandForLoss();
        realtimeEngine_.deviceError();
        certifiedDeviceSampleRate_ = {};
        certifiedDeviceBufferSize_ = 0;
        certifiedDevice_ = nullptr;
        stateModel_.markError("Audio output device reported an error");
        publishState();
        return;
    }
    if (event == PendingLifecycleEvent::stopped) {
        // JUCE has stopped the callback before this notification. This is the
        // loss (not controlled detach) path because detach uses the preserve
        // registration guard and suppresses the event.
        clearMonitoringDemandForLoss();
        realtimeEngine_.deviceStopped();
        certifiedDeviceSampleRate_ = {};
        certifiedDeviceBufferSize_ = 0;
        certifiedDevice_ = nullptr;
        stateModel_.markError("Audio output device stopped");
        publishState();
        return;
    }

    auto* device = deviceManager_.getCurrentAudioDevice();
    if (device != nullptr) {
        const auto actualRate = timeline::SampleRate{device->getCurrentSampleRate()};
        const auto actualBuffer = static_cast<std::size_t>(
            std::max(0, device->getCurrentBufferSizeSamples()));
        const bool configurationDiverged =
            !actualRate.isValid() || actualBuffer == 0 ||
            actualRate != certifiedDeviceSampleRate_ ||
            actualBuffer != certifiedDeviceBufferSize_ ||
            device != certifiedDevice_ ||
            (preparedProject_ &&
             preparedProject_->specification.processingSampleRate != actualRate) ||
            (preparedTemporalContext_ &&
             preparedTemporalContext_->deviceSampleRate != actualRate);
        if (configurationDiverged) {
            deviceSampleRate_ = actualRate;
            const auto callbackWasRegistered = callbackRegistered_;
            detachAudioCallback(true);
            std::string error;
            if (!reprepareForCurrentDevice(error)) {
                clearMonitoringDemandForLoss();
                realtimeEngine_.deviceErrorPreservingTransport();
                certifiedDeviceSampleRate_ = {};
                certifiedDeviceBufferSize_ = 0;
                certifiedDevice_ = nullptr;
                stateModel_.markError(std::move(error));
                publishState();
                return;
            }
            if (callbackWasRegistered) {
                attachAudioCallback(true);
            }
            refreshState();
            return;
        }
        if (realtimeEngine_.deviceState() == audio::DeviceProcessingState::operational &&
            stateModel_.state().status != audio::AudioDeviceStatus::active)
            refreshState();
    } else if (realtimeEngine_.deviceState() ==
                   audio::DeviceProcessingState::operational &&
               stateModel_.state().status != audio::AudioDeviceStatus::active) {
        refreshState();
    }
    // A confirmed device which no longer has the route that was opened for
    // monitoring is an input loss, not a reason to stop otherwise-valid
    // playback.  This check remains entirely on the control thread.
    if (monitoringInputDemand_) {
        if (device == nullptr || activeFoundationInputChannels(*device) == 0) {
            clearMonitoringDemandForLoss();
            // The output callback can still be running. It is the unique
            // writer of effective Monitoring state and consumes this request
            // at the start of its next block.
            realtimeEngine_.requestInputMonitoringLifecycleOff();
            stateModel_.markError("Audio input route for monitoring is unavailable");
            publishState();
        }
    }
}

const audio::AudioDeviceState& JuceAudioDeviceAdapter::state() const noexcept {
    return stateModel_.state();
}

void JuceAudioDeviceAdapter::setStateChangedCallback(StateChangedCallback callback) {
    stateChangedCallback_ = std::move(callback);
    publishState();
}

void JuceAudioDeviceAdapter::clearStateChangedCallback() noexcept {
    stateChangedCallback_ = nullptr;
}

audio::AudioFilePreparationResult JuceAudioDeviceAdapter::prepareWav(
    const std::filesystem::path& filePath) {
    return prepareWavForProject(filePath, 0);
}

audio::AudioFilePreparationResult JuceAudioDeviceAdapter::prepareWavForProject(
    const std::filesystem::path& filePath, std::size_t candidateBytes) {
    return decodeWav(filePath, candidateBytes, nullptr);
}
audio::AudioFilePreparationResult JuceAudioDeviceAdapter::prepareVerifiedWav(
    const std::filesystem::path& filePath, const media::MediaFingerprint& expected, std::size_t candidateBytes) {
    return decodeWav(filePath, candidateBytes, &expected);
}
audio::AudioFilePreparationResult JuceAudioDeviceAdapter::decodeWav(
    const std::filesystem::path& filePath, std::size_t candidateBytes, const media::MediaFingerprint* expected) {
    using namespace persistence;
    try {
        const auto nativePath = filePath.wstring();
        const juce::File file{juce::String(nativePath.c_str())};
        if (!file.hasFileExtension("wav")) {
            return {nullptr, "Only WAV files are supported", {},
                    audio::AudioFilePreparationFailure::unsupportedFormat};
        }
        const auto activeBytes = preparedBytes();
        if (activeBytes > preparationMemoryBudgetBytes || candidateBytes > preparationMemoryBudgetBytes - activeBytes)
            return {nullptr, {}, {PersistenceCode::capacityExceeded, PersistencePhase::prepare}};
        const auto retainedBytes = activeBytes + candidateBytes;
        auto encoded = files::nativeProjectFileIO().read(filePath, preparationMemoryBudgetBytes - retainedBytes);
        if (!encoded.result.success()) return {nullptr, {}, std::move(encoded.result)};
        const auto fingerprint = files::fingerprint(encoded.bytes);
        if (expected && *expected != fingerprint)
            return {nullptr, {}, {PersistenceCode::mediaChanged, PersistencePhase::media}};
        juce::WavAudioFormat wavFormat;
        auto inputStream = std::make_unique<juce::MemoryInputStream>(encoded.bytes.data(), encoded.bytes.size(), false);
        std::unique_ptr<juce::AudioFormatReader> reader{
            wavFormat.createReaderFor(inputStream.release(), true)};
        if (reader == nullptr || reader->lengthInSamples <= 0) {
            return {nullptr, "WAV is invalid or is not mono/stereo PCM or float", {},
                    audio::AudioFilePreparationFailure::decodeFailed};
        }
        if (reader->lengthInSamples > std::numeric_limits<int>::max()) {
            return {nullptr, "WAV file is too large to prepare safely", {},
                    audio::AudioFilePreparationFailure::capacityExceeded};
        }

        const auto frameCount = static_cast<std::uint64_t>(reader->lengthInSamples);
        const auto channelCount = static_cast<std::uint64_t>(reader->numChannels);
        const auto validation = audio::validatePreparationShape(
            timeline::SampleRate{reader->sampleRate}, channelCount, frameCount,
            retainedBytes + encoded.bytes.size(), preparationMemoryBudgetBytes);
        if (!validation.isValid()) {
            if (validation.error ==
                audio::PreparationValidationError::memoryBudgetExceeded) {
                return {nullptr,
                        "WAV exceeds the 512 MiB preparation memory budget", {},
                        audio::AudioFilePreparationFailure::capacityExceeded};
            }
            if (validation.error ==
                audio::PreparationValidationError::sizeOverflow) {
                return {nullptr,
                        "WAV decoded size overflows the platform size type", {},
                        audio::AudioFilePreparationFailure::capacityExceeded};
            }
            return {nullptr, "WAV has invalid sample rate, channels, or length", {},
                    audio::AudioFilePreparationFailure::decodeFailed};
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
            return {nullptr, "WAV samples could not be decoded", {},
                    audio::AudioFilePreparationFailure::decodeFailed};
        }
        for (const auto* samples : destinationChannels) {
            if (!audio::containsOnlyFiniteSamples(
                    {samples, static_cast<std::size_t>(frameCount)})) {
                return {nullptr, "WAV contains non-finite float samples", {},
                        audio::AudioFilePreparationFailure::decodeFailed};
            }
        }
        const audio::AudioFileMetadata metadata{
            prepared->sourceSampleRate, static_cast<std::uint32_t>(channelCount),
            {frameCount}, {static_cast<double>(frameCount) / reader->sampleRate}};

        waveform::PcmView waveformView;
        waveformView.channelCount = static_cast<std::uint32_t>(channelCount);
        waveformView.frameCount = {frameCount};
        waveformView.sampleRate = prepared->sourceSampleRate;
        for (std::uint32_t channel = 0; channel < waveformView.channelCount; ++channel)
            waveformView.channels[channel] = prepared->samples.getReadPointer(
                static_cast<int>(channel));
        auto waveformPreparation = waveform::prepareWaveform(waveformView);

        // Verify once more by streaming. PCM and fingerprint came from the same
        // immutable byte snapshot; replacing/changing the media during decode fails.
        auto verification = files::verifyFingerprint(filePath, fingerprint, &encoded.identity);
        if (!verification.success()) return {nullptr, {}, std::move(verification)};
        auto result = std::make_unique<PreparedJuceAudioFile>(metadata, std::move(prepared));
        result->media = {std::filesystem::absolute(filePath).lexically_normal(), {}, fingerprint};
        result->waveform = std::move(waveformPreparation.prepared);
        result->waveformDiagnostic = std::move(waveformPreparation.diagnostic);
        return {std::move(result), {}};
    } catch (const std::bad_alloc&) {
        return {nullptr, "Not enough memory to prepare WAV", {},
                audio::AudioFilePreparationFailure::capacityExceeded};
    } catch (const std::exception&) {
        return {nullptr, "Unexpected error while preparing WAV", {},
                audio::AudioFilePreparationFailure::preparationFailed};
    } catch (...) {
        return {nullptr, "Unknown error while preparing WAV", {},
                audio::AudioFilePreparationFailure::preparationFailed};
    }
}

bool JuceAudioDeviceAdapter::tryUpdateTrackMix(
    tracks::TrackId track, mixer::PreparedTrackMixState mix,
    audio::PreparedAudibilityState audibility) noexcept {
    if (preparedProject_ == nullptr || !track.isValid() || !mix.isValid()) {
        return false;
    }
    const auto planTrack = std::find_if(
        preparedProject_->specification.tracks.begin(),
        preparedProject_->specification.tracks.end(),
        [track](const auto& candidate) { return candidate.id == track; });
    if (planTrack == preparedProject_->specification.tracks.end() ||
        !realtimeEngine_.tryUpdateTrackMix(track, mix, audibility)) {
        return false;
    }
    planTrack->mix = mix;
    return true;
}

bool JuceAudioDeviceAdapter::tryUpdateBusMix(
    routing::BusId bus, mixer::PreparedBusMixState mix,
    audio::PreparedAudibilityState audibility) noexcept {
    if (preparedProject_ == nullptr || !bus.isValid() || !mix.isValid()) {
        return false;
    }
    const auto found = std::find_if(
        preparedProject_->specification.buses.begin(),
        preparedProject_->specification.buses.end(),
        [bus](const auto& candidate) { return candidate.id == bus; });
    if (found == preparedProject_->specification.buses.end() ||
        !realtimeEngine_.tryUpdateBusMix(bus, mix, audibility)) {
        return false;
    }
    found->mix = mix;
    return true;
}

bool JuceAudioDeviceAdapter::tryUpdateSendMix(
    routing::SendId send, mixer::PreparedSendMixState mix) noexcept {
    if (preparedProject_ == nullptr || !send.isValid() || !mix.isValid()) {
        return false;
    }
    const auto found = std::find_if(
        preparedProject_->specification.sends.begin(),
        preparedProject_->specification.sends.end(),
        [send](const auto& candidate) { return candidate.id == send; });
    if (found == preparedProject_->specification.sends.end() ||
        !realtimeEngine_.tryUpdateSendMix(send, mix)) {
        return false;
    }
    found->mix = mix;
    return true;
}

bool JuceAudioDeviceAdapter::tryUpdateMasterMix(
    mixer::PreparedMasterMixState mix) noexcept {
    if (!mix.isValid() || !realtimeEngine_.tryUpdateMasterMix(mix)) {
        return false;
    }
    masterMix_ = mix;
    if (preparedProject_ != nullptr) {
        preparedProject_->specification.masterMix = mix;
    }
    return true;
}

namespace {

processors::ProcessorState* findProcessorState(
    audio::ProcessingPlanSpecification& specification,
    processors::ProcessorInstanceId processor) noexcept {
    const auto findIn = [processor](processors::InsertChain& chain)
        -> processors::ProcessorState* {
        const auto found = std::find_if(
            chain.processors.begin(), chain.processors.end(),
            [processor](const auto& candidate) {
                return candidate.id == processor;
            });
        return found == chain.processors.end() ? nullptr : &*found;
    };
    for (auto& track : specification.tracks) {
        if (auto* found = findIn(track.inserts)) {
            return found;
        }
    }
    for (auto& bus : specification.buses) {
        if (auto* found = findIn(bus.inserts)) {
            return found;
        }
    }
    return findIn(specification.masterInserts);
}

} // namespace

bool JuceAudioDeviceAdapter::tryUpdateProcessorBypass(
    processors::ProcessorInstanceId processor, bool bypassed) noexcept {
    if (preparedProject_ == nullptr) {
        return false;
    }
    auto* state = findProcessorState(preparedProject_->specification,
                                     processor);
    if (state == nullptr ||
        !realtimeEngine_.tryUpdateProcessorBypass(processor, bypassed)) {
        return false;
    }
    state->bypassed = bypassed;
    return true;
}

bool JuceAudioDeviceAdapter::tryUpdateProcessorParameter(
    processors::ProcessorInstanceId processor,
    processors::ParameterId parameter, float desiredValue,
    float preparedValue) noexcept {
    if (preparedProject_ == nullptr || !std::isfinite(desiredValue) ||
        !std::isfinite(preparedValue)) {
        return false;
    }
    auto* state = findProcessorState(preparedProject_->specification,
                                     processor);
    if (state == nullptr) {
        return false;
    }
    const auto found = std::find_if(
        state->parameters.begin(), state->parameters.end(),
        [parameter](const auto& candidate) {
            return candidate.id == parameter;
        });
    if (found == state->parameters.end() ||
        !realtimeEngine_.tryUpdateProcessorParameter(
            processor, parameter, preparedValue)) {
        return false;
    }
    found->value = desiredValue;
    return true;
}

audio::StructuralPlanPreparationResult
JuceAudioDeviceAdapter::prepareProcessingPlan(
    const audio::ProcessingPlanSpecification& specification) {
    try {
        auto candidate = std::make_unique<PreparedProject>();
        if (preparedProject_ != nullptr) {
            candidate->resources = preparedProject_->resources;
        }
        std::string error;
        if (!prepareProjectPlan(*candidate, specification, error)) {
            return {nullptr, std::move(error)};
        }
        return {std::make_unique<PreparedJuceProcessingPlan>(
                    std::move(candidate)), {}};
    } catch (const std::bad_alloc&) {
        return {nullptr, "Not enough memory to prepare routing"};
    } catch (...) {
        return {nullptr, "Unexpected error while preparing routing"};
    }
}

audio::StructuralPlanPreparationResult
JuceAudioDeviceAdapter::prepareProcessingPlanWithAudio(
    const audio::ProcessingPlanSpecification& specification,
    media::SourceId source,
    audio::PreparedAudioFilePtr preparedAudio) {
    try {
        auto* decoded = dynamic_cast<PreparedJuceAudioFile*>(preparedAudio.get());
        if (decoded == nullptr || !source.isValid() || decoded->audio == nullptr) {
            return {nullptr, "Invalid prepared audio source"};
        }
        auto candidate = std::make_unique<PreparedProject>();
        if (preparedProject_ != nullptr) {
            candidate->resources = preparedProject_->resources;
        }
        if (std::any_of(candidate->resources.begin(),
                        candidate->resources.end(),
                        [source](const auto& resource) {
                            return resource.id == source;
                        })) {
            return {nullptr, "SourceId is already prepared"};
        }
        candidate->resources.push_back({source, decoded->audio});
        std::string error;
        if (!prepareProjectPlan(*candidate, specification, error)) {
            return {nullptr, std::move(error)};
        }
        return {std::make_unique<PreparedJuceProcessingPlan>(
                    std::move(candidate)), {}};
    } catch (const std::bad_alloc&) {
        return {nullptr, "Not enough memory to prepare imported source"};
    } catch (...) {
        return {nullptr, "Unexpected error while preparing imported source"};
    }
}

audio::StructuralPlanPreparationResult JuceAudioDeviceAdapter::prepareProjectReplacement(
    const audio::ProcessingPlanSpecification& specification,
    std::vector<audio::PreparedSourceAudio> resources) {
    try {
        auto candidate = std::make_unique<PreparedProject>();
        candidate->resources.reserve(resources.size());
        if (resources.size() != specification.sources.size()) return {nullptr, "Source count mismatch"};
        for (auto& resource : resources) {
            auto* decoded = dynamic_cast<PreparedJuceAudioFile*>(resource.audio.get());
            if (!decoded || !decoded->audio || !resource.id.isValid() ||
                std::any_of(candidate->resources.begin(), candidate->resources.end(),
                    [&](const auto& r) { return r.id == resource.id; }))
                return {nullptr, "Invalid replacement source"};
            candidate->resources.push_back({resource.id, std::move(decoded->audio)});
        }
        std::string error;
        if (!prepareProjectPlan(*candidate, specification, error)) return {nullptr, std::move(error)};
        return {std::make_unique<PreparedJuceProcessingPlan>(std::move(candidate)), {}};
    } catch (...) { return {nullptr, "Cannot prepare isolated project resources"}; }
}

bool JuceAudioDeviceAdapter::commitPreparedProcessingPlan(
    audio::PreparedProcessingPlanChangePtr prepared,
    audio::AudioFileCommitAction modelCommit) noexcept {
    auto* candidate = dynamic_cast<PreparedJuceProcessingPlan*>(prepared.get());
    if (candidate == nullptr || candidate->preparedProject == nullptr ||
        !modelCommit.isValid()) {
        return false;
    }
    return commitPreparedProject(candidate->preparedProject, modelCommit);
}

bool JuceAudioDeviceAdapter::commitPreparedProcessingPlanPreservingTransport(
    audio::PreparedProcessingPlanChangePtr prepared,
    audio::AudioFileCommitAction modelCommit) noexcept {
    auto* candidate = dynamic_cast<PreparedJuceProcessingPlan*>(prepared.get());
    if (candidate == nullptr || candidate->preparedProject == nullptr ||
        !modelCommit.isValid()) {
        return false;
    }
    return commitPreparedProject(candidate->preparedProject, modelCommit, true);
}

audio::TemporalContextPreparationResult
JuceAudioDeviceAdapter::prepareTemporalContext(
    const musical::MusicalTimeMap& map,
    std::optional<musical::MusicalLoopRange> loop,
    timeline::SampleRate projectRate, std::uint64_t revision) {
    return audio::prepareTemporalContext(
        map, loop, projectRate,
        deviceSampleRate_.isValid() ? deviceSampleRate_ : projectRate,
        revision);
}

bool JuceAudioDeviceAdapter::commitPreparedTemporalContext(
    std::unique_ptr<audio::PreparedTemporalContext> candidate,
    audio::AudioFileCommitAction modelCommit) noexcept {
    if (!candidate || !modelCommit.isValid()) return false;
    // A new application has a valid prepared musical context before its first
    // audio plan exists. Establish the empty portable engine at the project's
    // logical rate so this context is already certified if audio-device
    // initialisation happens before the first import.
    if (preparedProject_ == nullptr && !projectSampleRate_.isValid()) {
        realtimeEngine_.configure({candidate->projectSampleRate, {}, {}, {}, false});
        if (!realtimeEngine_.configureTemporalContext(candidate.get())) return false;
        projectSampleRate_ = candidate->projectSampleRate;
        preparedTemporalContext_.swap(candidate);
        modelCommit.execute();
        return true;
    }
    const auto callbackWasRegistered = callbackRegistered_;
    detachAudioCallback(true);
    const auto checkpoint = realtimeEngine_.temporalCheckpoint();
    if (!realtimeEngine_.canConfigureTemporalContext(candidate.get())) {
        if (callbackWasRegistered) {
            try { attachAudioCallback(true); }
            catch (...) { realtimeEngine_.deviceErrorPreservingTransport(); }
        }
        return false;
    }
    preparedTemporalContext_.swap(candidate);
    if (!realtimeEngine_.configureTemporalContext(preparedTemporalContext_.get()) ||
        !realtimeEngine_.restoreTemporalCheckpoint(checkpoint)) {
        preparedTemporalContext_.swap(candidate);
        static_cast<void>(realtimeEngine_.configureTemporalContext(
            preparedTemporalContext_.get()));
        static_cast<void>(realtimeEngine_.restoreTemporalCheckpoint(checkpoint));
        if (callbackWasRegistered) {
            try { attachAudioCallback(true); }
            catch (...) {
                realtimeEngine_.deviceError();
                pendingLifecycleEvent_.store(PendingLifecycleEvent::error,
                                             std::memory_order_release);
            }
        }
        return false;
    }
    modelCommit.execute();
    if (callbackWasRegistered) {
        try { attachAudioCallback(true); }
        catch (...) {
            realtimeEngine_.deviceError();
            pendingLifecycleEvent_.store(PendingLifecycleEvent::error,
                                         std::memory_order_release);
        }
    }
    return true;
}

bool JuceAudioDeviceAdapter::commitPreparedProjectAndTemporalContext(
    audio::PreparedProcessingPlanChangePtr project,
    std::unique_ptr<audio::PreparedTemporalContext> temporal,
    audio::AudioFileCommitAction modelCommit) noexcept {
    auto* projectCandidate = dynamic_cast<PreparedJuceProcessingPlan*>(project.get());
    if (!projectCandidate || !projectCandidate->preparedProject || !temporal ||
        !modelCommit.isValid() ||
        temporal->projectSampleRate != projectCandidate->preparedProject->processing->plan.projectSampleRate ||
        temporal->deviceSampleRate != projectCandidate->preparedProject->processing->plan.stereoProcessingFormat.sampleRate ||
        !audio::processingPlanSupportsClock(
            projectCandidate->preparedProject->processing->plan,
            temporal->exactClock)) return false;
    const auto callbackWasRegistered = callbackRegistered_;
    detachAudioCallback();
    preparedProject_.swap(projectCandidate->preparedProject);
    preparedTemporalContext_.swap(temporal);
    projectSampleRate_ = preparedProject_->specification.projectSampleRate;
    masterMix_ = preparedProject_->specification.masterMix;
    configureRealtimeEngine();
    realtimeEngine_.resetTemporalSessionState();
    modelCommit.execute();
    if (callbackWasRegistered) {
        try { attachAudioCallback(); }
        catch (...) {
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
audio::AudioControlRequestResult JuceAudioDeviceAdapter::tryRequestPause() noexcept {
    return realtimeEngine_.tryRequestPause();
}
audio::AudioControlRequestResult JuceAudioDeviceAdapter::tryRequestStop() noexcept {
    return realtimeEngine_.tryRequestStop();
}
audio::AudioControlRequestResult JuceAudioDeviceAdapter::tryRequestSeek(
    timeline::ProjectFramePosition position) noexcept {
    return realtimeEngine_.tryRequestSeek(position);
}
audio::AudioControlRequestResult JuceAudioDeviceAdapter::trySetLoopEnabled(bool enabled) noexcept {
    return realtimeEngine_.trySetLoopEnabled(enabled);
}
audio::AudioControlRequestResult JuceAudioDeviceAdapter::trySetMetronomeEnabled(bool enabled) noexcept {
    return realtimeEngine_.trySetMetronomeEnabled(enabled);
}
audio::AudioControlRequestResult JuceAudioDeviceAdapter::trySetMetronomeLevel(
    audio::MetronomeLevelDb level) noexcept {
    return realtimeEngine_.trySetMetronomeLevel(level);
}
audio::AudioControlRequestResult
JuceAudioDeviceAdapter::trySetInputMonitoringEnabled(bool enabled) noexcept {
    const auto request = realtimeEngine_.trySetInputMonitoringEnabled(enabled);
    if (!request.accepted) return request;
    if (enabled) {
        // prepareInputMonitoring has made the physical setup transactional;
        // acceptance of this bounded RT request commits that setup.
        monitoringInputDemand_ = true;
        pendingMonitoringPreflight_.reset();
        monitoringDiagnostic_.clear();
    } else {
        // Disable is intentionally only an RT-route transition.  Do not touch
        // AudioDeviceManager here: a Recording may still own the same input.
        monitoringInputDemand_ = false;
        monitoringInputChannels_ = 0;
    }
    return request;
}

int JuceAudioDeviceAdapter::activeFoundationInputChannels(
    juce::AudioIODevice& device) noexcept {
    const auto available = device.getInputChannelNames().size();
    const auto& active = device.getActiveInputChannels();
    int count{};
    for (; count < 2 && count < available; ++count) {
        if (!active[count]) break;
    }
    return count;
}

bool JuceAudioDeviceAdapter::restoreMonitoringPreflight(
    MonitoringPreflightRollback& rollback, std::string& errorMessage) noexcept {
    try {
        detachAudioCallback(true);
        const auto setupError = deviceManager_.setAudioDeviceSetup(rollback.setup, false);
        auto* device = deviceManager_.getCurrentAudioDevice();
        deviceSampleRate_ = timeline::SampleRate{
            device != nullptr ? device->getCurrentSampleRate() : 0.0};
        if (setupError.isNotEmpty() || !deviceSampleRate_.isValid() ||
            !reprepareForCurrentDevice(errorMessage) ||
            !realtimeEngine_.restoreTemporalCheckpoint(rollback.checkpoint)) {
            if (errorMessage.empty()) {
                errorMessage = setupError.isNotEmpty()
                    ? setupError.toStdString()
                    : "Previous audio configuration could not be restored";
            }
            return false;
        }
        if (rollback.callbackWasRegistered) attachAudioCallback(true);
        refreshState();
        return true;
    } catch (const std::exception& error) {
        errorMessage = error.what();
    } catch (...) {
        errorMessage = "Previous audio configuration could not be restored";
    }
    return false;
}

audio::InputMonitoringPreparationResult
JuceAudioDeviceAdapter::prepareInputMonitoring() {
    // Repeated Enable requests never touch an already-certified input route.
    if (monitoringInputDemand_) {
        realtimeEngine_.notifyInputMonitoringHardwarePrepared();
        return {true, {}};
    }
    auto* device = deviceManager_.getCurrentAudioDevice();
    if (device == nullptr || realtimeEngine_.deviceState() !=
                                 audio::DeviceProcessingState::operational) {
        monitoringDiagnostic_ = "No operational audio device is available for input monitoring";
        return {false, monitoringDiagnostic_};
    }
    if (const auto active = activeFoundationInputChannels(*device); active > 0) {
        monitoringInputChannels_ = active;
        monitoringDiagnostic_.clear();
        realtimeEngine_.notifyInputMonitoringHardwarePrepared();
        return {true, {}};
    }

    auto* deviceType = deviceManager_.getCurrentDeviceTypeObject();
    if (deviceType == nullptr) {
        monitoringDiagnostic_ = "No audio input device type is available for input monitoring";
        return {false, monitoringDiagnostic_};
    }
    const auto previousSetup = deviceManager_.getAudioDeviceSetup();
    MonitoringPreflightRollback rollback{previousSetup,
                                          realtimeEngine_.temporalCheckpoint(),
                                          callbackRegistered_};
    const auto names = deviceType->getDeviceNames(true);
    const auto defaultIndex = deviceType->getDefaultDeviceIndex(true);
    const auto configure = [&](int requestedChannels, std::string& error) -> bool {
        const auto candidate = detail::makeRecordingInputSetup(
            previousSetup, names, defaultIndex, requestedChannels);
        if (!candidate.success()) {
            error = candidate.errorMessage;
            return false;
        }
        const auto setupError = deviceManager_.setAudioDeviceSetup(candidate.setup, false);
        if (setupError.isNotEmpty()) {
            error = setupError.toStdString();
            return false;
        }
        auto* configured = deviceManager_.getCurrentAudioDevice();
        if (configured == nullptr ||
            activeFoundationInputChannels(*configured) < requestedChannels) {
            error = "The selected audio input device does not provide the requested active input channels";
            return false;
        }
        deviceSampleRate_ = timeline::SampleRate{configured->getCurrentSampleRate()};
        if (!deviceSampleRate_.isValid() || !reprepareForCurrentDevice(error)) {
            if (error.empty()) error = "The input device has an invalid sample rate";
            return false;
        }
        monitoringInputChannels_ = requestedChannels;
        return true;
    };

    try {
        detachAudioCallback(true);
        std::string error;
        // Prefer the first stereo pair; retry a mono route transactionally.
        bool configured = configure(2, error);
        if (!configured) configured = configure(1, error);
        if (!configured) {
            std::string restoreError;
            if (!restoreMonitoringPreflight(rollback, restoreError)) {
                realtimeEngine_.deviceErrorPreservingTransport();
                error += "; rollback failed: " + restoreError;
            }
            monitoringInputChannels_ = 0;
            monitoringDiagnostic_ = "Audio inputs could not be activated for monitoring: " + error;
            return {false, monitoringDiagnostic_};
        }
        if (rollback.callbackWasRegistered) attachAudioCallback(true);
        pendingMonitoringPreflight_ = rollback;
        monitoringDiagnostic_.clear();
        realtimeEngine_.notifyInputMonitoringHardwarePrepared();
        refreshState();
        return {true, {}};
    } catch (const std::exception& error) {
        std::string restoreError;
        if (!restoreMonitoringPreflight(rollback, restoreError)) {
            realtimeEngine_.deviceErrorPreservingTransport();
        }
        monitoringInputChannels_ = 0;
        monitoringDiagnostic_ = std::string{"Audio inputs could not be activated for monitoring: "} + error.what();
        if (!restoreError.empty()) monitoringDiagnostic_ += "; rollback failed: " + restoreError;
        return {false, monitoringDiagnostic_};
    } catch (...) {
        std::string restoreError;
        static_cast<void>(restoreMonitoringPreflight(rollback, restoreError));
        monitoringInputChannels_ = 0;
        monitoringDiagnostic_ = "Audio inputs could not be activated for monitoring";
        return {false, monitoringDiagnostic_};
    }
}

void JuceAudioDeviceAdapter::cancelPreparedInputMonitoring() noexcept {
    if (!pendingMonitoringPreflight_) return;
    std::string error;
    auto rollback = std::move(*pendingMonitoringPreflight_);
    pendingMonitoringPreflight_.reset();
    if (!restoreMonitoringPreflight(rollback, error)) {
        realtimeEngine_.deviceErrorPreservingTransport();
        monitoringDiagnostic_ = "Input monitoring preflight rollback failed: " + error;
    }
    monitoringInputChannels_ = 0;
}

void JuceAudioDeviceAdapter::clearMonitoringDemandForLoss() noexcept {
    monitoringInputDemand_ = false;
    monitoringInputChannels_ = 0;
    pendingMonitoringPreflight_.reset();
}
bool JuceAudioDeviceAdapter::trySetMonitorGain(audio::MonitorGainDb gain) noexcept {
    return realtimeEngine_.trySetMonitorGain(gain);
}
audio::RealtimeTransportSnapshot JuceAudioDeviceAdapter::transportSnapshot() const noexcept {
    return realtimeEngine_.transportSnapshot();
}
audio::RealtimeTransportSnapshot JuceAudioDeviceAdapter::projectedTransportSnapshot() noexcept {
    return realtimeEngine_.projectedTransportSnapshot();
}
mixer::MeterSnapshot JuceAudioDeviceAdapter::meterSnapshot() const noexcept {
    return realtimeEngine_.meterSnapshot();
}

audio::RecordingPreflightResult JuceAudioDeviceAdapter::prepareRecording(
    const audio::RecordingPreflightRequest& request) {
    if (!request.track.isValid() || request.projectFile.empty() ||
        !request.projectFile.is_absolute()) {
        return {{}, "Save the project to an absolute path before recording"};
    }
    if (recordingWriter_ || realtimeEngine_.recordingSnapshot().busy())
        return {{}, "Another recording is already prepared or active"};
    const auto channels = static_cast<int>(media::channelCount(request.layout));
    if (channels < 1 || channels > 2)
        return {{}, "Only mono and stereo recording are supported"};
    auto* device = deviceManager_.getCurrentAudioDevice();
    if (device == nullptr)
        return {{}, "No audio device is available"};

    const auto callbackWasRegistered = callbackRegistered_;
    const auto previousSetup = deviceManager_.getAudioDeviceSetup();
    const auto checkpoint = realtimeEngine_.temporalCheckpoint();
    auto* deviceType = deviceManager_.getCurrentDeviceTypeObject();
    if (deviceType == nullptr)
        return {{}, "No audio input device type is available"};
    const auto inputDeviceNames = deviceType->getDeviceNames(true);
    const auto requestedInputChannels = std::max(
        channels, monitoringInputDemand_ ? std::max(1, monitoringInputChannels_) : 0);
    const auto inputSetup = detail::makeRecordingInputSetup(
        previousSetup, inputDeviceNames, deviceType->getDefaultDeviceIndex(true),
        requestedInputChannels);
    if (!inputSetup.success()) return {{}, inputSetup.errorMessage};

    detachAudioCallback(true);
    const auto setup = inputSetup.setup;
    const auto rollback = [&](std::string message) {
        const auto restoreError = deviceManager_.setAudioDeviceSetup(previousSetup, false);
        auto* restoredDevice = deviceManager_.getCurrentAudioDevice();
        deviceSampleRate_ = timeline::SampleRate{
            restoredDevice != nullptr ? restoredDevice->getCurrentSampleRate() : 0.0};
        std::string preparationError;
        const bool restored = restoreError.isEmpty() && deviceSampleRate_.isValid() &&
            reprepareForCurrentDevice(preparationError) &&
            realtimeEngine_.restoreTemporalCheckpoint(checkpoint);
        if (restored && callbackWasRegistered) {
            try { attachAudioCallback(true); }
            catch (...) {
                realtimeEngine_.deviceErrorPreservingTransport();
                return audio::RecordingPreflightResult{
                    {}, message + "; previous audio callback could not be restored"};
            }
        }
        if (restored) {
            refreshState();
            return audio::RecordingPreflightResult{{}, std::move(message)};
        }
        realtimeEngine_.deviceErrorPreservingTransport();
        if (!preparationError.empty())
            message += "; rollback failed: " + preparationError;
        else if (restoreError.isNotEmpty())
            message += "; rollback failed: " + restoreError.toStdString();
        else
            message += "; rollback failed";
        return audio::RecordingPreflightResult{{}, std::move(message)};
    };
    const auto setupError = deviceManager_.setAudioDeviceSetup(setup, false);
    if (setupError.isNotEmpty())
        return rollback("Audio inputs could not be activated: " + setupError.toStdString());
    device = deviceManager_.getCurrentAudioDevice();
    if (device == nullptr || !detail::recordingInputChannelsAreAvailableAndActive(
                                 static_cast<std::size_t>(device->getInputChannelNames().size()),
                                 device->getActiveInputChannels(), requestedInputChannels)) {
        return rollback("The selected audio input device does not provide the requested active input channels");
    }
    deviceSampleRate_ = timeline::SampleRate{
        device != nullptr ? device->getCurrentSampleRate() : 0.0};
    std::string reprepareError;
    if (!deviceSampleRate_.isValid() ||
        !reprepareForCurrentDevice(reprepareError)) {
        return rollback(reprepareError.empty()
                            ? "The input device has an invalid sample rate"
                            : std::move(reprepareError));
    }
    if (monitoringInputDemand_) {
        monitoringInputChannels_ = activeFoundationInputChannels(*device);
    }

    try {
        constexpr double captureSeconds = 2.0;
        const auto capacityDouble = std::ceil(deviceSampleRate_.hertz() * captureSeconds);
        if (capacityDouble <= 0.0 ||
            capacityDouble > static_cast<double>(std::numeric_limits<std::size_t>::max()) ||
            !realtimeEngine_.prepareRecordingCapture(
                static_cast<std::size_t>(capacityDouble))) {
            return rollback("The realtime recording buffer could not be prepared");
        }

        const auto audioDirectory = request.projectFile.parent_path() /
            (request.projectFile.stem().string() + " Audio");
        std::error_code filesystemError;
        std::filesystem::create_directories(audioDirectory, filesystemError);
        if (filesystemError)
            throw std::filesystem::filesystem_error(
                "create recording directory", audioDirectory, filesystemError);

        const auto session = nextRecordingSession_;
        if (session == 0) throw std::runtime_error("Recording session counter exhausted");
        std::ostringstream name;
        name << "Recording " << std::setw(6) << std::setfill('0') << session;
        auto published = audioDirectory / (name.str() + ".wav");
        while (std::filesystem::exists(published, filesystemError) && !filesystemError) {
            if (++nextRecordingSession_ == 0)
                throw std::runtime_error("Recording session counter exhausted");
            name.str({});
            name.clear();
            name << "Recording " << std::setw(6) << std::setfill('0')
                 << nextRecordingSession_;
            published = audioDirectory / (name.str() + ".wav");
        }
        if (filesystemError)
            throw std::filesystem::filesystem_error(
                "inspect recording destination", published, filesystemError);
        std::filesystem::path temporary;
        for (;;) {
            if (nextRecordingTemporaryNonce_ == 0)
                throw std::runtime_error("Recording temporary counter exhausted");
            temporary = audioDirectory / ("." + name.str() + "." +
                                          std::to_string(nextRecordingTemporaryNonce_++) +
                                          ".part.wav");
            auto exclusive = createExclusiveTemporaryFile(temporary, filesystemError);
            if (exclusive) {
                // Publish cleanup metadata before allocation or writer setup can
                // throw.  The identity, not the pathname, authorizes cleanup.
                recordingTemporaryPath_ = temporary;
                recordingPublishedPath_ = published;
                recordingTemporaryIdentity_ = exclusive->identity;
                recordingRecoveryDirectory_ = audioDirectory;
                recordingRecoverySessionId_ = audio::makeRecordingRecoverySessionId();
                audio::RecordingRecoveryMarker marker;
                marker.sessionId = recordingRecoverySessionId_;
                marker.classification = audio::RecordingRecoveryClass::temporary;
                marker.temporaryName = temporary.filename();
                marker.publishedName = published.filename();
                marker.layout = request.layout;
                marker.deviceSampleRate = deviceSampleRate_;
                const auto recovery = persistInitialRecoveryMetadata(
                    audioDirectory, marker, recordingRecoveryMarkerWriteOptions_);
                recordingRecoveryMetadataAvailable_ = recovery.available;
                recordingRecoveryWarning_ = recovery.warning;
                auto file = std::make_unique<files::RecordingFileHandle>(
                    exclusive->release(), &recordingIoFaults_);
                std::unique_ptr<juce::OutputStream> stream =
                    std::make_unique<OwnedDescriptorOutputStream>(*file);
                juce::WavAudioFormat format;
                auto writer = format.createWriterFor(
                    stream, juce::AudioFormatWriterOptions{}
                                .withSampleRate(deviceSampleRate_.hertz())
                                .withNumChannels(channels)
                                .withBitsPerSample(32)
                                .withSampleFormat(
                                    juce::AudioFormatWriterOptions::SampleFormat::floatingPoint));
                if (!writer)
                    throw std::runtime_error("The WAV writer could not be created");
                recordingWriter_ = std::move(writer);
                recordingFile_ = std::move(file);
                break;
            }
            if (filesystemError == std::errc::file_exists) {
                filesystemError.clear();
                continue;
            }
            throw std::filesystem::filesystem_error(
                "create exclusive recording temporary", temporary, filesystemError);
        }
        recordingDrainBuffer_.setSize(channels, 8192, false, true, false);
        recordingWriterFailed_ = false;
        recordingWriterError_.clear();
        const audio::RecordingRequest prepared{
            nextRecordingSession_++, request.track, request.layout};
        if (callbackWasRegistered) attachAudioCallback(true);
        refreshState();
        return {prepared, {}, recordingRecoveryWarning_};
    } catch (const std::exception& error) {
        recordingWriter_.reset();
        recordingFile_.reset();
        realtimeEngine_.resetRecordingCapture();
        const bool retainedTemporary = !recordingTemporaryPath_.empty();
        recordingTemporaryPath_.clear();
        recordingPublishedPath_.clear();
        recordingTemporaryIdentity_.reset();
        recordingPublishedIdentity_.reset();
        recordingRecoverySessionId_.clear();
        recordingRecoveryDirectory_.clear();
        recordingRecoveryMetadataAvailable_ = false;
        recordingRecoveryWarning_.clear();
        return rollback(std::string{error.what()} + (retainedTemporary
            ? "; recording temporary retained as an ownership-safe orphan"
            : ""));
    }
}

audio::AudioControlRequestResult JuceAudioDeviceAdapter::tryRequestRecord(
    audio::RecordingRequest request) noexcept {
    if (!recordingWriter_ || request.session == 0) return {};
    return realtimeEngine_.tryRequestRecord(request);
}

bool JuceAudioDeviceAdapter::tryCancelRecording() noexcept {
    return realtimeEngine_.tryCancelRecording();
}

void JuceAudioDeviceAdapter::serviceRecording() noexcept {
    if (!recordingWriter_) return;
    const auto snapshot = realtimeEngine_.recordingSnapshot();
    const auto channels = static_cast<int>(media::channelCount(snapshot.layout));
    if (channels < 1 || channels > recordingDrainBuffer_.getNumChannels()) return;
    std::array<float*, 2> output{};
    for (int channel = 0; channel < channels; ++channel)
        output[static_cast<std::size_t>(channel)] =
            recordingDrainBuffer_.getWritePointer(channel);
    while (true) {
        const auto drained = realtimeEngine_.drainRecording(
            {output.data(), static_cast<std::size_t>(channels),
             static_cast<std::size_t>(recordingDrainBuffer_.getNumSamples())});
        if (drained == 0) break;
        if (!recordingWriter_->writeFromAudioSampleBuffer(
                recordingDrainBuffer_, 0, static_cast<int>(drained))) {
            recordingWriterFailed_ = true;
            recordingWriterError_ = "The captured samples could not be written to WAV";
            static_cast<void>(realtimeEngine_.failRecording(
                snapshot.session, audio::RecordingFailure::writerFailed));
            break;
        }
    }
    if (recordingWriterFailed_ && snapshot.phase == audio::RecordingPhase::capturing)
        static_cast<void>(realtimeEngine_.failRecording(
            snapshot.session, audio::RecordingFailure::writerFailed));
}

audio::RecordingSnapshot JuceAudioDeviceAdapter::recordingSnapshot() const noexcept {
    auto result = realtimeEngine_.recordingSnapshot();
    if (recordingWriterFailed_ && result.phase != audio::RecordingPhase::capturing &&
        result.phase != audio::RecordingPhase::prepared) {
        result.phase = audio::RecordingPhase::failed;
        result.failure = audio::RecordingFailure::writerFailed;
    }
    return result;
}

audio::RecordingFinalizationResult JuceAudioDeviceAdapter::finalizeRecording() {
    serviceRecording();
    const auto capture = recordingSnapshot();
    audio::RecordingFinalizationResult result;
    result.capture = capture;
    result.publishedFile = recordingPublishedPath_;
    result.warningMessage = recordingRecoveryWarning_;
    if (capture.phase != audio::RecordingPhase::complete ||
        capture.acceptedDeviceFrames.value == 0 || recordingWriterFailed_) {
        result.errorMessage = recordingWriterError_.empty()
                                  ? "Capture did not complete successfully"
                                  : recordingWriterError_;
        return result;
    }
    std::string finalizationError;
    if (!finalizeRecordingFile(finalizationError)) {
        result.errorMessage = std::move(finalizationError);
        return result;
    }
    // Hard-link publication is an atomic no-replace operation in the sibling
    // directory. Unlike rename on POSIX, it cannot replace a file which
    // appeared after name selection.
    if (!recordingTemporaryIdentity_ ||
        !pathHasIdentity(recordingTemporaryPath_, *recordingTemporaryIdentity_)) {
        result.errorMessage = "The owned recording temporary was replaced before publication";
        return result;
    }
    const char* storageError{};
    if (!files::publishRecordingNoReplace(recordingTemporaryPath_, recordingPublishedPath_,
                                          &recordingIoFaults_, storageError)) {
        result.errorMessage = storageError == nullptr ? "The recorded WAV could not be published"
                                                      : storageError;
        return result;
    }
    if (!pathHasIdentity(recordingPublishedPath_, *recordingTemporaryIdentity_)) {
        result.errorMessage = "The recording publication identity could not be verified";
        return result;
    }
    recordingPublishedIdentity_ = recordingTemporaryIdentity_;
    // Persist the final path before the directory durability barrier. If that
    // barrier fails, this immutable marker still tells recovery to prefer the
    // no-replace published candidate over its retained temporary sibling.
    audio::RecordingRecoveryMarker closedMarker;
    closedMarker.sessionId = recordingRecoverySessionId_;
    closedMarker.classification = audio::RecordingRecoveryClass::closedUncommitted;
    closedMarker.temporaryName = recordingTemporaryPath_.filename();
    closedMarker.publishedName = recordingPublishedPath_.filename();
    closedMarker.layout = capture.layout;
    closedMarker.deviceSampleRate = capture.deviceSampleRate;
    closedMarker.acceptedFrames = capture.acceptedDeviceFrames;
    std::string markerError;
    if (recordingRecoveryMetadataAvailable_ &&
        !audio::writeRecordingRecoveryMarker(recordingRecoveryDirectory_, closedMarker, markerError))
        result.warningMessage = "Publication recovery marker could not be persisted: " + markerError;
    if (!files::syncRecordingDirectory(recordingPublishedPath_.parent_path(),
                                       &recordingIoFaults_, storageError)) {
        result.errorMessage = storageError == nullptr ? "Recording directory fsync failed"
                                                      : storageError;
        return result;
    }
    // POSIX has no atomic identity-conditioned unlink.  Retaining this hidden
    // temporary is safer than a check-then-unlink that could delete a pathname
    // replacement.  It is reported to the application as an orphan.
    if (!result.warningMessage.empty()) result.warningMessage += "; ";
    result.warningMessage +=
        "Recorded WAV published; temporary retained as an ownership-safe orphan";
    auto prepared = prepareWav(recordingPublishedPath_);
    if (!prepared.success()) {
        result.errorMessage = prepared.errorMessage.empty()
                                  ? "The recorded WAV could not be decoded"
                                  : std::move(prepared.errorMessage);
        return result;
    }
    // Decoding is pathname-based. Reject a destination which changed while it
    // was being verified so ProjectState can never commit that replacement.
    if (!recordingPublishedIdentity_ ||
        !pathHasIdentity(recordingPublishedPath_, *recordingPublishedIdentity_)) {
        result.errorMessage = "The recorded WAV was replaced during verification";
        return result;
    }
    result.prepared = std::move(prepared.prepared);
    audio::RecordingRecoveryMarker marker;
    marker.sessionId = recordingRecoverySessionId_;
    marker.classification = audio::RecordingRecoveryClass::publishedFinal;
    marker.temporaryName = recordingTemporaryPath_.filename();
    marker.publishedName = recordingPublishedPath_.filename();
    marker.layout = capture.layout;
    marker.deviceSampleRate = capture.deviceSampleRate;
    marker.acceptedFrames = capture.acceptedDeviceFrames;
    marker.fingerprint = result.prepared->media.fingerprint;
    markerError.clear();
    if (recordingRecoveryMetadataAvailable_ &&
        !audio::writeRecordingRecoveryMarker(recordingRecoveryDirectory_, marker, markerError)) {
        result.warningMessage += "; recovery marker could not be updated: " + markerError;
    }
    return result;
}

audio::RecordingCleanupResult JuceAudioDeviceAdapter::discardRecordingWithDiagnostics(
    bool removePublished, std::string primaryError) noexcept {
    audio::RecordingCleanupResult result;
    result.primaryError = std::move(primaryError);
    if (!recordingRecoveryMetadataAvailable_ && !recordingRecoveryWarning_.empty()) {
        if (!result.primaryError.empty()) result.primaryError += "; ";
        result.primaryError += recordingRecoveryWarning_;
    }
    const auto capture = realtimeEngine_.recordingSnapshot();
    if (capture.phase == audio::RecordingPhase::prepared ||
        capture.phase == audio::RecordingPhase::capturing)
        static_cast<void>(realtimeEngine_.failRecording(
            capture.session, audio::RecordingFailure::cancelled));
    if (recordingWriter_) {
        std::string closeError;
        if (!finalizeRecordingFile(closeError) && result.primaryError.empty())
            result.primaryError = std::move(closeError);
    }
    // Never delete recording media by pathname: a prior identity observation is
    // not an atomic authorization for unlink on supported POSIX filesystems.
    // Retention is reported as an orphan instead of risking external deletion.
    const bool retainedTemporary = !recordingTemporaryPath_.empty();
    const bool retainedPublished = removePublished && recordingPublishedIdentity_.has_value();
    const auto session = recordingRecoverySessionId_;
    if (retainedTemporary) {
        result.retainedArtifacts.push_back({session, recordingTemporaryPath_,
            audio::RecordingRecoveryClass::temporary, false});
    }
    if (retainedPublished) {
        result.retainedArtifacts.push_back({session, recordingPublishedPath_,
            audio::RecordingRecoveryClass::publishedFinal, true});
    }
    recordingTemporaryPath_.clear();
    recordingPublishedPath_.clear();
    recordingTemporaryIdentity_.reset();
    recordingPublishedIdentity_.reset();
    recordingWriterFailed_ = false;
    recordingWriterError_.clear();
    recordingFile_.reset();
    recordingRecoverySessionId_.clear();
    recordingRecoveryDirectory_.clear();
    recordingRecoveryMetadataAvailable_ = false;
    recordingRecoveryWarning_.clear();
    realtimeEngine_.resetRecordingCapture();
    return result;
}

audio::RecordingCleanupResult JuceAudioDeviceAdapter::shutdownRecording() noexcept {
    const auto before = realtimeEngine_.recordingSnapshot();
    if (before.phase == audio::RecordingPhase::idle && !recordingWriter_) return {};
    // removeAudioCallback serialises with the final render before the non-RT
    // drain touches the ring. The adapter remains alive through this method.
    detachAudioCallback();
    static_cast<void>(realtimeEngine_.failRecording(before.session, audio::RecordingFailure::shutdown));
    serviceRecording(); // drain only frames the RT producer already published
    std::string finalizationError;
    const bool finishSucceeded = !recordingWriter_ || finalizeRecordingFile(finalizationError);
    const bool closed = finishSucceeded && !recordingWriterFailed_;
    if (recordingRecoveryMetadataAvailable_ && !recordingRecoverySessionId_.empty() &&
        !recordingRecoveryDirectory_.empty()) {
        audio::RecordingRecoveryMarker marker;
        marker.sessionId = recordingRecoverySessionId_;
        marker.classification = closed ? audio::RecordingRecoveryClass::closedUncommitted
                                       : audio::RecordingRecoveryClass::incomplete;
        marker.temporaryName = recordingTemporaryPath_.filename();
        marker.publishedName = recordingPublishedPath_.filename();
        marker.layout = before.layout;
        marker.deviceSampleRate = before.deviceSampleRate.isValid()
                                      ? before.deviceSampleRate : deviceSampleRate_;
        marker.acceptedFrames = before.acceptedDeviceFrames;
        std::string ignored;
        static_cast<void>(audio::writeRecordingRecoveryMarker(recordingRecoveryDirectory_, marker, ignored));
    }
    auto result = discardRecordingWithDiagnostics(
        true, closed ? "Recording cancelled during application shutdown" : finalizationError);
    for (auto& artifact : result.retainedArtifacts) {
        if (closed && artifact.classification == audio::RecordingRecoveryClass::temporary) {
            artifact.classification = audio::RecordingRecoveryClass::closedUncommitted;
            artifact.recoverable = true;
        }
    }
    return result;
}

bool JuceAudioDeviceAdapter::discardRecording(bool removePublished) noexcept {
    return discardRecordingWithDiagnostics(removePublished).clean();
}

void JuceAudioDeviceAdapter::confirmRecordingCommit() noexcept {
    recordingWriter_.reset();
    recordingFile_.reset();
    // See discardRecording: the successfully published final remains valid and
    // its hidden temporary is retained rather than deleted by pathname.
    recordingTemporaryPath_.clear();
    recordingPublishedPath_.clear();
    recordingTemporaryIdentity_.reset();
    recordingPublishedIdentity_.reset();
    recordingWriterFailed_ = false;
    recordingWriterError_.clear();
    recordingRecoverySessionId_.clear();
    recordingRecoveryDirectory_.clear();
    recordingRecoveryMetadataAvailable_ = false;
    recordingRecoveryWarning_.clear();
    realtimeEngine_.resetRecordingCapture();
}

void JuceAudioDeviceAdapter::audioDeviceIOCallbackWithContext(
    const float* const* input, int inputChannels,
    float* const* output, int outputChannels, int frames,
    const juce::AudioIODeviceCallbackContext&) noexcept {
    realtimeEngine_.processBlock(
        {input, static_cast<std::size_t>(std::max(0, inputChannels)),
         static_cast<std::size_t>(std::max(0, frames))},
        {output, static_cast<std::size_t>(std::max(0, outputChannels)),
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
        if (preserveTransportDuringRegistration_.load(std::memory_order_acquire))
            realtimeEngine_.deviceInitialisingPreservingTransport();
        else
            realtimeEngine_.deviceInitialising();
    } else {
        realtimeEngine_.deviceError();
    }
}

void JuceAudioDeviceAdapter::audioDeviceStopped() noexcept {
    if (preserveTransportDuringRegistration_.load(std::memory_order_acquire))
        realtimeEngine_.deviceInitialisingPreservingTransport();
    // An uncontrolled stop is consumed by pollDeviceLifecycle() on the
    // serialised control side.  This callback must not mutate demand or RT
    // state because JUCE does not give it a general control-thread contract.
    if (!suppressLifecycleNotification_.load(std::memory_order_acquire)) {
        pendingLifecycleEvent_.store(PendingLifecycleEvent::stopped,
                                     std::memory_order_release);
    }
}

void JuceAudioDeviceAdapter::audioDeviceError(const juce::String&) {
    // This callback can run on an unspecified JUCE thread. Publish only a
    // bounded event; all optional/demand cleanup happens in polling.
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
    // beginDeviceReinitialisation() uses closeDevice(false) as its controlled
    // quiescent handoff.  Preserve the demand there so a successful restart
    // can rearm Monitoring; a real close/loss always uses the full close path.
    if (publishClosedState) clearMonitoringDemandForLoss();
    if (publishClosedState) {
        certifiedDeviceSampleRate_ = {};
        certifiedDeviceBufferSize_ = 0;
        certifiedDevice_ = nullptr;
    }
    realtimeEngine_.deviceUnavailable();
    if (publishClosedState) {
        realtimeEngine_.releasePreparedReferences();
        preparedProject_.reset();
        preparedTemporalContext_.reset();
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

void JuceAudioDeviceAdapter::detachAudioCallback(bool preserveTransport) noexcept {
    if (callbackRegistered_) {
        // removeAudioCallback serialises against the last render. Its stopped
        // notification must not erase the checkpoint we read AFTER removal.
        preserveTransportDuringRegistration_.store(preserveTransport, std::memory_order_release);
        suppressLifecycleNotification_.store(true, std::memory_order_release);
        deviceManager_.removeAudioCallback(this);
        suppressLifecycleNotification_.store(false, std::memory_order_release);
        preserveTransportDuringRegistration_.store(false, std::memory_order_release);
        callbackRegistered_ = false;
    }
}

void JuceAudioDeviceAdapter::attachAudioCallback(bool preserveTransport) {
    if (!callbackRegistered_ && deviceManager_.getCurrentAudioDevice() != nullptr) {
        preserveTransportDuringRegistration_.store(preserveTransport,
                                                    std::memory_order_release);
        try {
            deviceManager_.addAudioCallback(this);
        } catch (...) {
            preserveTransportDuringRegistration_.store(false,
                                                        std::memory_order_release);
            throw;
        }
        preserveTransportDuringRegistration_.store(false,
                                                    std::memory_order_release);
        callbackRegistered_ = true;
        auto* device = deviceManager_.getCurrentAudioDevice();
        if (device != nullptr && device->isPlaying() && deviceSampleRate_.isValid()) {
            realtimeEngine_.deviceConsumerStarted();
        }
    }
}

void JuceAudioDeviceAdapter::configureRealtimeEngine() noexcept {
    if (preparedProject_ == nullptr || preparedProject_->processing == nullptr) {
        realtimeEngine_.configure({projectSampleRate_, {}, {}, {}, false});
        if (deviceSampleRate_.isValid() && !realtimeEngine_.prepareLegacyDeviceRate(deviceSampleRate_))
            realtimeEngine_.deviceError();
        static_cast<void>(realtimeEngine_.configureTemporalContext(
            preparedTemporalContext_.get()));
        return;
    }
    realtimeEngine_.configure(preparedProject_->processing->plan,
                              preparedProject_->processing->runtime);
    static_cast<void>(realtimeEngine_.configureTemporalContext(
        preparedTemporalContext_.get()));
}

bool JuceAudioDeviceAdapter::prepareProjectPlan(
    PreparedProject& candidate,
    const audio::ProcessingPlanSpecification& specification,
    std::string& errorMessage) {
    auto effectiveSpecification = specification;
    effectiveSpecification.processingSampleRate =
        deviceSampleRate_.isValid() ? deviceSampleRate_
                                    : specification.projectSampleRate;
    effectiveSpecification.processingMode =
        processors::ProcessingMode::realtime;
    candidate.resources.erase(
        std::remove_if(candidate.resources.begin(), candidate.resources.end(),
                       [&effectiveSpecification](const auto& resource) {
                           return std::none_of(
                               effectiveSpecification.sources.begin(),
                               effectiveSpecification.sources.end(),
                               [&resource](const auto& source) {
                                   return source.id == resource.id;
                               });
                       }),
        candidate.resources.end());
    std::vector<audio::PreparedSourceView> sources;
    sources.reserve(effectiveSpecification.sources.size());
    for (const auto& sourceSpecification : effectiveSpecification.sources) {
        const auto resource = std::find_if(
            candidate.resources.begin(), candidate.resources.end(),
            [id = sourceSpecification.id](const auto& candidateResource) {
                return candidateResource.id == id;
            });
        if (resource == candidate.resources.end() || resource->audio == nullptr) {
            errorMessage = "Project source has no prepared PCM resource";
            return false;
        }
        const auto& source = *resource->audio;
        audio::PreparedSourceView view;
        view.id = sourceSpecification.id;
        view.channelCount = static_cast<std::uint32_t>(
            source.samples.getNumChannels());
        view.frameCount = {static_cast<std::uint64_t>(
            source.samples.getNumSamples())};
        view.sampleRate = source.sourceSampleRate;
        view.layout = sourceSpecification.layout;
        for (std::size_t channel = 0; channel < view.channelCount; ++channel) {
            view.channels[channel] =
                source.samples.getReadPointer(static_cast<int>(channel));
        }
        sources.push_back(view);
    }
    // The plan owns all callback scratch, including Monitoring's alias-safe
    // staging.  Bind its capacity to the current device buffer while the
    // callback is quiescent so a controlled buffer-size change never asks RT
    // to resize storage.
    std::size_t blockCapacity = audio::defaultProcessingBlockCapacity;
    if (auto* device = deviceManager_.getCurrentAudioDevice(); device != nullptr &&
        device->getCurrentBufferSizeSamples() > 0) {
        blockCapacity = static_cast<std::size_t>(device->getCurrentBufferSizeSamples());
    }
    auto prepared = audio::prepareProcessingPlanFromSources(
        effectiveSpecification, sources, blockCapacity);
    if (!prepared.success()) {
        errorMessage = std::move(prepared.errorMessage);
        return false;
    }
    candidate.specification = std::move(effectiveSpecification);
    candidate.processing = std::move(prepared.prepared);
    return true;
}

bool JuceAudioDeviceAdapter::reprepareForCurrentDevice(
    std::string& errorMessage) {
    // This entire operation runs with the callback quiescent. Preparation and
    // certification do not touch the live owners, clock, policies or revision.
    auto* currentDevice = deviceManager_.getCurrentAudioDevice();
    if (currentDevice != nullptr) {
        deviceSampleRate_ = timeline::SampleRate{currentDevice->getCurrentSampleRate()};
    }
    if (!deviceSampleRate_.isValid() ||
        (currentDevice != nullptr && currentDevice->getCurrentBufferSizeSamples() <= 0)) {
        errorMessage = "The current audio device configuration is invalid";
        return false;
    }
    const auto rate = projectSampleRate_.isValid() ? projectSampleRate_ : deviceSampleRate_;
    const auto checkpoint = realtimeEngine_.temporalCheckpoint();
    std::unique_ptr<PreparedProject> project;
    if (preparedProject_) {
        project = std::make_unique<PreparedProject>();
        project->resources = preparedProject_->resources;
        if (!prepareProjectPlan(*project, preparedProject_->specification, errorMessage)) return false;
    }
    std::unique_ptr<audio::PreparedTemporalContext> temporal;
    if (preparedTemporalContext_) {
        auto result = audio::prepareTemporalContext(preparedTemporalContext_->documentMap,
            preparedTemporalContext_->loop
                ? std::optional<musical::MusicalLoopRange>{preparedTemporalContext_->loop->musical}
                : std::nullopt,
            rate, deviceSampleRate_, preparedTemporalContext_->revision);
        if (!result.success()) { errorMessage = std::move(result.errorMessage); return false; }
        temporal = std::move(result.prepared);
    }
    const auto format = temporal ? temporal->exactClock
        : audio::exact::clockForPreparation(rate.hertz(), deviceSampleRate_.hertz());
    audio::exact::ProjectPhase certificate;
    if ((project && !audio::processingPlanSupportsClock(project->processing->plan, format)) ||
        !certificate.installPreparedFormat(format) ||
        !certificate.restore({{checkpoint.clock.position.value, checkpoint.clock.phase}})) {
        errorMessage = "Device configuration cannot preserve the exact temporal checkpoint";
        return false;
    }
    // No fallible preparation below. Retire ALL raw views while both old owners
    // still live; locals retain them until the new configuration is installed.
    realtimeEngine_.releasePreparedReferences();
    preparedProject_.swap(project);
    preparedTemporalContext_.swap(temporal);
    projectSampleRate_ = rate;
    configureRealtimeEngine();
    const auto restored = realtimeEngine_.restoreTemporalCheckpoint(checkpoint);
    jassert(restored);
    static_cast<void>(restored);
    // Only a controlled and successful reprepare may preserve Monitoring.
    // Device error/stopped/close paths clear the demand before reaching here.
    realtimeEngine_.restoreInputMonitoringAfterControlledReconfigure(
        monitoringInputDemand_);
    if (auto* device = deviceManager_.getCurrentAudioDevice()) {
        certifiedDeviceSampleRate_ = timeline::SampleRate{device->getCurrentSampleRate()};
        certifiedDeviceBufferSize_ = static_cast<std::size_t>(
            std::max(0, device->getCurrentBufferSizeSamples()));
        certifiedDevice_ = device;
    } else {
        certifiedDeviceSampleRate_ = {};
        certifiedDeviceBufferSize_ = 0;
        certifiedDevice_ = nullptr;
    }
    return true;
}

bool JuceAudioDeviceAdapter::commitPreparedProject(
    std::unique_ptr<PreparedProject>& candidate,
    audio::AudioFileCommitAction modelCommit,
    bool preserveTransport) noexcept {
    if (candidate == nullptr || candidate->processing == nullptr ||
        !modelCommit.isValid()) {
        return false;
    }
    const auto& plan = candidate->processing->plan;
    if (preparedTemporalContext_ &&
        (preparedTemporalContext_->projectSampleRate != plan.projectSampleRate ||
         preparedTemporalContext_->deviceSampleRate != plan.stereoProcessingFormat.sampleRate ||
         !audio::processingPlanSupportsClock(plan, preparedTemporalContext_->exactClock)))
        return false;
    const auto callbackWasRegistered = callbackRegistered_;
    detachAudioCallback(true);
    const auto checkpoint = realtimeEngine_.temporalCheckpoint();
    if (preserveTransport) {
        audio::exact::ProjectPhase certificate;
        const auto format = preparedTemporalContext_ ? preparedTemporalContext_->exactClock
                                                    : candidate->processing->plan.exactClock;
        if (!audio::processingPlanSupportsClock(candidate->processing->plan, format) ||
            !certificate.installPreparedFormat(format) ||
            !certificate.restore({{checkpoint.clock.position.value, checkpoint.clock.phase}})) {
            if (callbackWasRegistered) {
                try { attachAudioCallback(true); }
                catch (...) { realtimeEngine_.deviceErrorPreservingTransport(); }
            }
            return false;
        }
    }
    preparedProject_.swap(candidate);
    projectSampleRate_ = preparedProject_->specification.projectSampleRate;
    masterMix_ = preparedProject_->specification.masterMix;
    configureRealtimeEngine();
    // Installing a project plan deliberately reconfigures the RT engine while
    // the callback is detached.  That reset is not a device/input loss: the
    // already-open physical route remains valid, and Monitoring's independent
    // control-side demand must therefore be restored after a successful plan
    // commit (including recording finalization).
    realtimeEngine_.restoreInputMonitoringAfterControlledReconfigure(
        monitoringInputDemand_);
    if (preserveTransport)
        realtimeEngine_.restoreTemporalCheckpoint(checkpoint);
    modelCommit.execute();
    if (callbackWasRegistered) {
        try {
            attachAudioCallback(preserveTransport);
        } catch (...) {
            realtimeEngine_.deviceError();
            pendingLifecycleEvent_.store(PendingLifecycleEvent::error,
                                         std::memory_order_release);
        }
    }
    return true;
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
