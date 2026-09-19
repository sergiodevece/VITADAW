#pragma once
#include "vitadaw/musical/MusicalTime.h"
#include "vitadaw/audio/PreparedTemporalContext.h"
#include "vitadaw/audio/InputMonitoring.h"
#include <type_traits>

#include "vitadaw/tracks/AudioTrack.h"
#include "vitadaw/routing/RoutingState.h"
#include "vitadaw/processors/InsertTarget.h"
#include "vitadaw/persistence/PersistenceResult.h"

#include <filesystem>
#include <string>
#include <variant>
#include <vector>

namespace vitadaw::commands {

struct AddAudioTrack {
    std::string name{};
    media::AudioChannelLayout layout{media::AudioChannelLayout::mono};
};
struct SetTrackRecordArmed { tracks::TrackId track; bool armed{}; };
struct Record {};
struct CancelRecording {};
struct DeleteAudioTrack { tracks::TrackId track; };
enum class TrackPlacement { before, after };
struct ReorderAudioTrack {
    tracks::TrackId track;
    tracks::TrackId anchor;
    TrackPlacement placement{TrackPlacement::before};
};

struct AddBus {
    std::string name;
};

struct LoadAudioFile {
    std::filesystem::path file;
    tracks::TrackId track;
};

struct ImportAudioToTrack {
    std::filesystem::path file;
    tracks::TrackId track;
    timeline::ProjectFramePosition projectStart{0};
};

// First import without a pre-existing target. The application creates the
// initial track transactionally only when the project has no audio tracks.
struct ImportAudioFile {
    std::filesystem::path file;
    timeline::ProjectFramePosition projectStart{0};
};

struct AddClip {
    tracks::TrackId track;
    media::SourceId source;
    timeline::ProjectFramePosition projectStart;
    timeline::ProjectFrameDuration duration;
    timeline::SourceFramePosition sourceOffset{0.0};
};

struct RemoveClip { clips::ClipId clip; };
struct RemoveSource { media::SourceId source; };
struct MoveClip {
    clips::ClipId clip;
    tracks::TrackId targetTrack;
    timeline::ProjectFramePosition projectStart;

    MoveClip(clips::ClipId id,
             timeline::ProjectFramePosition position) noexcept
        : clip(id), projectStart(position) {}
    MoveClip(clips::ClipId id, tracks::TrackId track,
             timeline::ProjectFramePosition position) noexcept
        : clip(id), targetTrack(track), projectStart(position) {}
};
struct DuplicateClip {
    clips::ClipId clip;
    timeline::ProjectFramePosition projectStart;
};
struct SplitClip {
    clips::ClipId clip;
    timeline::ProjectFramePosition splitPosition;
};
struct TrimClipLeft {
    clips::ClipId clip;
    timeline::ProjectFramePosition projectStart;
};
struct TrimClipRight {
    clips::ClipId clip;
    timeline::ProjectFramePosition projectEnd;
};
struct DeleteClip { clips::ClipId clip; };
struct MoveClips {
    std::vector<clips::ClipId> clips;
    std::int64_t deltaFrames{};
};
struct DeleteClips { std::vector<clips::ClipId> clips; };
struct DuplicateClips {
    std::vector<clips::ClipId> clips;
    std::int64_t deltaFrames{};
};

struct Play {};
struct AddTempoChange { musical::MusicalTickPosition tick; musical::TempoBpm bpm; };
struct MoveTempoChange { musical::TempoEventId id; musical::MusicalTickPosition tick; };
struct SetTempo { musical::TempoEventId id; musical::TempoBpm bpm; };
struct RemoveTempoChange { musical::TempoEventId id; };
struct AddTimeSignatureChange { musical::BarIndex bar; musical::TimeSignature signature; };
struct MoveTimeSignatureChange { musical::TimeSignatureEventId id; musical::BarIndex bar; };
struct SetTimeSignature { musical::TimeSignatureEventId id; musical::TimeSignature signature; };
struct RemoveTimeSignatureChange { musical::TimeSignatureEventId id; };
struct SetLoopRangeMusical { musical::MusicalTickPosition start, end; };
struct SetLoopEnabled { bool enabled{}; };
struct SetMetronomeEnabled { bool enabled{}; };
struct SetMetronomeLevel { audio::MetronomeLevelDb level; };
struct EnableInputMonitoring {};
struct DisableInputMonitoring {};
struct ToggleInputMonitoring {};
struct SetMonitorGain { audio::MonitorGainDb gain; };
struct SetAudioBufferSize { std::uint32_t frames{}; };
struct SetRecordingOffset { std::int64_t projectFrames{}; };
struct StartLoopbackLatencyTest {
    std::uint32_t inputChannel{};
    std::uint32_t outputChannel{};
};
struct CancelLoopbackLatencyTest {};
template<class T> inline constexpr bool isMusicalCommand =
    std::is_same_v<T, AddTempoChange> || std::is_same_v<T, MoveTempoChange> ||
    std::is_same_v<T, SetTempo> || std::is_same_v<T, RemoveTempoChange> ||
    std::is_same_v<T, AddTimeSignatureChange> || std::is_same_v<T, MoveTimeSignatureChange> ||
    std::is_same_v<T, SetTimeSignature> || std::is_same_v<T, RemoveTimeSignatureChange> ||
    std::is_same_v<T, SetLoopRangeMusical>;
struct Pause {};
struct Stop {};
struct SeekToProjectFrame { timeline::ProjectFramePosition position; };
struct GoToStart {};
struct GoToEnd {};
struct Undo {};
struct Redo {};
struct SaveProject {};
struct SaveProjectAs { std::filesystem::path path; };
struct LoadProject { std::filesystem::path path; bool discardUnsaved{}; };
struct SetTrackGain { tracks::TrackId track; mixer::GainDb gain; };
struct SetTrackPan { tracks::TrackId track; mixer::Pan pan; };
struct SetTrackMute { tracks::TrackId track; bool muted{}; };
struct SetTrackSolo { tracks::TrackId track; bool solo{}; };
struct SetMasterGain { mixer::GainDb gain; };
struct SetBusGain { routing::BusId bus; mixer::GainDb gain; };
struct SetBusPan { routing::BusId bus; mixer::Pan pan; };
struct SetBusMute { routing::BusId bus; bool muted{}; };
struct SetBusSolo { routing::BusId bus; bool solo{}; };
struct SetTrackOutputDestination {
    tracks::TrackId track;
    routing::OutputDestination destination;
};
struct SetBusOutputDestination {
    routing::BusId bus;
    routing::OutputDestination destination;
};
struct AddTrackSend {
    tracks::TrackId track;
    routing::BusId destination;
    routing::SendTapPoint tapPoint{routing::SendTapPoint::postFaderPostPan};
    mixer::GainDb level;
};
struct AddBusSend {
    routing::BusId bus;
    routing::BusId destination;
    routing::SendTapPoint tapPoint{routing::SendTapPoint::postFaderPostPan};
    mixer::GainDb level;
};
struct SetSendRoute {
    routing::SendId send;
    routing::BusId destination;
    routing::SendTapPoint tapPoint{routing::SendTapPoint::postFaderPostPan};
};
struct RemoveSend { routing::SendId send; };
struct SetSendLevel { routing::SendId send; mixer::GainDb level; };
struct SetSendMute { routing::SendId send; bool muted{}; };
struct AddProcessor {
    processors::InsertTarget target;
    processors::ProcessorType type;
};
struct RemoveProcessor { processors::ProcessorInstanceId processor; };
struct MoveProcessor {
    processors::ProcessorInstanceId processor;
    std::size_t newIndex{};
};
struct SetProcessorBypass {
    processors::ProcessorInstanceId processor;
    bool bypassed{};
};
struct SetProcessorParameter {
    processors::ProcessorInstanceId processor;
    processors::ParameterId parameter;
    float value{};
};

using Command = std::variant<AddAudioTrack, DeleteAudioTrack, ReorderAudioTrack,
                             SetTrackRecordArmed, Record, CancelRecording,
                             AddBus, LoadAudioFile,
                             ImportAudioToTrack, ImportAudioFile,
                             AddClip, RemoveClip,
                             RemoveSource, MoveClip, DuplicateClip, SplitClip,
                             TrimClipLeft, TrimClipRight, DeleteClip,
                             MoveClips, DeleteClips, DuplicateClips,
                             AddTempoChange, MoveTempoChange, SetTempo, RemoveTempoChange,
                             AddTimeSignatureChange, MoveTimeSignatureChange, SetTimeSignature, RemoveTimeSignatureChange,
                             SetLoopRangeMusical, SetLoopEnabled,
                             SetMetronomeEnabled, SetMetronomeLevel,
                             EnableInputMonitoring, DisableInputMonitoring,
                             ToggleInputMonitoring, SetMonitorGain, SetAudioBufferSize,
                             SetRecordingOffset, StartLoopbackLatencyTest,
                             CancelLoopbackLatencyTest,
                             Play, Pause, Stop, SeekToProjectFrame, GoToStart,
                             GoToEnd, Undo, Redo, SaveProject, SaveProjectAs, LoadProject,
                             SetTrackGain, SetTrackPan, SetTrackMute,
                             SetTrackSolo, SetMasterGain,
                             SetBusGain, SetBusPan, SetBusMute, SetBusSolo,
                             SetTrackOutputDestination,
                             SetBusOutputDestination, AddTrackSend, AddBusSend,
                             SetSendRoute, RemoveSend, SetSendLevel,
                             SetSendMute, AddProcessor, RemoveProcessor,
                             MoveProcessor, SetProcessorBypass,
                             SetProcessorParameter>;

enum class CommandStatus {
    accepted,
    rejected,
};

enum class CommandError {
    none,
    nothingToUndo,
    nothingToRedo,
    validationFailed,
    historyInvalid,
    historyCapacityExceeded,
    clipNotFound,
    invalidPosition,
    zeroLengthClip,
    sourceBoundsExceeded,
    transportMustBeStopped,
    invalidState,
    transportUnavailable,
    seekRejectedWhilePlaying,
    preparationFailed,
    capacityExceeded,
    userCancelled,
    fileNotFound,
    permissionDenied,
    unsupportedFormat,
    decodeFailed,
    trackNotFound,
    layoutMismatch,
    selectTargetTrack,
    noTargetTrack,
    commitFailed,
};

struct CommandResult {
    CommandResult(CommandStatus s = CommandStatus::accepted, std::string text = {},
        CommandError e = CommandError::none, persistence::PersistenceResult p = {},
        std::vector<clips::ClipId> created = {}) noexcept
        : status(s), message(std::move(text)), error(e), persistence(std::move(p)),
          createdClips(std::move(created)) {}
    CommandStatus status{CommandStatus::accepted};
    std::string message;
    CommandError error{CommandError::none};
    persistence::PersistenceResult persistence;
    // Specific structural result used to transfer DuplicateClips selection.
    std::vector<clips::ClipId> createdClips;
};

class ICommandHandler {
public:
    virtual ~ICommandHandler() = default;
    virtual CommandResult handle(const Command& command) = 0;
};

} // namespace vitadaw::commands
