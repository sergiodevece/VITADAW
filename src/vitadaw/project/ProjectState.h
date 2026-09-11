#pragma once

#include "vitadaw/tracks/AudioTrack.h"

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace vitadaw::project {

// Mutable only from the application thread.
class ProjectState {
public:
    struct PreparedAudioClipUpdate {
        tracks::TrackId track;
        std::size_t trackIndex{};
        std::optional<clips::AudioClip> replacement;
        clips::ClipId nextClipId{};
    };

    explicit ProjectState(timeline::SampleRate projectSampleRate);

    [[nodiscard]] timeline::SampleRate sampleRate() const noexcept;
    [[nodiscard]] const std::vector<tracks::AudioTrack>& tracks() const noexcept;
    [[nodiscard]] tracks::TrackId addAudioTrack(std::string name);
    [[nodiscard]] const tracks::AudioTrack* findTrack(
        tracks::TrackId track) const noexcept;
    [[nodiscard]] timeline::ProjectFrameCount duration() const noexcept;
    [[nodiscard]] PreparedAudioClipUpdate prepareAudioClipUpdate(
        tracks::TrackId track,
        const std::filesystem::path& sourceFile,
        timeline::SourceFrameCount sourceFrameCount,
        timeline::SampleRate sourceSampleRate) const;
    void commitAudioClipUpdate(PreparedAudioClipUpdate& update) noexcept;

private:
    timeline::SampleRate projectSampleRate_;
    std::vector<tracks::AudioTrack> tracks_;
    tracks::TrackId nextTrackId_{1};
    clips::ClipId nextClipId_{1};
};

} // namespace vitadaw::project
