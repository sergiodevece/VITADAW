#pragma once

#include "vitadaw/audio/RealtimeTransportExchange.h"
#include "vitadaw/audio/PreparedProcessingPlan.h"
#include "vitadaw/audio/PreparedTemporalContext.h"
#include "vitadaw/mixer/Metering.h"
#include "vitadaw/timeline/Time.h"
#include "vitadaw/tracks/AudioTrack.h"
#include "vitadaw/persistence/PersistenceResult.h"

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
};

using PreparedAudioFilePtr = std::unique_ptr<PreparedAudioFile>;

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
    [[nodiscard]] virtual std::size_t preparedAudioBytes() const noexcept { return 0; }
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
    [[nodiscard]] virtual AudioControlRequestResult tryRequestPlay() noexcept = 0;
    [[nodiscard]] virtual AudioControlRequestResult tryRequestPause() noexcept { return {}; }
    [[nodiscard]] virtual AudioControlRequestResult tryRequestStop() noexcept = 0;
    [[nodiscard]] virtual AudioControlRequestResult tryRequestSeek(
        timeline::ProjectFramePosition) noexcept { return {}; }
    [[nodiscard]] virtual AudioControlRequestResult trySetLoopEnabled(bool) noexcept { return {}; }
    [[nodiscard]] virtual AudioControlRequestResult trySetMetronomeEnabled(bool) noexcept { return {}; }
    [[nodiscard]] virtual AudioControlRequestResult trySetMetronomeLevel(
        MetronomeLevelDb) noexcept { return {}; }
    [[nodiscard]] virtual RealtimeTransportSnapshot transportSnapshot() const noexcept = 0;
    [[nodiscard]] virtual mixer::MeterSnapshot meterSnapshot() const noexcept = 0;
};

} // namespace vitadaw::audio
