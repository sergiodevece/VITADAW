#pragma once

#include "vitadaw/audio/OfflineRenderer.h"

#include <cstdint>
#include <filesystem>
#include <string>

namespace vitadaw::audio {

// Public control-side request for the 0.8.1A streaming WAV foundation. The
// nested render request owns the explicit half-open project range and output
// format. Only float32 WAV mono/stereo is supported in this iteration. Mono
// uses the existing OfflineRenderer one-channel semantics; no new downmix law
// is introduced here. Classic RIFF/WAV is rejected before writing when its
// 32-bit data chunk would overflow (RF64 is deliberately out of scope).
struct WavExportRequest {
    std::filesystem::path destination;
    OfflineRenderRequest render;
};

// Export progress uses the nested render's total frame count. The terminal
// total/total callback is reserved for a completely finalized, published and
// directory-synced export; a completed DSP block alone is not completion.

enum class WavExportStatus : std::uint8_t {
    success,
    cancelled,
    invalidRequest,
    preparationFailed,
    destinationExists,
    temporaryCreationFailed,
    wavInitialisationFailed,
    wavWriteFailed,
    wavFinalizationFailed,
    publicationFailed,
};

struct WavExportResult {
    WavExportStatus status{WavExportStatus::invalidRequest};
    timeline::DeviceFrameCount renderedFrames;
    // Empty until no-replace publication has actually happened. A non-empty
    // value with a non-success status denotes a retained, uncommitted final
    // candidate after a later identity/durability failure; callers must never
    // infer completed export from this path alone.
    std::filesystem::path publishedFile;
    // A pathname observation cannot safely authorize unlink on the supported
    // filesystems. Temporaries are retained rather than risking deletion of a
    // replacement object, and are reported explicitly to the caller.
    std::filesystem::path retainedTemporaryFile;
    std::string errorMessage;
    std::string warningMessage;

    [[nodiscard]] bool success() const noexcept {
        return status == WavExportStatus::success;
    }
};

} // namespace vitadaw::audio
