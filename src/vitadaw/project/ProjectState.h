#pragma once

#include "vitadaw/tracks/AudioTrack.h"

#include <filesystem>
#include <string>
#include <vector>

namespace vitadaw::project {

// Mutable only from the application thread.
class ProjectState {
public:
    explicit ProjectState(timeline::SampleRate projectSampleRate);

    [[nodiscard]] timeline::SampleRate sampleRate() const noexcept;
    [[nodiscard]] const std::vector<tracks::AudioTrack>& tracks() const noexcept;
    tracks::TrackId addAudioTrack(std::string name);
    void setSingleAudioClip(const std::filesystem::path& sourceFile,
                            timeline::SourceFrameCount sourceFrameCount,
                            timeline::SampleRate sourceSampleRate);

private:
    timeline::SampleRate projectSampleRate_;
    std::vector<tracks::AudioTrack> tracks_;
    tracks::TrackId nextTrackId_{1};
    clips::ClipId nextClipId_{1};
};

} // namespace vitadaw::project
