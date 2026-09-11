#pragma once

#include "vitadaw/tracks/AudioTrack.h"

#include <filesystem>
#include <string>
#include <variant>

namespace vitadaw::commands {

struct AddAudioTrack {
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

using Command = std::variant<AddAudioTrack, LoadAudioFile, Play, Stop,
                             SetTrackGain, SetTrackPan, SetTrackMute,
                             SetTrackSolo, SetMasterGain>;

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
