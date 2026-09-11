#include "vitadaw/application/DawApplication.h"

#include <type_traits>
#include <iomanip>
#include <sstream>
#include <utility>

namespace vitadaw::application {

DawApplication::DawApplication(audio::IAudioEngineControl& audioEngine,
                               timeline::SampleRate projectSampleRate)
    : audioEngine_(audioEngine), project_(projectSampleRate) {}

commands::CommandResult DawApplication::handle(const commands::Command& command) {
    return std::visit(
        [this](const auto& value) -> commands::CommandResult {
            using T = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<T, commands::AddAudioTrack>) {
                if (value.name.empty()) {
                    return {commands::CommandStatus::rejected, "Track name cannot be empty"};
                }
                project_.addAudioTrack(value.name);
            } else if constexpr (std::is_same_v<T, commands::LoadAudioFile>) {
                const auto loadResult =
                    audioEngine_.loadWav(value.file, project_.sampleRate());
                if (!loadResult.success) {
                    return {commands::CommandStatus::rejected, loadResult.errorMessage};
                }

                project_.setSingleAudioClip(
                    value.file,
                    loadResult.metadata.sourceFrameCount,
                    loadResult.metadata.sourceSampleRate);
                const auto duration = timeline::sourceFramesToProjectFrames(
                    loadResult.metadata.sourceFrameCount,
                    loadResult.metadata.sourceSampleRate,
                    project_.sampleRate());
                transport_.setDuration(duration);
                pendingAudioCommandSequence_ =
                    audioEngine_.transportSnapshot().lastProcessedCommandSequence;
                std::ostringstream message;
                message << "Loaded WAV: " << value.file.filename().string() << " | "
                        << std::fixed << std::setprecision(0)
                        << loadResult.metadata.sourceSampleRate.hertz() << " Hz | "
                        << loadResult.metadata.channelCount << " ch | "
                        << std::setprecision(3) << loadResult.metadata.duration.value
                        << " s";
                return {commands::CommandStatus::accepted, message.str()};
            } else if constexpr (std::is_same_v<T, commands::Play>) {
                const auto request = audioEngine_.tryRequestPlay();
                if (!request.accepted) {
                    return {commands::CommandStatus::rejected,
                            "Play requires a valid prepared WAV"};
                }
                pendingAudioCommandSequence_ = request.sequence;
                transport_.markPlaying();
                return {commands::CommandStatus::accepted, "Playing"};
            } else if constexpr (std::is_same_v<T, commands::Stop>) {
                const auto request = audioEngine_.tryRequestStop();
                if (!request.accepted) {
                    return {commands::CommandStatus::rejected, "Audio command queue is full"};
                }
                pendingAudioCommandSequence_ = request.sequence;
                transport_.stopAndRewind();
                return {commands::CommandStatus::accepted, "Stopped at start"};
            }
            return {commands::CommandStatus::accepted, {}};
        },
        command);
}

void DawApplication::synchroniseTransport() noexcept {
    const auto snapshot = audioEngine_.transportSnapshot();
    if (snapshot.lastProcessedCommandSequence < pendingAudioCommandSequence_) {
        return;
    }

    transport_.synchronise(snapshot.playing, snapshot.position, snapshot.duration);
}

const project::ProjectState& DawApplication::project() const noexcept {
    return project_;
}

const transport::TransportState& DawApplication::transport() const noexcept {
    return transport_;
}

} // namespace vitadaw::application
