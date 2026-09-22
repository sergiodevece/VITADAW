#pragma once

#include "vitadaw/audio/RecordingTypes.h"

#include <filesystem>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace vitadaw::audio {

// A marker is evidence that VitaDAW created a recording attempt, not a durable
// filesystem ownership capability.  The scanner only discovers candidates;
// recovery still goes through normal WAV preparation and project commit.
struct RecordingRecoveryMarker {
    std::string sessionId;
    RecordingRecoveryClass classification{RecordingRecoveryClass::temporary};
    std::filesystem::path temporaryName;
    std::filesystem::path publishedName;
    media::AudioChannelLayout layout{media::AudioChannelLayout::mono};
    timeline::SampleRate deviceSampleRate;
    timeline::SourceFrameCount acceptedFrames;
    std::optional<media::MediaFingerprint> fingerprint;
};

enum class RecordingRecoveryMarkerFault : std::uint8_t {
    none, create, write, sync, close,
};

// Deterministic non-RT test seam. Production always supplies `none`.
struct RecordingRecoveryMarkerWriteOptions {
    RecordingRecoveryMarkerFault injectedFault{RecordingRecoveryMarkerFault::none};
};

[[nodiscard]] std::string makeRecordingRecoverySessionId();
[[nodiscard]] bool writeRecordingRecoveryMarker(const std::filesystem::path& directory,
                                                 const RecordingRecoveryMarker&,
                                                 std::string& errorMessage,
                                                 RecordingRecoveryMarkerWriteOptions = {});
// Reads and validates the existing marker format without changing it. Callers
// that enumerate directories must independently reject symlinks before using
// this parser; a marker is discovery evidence, not filesystem ownership.
[[nodiscard]] std::optional<RecordingRecoveryMarker> readRecordingRecoveryMarker(
    const std::filesystem::path& path);
[[nodiscard]] bool isRecordingRecoveryMarkerPath(const std::filesystem::path&) noexcept;
[[nodiscard]] std::vector<RecordingRecoveryArtifact> scanRecordingRecoveryMarkers(
    const std::filesystem::path& directory,
    const std::function<bool(const std::filesystem::path&)>& readableWav);

} // namespace vitadaw::audio
