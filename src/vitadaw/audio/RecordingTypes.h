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
    // The device stopped delivering callbacks during a take without any
    // lifecycle notification (0.8.3-C2-2 watchdog, finding M-1).
    deviceStalled,
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

// Length of the non-RT capture ring, in seconds of device audio. This is a
// recording-domain decision: together with the cadence at which the host
// platform actually services the writer, it defines the headroom the ring has
// to absorb. Core deliberately does not describe that cadence - the layer
// which owns the service timer does.
inline constexpr double recordingCaptureRingSeconds = 2.0;

// Bounded capture telemetry. Every field is published through a plain
// lock-free integer counter: the audio callback writes only the producer-owned
// counters, the recording service writes only the consumer-owned ones, and no
// path allocates, locks, formats a string or performs I/O. Reading a snapshot
// is non-RT and may observe producer and consumer counters a bounded distance
// apart; it is a diagnostic, never a synchronisation mechanism.
struct RecordingCaptureTelemetry {
    std::size_t ringCapacityFrames{};
    // Instantaneous ring occupancy and the producer-observed high-water mark.
    std::size_t occupancyFrames{};
    std::size_t peakOccupancyFrames{};
    // capacity - peakOccupancy: the smallest margin the take ever had.
    std::size_t minimumHeadroomFrames{};
    // Consumer-observed backlog at service entry, i.e. the frames the writer
    // had to absorb because of its own scheduling lateness.
    std::size_t peakServiceBacklogFrames{};
    std::size_t serviceBudgetFrames{};
    std::uint64_t producedFrames{};
    std::uint64_t consumedFrames{};
    std::uint64_t droppedFrames{};
    std::uint64_t captureCalls{};
    std::uint64_t overflowEvents{};
    // Drain calls, not service passes: a pass drains in bounded chunks until
    // the ring is empty, so a healthy pass contributes one or more draining
    // calls plus the empty call that ends it.
    std::uint64_t drainCalls{};
    // Drain calls that found the ring empty. Recording has no underrun: an
    // empty ring is the normal steady state of a writer which keeps up.
    std::uint64_t emptyDrainCalls{};
    // Drain calls which observed a backlog above the nominal budget. One very
    // late service pass can report several, one per chunk it has to catch up.
    std::uint64_t lateServiceEvents{};

    [[nodiscard]] bool lossless() const noexcept {
        return droppedFrames == 0 && overflowEvents == 0;
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
