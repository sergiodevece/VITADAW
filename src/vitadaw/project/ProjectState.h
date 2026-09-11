#pragma once

#include "vitadaw/tracks/AudioTrack.h"

#include <array>
#include <filesystem>
#include <optional>
#include <string>

namespace vitadaw::project {

// Mutable only from the application thread.
class ProjectState {
public:
    struct PreparedAudioClipUpdate {
        tracks::AudioTrackSlot track{tracks::AudioTrackSlot::first};
        std::optional<clips::AudioClip> replacement;
        clips::ClipId nextClipId{};
    };

    explicit ProjectState(timeline::SampleRate projectSampleRate);

    [[nodiscard]] timeline::SampleRate sampleRate() const noexcept;
    [[nodiscard]] const std::array<tracks::AudioTrack, tracks::audioTrackCount>&
    tracks() const noexcept;
    [[nodiscard]] timeline::ProjectFrameCount duration() const noexcept;
    [[nodiscard]] PreparedAudioClipUpdate prepareAudioClipUpdate(
        tracks::AudioTrackSlot track,
        const std::filesystem::path& sourceFile,
        timeline::SourceFrameCount sourceFrameCount,
        timeline::SampleRate sourceSampleRate) const;
    void commitAudioClipUpdate(PreparedAudioClipUpdate& update) noexcept;

private:
    timeline::SampleRate projectSampleRate_;
    std::array<tracks::AudioTrack, tracks::audioTrackCount> tracks_;
    clips::ClipId nextClipId_{1};
};

} // namespace vitadaw::project
