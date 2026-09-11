#pragma once

#include <filesystem>
#include <string>
#include <variant>

namespace vitadaw::commands {

struct AddAudioTrack {
    std::string name;
};

struct LoadAudioFile {
    std::filesystem::path file;
};

struct Play {};
struct Stop {};

using Command = std::variant<AddAudioTrack, LoadAudioFile, Play, Stop>;

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
