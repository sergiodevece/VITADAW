#pragma once

#include "vitadaw/tracks/AudioTrack.h"
#include "vitadaw/routing/RoutingState.h"

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
    [[nodiscard]] const routing::RoutingState& routing() const noexcept;
    [[nodiscard]] tracks::TrackId addAudioTrack(std::string name);
    [[nodiscard]] routing::BusId addBus(std::string name);
    [[nodiscard]] bool setTrackOutputDestination(
        tracks::TrackId track,
        routing::OutputDestination destination) noexcept;
    [[nodiscard]] const routing::AudioBus* findBus(
        routing::BusId bus) const noexcept;
    [[nodiscard]] bool setBusMix(routing::BusId bus,
                                 mixer::BusMixState state) noexcept;
    [[nodiscard]] bool setBusOutputDestination(
        routing::BusId bus,
        routing::OutputDestination destination) noexcept;
    [[nodiscard]] const tracks::AudioTrack* findTrack(
        tracks::TrackId track) const noexcept;
    [[nodiscard]] const mixer::MasterMixState& masterMix() const noexcept;
    [[nodiscard]] bool setTrackMix(tracks::TrackId track,
                                   mixer::TrackMixState state) noexcept;
    [[nodiscard]] bool setMasterMix(mixer::MasterMixState state) noexcept;
    [[nodiscard]] timeline::ProjectFrameCount duration() const noexcept;
    [[nodiscard]] PreparedAudioClipUpdate prepareAudioClipUpdate(
        tracks::TrackId track,
        const std::filesystem::path& sourceFile,
        timeline::SourceFrameCount sourceFrameCount,
        timeline::SampleRate sourceSampleRate) const;
    void commitAudioClipUpdate(PreparedAudioClipUpdate& update) noexcept;
    void swap(ProjectState& other) noexcept;

private:
    timeline::SampleRate projectSampleRate_;
    std::vector<tracks::AudioTrack> tracks_;
    routing::RoutingState routing_;
    tracks::TrackId nextTrackId_{1};
    clips::ClipId nextClipId_{1};
    mixer::MasterMixState masterMix_;
};

} // namespace vitadaw::project
