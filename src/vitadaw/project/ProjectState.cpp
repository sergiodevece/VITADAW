#include "vitadaw/project/ProjectState.h"

#include <stdexcept>
#include <utility>

namespace vitadaw::project {

ProjectState::ProjectState(timeline::SampleRate projectSampleRate)
    : projectSampleRate_(projectSampleRate) {
    if (!projectSampleRate_.isValid()) {
        throw std::invalid_argument{"Project sample rate must be positive"};
    }
}

timeline::SampleRate ProjectState::sampleRate() const noexcept {
    return projectSampleRate_;
}

const std::vector<tracks::AudioTrack>& ProjectState::tracks() const noexcept {
    return tracks_;
}

tracks::TrackId ProjectState::addAudioTrack(std::string name) {
    const auto id = nextTrackId_++;
    tracks_.push_back(tracks::AudioTrack{id, std::move(name), {}});
    return id;
}

void ProjectState::setSingleAudioClip(const std::filesystem::path& sourceFile,
                                      timeline::SourceFrameCount sourceFrameCount,
                                      timeline::SampleRate sourceSampleRate) {
    if (tracks_.empty()) {
        addAudioTrack("Audio 1");
    } else if (tracks_.size() > 1) {
        tracks_.resize(1);
    }

    auto& track = tracks_.front();
    track.clips.clear();
    const auto projectFrameCount = timeline::sourceFramesToProjectFrames(
        sourceFrameCount, sourceSampleRate, projectSampleRate_);
    track.clips.push_back(clips::AudioClip{
        nextClipId_++,
        sourceFile,
        {{0}, projectFrameCount},
        {0},
        sourceFrameCount,
        sourceSampleRate});
}

} // namespace vitadaw::project
