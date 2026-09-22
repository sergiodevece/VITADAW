#pragma once

#include "vitadaw/audio/RecordingTypes.h"

#include <filesystem>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
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

// Version-1 markers contain at most ten metadata fields. These bounds leave
// room for the existing filename and numeric fields while keeping untrusted
// discovery input strictly metadata-sized.
inline constexpr std::size_t maximumRecordingRecoveryMarkerBytes = 8U * 1024U;
inline constexpr std::size_t maximumRecordingRecoveryMarkerLineBytes = 1024U;
inline constexpr std::size_t maximumRecordingRecoveryMarkerFields = 12U;

enum class RecordingRecoveryMarkerParseStatus : std::uint8_t {
    valid,
    invalidFormat,
    exceedsTotalLimit,
    exceedsLineLimit,
    exceedsFieldLimit,
};

struct RecordingRecoveryMarkerParseResult {
    std::optional<RecordingRecoveryMarker> marker;
    RecordingRecoveryMarkerParseStatus status{RecordingRecoveryMarkerParseStatus::invalidFormat};
};

// Portable, bounded parser shared by the historical reader and platform-safe
// inventory reader. `bytes` must be marker content, not a pathname.
[[nodiscard]] RecordingRecoveryMarkerParseResult parseRecordingRecoveryMarker(
    std::string_view bytes);

[[nodiscard]] std::string makeRecordingRecoverySessionId();
[[nodiscard]] bool writeRecordingRecoveryMarker(const std::filesystem::path& directory,
                                                 const RecordingRecoveryMarker&,
                                                 std::string& errorMessage,
                                                 RecordingRecoveryMarkerWriteOptions = {});
// Compatibility reader for the historical recording-recovery scanner. Its
// input is bounded and uses the shared parser. Recovery Inventory instead
// opens markers through its platform no-follow descriptor path.
[[nodiscard]] std::optional<RecordingRecoveryMarker> readRecordingRecoveryMarker(
    const std::filesystem::path& path);
[[nodiscard]] bool isRecordingRecoveryMarkerPath(const std::filesystem::path&) noexcept;
[[nodiscard]] std::vector<RecordingRecoveryArtifact> scanRecordingRecoveryMarkers(
    const std::filesystem::path& directory,
    const std::function<bool(const std::filesystem::path&)>& readableWav);

} // namespace vitadaw::audio
