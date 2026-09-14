#pragma once

#include "vitadaw/tracks/AudioTrack.h"
#include "vitadaw/routing/RoutingState.h"

#include <filesystem>
#include <string>
#include <variant>

namespace vitadaw::commands {

struct AddAudioTrack {
    std::string name;
};

struct AddBus {
    std::string name;
};

struct LoadAudioFile {
    std::filesystem::path file;
    tracks::TrackId track;
};

struct Play {};
struct Stop {};
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
struct RemoveSend { routing::SendId send; };
struct SetSendLevel { routing::SendId send; mixer::GainDb level; };
struct SetSendMute { routing::SendId send; bool muted{}; };

using Command = std::variant<AddAudioTrack, AddBus, LoadAudioFile, Play, Stop,
                             SetTrackGain, SetTrackPan, SetTrackMute,
                             SetTrackSolo, SetMasterGain,
                             SetBusGain, SetBusPan, SetBusMute, SetBusSolo,
                             SetTrackOutputDestination,
                             SetBusOutputDestination, AddTrackSend, RemoveSend,
                             SetSendLevel, SetSendMute>;

enum class CommandStatus {
    accepted,
    rejected,
};

struct CommandResult {
    CommandStatus status{CommandStatus::accepted};
    std::string message;
};

class ICommandHandler {
public:
    virtual ~ICommandHandler() = default;
    virtual CommandResult handle(const Command& command) = 0;
};

} // namespace vitadaw::commands
