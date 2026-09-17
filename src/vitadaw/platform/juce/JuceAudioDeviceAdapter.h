#pragma once

#include "vitadaw/audio/AudioDeviceState.h"
#include "vitadaw/audio/IAudioEngineControl.h"
#include "vitadaw/audio/RecordingRecovery.h"
#include "vitadaw/audio/RealtimeAudioEngine.h"
#include "vitadaw/platform/files/RecordingMediaIO.h"

#include <juce_audio_devices/juce_audio_devices.h>

#include <atomic>
#include <cstddef>
#include <functional>
#include <memory>
#include <filesystem>
#include <optional>

namespace juce { class AudioFormatWriter; }

namespace vitadaw::platform::juce_adapter {

class JuceAudioDeviceAdapter final : public audio::IAudioEngineControl,
                                     private juce::AudioIODeviceCallback,
                                     private juce::ChangeListener {
public:
    using StateChangedCallback = std::function<void(const audio::AudioDeviceState&)>;
    static constexpr std::size_t preparationMemoryBudgetBytes = 512U * 1024U * 1024U;
    // Pathnames are not ownership. Capture identity at O_EXCL creation and
    // verify it again before destructive pathname operations.
    struct FileIdentity {
        std::uint64_t device{};
        std::uint64_t inode{};

        [[nodiscard]] bool valid() const noexcept { return device != 0 || inode != 0; }
        bool operator==(const FileIdentity&) const = default;
    };
    struct ExclusiveTemporaryFile {
        ExclusiveTemporaryFile(int descriptor, FileIdentity fileIdentity) noexcept
            : identity(fileIdentity), descriptor_(descriptor) {}
        ~ExclusiveTemporaryFile() noexcept;
        ExclusiveTemporaryFile(const ExclusiveTemporaryFile&) = delete;
        ExclusiveTemporaryFile& operator=(const ExclusiveTemporaryFile&) = delete;
        ExclusiveTemporaryFile(ExclusiveTemporaryFile&&) noexcept;
        ExclusiveTemporaryFile& operator=(ExclusiveTemporaryFile&&) noexcept;

        [[nodiscard]] int release() noexcept;
        FileIdentity identity;

    private:
        friend class PersistenceIntegrationAccess; // Hardware-free ownership tests.
        int descriptor_{-1};
    };

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
    void clearStateChangedCallback() noexcept;

    [[nodiscard]] audio::AudioFilePreparationResult prepareWav(
        const std::filesystem::path& file) override;
    [[nodiscard]] audio::AudioFilePreparationResult prepareWavForProject(
        const std::filesystem::path& file, std::size_t candidateBytes) override;
    [[nodiscard]] audio::AudioFilePreparationResult prepareVerifiedWav(
        const std::filesystem::path&, const media::MediaFingerprint&, std::size_t) override;
    [[nodiscard]] std::size_t preparedAudioBytes() const noexcept override { return preparedBytes(); }
    [[nodiscard]] audio::StructuralPlanPreparationResult prepareProjectReplacement(
        const audio::ProcessingPlanSpecification&, std::vector<audio::PreparedSourceAudio>) override;
    [[nodiscard]] audio::StructuralPlanPreparationResult prepareProcessingPlan(
        const audio::ProcessingPlanSpecification& specification) override;
    [[nodiscard]] audio::StructuralPlanPreparationResult
    prepareProcessingPlanWithAudio(
        const audio::ProcessingPlanSpecification& specification,
        media::SourceId source,
        audio::PreparedAudioFilePtr preparedAudio) override;
    [[nodiscard]] bool commitPreparedProcessingPlan(
        audio::PreparedProcessingPlanChangePtr prepared,
        audio::AudioFileCommitAction modelCommit) noexcept override;
    [[nodiscard]] bool commitPreparedProcessingPlanPreservingTransport(
        audio::PreparedProcessingPlanChangePtr prepared,
        audio::AudioFileCommitAction modelCommit) noexcept override;
    [[nodiscard]] audio::TemporalContextPreparationResult prepareTemporalContext(
        const musical::MusicalTimeMap&,
        std::optional<musical::MusicalLoopRange>,
        timeline::SampleRate, std::uint64_t) override;
    [[nodiscard]] bool commitPreparedTemporalContext(
        std::unique_ptr<audio::PreparedTemporalContext>,
        audio::AudioFileCommitAction) noexcept override;
    [[nodiscard]] bool commitPreparedProjectAndTemporalContext(
        audio::PreparedProcessingPlanChangePtr,
        std::unique_ptr<audio::PreparedTemporalContext>,
        audio::AudioFileCommitAction) noexcept override;
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
    [[nodiscard]] audio::AudioControlRequestResult tryRequestPause() noexcept override;
    [[nodiscard]] audio::AudioControlRequestResult tryRequestStop() noexcept override;
    [[nodiscard]] audio::AudioControlRequestResult tryRequestSeek(
        timeline::ProjectFramePosition) noexcept override;
    [[nodiscard]] audio::AudioControlRequestResult trySetLoopEnabled(bool) noexcept override;
    [[nodiscard]] audio::AudioControlRequestResult trySetMetronomeEnabled(bool) noexcept override;
    [[nodiscard]] audio::AudioControlRequestResult trySetMetronomeLevel(
        audio::MetronomeLevelDb) noexcept override;
    [[nodiscard]] audio::RealtimeTransportSnapshot transportSnapshot() const noexcept override;
    [[nodiscard]] audio::RealtimeTransportSnapshot projectedTransportSnapshot() noexcept override;
    [[nodiscard]] mixer::MeterSnapshot meterSnapshot() const noexcept override;
    [[nodiscard]] audio::RecordingPreflightResult prepareRecording(
        const audio::RecordingPreflightRequest&) override;
    [[nodiscard]] audio::AudioControlRequestResult tryRequestRecord(
        audio::RecordingRequest) noexcept override;
    [[nodiscard]] bool tryCancelRecording() noexcept override;
    void serviceRecording() noexcept override;
    [[nodiscard]] audio::RecordingSnapshot recordingSnapshot() const noexcept override;
    [[nodiscard]] audio::RecordingFinalizationResult finalizeRecording() override;
    [[nodiscard]] audio::RecordingCleanupResult discardRecordingWithDiagnostics(
        bool removePublished, std::string primaryError = {}) noexcept override;
    [[nodiscard]] audio::RecordingCleanupResult shutdownRecording() noexcept override;
    [[nodiscard]] bool discardRecording(bool removePublished) noexcept override;
    void confirmRecordingCommit() noexcept override;

private:
    friend class PersistenceIntegrationAccess; // Hardware-free test harness only.
    [[nodiscard]] audio::AudioFilePreparationResult decodeWav(
        const std::filesystem::path&, std::size_t, const media::MediaFingerprint*);
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
    struct RecoveryMetadataStatus {
        bool available{};
        std::string warning;
    };

    void closeDevice(bool publishClosedState) noexcept;
    void beginDeviceReinitialisation() noexcept;
    void refreshState();
    void publishState();
    void detachAudioCallback(bool preserveTransport = false) noexcept;
    void attachAudioCallback(bool preserveTransport = false);
    void configureRealtimeEngine() noexcept;
    [[nodiscard]] bool prepareProjectPlan(
        PreparedProject& candidate,
        const audio::ProcessingPlanSpecification& specification,
        std::string& errorMessage);
    [[nodiscard]] bool reprepareForCurrentDevice(std::string& errorMessage);
    [[nodiscard]] static std::optional<ExclusiveTemporaryFile> createExclusiveTemporaryFile(
        const std::filesystem::path&, std::error_code&) noexcept;
    [[nodiscard]] static bool pathHasIdentity(
        const std::filesystem::path&, FileIdentity) noexcept;
    [[nodiscard]] static RecoveryMetadataStatus persistInitialRecoveryMetadata(
        const std::filesystem::path&, const audio::RecordingRecoveryMarker&,
        audio::RecordingRecoveryMarkerWriteOptions = {});
    [[nodiscard]] bool finalizeRecordingFile(std::string&) noexcept;
    [[nodiscard]] bool commitPreparedProject(
        std::unique_ptr<PreparedProject>& candidate,
        audio::AudioFileCommitAction modelCommit,
        bool preserveTransport = false) noexcept;
    [[nodiscard]] std::size_t preparedBytes() const noexcept;

    juce::AudioDeviceManager deviceManager_;
    audio::AudioDeviceStateModel stateModel_;
    StateChangedCallback stateChangedCallback_;
    std::unique_ptr<PreparedProject> preparedProject_;
    std::unique_ptr<audio::PreparedTemporalContext> preparedTemporalContext_;
    audio::RealtimeAudioEngine realtimeEngine_;
    timeline::SampleRate projectSampleRate_;
    timeline::SampleRate deviceSampleRate_;
    mixer::PreparedMasterMixState masterMix_;
    std::atomic<PendingLifecycleEvent> pendingLifecycleEvent_{};
    std::atomic<bool> suppressLifecycleNotification_{};
    std::atomic<bool> preserveTransportDuringRegistration_{};
    std::unique_ptr<juce::AudioFormatWriter> recordingWriter_;
    std::unique_ptr<files::RecordingFileHandle> recordingFile_;
    files::RecordingIoFaultInjection recordingIoFaults_;
    juce::AudioBuffer<float> recordingDrainBuffer_;
    std::filesystem::path recordingTemporaryPath_;
    std::filesystem::path recordingPublishedPath_;
    std::optional<FileIdentity> recordingTemporaryIdentity_;
    std::optional<FileIdentity> recordingPublishedIdentity_;
    audio::RecordingSessionId nextRecordingSession_{1};
    std::uint64_t nextRecordingTemporaryNonce_{1};
    bool recordingWriterFailed_{};
    std::string recordingWriterError_;
    std::string recordingRecoverySessionId_;
    std::filesystem::path recordingRecoveryDirectory_;
    bool recordingRecoveryMetadataAvailable_{};
    std::string recordingRecoveryWarning_;
    // Test-only fault seam; production retains the all-clear default.
    audio::RecordingRecoveryMarkerWriteOptions recordingRecoveryMarkerWriteOptions_;
    bool callbackRegistered_{};
    bool changeListenerRegistered_{};
};

} // namespace vitadaw::platform::juce_adapter
