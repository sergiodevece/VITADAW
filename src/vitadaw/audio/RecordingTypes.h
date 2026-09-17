#pragma once

#include "vitadaw/media/AudioSource.h"
#include "vitadaw/tracks/AudioTrack.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>

namespace vitadaw::audio {

using RecordingSessionId = std::uint64_t;

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
};

struct RecordingRequest {
    RecordingSessionId session{};
    tracks::TrackId track;
    media::AudioChannelLayout layout{media::AudioChannelLayout::mono};
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
};

struct RecordingPreflightResult {
    RecordingRequest request;
    std::string errorMessage;

    [[nodiscard]] bool success() const noexcept {
        return request.session != 0 && errorMessage.empty();
    }
};

} // namespace vitadaw::audio
