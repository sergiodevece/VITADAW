#pragma once

#include "vitadaw/audio/IAudioEngineControl.h"
#include "vitadaw/commands/Command.h"
#include "vitadaw/project/ProjectState.h"
#include "vitadaw/transport/TransportState.h"

namespace vitadaw::application {

class DawApplication final : public commands::ICommandHandler {
public:
    DawApplication(audio::IAudioEngineControl& audioEngine,
                   timeline::SampleRate projectSampleRate);

    commands::CommandResult handle(const commands::Command& command) override;
    void synchroniseTransport() noexcept;

    [[nodiscard]] const project::ProjectState& project() const noexcept;
    [[nodiscard]] const transport::TransportState& transport() const noexcept;

private:
    audio::IAudioEngineControl& audioEngine_;
    project::ProjectState project_;
    transport::TransportState transport_;
    audio::AudioCommandSequence pendingAudioCommandSequence_{};
};

} // namespace vitadaw::application
