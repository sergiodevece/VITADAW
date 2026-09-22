#pragma once

#include "vitadaw/audio/RealtimeTransportExchange.h"
#include "vitadaw/audio/AudioDeviceState.h"
#include "vitadaw/audio/InputMonitoring.h"
#include "vitadaw/audio/LoopbackLatency.h"
#include "vitadaw/audio/OfflineRenderer.h"
#include "vitadaw/audio/PreparedProcessingPlan.h"
#include "vitadaw/audio/PreparedTemporalContext.h"
#include "vitadaw/audio/RecordingTypes.h"
#include "vitadaw/mixer/Metering.h"
#include "vitadaw/timeline/Time.h"
#include "vitadaw/tracks/AudioTrack.h"
#include "vitadaw/persistence/PersistenceResult.h"
#include "vitadaw/waveform/WaveformCache.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

namespace vitadaw::audio {

struct AudioFileMetadata {
    timeline::SampleRate sourceSampleRate;
    std::uint32_t channelCount{};
    timeline::SourceFrameCount sourceFrameCount;
    timeline::Seconds duration;

    bool operator==(const AudioFileMetadata&) const = default;
};

enum class AudioFilePreparationFailure : std::uint8_t {
    none,
    fileNotFound,
    permissionDenied,
    unsupportedFormat,
    decodeFailed,
    preparationFailed,
    capacityExceeded,
    ioError,
};

class PreparedAudioFile {
public:
    explicit PreparedAudioFile(AudioFileMetadata preparedMetadata) noexcept
        : metadata(preparedMetadata) {}
    virtual ~PreparedAudioFile() = default;

    AudioFileMetadata metadata;
    media::MediaReference media;
    std::shared_ptr<const waveform::PreparedWaveformData> waveform;
    std::string waveformDiagnostic;
};

using PreparedAudioFilePtr = std::unique_ptr<PreparedAudioFile>;

struct RecordingFinalizationResult {
    PreparedAudioFilePtr prepared;
    RecordingSnapshot capture;
    std::filesystem::path publishedFile;
    std::string errorMessage;
    std::string warningMessage;

    [[nodiscard]] bool success() const noexcept {
        return prepared != nullptr && errorMessage.empty();
    }
};

// Non-RT admission result for a monitoring enable request.  The device adapter
// owns the physical route; the realtime engine only receives the accepted
// enable command after this preparation has succeeded.
struct InputMonitoringPreparationResult {
    bool ready{true};
    std::string errorMessage;

    [[nodiscard]] bool success() const noexcept { return ready; }
};

struct AudioFilePreparationResult {
    AudioFilePreparationResult(PreparedAudioFilePtr p = {}, std::string message = {},
        persistence::PersistenceResult r = {},
        AudioFilePreparationFailure failure = AudioFilePreparationFailure::none) noexcept
        : prepared(std::move(p)), errorMessage(std::move(message)), result(std::move(r)),
          failure(failure) {
        if (!prepared && result.success()) result = {persistence::PersistenceCode::preparationFailed,
                                                     persistence::PersistencePhase::prepare};
        if (!prepared && this->failure == AudioFilePreparationFailure::none) {
            switch (result.code) {
            case persistence::PersistenceCode::fileNotFound:
                this->failure = AudioFilePreparationFailure::fileNotFound;
                break;
            case persistence::PersistenceCode::permissionDenied:
                this->failure = AudioFilePreparationFailure::permissionDenied;
                break;
            case persistence::PersistenceCode::capacityExceeded:
            case persistence::PersistenceCode::fileTooLarge:
                this->failure = AudioFilePreparationFailure::capacityExceeded;
                break;
            case persistence::PersistenceCode::ioError:
                this->failure = AudioFilePreparationFailure::ioError;
                break;
            default:
                this->failure = AudioFilePreparationFailure::preparationFailed;
                break;
            }
        }
    }
    PreparedAudioFilePtr prepared;
    std::string errorMessage;
    persistence::PersistenceResult result;
    AudioFilePreparationFailure failure{AudioFilePreparationFailure::none};

    [[nodiscard]] bool success() const noexcept { return prepared != nullptr; }
};

class PreparedProcessingPlanChange {
public:
    virtual ~PreparedProcessingPlanChange() = default;
};

using PreparedProcessingPlanChangePtr =
    std::unique_ptr<PreparedProcessingPlanChange>;

struct PreparedSourceAudio { media::SourceId id; PreparedAudioFilePtr audio; };

struct StructuralPlanPreparationResult {
    PreparedProcessingPlanChangePtr prepared;
    std::string errorMessage;

    [[nodiscard]] bool success() const noexcept { return prepared != nullptr; }
};

// Allocation-free, noexcept half of a prepared load commit. The application
// builds every potentially-throwing ProjectState value before supplying it.
struct AudioFileCommitAction {
    void* context{};
    void (*function)(void*) noexcept {};

    [[nodiscard]] bool isValid() const noexcept {
        return context != nullptr && function != nullptr;
    }
    void execute() const noexcept { function(context); }
};

// Called from the application thread. Realtime control methods must enqueue
// bounded, non-blocking requests. prepareWav performs file I/O explicitly
// outside the realtime thread. A decoded source is published only as part of
// a complete prepared processing-plan transaction.
class IAudioEngineControl {
public:
    virtual ~IAudioEngineControl() = default;
    // Device configuration is a synchronous control-side transaction. It must
    // never be routed through the bounded RT command queue.
    [[nodiscard]] virtual AudioDeviceBufferChangeResult setAudioBufferSize(
        std::size_t) {
        return {false, "Audio buffer control is not supported"};
    }
    [[nodiscard]] virtual DeviceLatencyReadModel deviceLatencyReadModel() const {
        return {};
    }
    [[nodiscard]] virtual LoopbackLatencyControlResult startLoopbackLatencyTest(
        LoopbackLatencyRequest) {
        return {false, "Physical loopback validation is not supported"};
    }
    [[nodiscard]] virtual bool cancelLoopbackLatencyTest() noexcept { return false; }
    virtual void serviceLoopbackLatencyTest() noexcept {}
    [[nodiscard]] virtual LoopbackLatencyReadModel loopbackLatencyReadModel() const {
        return {};
    }
    [[nodiscard]] virtual std::size_t preparedAudioBytes() const noexcept { return 0; }
    // Synchronous render of the currently prepared project. Implementations
    // must not require or mutate a physical device, the installed realtime
    // engine, or application transport state. Resource retention guarantees
    // PCM lifetime only: project/media/plan mutation and adapter
    // reconfiguration must not overlap the call until a future explicit
    // concurrency contract defines otherwise.
    [[nodiscard]] virtual OfflineRenderResult renderOffline(
        const OfflineRenderRequest&,
        OfflineRenderCallbacks = {}) {
        return {OfflineRenderStatus::preparationFailed, {}, {},
                "Offline rendering is not supported"};
    }
    // Complete document adoption. Deliberately separate from within-project imports:
    // IDs in this vector never resolve through the active project's source cache.
    [[nodiscard]] virtual StructuralPlanPreparationResult prepareProjectReplacement(
        const ProcessingPlanSpecification&, std::vector<PreparedSourceAudio>) {
        return {nullptr, "Project replacement is not supported"};
    }
    // File I/O and decoding are allowed here because this method is never called
    // by the realtime thread.
    [[nodiscard]] virtual AudioFilePreparationResult prepareWav(
        const std::filesystem::path& file) = 0;
    [[nodiscard]] virtual AudioFilePreparationResult prepareWavForProject(
        const std::filesystem::path& file, std::size_t /* candidateBytes */) {
        return prepareWav(file);
    }
    [[nodiscard]] virtual AudioFilePreparationResult prepareVerifiedWav(
        const std::filesystem::path& file, const media::MediaFingerprint& expected,
        std::size_t candidateBytes) {
        auto result = prepareWavForProject(file, candidateBytes);
        if (result.success() && result.prepared->media.fingerprint != expected)
            return {nullptr, {}, {persistence::PersistenceCode::mediaChanged, persistence::PersistencePhase::media}};
        return result;
    }
    [[nodiscard]] virtual StructuralPlanPreparationResult prepareProcessingPlan(
        const ProcessingPlanSpecification& specification) = 0;
    // Consumes a fully decoded source and prepares it together with a complete
    // candidate project. No partial publication is allowed.
    [[nodiscard]] virtual StructuralPlanPreparationResult
    prepareProcessingPlanWithAudio(
        const ProcessingPlanSpecification&,
        media::SourceId,
        PreparedAudioFilePtr) {
        return {nullptr, "Prepared source import is not supported"};
    }
    [[nodiscard]] virtual bool commitPreparedProcessingPlan(
        PreparedProcessingPlanChangePtr prepared,
        AudioFileCommitAction modelCommit) noexcept = 0;
    // Structural timeline/track edits may preserve a stopped/paused checkpoint.
    // Import and project replacement retain their explicit transport semantics.
    [[nodiscard]] virtual bool commitPreparedProcessingPlanPreservingTransport(
        PreparedProcessingPlanChangePtr prepared,
        AudioFileCommitAction modelCommit) noexcept {
        return commitPreparedProcessingPlan(std::move(prepared), modelCommit);
    }
    [[nodiscard]] virtual TemporalContextPreparationResult prepareTemporalContext(
        const musical::MusicalTimeMap& map,
        std::optional<musical::MusicalLoopRange> loop,
        timeline::SampleRate projectRate, std::uint64_t revision) {
        return audio::prepareTemporalContext(map, loop, projectRate,
                                             projectRate, revision);
    }
    [[nodiscard]] virtual bool commitPreparedTemporalContext(
        std::unique_ptr<PreparedTemporalContext> /*prepared*/,
        AudioFileCommitAction modelCommit) noexcept {
        if (!modelCommit.isValid()) return false;
        modelCommit.execute();
        return true;
    }
    [[nodiscard]] virtual bool commitPreparedProjectAndTemporalContext(
        PreparedProcessingPlanChangePtr project,
        std::unique_ptr<PreparedTemporalContext> temporal,
        AudioFileCommitAction modelCommit) noexcept {
        if (!commitPreparedProcessingPlan(std::move(project), modelCommit)) return false;
        int marker{};
        return commitPreparedTemporalContext(std::move(temporal),
            {&marker, [](void*) noexcept {}});
    }
    [[nodiscard]] virtual bool tryUpdateTrackMix(
        tracks::TrackId track,
        mixer::PreparedTrackMixState mix,
        PreparedAudibilityState audibility) noexcept = 0;
    [[nodiscard]] virtual bool tryUpdateBusMix(
        routing::BusId bus,
        mixer::PreparedBusMixState mix,
        PreparedAudibilityState audibility) noexcept = 0;
    [[nodiscard]] virtual bool tryUpdateSendMix(
        routing::SendId send,
        mixer::PreparedSendMixState mix) noexcept = 0;
    [[nodiscard]] virtual bool tryUpdateMasterMix(
        mixer::PreparedMasterMixState mix) noexcept = 0;
    [[nodiscard]] virtual bool tryUpdateProcessorBypass(
        processors::ProcessorInstanceId, bool) noexcept { return false; }
    [[nodiscard]] virtual bool tryUpdateProcessorParameter(
        processors::ProcessorInstanceId,
        processors::ParameterId, float, float) noexcept { return false; }
    // An accepted transport request must include the engine-owned projected
    // playback/position after all earlier accepted requests. Application/UI
    // mirrors are consumers of that projection, never admission authorities.
    [[nodiscard]] virtual AudioControlRequestResult tryRequestPlay() noexcept = 0;
    [[nodiscard]] virtual AudioControlRequestResult tryRequestPause() noexcept { return {}; }
    [[nodiscard]] virtual AudioControlRequestResult tryRequestStop() noexcept = 0;
    [[nodiscard]] virtual AudioControlRequestResult tryRequestSeek(
        timeline::ProjectFramePosition) noexcept { return {}; }
    [[nodiscard]] virtual AudioControlRequestResult trySetLoopEnabled(bool) noexcept { return {}; }
    [[nodiscard]] virtual AudioControlRequestResult trySetMetronomeEnabled(bool) noexcept { return {}; }
    [[nodiscard]] virtual AudioControlRequestResult trySetMetronomeLevel(
        MetronomeLevelDb) noexcept { return {}; }
    // Monitoring control is ephemeral and deliberately separate from the
    // project/history command path. Frequent gain publication is latest-value
    // wins rather than a FIFO of automation events.
    [[nodiscard]] virtual AudioControlRequestResult
    trySetInputMonitoringEnabled(bool) noexcept { return {}; }
    // Called on the application/control thread before a request which enables
    // monitoring.  Default controls used by core-only tests already provide a
    // valid synthetic input route.
    [[nodiscard]] virtual InputMonitoringPreparationResult
    prepareInputMonitoring() { return {}; }
    // Rolls back only an uncommitted monitoring preflight.  It is intentionally
    // distinct from Disable: disabling the RT route must not reconfigure input
    // while Recording may still be using it.
    virtual void cancelPreparedInputMonitoring() noexcept {}
    [[nodiscard]] virtual bool trySetMonitorGain(MonitorGainDb) noexcept {
        return false;
    }
    [[nodiscard]] virtual RealtimeTransportSnapshot transportSnapshot() const noexcept = 0;
    [[nodiscard]] virtual RealtimeTransportSnapshot projectedTransportSnapshot() noexcept {
        return transportSnapshot();
    }
    [[nodiscard]] virtual mixer::MeterSnapshot meterSnapshot() const noexcept = 0;

    // Recording preflight/finalization are non-RT. Implementations may perform
    // device configuration and filesystem work there. Only tryRequestRecord and
    // the callback-side capture path cross the bounded RT command boundary.
    [[nodiscard]] virtual RecordingPreflightResult prepareRecording(
        const RecordingPreflightRequest&) {
        return {{}, "Recording is not supported"};
    }
    [[nodiscard]] virtual AudioControlRequestResult tryRequestRecord(
        RecordingRequest) noexcept {
        AudioControlRequestResult result;
        result.rejection = AudioControlRejection::unavailable;
        return result;
    }
    [[nodiscard]] virtual bool tryCancelRecording() noexcept { return false; }
    virtual void serviceRecording() noexcept {}
    [[nodiscard]] virtual RecordingSnapshot recordingSnapshot() const noexcept {
        return {};
    }
    [[nodiscard]] virtual RecordingFinalizationResult finalizeRecording() {
        return {{}, {}, {}, "Recording finalization is not supported", {}};
    }
    // Structured cleanup preserves a primary cause plus every retained media
    // candidate.  The bool API remains as a compatibility view for existing
    // non-recording engines.
    [[nodiscard]] virtual RecordingCleanupResult discardRecordingWithDiagnostics(
        bool removePublished, std::string primaryError = {}) noexcept {
        RecordingCleanupResult result;
        result.primaryError = std::move(primaryError);
        static_cast<void>(discardRecording(removePublished));
        return result;
    }
    // Invoked while the application owner is still alive. Implementations must
    // quiesce RT, drain already accepted PCM if possible, retain media and never
    // commit model/history from this operation.
    [[nodiscard]] virtual RecordingCleanupResult shutdownRecording() noexcept {
        return discardRecordingWithDiagnostics(true, "Recording cancelled during shutdown");
    }
    [[nodiscard]] virtual bool discardRecording(
        bool /* removePublished */) noexcept { return true; }
    virtual void confirmRecordingCommit() noexcept {}
};

} // namespace vitadaw::audio
