#pragma once

#include "vitadaw/commands/Command.h"

namespace vitadaw::commands {

// The only application capability exposed to UI and future input adapters.
class ICommandDispatcher {
public:
    virtual ~ICommandDispatcher() = default;
    virtual CommandResult dispatch(const Command& command) = 0;
};

class CommandDispatcher final : public ICommandDispatcher {
public:
    explicit CommandDispatcher(ICommandHandler& handler) noexcept;
    CommandResult dispatch(const Command& command) override;

private:
    ICommandHandler& handler_;
};

} // namespace vitadaw::commands

