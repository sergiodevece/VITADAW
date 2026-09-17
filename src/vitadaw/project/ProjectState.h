#pragma once

#include "vitadaw/tracks/AudioTrack.h"
#include "vitadaw/media/AudioSource.h"
#include "vitadaw/routing/RoutingState.h"
#include "vitadaw/processors/InsertTarget.h"
#include "vitadaw/musical/MusicalTime.h"

#include <cstddef>
#include <filesystem>
#include <span>
#include <string>
#include <vector>
#include <memory>
#include <optional>

namespace vitadaw::history { class UndoableOperation; }
namespace vitadaw::project {

// Mutable only from the application thread.
class ProjectState {
public:
    static constexpr std::size_t maximumTracks = 256;
    static constexpr std::size_t maximumSources = 2048;
    static constexpr std::size_t maximumClips = 32768;
    static constexpr std::size_t maximumClipsPerTrack = 4096;

    struct ProjectSettings {
        std::string name{"Untitled"};
        timeline::SampleRate sampleRate;
    };
    struct DocumentData {
        ProjectSettings settings;
        std::vector<tracks::AudioTrack> tracks;
        std::vector<media::AudioSource> sources;
        routing::RoutingState::DocumentData routing;
        tracks::TrackId nextTrackId{1};
        media::SourceId nextSourceId{1};
        clips::ClipId nextClipId{1};
        processors::ProcessorInstanceId nextProcessorId{1};
        mixer::MasterMixState masterMix;
        processors::InsertChain masterInserts;
        musical::MusicalTimeMap musicalTime;
        std::optional<musical::MusicalLoopRange> loopRange;
    };
    [[nodiscard]] DocumentData documentData() const;
    // Validates a complete detached model. IDs/counters are adopted, never generated.
    [[nodiscard]] static std::unique_ptr<ProjectState> fromDocumentData(DocumentData data);

    struct ImportedAudio {
        media::SourceId source;
        clips::ClipId clip;
    };

    struct IndexedSendRoute {
        std::size_t index{};
        routing::SendRoute route;
        bool operator==(const IndexedSendRoute&) const = default;
    };

    // Bounded owning submodel used by track Undo/Redo. It never contains
    // prepared DSP/runtime state or borrowed views.
    struct TrackHistoryState {
        std::size_t trackIndex{};
        tracks::AudioTrack track;
        std::size_t routeIndex{};
        routing::TrackRoute route;
        std::vector<IndexedSendRoute> sends;
        bool operator==(const TrackHistoryState&) const = default;
    };

    enum class ClipEditStatus {
        success,
        clipNotFound,
        invalidPosition,
        zeroLengthClip,
        sourceBoundsExceeded,
        capacityExceeded,
        trackNotFound,
        layoutMismatch,
    };

    struct ClipEditResult {
        ClipEditStatus status{ClipEditStatus::success};
        clips::ClipId clip{};
        clips::ClipId createdClip{};

        [[nodiscard]] constexpr bool succeeded() const noexcept {
            return status == ClipEditStatus::success;
        }
        constexpr explicit operator bool() const noexcept { return succeeded(); }
    };

    struct ClipHistoryState {
        tracks::TrackId track;
        clips::AudioClip clip;
        bool operator==(const ClipHistoryState&) const = default;
    };

    struct BatchClipEditResult {
        ClipEditStatus status{ClipEditStatus::success};
        std::vector<ClipHistoryState> before;
        std::vector<ClipHistoryState> after;
        std::vector<clips::ClipId> createdClips;
        bool changed{};

        [[nodiscard]] bool succeeded() const noexcept {
            return status == ClipEditStatus::success;
        }
        explicit operator bool() const noexcept { return succeeded(); }
    };

    struct TrackReorderResult {
        ClipEditStatus status{ClipEditStatus::success};
        std::size_t beforeIndex{};
        std::size_t afterIndex{};
        bool changed{};
        [[nodiscard]] bool succeeded() const noexcept {
            return status == ClipEditStatus::success;
        }
        explicit operator bool() const noexcept { return succeeded(); }
    };

    explicit ProjectState(timeline::SampleRate projectSampleRate,
                          std::string name = "Untitled");

    [[nodiscard]] const ProjectSettings& settings() const noexcept;
    [[nodiscard]] timeline::SampleRate sampleRate() const noexcept;
    [[nodiscard]] const std::vector<tracks::AudioTrack>& tracks() const noexcept;
    [[nodiscard]] const std::vector<media::AudioSource>& sources() const noexcept;
    [[nodiscard]] const routing::RoutingState& routing() const noexcept;
    [[nodiscard]] tracks::TrackId addAudioTrack(
        std::string name,
        media::AudioChannelLayout layout = media::AudioChannelLayout::mono);
    [[nodiscard]] std::optional<TrackHistoryState> captureTrackHistoryState(
        tracks::TrackId track) const;
    [[nodiscard]] std::optional<TrackHistoryState> removeAudioTrack(
        tracks::TrackId track);
    [[nodiscard]] TrackReorderResult reorderAudioTrack(
        tracks::TrackId track, tracks::TrackId anchor, bool placeAfter) noexcept;
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
    [[nodiscard]] routing::SendId addSend(
        routing::SendSource source, routing::BusId destination,
        routing::SendTapPoint tapPoint, mixer::SendMixState mix);
    [[nodiscard]] bool setSendRoute(
        routing::SendId send, routing::BusId destination,
        routing::SendTapPoint tapPoint) noexcept;
    [[nodiscard]] bool removeSend(routing::SendId send) noexcept;
    [[nodiscard]] bool setSendMix(routing::SendId send,
                                  mixer::SendMixState mix) noexcept;
    [[nodiscard]] const routing::SendRoute* findSend(
        routing::SendId send) const noexcept;
    [[nodiscard]] const tracks::AudioTrack* findTrack(
        tracks::TrackId track) const noexcept;
    [[nodiscard]] const media::AudioSource* findSource(
        media::SourceId source) const noexcept;
    [[nodiscard]] const clips::AudioClip* findClip(
        clips::ClipId clip) const noexcept;
    [[nodiscard]] tracks::TrackId trackContainingClip(
        clips::ClipId clip) const noexcept;
    [[nodiscard]] std::span<const clips::AudioClip> clipsForTrack(
        tracks::TrackId track) const noexcept;
    [[nodiscard]] std::vector<clips::ClipId> clipsIntersectingRange(
        tracks::TrackId track, timeline::ProjectFramePosition rangeStart,
        timeline::ProjectFramePosition rangeEnd) const;
    [[nodiscard]] const mixer::MasterMixState& masterMix() const noexcept;
    [[nodiscard]] const processors::InsertChain& masterInserts() const noexcept;
    [[nodiscard]] const processors::ProcessorState* findProcessor(
        processors::ProcessorInstanceId processor) const noexcept;
    [[nodiscard]] processors::ProcessorInstanceId addProcessor(
        const processors::InsertTarget& target,
        processors::ProcessorType type);
    [[nodiscard]] bool removeProcessor(
        processors::ProcessorInstanceId processor) noexcept;
    [[nodiscard]] bool moveProcessor(processors::ProcessorInstanceId processor,
                                     std::size_t newIndex) noexcept;
    [[nodiscard]] bool setProcessorBypass(
        processors::ProcessorInstanceId processor, bool bypassed) noexcept;
    [[nodiscard]] bool setProcessorParameter(
        processors::ProcessorInstanceId processor,
        processors::ParameterId parameter, float value) noexcept;
    [[nodiscard]] bool setTrackMix(tracks::TrackId track,
                                   mixer::TrackMixState state) noexcept;
    [[nodiscard]] bool setMasterMix(mixer::MasterMixState state) noexcept;
    [[nodiscard]] timeline::ProjectFrameCount duration() const noexcept;
    [[nodiscard]] ImportedAudio importAudioToTrack(
        tracks::TrackId track, media::MediaReference mediaReference,
        timeline::SourceFrameCount sourceFrameCount,
        timeline::SampleRate sourceSampleRate,
        media::AudioChannelLayout sourceLayout,
        timeline::ProjectFramePosition projectStart = {0});
    [[nodiscard]] clips::ClipId addClip(
        tracks::TrackId track, media::SourceId source,
        timeline::ProjectFramePosition projectStart,
        timeline::ProjectFrameDuration duration,
        timeline::SourceFramePosition sourceOffset = {0.0});
    [[nodiscard]] ClipEditResult removeClip(clips::ClipId clip) noexcept;
    [[nodiscard]] ClipEditResult deleteClip(clips::ClipId clip) noexcept;
    [[nodiscard]] BatchClipEditResult moveClips(
        std::span<const clips::ClipId> clips, std::int64_t deltaFrames);
    [[nodiscard]] BatchClipEditResult deleteClips(
        std::span<const clips::ClipId> clips);
    [[nodiscard]] BatchClipEditResult duplicateClips(
        std::span<const clips::ClipId> clips, std::int64_t deltaFrames);
    [[nodiscard]] ClipEditResult moveClip(
        clips::ClipId clip,
        timeline::ProjectFramePosition projectStart) noexcept;
    [[nodiscard]] ClipEditResult moveClip(
        clips::ClipId clip, tracks::TrackId targetTrack,
        timeline::ProjectFramePosition projectStart);
    [[nodiscard]] ClipEditResult duplicateClip(
        clips::ClipId clip,
        timeline::ProjectFramePosition projectStart);
    [[nodiscard]] ClipEditResult splitClip(
        clips::ClipId clip,
        timeline::ProjectFramePosition splitPosition);
    [[nodiscard]] ClipEditResult trimClipLeft(
        clips::ClipId clip,
        timeline::ProjectFramePosition projectStart) noexcept;
    [[nodiscard]] ClipEditResult trimClipRight(
        clips::ClipId clip,
        timeline::ProjectFramePosition projectEnd) noexcept;
    [[nodiscard]] bool removeSource(media::SourceId source) noexcept;
    [[nodiscard]] timeline::ProjectFrameCount projectContentDuration() const noexcept;
    void swap(ProjectState& other) noexcept;
    [[nodiscard]] const musical::MusicalTimeMap& musicalTime() const noexcept { return musicalTime_; }
    [[nodiscard]] const std::optional<musical::MusicalLoopRange>& loopRange() const noexcept { return loopRange_; }
private:
    friend class history::UndoableOperation;
    // Candidate mutation is restricted to history; active publication is a swap.
    void setMusicalTime(musical::MusicalTimeMap map) noexcept { musicalTime_ = std::move(map); }
    void setLoopRange(std::optional<musical::MusicalLoopRange> range) noexcept { loopRange_ = range; }
    [[nodiscard]] bool restoreHistoryClip(tracks::TrackId track,
                                          const clips::AudioClip& clip);
    [[nodiscard]] bool restoreHistoryTrack(const TrackHistoryState& state);
    [[nodiscard]] bool transferHistoryClip(
        tracks::TrackId fromTrack, const clips::AudioClip& before,
        tracks::TrackId toTrack, const clips::AudioClip& after);
    [[nodiscard]] bool replaceHistoryClip(tracks::TrackId track,
                                          const clips::AudioClip& clip) noexcept;
    [[nodiscard]] bool reorderHistoryTrack(
        tracks::TrackId track, std::size_t expectedIndex,
        std::size_t targetIndex) noexcept;
    [[nodiscard]] bool restoreHistoryClips(
        std::span<const ClipHistoryState> clips);
    [[nodiscard]] bool restoreHistoryRecording(
        tracks::TrackId, const media::AudioSource&,
        const clips::AudioClip&);
    [[nodiscard]] bool deleteHistoryRecording(
        tracks::TrackId, const media::AudioSource&,
        const clips::AudioClip&) noexcept;
    [[nodiscard]] bool deleteHistoryClips(
        std::span<const ClipHistoryState> clips) noexcept;
    [[nodiscard]] bool replaceHistoryClips(
        std::span<const ClipHistoryState> expected,
        std::span<const ClipHistoryState> replacement) noexcept;
    [[nodiscard]] processors::InsertChain* findProcessorChain(
        processors::ProcessorInstanceId processor) noexcept;
    [[nodiscard]] const processors::InsertChain* findProcessorChain(
        processors::ProcessorInstanceId processor) const noexcept;

    [[nodiscard]] tracks::AudioTrack* findTrackMutable(
        tracks::TrackId track) noexcept;
    [[nodiscard]] tracks::AudioTrack* findTrackContainingClip(
        clips::ClipId clip) noexcept;
    [[nodiscard]] bool validateClip(
        const tracks::AudioTrack& track, const media::AudioSource& source,
        timeline::ProjectFramePosition projectStart,
        timeline::ProjectFrameDuration duration,
        timeline::SourceFramePosition sourceOffset) const noexcept;
    void sortTrackClips(tracks::AudioTrack& track) noexcept;

    ProjectSettings settings_;
    musical::MusicalTimeMap musicalTime_;
    std::optional<musical::MusicalLoopRange> loopRange_;
    std::vector<tracks::AudioTrack> tracks_;
    std::vector<media::AudioSource> sources_;
    routing::RoutingState routing_;
    tracks::TrackId nextTrackId_{1};
    media::SourceId nextSourceId_{1};
    clips::ClipId nextClipId_{1};
    mixer::MasterMixState masterMix_;
    processors::InsertChain masterInserts_;
    processors::ProcessorInstanceId nextProcessorId_{1};
};

} // namespace vitadaw::project
