#include "vitadaw/application/DawApplication.h"
#include "vitadaw/commands/CommandDispatcher.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string_view>

namespace {

class FakeAudioEngine final : public vitadaw::audio::IAudioEngineControl {
public:
    struct ResourceCounters {
        int destroyed{};
        int destroyedInRealtime{};
        bool realtimeActive{};
    };

    class PreparedFile final : public vitadaw::audio::PreparedAudioFile {
    public:
        PreparedFile(vitadaw::audio::AudioFileMetadata metadata,
                     std::filesystem::path source,
                     vitadaw::tracks::AudioTrackSlot destination,
                     vitadaw::timeline::SampleRate projectRate,
                     ResourceCounters& resourceCounters)
            : vitadaw::audio::PreparedAudioFile(metadata), file(std::move(source)),
              track(destination), projectSampleRate(projectRate),
              counters(resourceCounters) {}

        ~PreparedFile() override {
            ++counters.destroyed;
            if (counters.realtimeActive) {
                ++counters.destroyedInRealtime;
            }
        }

        std::filesystem::path file;
        vitadaw::tracks::AudioTrackSlot track;
        vitadaw::timeline::SampleRate projectSampleRate;
        ResourceCounters& counters;
    };

    FakeAudioEngine() {
        metadata[0] = {vitadaw::timeline::SampleRate{44100.0},
                       1,
                       {44100},
                       {1.0}};
        metadata[1] = {vitadaw::timeline::SampleRate{48000.0},
                       2,
                       {96000},
                       {2.0}};
    }

    vitadaw::audio::AudioFilePreparationResult prepareWav(
        const std::filesystem::path& file,
        vitadaw::tracks::AudioTrackSlot track,
        vitadaw::timeline::SampleRate projectSampleRate) override {
        ++loadRequests;
        if (throwNextPreparation) {
            throwNextPreparation = false;
            throw std::runtime_error{"injected preparation exception"};
        }
        if (rejectNextLoad) {
            rejectNextLoad = false;
            return {nullptr, "Invalid WAV"};
        }

        const auto index = vitadaw::tracks::toIndex(track);
        const auto metadataIndex = index < metadata.size() ? index : 0;
        return {std::make_unique<PreparedFile>(
                    metadata[metadataIndex], file, track, projectSampleRate,
                    resourceCounters),
                {}};
    }

    bool commitPreparedWav(
        vitadaw::audio::PreparedAudioFilePtr preparedFile,
        vitadaw::audio::AudioFileCommitAction modelCommit) noexcept override {
        auto* candidate = dynamic_cast<PreparedFile*>(preparedFile.get());
        if (candidate == nullptr || !modelCommit.isValid()) {
            return false;
        }
        const auto index = vitadaw::tracks::toIndex(candidate->track);
        if (index >= liveResources.size()) {
            return false;
        }

        ++commitRequests;
        loadedFiles[index] = candidate->file;
        this->prepared[index] = true;
        snapshotState.playing = false;
        snapshotState.position = {0};
        snapshotState.duration = {0};
        for (std::size_t slot = 0; slot < this->prepared.size(); ++slot) {
            if (this->prepared[slot]) {
                snapshotState.duration.value = std::max(
                    snapshotState.duration.value,
                    vitadaw::timeline::sourceFramesToProjectDuration(
                        metadata[slot].sourceFrameCount,
                        metadata[slot].sourceSampleRate,
                        candidate->projectSampleRate).value);
            }
        }

        auto previous = std::move(liveResources[index]);
        liveResources[index] = std::move(preparedFile);
        modelCommit.execute();
        return true;
    }

    vitadaw::audio::AudioControlRequestResult tryRequestPlay() noexcept override {
        ++playRequests;
        if (!acceptRequests || (!prepared[0] && !prepared[1])) {
            return {};
        }
        const auto sequence = nextSequence++;
        if (snapshotState.position.value >= snapshotState.duration.value) {
            snapshotState.position = {0};
        }
        snapshotState.playing = true;
        snapshotState.lastProcessedCommandSequence = sequence;
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
        return {true, sequence};
    }

    vitadaw::audio::RealtimeTransportSnapshot transportSnapshot() const noexcept override {
        return snapshotState;
    }

    void publishProgress(std::int64_t projectFrame, bool playing) noexcept {
        snapshotState.position = {projectFrame};
        snapshotState.playing = playing;
    }

    void close() noexcept { liveResources = {}; }

    std::array<vitadaw::audio::AudioFileMetadata, 2> metadata;
    std::array<std::optional<std::filesystem::path>, 2> loadedFiles;
    std::array<bool, 2> prepared{};
    int loadRequests{};
    int commitRequests{};
    int playRequests{};
    int stopRequests{};
    bool acceptRequests{true};
    bool rejectNextLoad{};
    bool throwNextPreparation{};
    vitadaw::audio::RealtimeTransportSnapshot snapshotState;
    vitadaw::audio::AudioCommandSequence nextSequence{1};
    ResourceCounters resourceCounters;
    std::array<vitadaw::audio::PreparedAudioFilePtr, 2> liveResources;
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

    check(app.project().tracks().size() == tracks::audioTrackCount,
          "project should always contain exactly two tracks");
    const auto addResult = dispatcher.dispatch(commands::AddAudioTrack{"Audio 3"});
    check(addResult.status == commands::CommandStatus::rejected,
          "a third track should be rejected");

    const auto playWithoutFile = dispatcher.dispatch(commands::Play{});
    check(playWithoutFile.status == commands::CommandStatus::rejected,
          "play without prepared audio should be rejected");
    const auto stopWithoutFile = dispatcher.dispatch(commands::Stop{});
    check(stopWithoutFile.status == commands::CommandStatus::accepted &&
              app.transport().position.value == 0,
          "stop without audio should remain valid at zero");

    audio.rejectNextLoad = true;
    const auto invalidSecond = dispatcher.dispatch(commands::LoadAudioFile{
        "invalid.wav", tracks::AudioTrackSlot::second});
    check(invalidSecond.status == commands::CommandStatus::rejected,
          "invalid WAV on track two should be rejected");
    check(!app.project().tracks()[0].hasAudio() &&
              !app.project().tracks()[1].hasAudio(),
          "failed load should not mutate either track");

    const auto firstLoad = dispatcher.dispatch(commands::LoadAudioFile{
        "first.wav", tracks::AudioTrackSlot::first});
    check(firstLoad.status == commands::CommandStatus::accepted,
          "valid WAV should load into track one");
    check(app.project().tracks()[0].clip->sourceFile == "first.wav" &&
              !app.project().tracks()[1].hasAudio(),
          "one loaded track should leave the other coherent and empty");
    check(app.transport().duration.value == 48000,
          "single loaded track should define project duration");

    const auto firstReplacement = dispatcher.dispatch(commands::LoadAudioFile{
        "first-replacement.wav", tracks::AudioTrackSlot::first});
    check(firstReplacement.status == commands::CommandStatus::accepted &&
              audio.resourceCounters.destroyed == 1 &&
              audio.resourceCounters.destroyedInRealtime == 0,
          "successive load should destroy the old real resource outside RT");
    check(app.project().tracks()[0].clip->sourceFile == "first-replacement.wav",
          "resource replacement should commit model and engine together");

    const auto secondLoad = dispatcher.dispatch(commands::LoadAudioFile{
        "second.wav", tracks::AudioTrackSlot::second});
    check(secondLoad.status == commands::CommandStatus::accepted,
          "valid WAV should load into track two");
    check(app.project().tracks()[0].hasAudio() && app.project().tracks()[1].hasAudio(),
          "both fixed tracks should contain one clip");
    check(app.transport().duration.value == 96000,
          "project duration should be the longest track");

    audio.rejectNextLoad = true;
    const auto failedReplacement = dispatcher.dispatch(commands::LoadAudioFile{
        "broken.wav", tracks::AudioTrackSlot::second});
    check(failedReplacement.status == commands::CommandStatus::rejected,
          "invalid replacement should be rejected");
    check(app.project().tracks()[0].clip->sourceFile == "first-replacement.wav" &&
              app.project().tracks()[1].clip->sourceFile == "second.wav",
          "invalid load in one track must preserve both valid resources");

    const auto destroyedBeforeException = audio.resourceCounters.destroyed;
    audio.throwNextPreparation = true;
    const auto exceptionalLoad = dispatcher.dispatch(commands::LoadAudioFile{
        "throws.wav", tracks::AudioTrackSlot::first});
    check(exceptionalLoad.status == commands::CommandStatus::rejected &&
              audio.resourceCounters.destroyed == destroyedBeforeException &&
              app.project().tracks()[0].clip->sourceFile ==
                  "first-replacement.wav",
          "recoverable preparation exception must preserve model and resource");

    const auto commitsBeforeForcedFailure = audio.commitRequests;
    const auto resourcesBeforeForcedFailure = audio.resourceCounters.destroyed;
    const auto forcedModelFailure = dispatcher.dispatch(commands::LoadAudioFile{
        "prepared-but-invalid-slot.wav",
        static_cast<tracks::AudioTrackSlot>(tracks::audioTrackCount)});
    check(forcedModelFailure.status == commands::CommandStatus::rejected &&
              audio.commitRequests == commitsBeforeForcedFailure &&
              audio.resourceCounters.destroyed == resourcesBeforeForcedFailure + 1,
          "a staged ProjectState failure must discard preparation before publication");
    check(app.project().tracks()[0].clip->sourceFile == "first-replacement.wav" &&
              app.project().tracks()[1].clip->sourceFile == "second.wav" &&
              audio.loadedFiles[0] == std::filesystem::path{"first-replacement.wav"} &&
              audio.loadedFiles[1] == std::filesystem::path{"second.wav"},
          "transaction failure must leave ProjectState and engine on old resources");

    const auto firstPlay = dispatcher.dispatch(commands::Play{});
    check(firstPlay.status == commands::CommandStatus::accepted &&
              app.transport().playback == transport::PlaybackState::playing,
          "play should start the global transport");
    audio.publishProgress(48000, true);
    app.synchroniseTransport();
    check(app.transport().position.value == 48000 &&
              app.transport().playback == transport::PlaybackState::playing,
          "transport should continue when the shorter track ends");

    const auto stop = dispatcher.dispatch(commands::Stop{});
    check(stop.status == commands::CommandStatus::accepted &&
              app.transport().position.value == 0,
          "Stop should rewind the shared clock");
    const auto secondPlay = dispatcher.dispatch(commands::Play{});
    check(secondPlay.status == commands::CommandStatus::accepted,
          "Play should restart both tracks after Stop");

    audio.publishProgress(96000, false);
    app.synchroniseTransport();
    check(app.transport().playback == transport::PlaybackState::stopped &&
              app.transport().position.value == app.transport().duration.value,
          "global transport should stop at the longest resource end");

    const auto replayAfterEnd = dispatcher.dispatch(commands::Play{});
    check(replayAfterEnd.status == commands::CommandStatus::accepted,
          "Play after project end should be accepted");
    app.synchroniseTransport();
    check(app.transport().position.value == 0,
          "Play after project end should restart the shared clock at zero");

    audio.acceptRequests = false;
    const auto rejectedStop = dispatcher.dispatch(commands::Stop{});
    check(rejectedStop.status == commands::CommandStatus::rejected &&
              app.transport().playback == transport::PlaybackState::playing,
          "rejected RT command must not mutate application transport");

    const auto destroyedBeforeClose = audio.resourceCounters.destroyed;
    audio.close();
    check(audio.resourceCounters.destroyed == destroyedBeforeClose + 2 &&
              audio.resourceCounters.destroyedInRealtime == 0,
          "closing should destroy every live resource outside RT");

    std::cout << "All command-flow tests passed\n";
    return EXIT_SUCCESS;
}
