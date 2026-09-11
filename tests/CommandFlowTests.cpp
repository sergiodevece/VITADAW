#include "vitadaw/application/DawApplication.h"
#include "vitadaw/commands/CommandDispatcher.h"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace {

class FakeAudioEngine final : public vitadaw::audio::IAudioEngineControl {
public:
    struct ResourceCounters { int destroyed{}; };

    class PreparedFile final : public vitadaw::audio::PreparedAudioFile {
    public:
        PreparedFile(vitadaw::audio::AudioFileMetadata metadata,
                     std::filesystem::path source,
                     vitadaw::tracks::TrackId destination,
                     vitadaw::timeline::SampleRate projectRate,
                     ResourceCounters& resourceCounters)
            : vitadaw::audio::PreparedAudioFile(metadata), file(std::move(source)),
              track(destination), projectSampleRate(projectRate),
              counters(resourceCounters) {}
        ~PreparedFile() override { ++counters.destroyed; }

        std::filesystem::path file;
        vitadaw::tracks::TrackId track;
        vitadaw::timeline::SampleRate projectSampleRate;
        ResourceCounters& counters;
    };

    struct LiveTrack {
        vitadaw::tracks::TrackId id;
        vitadaw::audio::AudioFileMetadata metadata;
        std::filesystem::path file;
        vitadaw::audio::PreparedAudioFilePtr resource;
    };

    FakeAudioEngine() { live.reserve(32); }

    vitadaw::audio::AudioFilePreparationResult prepareWav(
        const std::filesystem::path& file,
        vitadaw::tracks::TrackId track,
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
        const auto metadata = metadataFor(track);
        return {std::make_unique<PreparedFile>(metadata, file, track,
                                               projectSampleRate,
                                               resourceCounters), {}};
    }

    bool commitPreparedWav(
        vitadaw::audio::PreparedAudioFilePtr prepared,
        vitadaw::audio::AudioFileCommitAction modelCommit) noexcept override {
        auto* candidate = dynamic_cast<PreparedFile*>(prepared.get());
        if (candidate == nullptr || !modelCommit.isValid()) {
            return false;
        }
        ++commitRequests;
        auto found = std::find_if(live.begin(), live.end(),
                                  [id = candidate->track](const auto& track) {
                                      return track.id == id;
                                  });
        if (found == live.end()) {
            live.push_back({candidate->track, candidate->metadata,
                            candidate->file, std::move(prepared)});
        } else {
            found->metadata = candidate->metadata;
            found->file = candidate->file;
            found->resource = std::move(prepared);
        }
        snapshot.playing = false;
        snapshot.position = {0};
        snapshot.duration = {0};
        for (const auto& track : live) {
            snapshot.duration.value = std::max(
                snapshot.duration.value,
                vitadaw::timeline::sourceFramesToProjectDuration(
                    track.metadata.sourceFrameCount,
                    track.metadata.sourceSampleRate,
                    candidate->projectSampleRate).value);
        }
        modelCommit.execute();
        return true;
    }

    vitadaw::audio::AudioControlRequestResult tryRequestPlay() noexcept override {
        ++playRequests;
        if (!acceptRequests || live.empty()) {
            return {};
        }
        const auto sequence = nextSequence++;
        if (snapshot.position.value >= snapshot.duration.value) {
            snapshot.position = {0};
        }
        snapshot.playing = true;
        snapshot.lastProcessedCommandSequence = sequence;
        return {true, sequence};
    }

    vitadaw::audio::AudioControlRequestResult tryRequestStop() noexcept override {
        ++stopRequests;
        if (!acceptRequests) {
            return {};
        }
        const auto sequence = nextSequence++;
        snapshot.playing = false;
        snapshot.position = {0};
        snapshot.lastProcessedCommandSequence = sequence;
        return {true, sequence};
    }

    vitadaw::audio::RealtimeTransportSnapshot transportSnapshot() const noexcept override {
        return snapshot;
    }

    void publishProgress(std::int64_t frame, bool playing) noexcept {
        snapshot.position = {frame};
        snapshot.playing = playing;
    }

    void close() noexcept { live.clear(); }

    [[nodiscard]] const LiveTrack* loaded(vitadaw::tracks::TrackId id) const {
        const auto found = std::find_if(live.begin(), live.end(),
                                        [id](const auto& track) {
                                            return track.id == id;
                                        });
        return found == live.end() ? nullptr : &*found;
    }

    int loadRequests{};
    int commitRequests{};
    int playRequests{};
    int stopRequests{};
    bool acceptRequests{true};
    bool rejectNextLoad{};
    bool throwNextPreparation{};
    ResourceCounters resourceCounters;

private:
    [[nodiscard]] static vitadaw::audio::AudioFileMetadata metadataFor(
        vitadaw::tracks::TrackId track) {
        if (track.value == 1) {
            return {vitadaw::timeline::SampleRate{44100.0}, 1, {44100}, {1.0}};
        }
        if (track.value == 4) {
            return {vitadaw::timeline::SampleRate{48000.0}, 2, {192000}, {4.0}};
        }
        return {vitadaw::timeline::SampleRate{48000.0}, 2, {96000}, {2.0}};
    }

    std::vector<LiveTrack> live;
    vitadaw::audio::RealtimeTransportSnapshot snapshot;
    vitadaw::audio::AudioCommandSequence nextSequence{1};
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

    check(app.project().tracks().empty(),
          "a project should support a zero-track topology");
    check(dispatcher.dispatch(commands::Play{}).status ==
              commands::CommandStatus::rejected,
          "Play with zero tracks should be rejected");

    for (int index = 1; index <= 4; ++index) {
        check(dispatcher.dispatch(commands::AddAudioTrack{
                  "Audio " + std::to_string(index)}).status ==
                  commands::CommandStatus::accepted,
              "adding an audio track should succeed");
    }
    check(app.project().tracks().size() == 4 &&
              app.project().tracks()[0].id == tracks::TrackId{1} &&
              app.project().tracks()[3].id == tracks::TrackId{4},
          "track identities should be stable and monotonic");

    const tracks::TrackId first{1};
    const tracks::TrackId emptyBetween{2};
    const tracks::TrackId third{3};
    const tracks::TrackId fourth{4};

    const auto invalid = dispatcher.dispatch(
        commands::LoadAudioFile{"invalid.wav", tracks::TrackId{999}});
    check(invalid.status == commands::CommandStatus::rejected &&
              app.project().duration().value == 0 && audio.commitRequests == 0,
          "an unknown track must fail before either side commits");

    check(dispatcher.dispatch(commands::LoadAudioFile{"first.wav", first}).status ==
              commands::CommandStatus::accepted,
          "a valid WAV should load by stable track identity");
    check(dispatcher.dispatch(commands::LoadAudioFile{"third.wav", third}).status ==
              commands::CommandStatus::accepted,
          "a track after an empty track should load independently");
    check(!app.project().tracks()[1].hasAudio() &&
              app.project().tracks()[2].hasAudio(),
          "empty tracks between loaded tracks should remain coherent");

    const auto destroyedBeforeReplacement = audio.resourceCounters.destroyed;
    check(dispatcher.dispatch(commands::LoadAudioFile{
              "first-replacement.wav", first}).status ==
              commands::CommandStatus::accepted &&
              audio.resourceCounters.destroyed == destroyedBeforeReplacement + 1 &&
              audio.loaded(first)->file == "first-replacement.wav",
          "replacement should destroy the old resource and preserve track identity");

    check(dispatcher.dispatch(commands::LoadAudioFile{"fourth.wav", fourth}).status ==
              commands::CommandStatus::accepted &&
              app.project().duration().value == 192000,
          "global duration should be the maximum active track end");

    audio.rejectNextLoad = true;
    check(dispatcher.dispatch(commands::LoadAudioFile{"broken.wav", third}).status ==
              commands::CommandStatus::rejected &&
              audio.loaded(third)->file == "third.wav" &&
              !app.project().findTrack(emptyBetween)->hasAudio(),
          "a failed load should preserve every existing resource and empty track");

    audio.throwNextPreparation = true;
    check(dispatcher.dispatch(commands::LoadAudioFile{"throws.wav", first}).status ==
              commands::CommandStatus::rejected &&
              audio.loaded(first)->file == "first-replacement.wav",
          "a preparation exception should preserve the published project");

    check(dispatcher.dispatch(commands::Play{}).status ==
              commands::CommandStatus::accepted,
          "a variable project with prepared tracks should play");
    audio.publishProgress(96000, true);
    app.synchroniseTransport();
    check(app.transport().playback == transport::PlaybackState::playing,
          "shorter tracks ending must not stop the global transport");

    check(dispatcher.dispatch(commands::Stop{}).status ==
              commands::CommandStatus::accepted &&
              app.transport().position.value == 0,
          "Stop should rewind the master clock");
    check(dispatcher.dispatch(commands::Play{}).status ==
              commands::CommandStatus::accepted,
          "Play should restart after Stop");

    audio.publishProgress(192000, false);
    app.synchroniseTransport();
    check(app.transport().playback == transport::PlaybackState::stopped &&
              app.transport().position.value == app.transport().duration.value,
          "all tracks ending should stop at the global duration");
    check(dispatcher.dispatch(commands::Play{}).status ==
              commands::CommandStatus::accepted,
          "Play after global end should restart");
    app.synchroniseTransport();
    check(app.transport().position.value == 0,
          "replay should start the shared clock at zero");

    audio.acceptRequests = false;
    check(dispatcher.dispatch(commands::Stop{}).status ==
              commands::CommandStatus::rejected &&
              app.transport().playback == transport::PlaybackState::playing,
          "a rejected RT command must not mutate application transport");

    const auto destroyedBeforeClose = audio.resourceCounters.destroyed;
    audio.close();
    check(audio.resourceCounters.destroyed == destroyedBeforeClose + 3,
          "closing should destroy all live N-track resources");

    std::cout << "All command-flow tests passed\n";
    return EXIT_SUCCESS;
}
