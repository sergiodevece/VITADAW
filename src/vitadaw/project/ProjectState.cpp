#include "vitadaw/project/ProjectState.h"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <type_traits>

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
    if (!nextTrackId_.isValid() ||
        nextTrackId_.value == std::numeric_limits<std::uint64_t>::max()) {
        throw std::overflow_error{"Audio track identity space exhausted"};
    }
    const auto id = nextTrackId_;
    tracks_.push_back({id, std::move(name), std::nullopt});
    ++nextTrackId_.value;
    return id;
}

const tracks::AudioTrack* ProjectState::findTrack(
    tracks::TrackId track) const noexcept {
    const auto found = std::find_if(tracks_.begin(), tracks_.end(),
                                    [track](const auto& candidate) {
                                        return candidate.id == track;
                                    });
    return found == tracks_.end() ? nullptr : &*found;
}

timeline::ProjectFrameCount ProjectState::duration() const noexcept {
    timeline::ProjectFrameCount result;
    for (const auto& track : tracks_) {
        if (track.clip.has_value()) {
            result.value = std::max(result.value, track.clip->timelineRange.end().value);
        }
    }
    return result;
}

ProjectState::PreparedAudioClipUpdate ProjectState::prepareAudioClipUpdate(
    tracks::TrackId track,
    const std::filesystem::path& sourceFile,
    timeline::SourceFrameCount sourceFrameCount,
    timeline::SampleRate sourceSampleRate) const {
    const auto found = std::find_if(tracks_.begin(), tracks_.end(),
                                    [track](const auto& candidate) {
                                        return candidate.id == track;
                                    });
    if (found == tracks_.end()) {
        throw std::out_of_range{"Audio track does not exist"};
    }
    const auto projectFrameCount = timeline::sourceFramesToProjectDuration(
        sourceFrameCount, sourceSampleRate, projectSampleRate_);
    PreparedAudioClipUpdate update;
    update.track = track;
    update.trackIndex = static_cast<std::size_t>(found - tracks_.begin());
    update.replacement.emplace(clips::AudioClip{
        nextClipId_, sourceFile, {{0}, projectFrameCount}, {0},
        sourceFrameCount, sourceSampleRate});
    update.nextClipId = nextClipId_ + 1;
    return update;
}

void ProjectState::commitAudioClipUpdate(
    PreparedAudioClipUpdate& update) noexcept {
    static_assert(std::is_nothrow_swappable_v<
                  std::optional<clips::AudioClip>>);
    tracks_[update.trackIndex].clip.swap(update.replacement);
    nextClipId_ = update.nextClipId;
}

} // namespace vitadaw::project
