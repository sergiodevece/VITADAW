#pragma once

#include "vitadaw/media/AudioSource.h"
#include "vitadaw/tracks/AudioTrack.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace vitadaw::audio {

using RecordingSessionId = std::uint64_t;

// Placement is documentary only: it accompanies a take until finalization and
// never participates in callback-side PCM capture or WAV writing.
enum class RecordingLatencyStatus : std::uint8_t { reported, unavailable, invalid };

struct RecordingPlacementSnapshot {
    timeline::SampleRate projectSampleRate;
    timeline::SampleRate deviceSampleRate;
    std::optional<timeline::DeviceFrameCount> reportedInputLatencyDeviceFrames;
    timeline::ProjectFrameCount reportedInputLatencyProjectFrames;
    timeline::ProjectFrameCount manualOffsetProjectFrames;
    timeline::ProjectFrameCount effectiveCompensationProjectFrames;
    std::uint32_t deviceBufferFrames{};
    RecordingLatencyStatus latencyStatus{RecordingLatencyStatus::unavailable};
};

struct RecordingPlacementResult {
    timeline::ProjectFramePosition start;
    std::uint64_t unappliedEarlyFrames{};
    bool upperBoundExceeded{};
};

[[nodiscard]] std::optional<timeline::ProjectFrameCount>
convertRecordingLatencyToProjectFrames(timeline::DeviceFrameCount,
                                       timeline::SampleRate deviceRate,
                                       timeline::SampleRate projectRate) noexcept;
[[nodiscard]] std::optional<timeline::ProjectFrameCount>
computeEffectiveRecordingCompensation(RecordingLatencyStatus,
                                      timeline::ProjectFrameCount automatic,
                                      timeline::ProjectFrameCount manual) noexcept;
[[nodiscard]] RecordingPlacementResult computeRecordingPlacement(
    timeline::ProjectFramePosition capturedStart,
    const RecordingPlacementSnapshot&) noexcept;

enum class RecordingPhase : std::uint8_t {
    idle,
    prepared,
    capturing,
    finalizing,
    complete,
    failed,
};

enum class RecordingFailure : std::uint8_t {
    none,
    overflow,
    missingInput,
    deviceLost,
    sampleRateChanged,
    cancelled,
    writerFailed,
    finalizationFailed,
    shutdown,
};

// These values describe retained media only.  They are deliberately not an
// ownership claim: recovery may inspect/import a candidate, but never delete
// it merely because it has a VitaDAW marker.
enum class RecordingRecoveryClass : std::uint8_t {
    temporary,
    closedUncommitted,
    publishedFinal,
    incomplete,
    ambiguous,
};

struct RecordingRecoveryArtifact {
    std::string persistentSessionId;
    std::filesystem::path path;
    RecordingRecoveryClass classification{RecordingRecoveryClass::ambiguous};
    bool recoverable{};
};

struct RecordingCleanupResult {
    std::string primaryError;
    std::vector<RecordingRecoveryArtifact> retainedArtifacts;

    [[nodiscard]] bool clean() const noexcept { return retainedArtifacts.empty(); }
};

struct RecordingRequest {
    RecordingSessionId session{};
    tracks::TrackId track;
    media::AudioChannelLayout layout{media::AudioChannelLayout::mono};
    RecordingPlacementSnapshot placement;
};

struct RecordingSnapshot {
    RecordingPhase phase{RecordingPhase::idle};
    RecordingFailure failure{RecordingFailure::none};
    RecordingSessionId session{};
    tracks::TrackId track;
    media::AudioChannelLayout layout{media::AudioChannelLayout::mono};
    timeline::ProjectFramePosition projectStart;
    timeline::SourceFrameCount acceptedDeviceFrames;
    timeline::SampleRate deviceSampleRate;
    RecordingPlacementSnapshot placement;

    [[nodiscard]] bool busy() const noexcept {
        return phase == RecordingPhase::prepared ||
               phase == RecordingPhase::capturing;
    }
    [[nodiscard]] bool terminal() const noexcept {
        return phase == RecordingPhase::complete ||
               phase == RecordingPhase::failed;
    }
};

struct RecordingPreflightRequest {
    tracks::TrackId track;
    media::AudioChannelLayout layout{media::AudioChannelLayout::mono};
    std::filesystem::path projectFile;
    timeline::SampleRate projectSampleRate;
    timeline::ProjectFrameCount manualOffsetProjectFrames;
};

struct RecordingPreflightResult {
    RecordingPreflightResult(RecordingRequest prepared = {}, std::string error = {},
                             std::string warning = {})
        : request(prepared), errorMessage(std::move(error)), warningMessage(std::move(warning)) {}

    RecordingRequest request;
    std::string errorMessage;
    std::string warningMessage;

    [[nodiscard]] bool success() const noexcept {
        return request.session != 0 && errorMessage.empty();
    }
};

} // namespace vitadaw::audio
