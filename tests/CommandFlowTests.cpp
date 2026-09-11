#include "vitadaw/application/DawApplication.h"
#include "vitadaw/commands/CommandDispatcher.h"

#include <cstdlib>
#include <iostream>
#include <string_view>

namespace {

class FakeAudioEngine final : public vitadaw::audio::IAudioEngineControl {
public:
    vitadaw::audio::AudioFileLoadResult loadWav(
        const std::filesystem::path& file,
        vitadaw::timeline::SampleRate projectSampleRate) override {
        ++loadRequests;
        if (!loadShouldSucceed) {
            return {false, {}, "Invalid WAV"};
        }
        loadedFile = file;
        prepared = true;
        snapshotState = {false, {0},
                         vitadaw::timeline::sourceFramesToProjectFrames(
                             {44100}, vitadaw::timeline::SampleRate{44100.0},
                             projectSampleRate),
                         lastProcessedSequence};
        return {true,
                {vitadaw::timeline::SampleRate{44100.0}, 1, {44100}, {1.0}},
                {}};
    }

    vitadaw::audio::AudioControlRequestResult tryRequestPlay() noexcept override {
        ++playRequests;
        if (!acceptRequests || !prepared) {
            return {};
        }
        const auto sequence = nextSequence++;
        if (snapshotState.position.value >= snapshotState.duration.value) {
            snapshotState.position = {0};
        }
        snapshotState.playing = true;
        snapshotState.lastProcessedCommandSequence = sequence;
        lastProcessedSequence = sequence;
        return {true, sequence};
    }

    vitadaw::audio::AudioControlRequestResult tryRequestStop() noexcept override {
        ++stopRequests;
        if (!acceptRequests) {
            return {};
        }
        const auto sequence = nextSequence++;
        snapshotState.playing = false;
        snapshotState.position = {0};
        snapshotState.lastProcessedCommandSequence = sequence;
        lastProcessedSequence = sequence;
        return {true, sequence};
    }

    vitadaw::audio::RealtimeTransportSnapshot transportSnapshot() const noexcept override {
        return snapshotState;
    }

    void publishProgress(std::int64_t projectFrame, bool playing) noexcept {
        snapshotState.position = {projectFrame};
        snapshotState.playing = playing;
    }

    int loadRequests{};
    int playRequests{};
    int stopRequests{};
    bool acceptRequests{true};
    bool loadShouldSucceed{true};
    bool prepared{};
    std::filesystem::path loadedFile;
    vitadaw::audio::RealtimeTransportSnapshot snapshotState;
    vitadaw::audio::AudioCommandSequence nextSequence{1};
    vitadaw::audio::AudioCommandSequence lastProcessedSequence{};
};

void check(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

} // namespace

int main() {
    using namespace vitadaw;
    FakeAudioEngine audio;
    application::DawApplication app{audio, timeline::SampleRate{48000.0}};
    commands::CommandDispatcher dispatcher{app};

    const auto addResult = dispatcher.dispatch(commands::AddAudioTrack{"Audio 1"});
    check(addResult.status == commands::CommandStatus::accepted,
          "add-track command should be accepted");
    check(app.project().tracks().size() == 1, "command should create one track");
    check(app.project().tracks().front().name == "Audio 1", "track should keep its name");

    const auto invalidResult = dispatcher.dispatch(commands::AddAudioTrack{""});
    check(invalidResult.status == commands::CommandStatus::rejected,
          "empty track name should be rejected");
    check(app.project().tracks().size() == 1, "rejected command must not mutate state");

    const auto playWithoutFile = dispatcher.dispatch(commands::Play{});
    check(playWithoutFile.status == commands::CommandStatus::rejected,
          "play without a prepared file should be rejected");
    check(app.transport().playback == transport::PlaybackState::stopped,
          "rejected play should preserve stopped transport");

    const auto stopWithoutFile = dispatcher.dispatch(commands::Stop{});
    check(stopWithoutFile.status == commands::CommandStatus::accepted,
          "stop without a file should remain valid");
    check(app.transport().position.value == 0,
          "stop without a file should stay at the start");

    audio.loadShouldSucceed = false;
    const auto invalidLoad = dispatcher.dispatch(commands::LoadAudioFile{"invalid.wav"});
    check(invalidLoad.status == commands::CommandStatus::rejected,
          "invalid WAV should be rejected");
    check(app.project().tracks().front().clips.empty(),
          "invalid WAV must not add a clip");

    audio.loadShouldSucceed = true;
    const auto validLoad = dispatcher.dispatch(commands::LoadAudioFile{"valid.wav"});
    check(validLoad.status == commands::CommandStatus::accepted,
          "valid WAV should be accepted");
    check(validLoad.message.find("44100 Hz | 1 ch | 1.000 s") != std::string::npos,
          "valid load should report portable audio metadata");
    check(app.project().sampleRate() == timeline::SampleRate{48000.0},
          "project sample rate should be explicit and stable");
    check(app.transport().duration.value == 48000,
          "source duration should be represented in project frames");
    check(app.project().tracks().front().clips.size() == 1,
          "load should create exactly one clip");

    const auto firstPlay = dispatcher.dispatch(commands::Play{});
    check(firstPlay.status == commands::CommandStatus::accepted,
          "play should be accepted after a valid load");
    check(app.transport().playback == transport::PlaybackState::playing,
          "accepted play should update transport");

    audio.publishProgress(24000, true);
    app.synchroniseTransport();
    check(app.transport().position.value == 24000,
          "application transport should follow RT progress");

    const auto stop = dispatcher.dispatch(commands::Stop{});
    check(stop.status == commands::CommandStatus::accepted, "stop should be accepted");
    check(audio.stopRequests == 2, "stop command should reach audio control");
    check(app.transport().playback == transport::PlaybackState::stopped,
          "stop command should update application transport");
    check(app.transport().position.value == 0,
          "application transport should rewind on stop");

    const auto secondPlay = dispatcher.dispatch(commands::Play{});
    check(secondPlay.status == commands::CommandStatus::accepted,
          "play should work again after stop");

    audio.publishProgress(48000, false);
    app.synchroniseTransport();
    check(app.transport().playback == transport::PlaybackState::stopped,
          "natural end should transition Playing to Stopped");
    check(app.transport().position.value == app.transport().duration.value,
          "natural end should preserve the logical end position");

    const auto replayAfterEnd = dispatcher.dispatch(commands::Play{});
    check(replayAfterEnd.status == commands::CommandStatus::accepted,
          "play after natural end should restart");
    app.synchroniseTransport();
    check(app.transport().position.value == 0,
          "play after natural end should restart at zero");

    audio.loadShouldSucceed = false;
    const auto failedReplacement =
        dispatcher.dispatch(commands::LoadAudioFile{"broken.wav"});
    check(failedReplacement.status == commands::CommandStatus::rejected,
          "failed replacement should be rejected");
    check(app.project().tracks().front().clips.front().sourceFile == "valid.wav",
          "failed replacement should preserve the valid project clip");
    check(audio.loadedFile == "valid.wav",
          "failed replacement should preserve the prepared resource");

    audio.acceptRequests = false;
    const auto rejectedStop = dispatcher.dispatch(commands::Stop{});
    check(rejectedStop.status == commands::CommandStatus::rejected,
          "a full audio queue should reject the command");
    check(app.transport().playback == transport::PlaybackState::playing,
          "rejected audio command must not update transport state");

    std::cout << "All command-flow tests passed\n";
    return EXIT_SUCCESS;
}
