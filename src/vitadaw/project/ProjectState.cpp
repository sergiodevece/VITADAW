#include "vitadaw/project/ProjectState.h"

#include <algorithm>
#include <stdexcept>
#include <type_traits>

namespace vitadaw::project {

ProjectState::ProjectState(timeline::SampleRate projectSampleRate)
    : projectSampleRate_(projectSampleRate),
      tracks_{{{1, "Audio 1", std::nullopt},
               {2, "Audio 2", std::nullopt}}} {
    if (!projectSampleRate_.isValid()) {
        throw std::invalid_argument{"Project sample rate must be positive"};
    }
}

timeline::SampleRate ProjectState::sampleRate() const noexcept {
    return projectSampleRate_;
}

const std::array<tracks::AudioTrack, tracks::audioTrackCount>&
ProjectState::tracks() const noexcept {
    return tracks_;
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
    tracks::AudioTrackSlot slot,
    const std::filesystem::path& sourceFile,
    timeline::SourceFrameCount sourceFrameCount,
    timeline::SampleRate sourceSampleRate) const {
    static_cast<void>(tracks_.at(tracks::toIndex(slot)));
    const auto projectFrameCount = timeline::sourceFramesToProjectDuration(
        sourceFrameCount, sourceSampleRate, projectSampleRate_);
    PreparedAudioClipUpdate update;
    update.track = slot;
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
    tracks_[tracks::toIndex(update.track)].clip.swap(update.replacement);
    nextClipId_ = update.nextClipId;
}

} // namespace vitadaw::project
