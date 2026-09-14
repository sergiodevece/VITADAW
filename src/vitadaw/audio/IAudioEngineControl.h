#pragma once

#include "vitadaw/audio/RealtimeTransportExchange.h"
#include "vitadaw/audio/PreparedProcessingPlan.h"
#include "vitadaw/mixer/Metering.h"
#include "vitadaw/timeline/Time.h"
#include "vitadaw/tracks/AudioTrack.h"

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

class PreparedAudioFile {
public:
    explicit PreparedAudioFile(AudioFileMetadata preparedMetadata) noexcept
        : metadata(preparedMetadata) {}
    virtual ~PreparedAudioFile() = default;

    AudioFileMetadata metadata;
};

using PreparedAudioFilePtr = std::unique_ptr<PreparedAudioFile>;

struct AudioFilePreparationResult {
    PreparedAudioFilePtr prepared;
    std::string errorMessage;

    [[nodiscard]] bool success() const noexcept { return prepared != nullptr; }
};

class PreparedProcessingPlanChange {
public:
    virtual ~PreparedProcessingPlanChange() = default;
};

using PreparedProcessingPlanChangePtr =
    std::unique_ptr<PreparedProcessingPlanChange>;

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
// outside the realtime thread. commitPreparedWav publishes the engine resource
// and invokes the already-prepared model commit while render is quiescent.
class IAudioEngineControl {
public:
    virtual ~IAudioEngineControl() = default;
    // File I/O and decoding are allowed here because this method is never called
    // by the realtime thread.
    [[nodiscard]] virtual AudioFilePreparationResult prepareWav(
        const std::filesystem::path& file,
        tracks::TrackId track,
        timeline::SampleRate projectSampleRate,
        mixer::PreparedTrackMixState trackMix) = 0;
    [[nodiscard]] virtual bool commitPreparedWav(
        PreparedAudioFilePtr prepared,
        AudioFileCommitAction modelCommit) noexcept = 0;
    [[nodiscard]] virtual StructuralPlanPreparationResult prepareProcessingPlan(
        const ProcessingPlanSpecification& specification) = 0;
    [[nodiscard]] virtual bool commitPreparedProcessingPlan(
        PreparedProcessingPlanChangePtr prepared,
        AudioFileCommitAction modelCommit) noexcept = 0;
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
    [[nodiscard]] virtual AudioControlRequestResult tryRequestPlay() noexcept = 0;
    [[nodiscard]] virtual AudioControlRequestResult tryRequestStop() noexcept = 0;
    [[nodiscard]] virtual RealtimeTransportSnapshot transportSnapshot() const noexcept = 0;
    [[nodiscard]] virtual mixer::MeterSnapshot meterSnapshot() const noexcept = 0;
};

} // namespace vitadaw::audio
