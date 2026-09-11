#include "vitadaw/commands/CommandDispatcher.h"

namespace vitadaw::commands {

CommandDispatcher::CommandDispatcher(ICommandHandler& handler) noexcept
    : handler_(handler) {}

CommandResult CommandDispatcher::dispatch(const Command& command) {
    return handler_.handle(command);
}

} // namespace vitadaw::commands

