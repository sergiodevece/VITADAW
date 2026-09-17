#include "vitadaw/application/DawApplication.h"
#include "vitadaw/commands/CommandDispatcher.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <limits>
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

    class PreparedPlan final
        : public vitadaw::audio::PreparedProcessingPlanChange {
    public:
        explicit PreparedPlan(
            vitadaw::audio::ProcessingPlanSpecification candidate,
            vitadaw::audio::PreparedAudioFilePtr importedSource = {})
            : specification(std::move(candidate)),
              imported(std::move(importedSource)) {}

        vitadaw::audio::ProcessingPlanSpecification specification;
        vitadaw::audio::PreparedAudioFilePtr imported;
    };

    struct LiveTrack {
        vitadaw::tracks::TrackId id;
        vitadaw::audio::AudioFileMetadata metadata;
        std::filesystem::path file;
        vitadaw::audio::PreparedAudioFilePtr resource;
    };

    FakeAudioEngine() { live.reserve(32); }

    vitadaw::audio::AudioFilePreparationResult prepareWav(
        const std::filesystem::path& file) override {
        ++loadRequests;
        if (throwNextPreparation) {
            throwNextPreparation = false;
            throw std::runtime_error{"injected preparation exception"};
        }
        if (rejectNextLoad) {
            rejectNextLoad = false;
            return {nullptr, "Invalid WAV"};
        }
        if (nextPreparationFailure !=
            vitadaw::audio::AudioFilePreparationFailure::none) {
            const auto failure = nextPreparationFailure;
            nextPreparationFailure =
                vitadaw::audio::AudioFilePreparationFailure::none;
            auto persistence = vitadaw::persistence::PersistenceResult{
                vitadaw::persistence::PersistenceCode::preparationFailed,
                vitadaw::persistence::PersistencePhase::prepare};
            if (failure == vitadaw::audio::AudioFilePreparationFailure::fileNotFound)
                persistence.code = vitadaw::persistence::PersistenceCode::fileNotFound;
            else if (failure == vitadaw::audio::AudioFilePreparationFailure::permissionDenied)
                persistence.code = vitadaw::persistence::PersistenceCode::permissionDenied;
            return {nullptr, {}, std::move(persistence), failure};
        }
        const auto track = trackFor(file);
        const auto metadata = metadataFor(track);
        return {std::make_unique<PreparedFile>(metadata, file, track,
                                               vitadaw::timeline::SampleRate{48000.0},
                                               resourceCounters), {}};
    }

    vitadaw::audio::StructuralPlanPreparationResult prepareProcessingPlan(
        const vitadaw::audio::ProcessingPlanSpecification& specification) override {
        ++structuralPrepareRequests;
        if (rejectNextStructuralPreparation) {
            rejectNextStructuralPreparation = false;
            return {nullptr, "Injected routing preparation failure"};
        }
        if (specification.sources.empty()) {
            const auto validation = vitadaw::audio::prepareProcessingPlan(
                specification, {});
            if (!validation.success()) {
                return {nullptr, validation.errorMessage};
            }
        }
        return {std::make_unique<PreparedPlan>(specification), {}};
    }

    vitadaw::audio::StructuralPlanPreparationResult
    prepareProcessingPlanWithAudio(
        const vitadaw::audio::ProcessingPlanSpecification& specification,
        vitadaw::media::SourceId source,
        vitadaw::audio::PreparedAudioFilePtr preparedAudio) override {
        ++structuralPrepareRequests;
        if (rejectNextStructuralPreparation) {
            rejectNextStructuralPreparation = false;
            return {nullptr, "Injected routing preparation failure"};
        }
        if (auto* file = dynamic_cast<PreparedFile*>(preparedAudio.get())) {
            for (const auto& track : specification.tracks) {
                const auto ownsSource = std::any_of(
                    track.clips.begin(), track.clips.end(),
                    [source](const auto& clip) { return clip.source == source; });
                if (ownsSource) {
                    file->track = track.id;
                    break;
                }
            }
        }
        return {std::make_unique<PreparedPlan>(specification,
                                                std::move(preparedAudio)), {}};
    }

    bool commitPreparedProcessingPlan(
        vitadaw::audio::PreparedProcessingPlanChangePtr prepared,
        vitadaw::audio::AudioFileCommitAction modelCommit) noexcept override {
        auto* candidate = dynamic_cast<PreparedPlan*>(prepared.get());
        if (candidate == nullptr || !modelCommit.isValid() ||
            rejectNextStructuralCommit) {
            rejectNextStructuralCommit = false;
            return false;
        }
        ++structuralCommitRequests;
        if (candidate->imported != nullptr) {
            auto* file = dynamic_cast<PreparedFile*>(candidate->imported.get());
            if (file == nullptr) {
                return false;
            }
            auto found = std::find_if(live.begin(), live.end(),
                                      [id = file->track](const auto& track) {
                                          return track.id == id;
                                      });
            if (found == live.end()) {
                live.push_back({file->track, file->metadata, file->file,
                                std::move(candidate->imported)});
            } else {
                found->metadata = file->metadata;
                found->file = file->file;
                found->resource = std::move(candidate->imported);
            }
            ++commitRequests;
        }
        liveSpecification = std::move(candidate->specification);
        snapshot.playing = false;
        snapshot.playback = vitadaw::transport::PlaybackState::stopped;
        snapshot.position = {0};
        snapshot.duration = {};
        for (const auto& track : liveSpecification.tracks) {
            for (const auto& clip : track.clips) {
                snapshot.duration.value = std::max(
                    snapshot.duration.value,
                    static_cast<std::int64_t>(std::ceil(
                        static_cast<double>(clip.projectStart.value) +
                        clip.duration.value)));
            }
        }
        modelCommit.execute();
        return true;
    }

    bool commitPreparedTemporalContext(
        std::unique_ptr<vitadaw::audio::PreparedTemporalContext> prepared,
        vitadaw::audio::AudioFileCommitAction commit) noexcept override {
        if (!prepared || !commit.isValid() || rejectNextTemporalCommit) {
            rejectNextTemporalCommit = false;
            return false;
        }
        temporal = std::move(prepared);
        snapshot.temporalRevision = temporal->revision;
        commit.execute();
        return true;
    }

    bool tryUpdateTrackMix(
        vitadaw::tracks::TrackId track,
        vitadaw::mixer::PreparedTrackMixState mix,
        vitadaw::audio::PreparedAudibilityState audibility) noexcept override {
        ++trackMixRequests;
        const auto planTrack = std::find_if(
            liveSpecification.tracks.begin(), liveSpecification.tracks.end(),
            [track](const auto& candidate) { return candidate.id == track; });
        if (!acceptMixerRequests || planTrack == liveSpecification.tracks.end() ||
            !mix.isValid()) {
            return false;
        }
        planTrack->mix = mix;
        lastAudibility = audibility;
        return true;
    }

    bool tryUpdateBusMix(
        vitadaw::routing::BusId bus,
        vitadaw::mixer::PreparedBusMixState mix,
        vitadaw::audio::PreparedAudibilityState audibility) noexcept override {
        ++busMixRequests;
        const auto found = std::find_if(
            liveSpecification.buses.begin(), liveSpecification.buses.end(),
            [bus](const auto& candidate) { return candidate.id == bus; });
        if (!acceptMixerRequests || found == liveSpecification.buses.end() ||
            !mix.isValid()) {
            return false;
        }
        found->mix = mix;
        lastAudibility = audibility;
        return true;
    }

    bool tryUpdateSendMix(
        vitadaw::routing::SendId send,
        vitadaw::mixer::PreparedSendMixState mix) noexcept override {
        ++sendMixRequests;
        const auto found = std::find_if(
            liveSpecification.sends.begin(), liveSpecification.sends.end(),
            [send](const auto& candidate) { return candidate.id == send; });
        if (!acceptMixerRequests || found == liveSpecification.sends.end() ||
            !mix.isValid()) {
            return false;
        }
        found->mix = mix;
        return true;
    }

    bool tryUpdateMasterMix(
        vitadaw::mixer::PreparedMasterMixState mix) noexcept override {
        ++masterMixRequests;
        return acceptMixerRequests && mix.isValid();
    }

    vitadaw::audio::AudioControlRequestResult tryRequestPlay() noexcept override {
        ++playRequests;
        if (!acceptRequests || (live.empty() && !snapshot.loopEnabled &&
                                !snapshot.metronomeEnabled)) {
            return {};
        }
        const auto sequence = nextSequence++;
        if (snapshot.position.value >= snapshot.duration.value) {
            snapshot.position = {0};
        }
        snapshot.playing = true;
        snapshot.playback = vitadaw::transport::PlaybackState::playing;
        runUntilStop = runUntilStop || snapshot.metronomeEnabled;
        snapshot.lastProcessedCommandSequence = sequence;
        return {true, sequence, vitadaw::audio::AudioControlRejection::none,
                snapshot.playback, snapshot.position, true};
    }

    vitadaw::audio::AudioControlRequestResult tryRequestStop() noexcept override {
        ++stopRequests;
        if (!acceptRequests) {
            return {};
        }
        const auto sequence = nextSequence++;
        const auto wasStopped = snapshot.playback ==
                                vitadaw::transport::PlaybackState::stopped;
        snapshot.playing = false;
        snapshot.playback = vitadaw::transport::PlaybackState::stopped;
        if (wasStopped) snapshot.position = {0};
        snapshot.lastProcessedCommandSequence = sequence;
        return {true, sequence, vitadaw::audio::AudioControlRejection::none,
                snapshot.playback, snapshot.position, true};
    }

    vitadaw::audio::AudioControlRequestResult tryRequestPause() noexcept override {
        const auto sequence = nextSequence++;
        snapshot.playing = false;
        snapshot.playback = vitadaw::transport::PlaybackState::paused;
        snapshot.lastProcessedCommandSequence = sequence;
        return {true, sequence, vitadaw::audio::AudioControlRejection::none,
                snapshot.playback, snapshot.position, true};
    }

    vitadaw::audio::AudioControlRequestResult tryRequestSeek(
        vitadaw::timeline::ProjectFramePosition position) noexcept override {
        if (!vitadaw::timeline::isSupportedProjectFramePosition(position)) {
            return {false, 0,
                    vitadaw::audio::AudioControlRejection::invalidPosition};
        }
        if (snapshot.playback == vitadaw::transport::PlaybackState::playing) {
            return {false, 0,
                    vitadaw::audio::AudioControlRejection::disallowedState};
        }
        const auto sequence = nextSequence++;
        snapshot.position = position;
        snapshot.lastProcessedCommandSequence = sequence;
        return {true, sequence, vitadaw::audio::AudioControlRejection::none,
                snapshot.playback, snapshot.position, true};
    }

    vitadaw::audio::AudioControlRequestResult trySetLoopEnabled(bool enabled) noexcept override {
        if (!acceptRequests || (enabled && (!temporal || !temporal->loop))) return {};
        const auto sequence = nextSequence++;
        snapshot.loopEnabled = enabled;
        snapshot.lastProcessedCommandSequence = sequence;
        return {true,sequence};
    }
    vitadaw::audio::AudioControlRequestResult trySetMetronomeEnabled(bool enabled) noexcept override {
        if (!acceptRequests) return {};
        const auto sequence = nextSequence++;
        const auto preserveOpenPlayback = snapshot.playing && runUntilStop;
        snapshot.metronomeEnabled = enabled;
        runUntilStop = enabled || preserveOpenPlayback;
        snapshot.lastProcessedCommandSequence = sequence;
        return {true,sequence};
    }
    vitadaw::audio::AudioControlRequestResult trySetMetronomeLevel(
        vitadaw::audio::MetronomeLevelDb level) noexcept override {
        if (!acceptRequests || !level.isValid()) return {};
        const auto sequence = nextSequence++;
        snapshot.metronomeLevelDb = level.value;
        snapshot.lastProcessedCommandSequence = sequence;
        return {true,sequence};
    }

    vitadaw::audio::RealtimeTransportSnapshot transportSnapshot() const noexcept override {
        return snapshot;
    }

    vitadaw::mixer::MeterSnapshot meterSnapshot() const noexcept override {
        return {};
    }

    void publishProgress(std::int64_t frame, bool playing) noexcept {
        snapshot.position = {frame};
        snapshot.playing = playing;
        snapshot.playback = playing ? vitadaw::transport::PlaybackState::playing
                                    : vitadaw::transport::PlaybackState::stopped;
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
    vitadaw::audio::AudioFilePreparationFailure nextPreparationFailure{
        vitadaw::audio::AudioFilePreparationFailure::none};
    bool throwNextPreparation{};
    bool acceptMixerRequests{true};
    int trackMixRequests{};
    int busMixRequests{};
    int sendMixRequests{};
    int masterMixRequests{};
    vitadaw::audio::PreparedAudibilityState lastAudibility;
    int structuralPrepareRequests{};
    int structuralCommitRequests{};
    bool rejectNextStructuralPreparation{};
    bool rejectNextStructuralCommit{};
    bool rejectNextTemporalCommit{};
    bool runUntilStop{};
    vitadaw::audio::ProcessingPlanSpecification liveSpecification;
    ResourceCounters resourceCounters;
    std::unique_ptr<vitadaw::audio::PreparedTemporalContext> temporal;

private:
    [[nodiscard]] static vitadaw::tracks::TrackId trackFor(
        const std::filesystem::path& file) {
        const auto name = file.filename().string();
        if (name.find("first") != std::string::npos ||
            name.find("throws") != std::string::npos) {
            return {1};
        }
        if (name.find("third") != std::string::npos ||
            name.find("broken") != std::string::npos) {
            return {3};
        }
        return {4};
    }

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

void timeSelectionLoopApplicationTests() {
    using namespace vitadaw;
    FakeAudioEngine audio;
    application::DawApplication app{audio, timeline::SampleRate{48000.0}};
    commands::CommandDispatcher dispatch{app};
    ui::timeline::TimelineInteraction interaction;
    const auto token = app.history().currentStateToken();
    const auto structuralPreparations = audio.structuralPrepareRequests;
    const auto musicalRevision = app.musicalRevision();
    interaction.setSnapEnabled(true);
    interaction.beginTimeSelection({24001}, app.timelineSnapshot(),
                                   app.musicalTime(), 0);
    interaction.updateTimeSelection({71999}, app.musicalTime(), 0);
    check(interaction.endTimeSelection() &&
              app.project().tracks().empty() &&
              app.project().sources().empty() &&
              !app.project().loopRange() &&
              app.history().currentStateToken() == token &&
              !app.session().dirty() &&
              audio.structuralPrepareRequests == structuralPreparations &&
              app.musicalRevision() == musicalRevision,
          "time selection and snap are ephemeral without plan preparation");
    const auto command = interaction.setLoopFromTimeSelectionCommand(
        app.musicalTime());
    check(command.has_value(), "time selection builds existing loop command");
    const auto result = dispatch.dispatch(*command);
    check(result.status == commands::CommandStatus::accepted &&
              app.project().loopRange().has_value() &&
              app.history().size() == 1 && app.session().dirty() &&
              interaction.timeSelection() ==
                  ui::timeline::TimeSelection{{24001}, {71999}} &&
              !app.loopEnabled(),
          "Set Loop From Selection is one documentary edit and preserves selection");
    const auto committedLoop = app.project().loopRange();
    check(dispatch.dispatch(commands::Undo{}).status ==
                  commands::CommandStatus::accepted &&
              !app.project().loopRange() &&
              dispatch.dispatch(commands::Redo{}).status ==
                  commands::CommandStatus::accepted &&
              app.project().loopRange() == committedLoop,
          "selection-derived loop uses existing Undo/Redo history");

    check(dispatch.dispatch(commands::Pause{}).status ==
                  commands::CommandStatus::accepted,
          "loop transport matrix enters Paused");
    app.synchroniseTransport();
    const auto pausedToken = app.history().currentStateToken();
    const auto paused = dispatch.dispatch(*command);
    check(paused.error == commands::CommandError::transportMustBeStopped &&
              app.history().currentStateToken() == pausedToken &&
              app.project().loopRange() == committedLoop,
          "selection-derived loop remains Stopped-only while Paused");
    check(dispatch.dispatch(commands::Stop{}).status ==
                  commands::CommandStatus::accepted,
          "loop transport matrix stops");
    app.synchroniseTransport();
    check(dispatch.dispatch(commands::SetLoopEnabled{true}).status ==
                  commands::CommandStatus::accepted &&
              dispatch.dispatch(commands::Play{}).status ==
                  commands::CommandStatus::accepted,
          "loop transport matrix enters Playing");
    app.synchroniseTransport();
    const auto playing = dispatch.dispatch(*command);
    check(playing.error == commands::CommandError::transportMustBeStopped &&
              app.project().loopRange() == committedLoop,
          "selection-derived loop remains Stopped-only while Playing");
}

void firstImportRegressionTests() {
    using namespace vitadaw;
    {
        FakeAudioEngine audio;
        application::DawApplication app{audio, timeline::SampleRate{48000.0}};
        commands::CommandDispatcher dispatch{app};
        const auto initialToken = app.history().currentStateToken();
        check(app.project().tracks().empty() && app.project().sources().empty() &&
                  app.project().routing().buses().empty() &&
                  app.project().routing().sends().empty() &&
                  app.project().masterMix() == mixer::MasterMixState{} &&
                  app.project().musicalTime() == musical::MusicalTimeMap{} &&
                  !app.project().loopRange() && !app.session().dirty(),
              "New Project is an empty, clean audio topology with master defaults");

        const auto cancelled = dispatch.dispatch(commands::ImportAudioFile{{}, {0}});
        check(cancelled.status == commands::CommandStatus::rejected &&
                  cancelled.error == commands::CommandError::userCancelled &&
                  audio.loadRequests == 0 &&
                  app.history().currentStateToken() == initialToken &&
                  app.project().tracks().empty(),
              "chooser cancellation is explicit and leaves the session unchanged");

        const std::array failures{
            std::pair{audio::AudioFilePreparationFailure::fileNotFound,
                      commands::CommandError::fileNotFound},
            std::pair{audio::AudioFilePreparationFailure::permissionDenied,
                      commands::CommandError::permissionDenied},
            std::pair{audio::AudioFilePreparationFailure::unsupportedFormat,
                      commands::CommandError::unsupportedFormat},
            std::pair{audio::AudioFilePreparationFailure::decodeFailed,
                      commands::CommandError::decodeFailed},
            std::pair{audio::AudioFilePreparationFailure::preparationFailed,
                      commands::CommandError::preparationFailed}};
        for (const auto& [failure, expected] : failures) {
            audio.nextPreparationFailure = failure;
            const auto result = dispatch.dispatch(
                commands::ImportAudioFile{"unreadable.wav", {0}});
            check(result.status == commands::CommandStatus::rejected &&
                      result.error == expected &&
                      app.history().currentStateToken() == initialToken &&
                      app.project().tracks().empty() && app.project().sources().empty(),
                  "media/access failures remain typed and transactional");
        }

        const auto imported = dispatch.dispatch(
            commands::ImportAudioFile{"first.wav", {0}});
        check(imported.status == commands::CommandStatus::accepted &&
                  app.project().tracks().size() == 1 &&
                  app.project().sources().size() == 1,
              "first import atomically creates the initial track, source and clip");
        const auto& track = app.project().tracks().front();
        const auto& source = app.project().sources().front();
        const auto& clip = track.clips.front();
        check(track.id.isValid() && source.id.isValid() && clip.id.isValid() &&
                  clip.source == source.id && clip.projectStart.value == 0 &&
                  source.frameCount.value == 44100 &&
                  source.sampleRate == timeline::SampleRate{44100.0} &&
                  clip.duration.value == 48000.0 &&
                  app.project().projectContentDuration().value == 48000,
              "first import preserves Source/Clip ownership and converted duration");
        const auto timeline = app.timelineSnapshot();
        check(timeline.tracks.size() == 1 &&
                  timeline.tracks.front().clips.size() == 1 &&
                  timeline.contentDuration.value == 48000 &&
                  timeline.tracks.front().clips.front().id == clip.id,
              "the committed first clip is immediately present in TimelineSnapshot");
        check(app.session().dirty() && !app.canUndo() && !app.canRedo() &&
                  app.history().currentStateToken() != initialToken,
              "successful non-undoable import uses a dirty history barrier");
        check(dispatch.dispatch(commands::Play{}).status ==
                  commands::CommandStatus::accepted,
              "the first imported prepared resource is playable");
    }

    {
        FakeAudioEngine audio;
        application::DawApplication app{audio, timeline::SampleRate{48000.0}};
        commands::CommandDispatcher dispatch{app};
        check(dispatch.dispatch(commands::AddAudioTrack{
                  "Existing", media::AudioChannelLayout::mono}).status ==
                  commands::CommandStatus::accepted,
              "existing empty mono track setup succeeds");
        const auto target = app.project().tracks().front().id;
        check(dispatch.dispatch(commands::ImportAudioToTrack{
                  "first.wav", target, {0}}).status ==
                  commands::CommandStatus::accepted &&
                  app.project().findTrack(target)->clips.size() == 1,
              "target-specific import into an existing empty track still works");
    }

    {
        FakeAudioEngine audio;
        application::DawApplication app{audio, timeline::SampleRate{48000.0}};
        commands::CommandDispatcher dispatch{app};
        check(dispatch.dispatch(commands::AddAudioTrack{
                  "Stereo", media::AudioChannelLayout::stereo}).status ==
                  commands::CommandStatus::accepted,
              "incompatible target setup succeeds");
        const auto token = app.history().currentStateToken();
        const auto noTarget = dispatch.dispatch(
            commands::ImportAudioFile{"first.wav", {0}});
        check(noTarget.error == commands::CommandError::noTargetTrack &&
                  app.history().currentStateToken() == token &&
                  app.project().sources().empty(),
              "general import requires an explicit target in a non-empty topology");

        audio.rejectNextStructuralPreparation = true;
        const auto preparation = dispatch.dispatch(
            commands::ImportAudioToTrack{"b.wav", app.project().tracks().front().id, {0}});
        check(preparation.error == commands::CommandError::preparationFailed &&
                  app.history().currentStateToken() == token &&
                  app.project().sources().empty(),
              "prepared-plan failure rolls back the candidate model");

        audio.rejectNextStructuralCommit = true;
        const auto commit = dispatch.dispatch(
            commands::ImportAudioToTrack{"b.wav", app.project().tracks().front().id, {0}});
        check(commit.error == commands::CommandError::commitFailed &&
                  app.history().currentStateToken() == token &&
                  app.project().sources().empty(),
              "commit failure preserves the active project and StateToken");
    }

    {
        FakeAudioEngine audio;
        application::DawApplication app{audio, timeline::SampleRate{48000.0}};
        commands::CommandDispatcher dispatch{app};
        check(dispatch.dispatch(commands::AddTempoChange{
                  {8 * musical::ppq}, {90.0}}).status ==
                  commands::CommandStatus::accepted &&
                  dispatch.dispatch(commands::SetLoopRangeMusical{
                      {4 * musical::ppq}, {12 * musical::ppq}}).status ==
                  commands::CommandStatus::accepted &&
                  dispatch.dispatch(commands::SetLoopEnabled{true}).status ==
                  commands::CommandStatus::accepted &&
                  dispatch.dispatch(commands::SetMetronomeEnabled{true}).status ==
                  commands::CommandStatus::accepted &&
                  dispatch.dispatch(commands::SetMetronomeLevel{{-18.0F}}).status ==
                  commands::CommandStatus::accepted,
              "musical and session state setup succeeds before first import");
        app.synchroniseTransport();
        const auto map = app.project().musicalTime();
        const auto loop = app.project().loopRange();
        check(dispatch.dispatch(commands::ImportAudioFile{"first.wav", {0}}).status ==
                  commands::CommandStatus::accepted &&
                  app.project().musicalTime() == map &&
                  app.project().loopRange() == loop && app.loopEnabled() &&
                  app.metronomeEnabled() && app.metronomeLevel().value == -18.0F,
              "first import preserves musical maps, loop locators and session controls");
    }
}

} // namespace

int main() {
    using namespace vitadaw;
    firstImportRegressionTests();
    timeSelectionLoopApplicationTests();
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
                  "Audio " + std::to_string(index),
                  index == 1 ? media::AudioChannelLayout::mono
                             : media::AudioChannelLayout::stereo}).status ==
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

    check(dispatcher.dispatch(commands::AddBus{"Bus A"}).status ==
              commands::CommandStatus::accepted &&
              dispatcher.dispatch(commands::AddBus{"Bus B"}).status ==
                  commands::CommandStatus::accepted &&
              app.project().routing().buses().size() == 2 &&
              app.project().routing().buses()[0].id == routing::BusId{1} &&
              app.project().routing().buses()[1].id == routing::BusId{2},
          "stereo bus identities should be stable and monotonic");
    const auto busA = app.project().routing().buses()[0].id;
    const auto busB = app.project().routing().buses()[1].id;
    check(dispatcher.dispatch(commands::SetBusOutputDestination{
              {999}, routing::OutputDestination::master()}).status ==
              commands::CommandStatus::rejected,
          "an unknown source BusId must be rejected before preparation");
    const auto loadsBeforeRouting = audio.loadRequests;
    check(dispatcher.dispatch(commands::SetTrackOutputDestination{
              first, routing::TrackOutputDestination::toBus(busA)}).status ==
              commands::CommandStatus::accepted &&
              app.project().routing().findTrackRoute(first)->destination ==
                  routing::TrackOutputDestination::toBus(busA) &&
              audio.loadRequests == loadsBeforeRouting,
          "routing should rebuild a WAV-independent prepared plan");
    check(dispatcher.dispatch(commands::SetTrackOutputDestination{
              first, routing::TrackOutputDestination::toBus({999})}).status ==
              commands::CommandStatus::rejected &&
              app.project().routing().findTrackRoute(first)->destination ==
                  routing::TrackOutputDestination::toBus(busA),
          "an unknown bus must preserve the previous model and plan");
    check(dispatcher.dispatch(commands::SetBusOutputDestination{
              busA, routing::OutputDestination::toBus(busB)}).status ==
              commands::CommandStatus::accepted &&
              app.project().findBus(busA)->outputDestination ==
                  routing::OutputDestination::toBus(busB),
          "a stopped project must commit Bus-to-Bus routing structurally");
    const auto specificationBeforeCycle = audio.liveSpecification;
    const auto cycle = dispatcher.dispatch(commands::SetBusOutputDestination{
        busB, routing::OutputDestination::toBus(busA)});
    check(cycle.status == commands::CommandStatus::rejected &&
              cycle.message.find("Bus 1") != std::string::npos &&
              app.project().findBus(busB)->outputDestination ==
                  routing::OutputDestination::master() &&
              audio.liveSpecification.buses.size() ==
                  specificationBeforeCycle.buses.size() &&
              audio.liveSpecification.buses[0].destination ==
                  specificationBeforeCycle.buses[0].destination &&
              audio.liveSpecification.buses[1].destination ==
                  specificationBeforeCycle.buses[1].destination,
          "a cycle must preserve both editable routing and the published plan");
    check(dispatcher.dispatch(commands::SetTrackOutputDestination{
              third, routing::OutputDestination::toBus(busB)}).status ==
              commands::CommandStatus::accepted,
          "a sibling source should route directly to the downstream bus");

    check(dispatcher.dispatch(commands::AddTrackSend{
              {999}, busB, routing::SendTapPoint::preFaderPrePan, {}}).status ==
              commands::CommandStatus::rejected &&
              dispatcher.dispatch(commands::AddTrackSend{
                  first, {999}, routing::SendTapPoint::preFaderPrePan, {}}).status ==
                  commands::CommandStatus::rejected,
          "Track Send creation must validate source and destination before commit");
    check(dispatcher.dispatch(commands::AddTrackSend{
              first, busB, routing::SendTapPoint::preFaderPrePan,
              mixer::GainDb{-6.0F}}).status == commands::CommandStatus::accepted &&
              dispatcher.dispatch(commands::AddTrackSend{
                  first, busB, routing::SendTapPoint::postFaderPostPan,
                  mixer::GainDb{-3.0F}}).status == commands::CommandStatus::accepted &&
              app.project().routing().sends().size() == 2 &&
              app.project().routing().sends()[0].id == routing::SendId{1} &&
              app.project().routing().sends()[1].id == routing::SendId{2} &&
              audio.liveSpecification.sends.size() == 2,
          "parallel Track Sends must commit with stable monotonic identities");
    const auto structuralBeforeSendParameters = audio.structuralPrepareRequests;
    check(dispatcher.dispatch(commands::SetSendLevel{
              {1}, mixer::GainDb{-9.0F}}).status ==
              commands::CommandStatus::accepted &&
              dispatcher.dispatch(commands::SetSendMute{{1}, true}).status ==
              commands::CommandStatus::accepted &&
              audio.sendMixRequests == 2 &&
              audio.structuralPrepareRequests == structuralBeforeSendParameters &&
              app.project().findSend({1})->mix ==
                  mixer::SendMixState{{-9.0F}, true} &&
              audio.liveSpecification.sends[0].mix ==
                  mixer::prepare(mixer::SendMixState{{-9.0F}, true}),
          "send parameters must update RT, persisted specification and model without rebuilding");
    check(dispatcher.dispatch(commands::SetSendLevel{
              {1}, mixer::GainDb{std::numeric_limits<float>::infinity()}}).status ==
              commands::CommandStatus::rejected &&
              app.project().findSend({1})->mix.level == mixer::GainDb{-9.0F},
          "invalid send level must preserve model and RT state");
    audio.acceptMixerRequests = false;
    check(dispatcher.dispatch(commands::SetSendMute{{1}, false}).status ==
              commands::CommandStatus::rejected &&
              app.project().findSend({1})->mix.muted,
          "a rejected send parameter publication must preserve the model");
    audio.acceptMixerRequests = true;
    check(dispatcher.dispatch(commands::RemoveSend{{2}}).status ==
              commands::CommandStatus::accepted &&
              app.project().routing().sends().size() == 1 &&
              app.project().routing().sends()[0].id == routing::SendId{1} &&
              audio.liveSpecification.sends.size() == 1,
          "removing a send must commit model and prepared specification together");

    check(dispatcher.dispatch(commands::AddBus{"Bus C"}).status ==
              commands::CommandStatus::accepted,
          "a destination for Bus Send retargeting should be addable while stopped");
    const auto busC = app.project().routing().buses()[2].id;
    check(dispatcher.dispatch(commands::AddBusSend{
              {999}, busB, routing::SendTapPoint::preFaderPrePan, {}}).status ==
              commands::CommandStatus::rejected &&
              dispatcher.dispatch(commands::AddBusSend{
                  busA, {999}, routing::SendTapPoint::preFaderPrePan, {}}).status ==
                  commands::CommandStatus::rejected,
          "Bus Send creation must validate source and destination");
    check(dispatcher.dispatch(commands::AddBusSend{
              busA, busB, routing::SendTapPoint::preFaderPrePan,
              mixer::GainDb{-6.0F}}).status == commands::CommandStatus::accepted &&
              app.project().findSend({3}) != nullptr &&
              std::get<routing::BusId>(app.project().findSend({3})->source) ==
                  busA &&
              audio.liveSpecification.sends.size() == 2,
          "AddBusSend must commit a typed BusId source and stable SendId");
    check(dispatcher.dispatch(commands::SetSendRoute{
              {3}, busC, routing::SendTapPoint::postFaderPostPan}).status ==
              commands::CommandStatus::accepted &&
              app.project().findSend({3})->destination == busC &&
              app.project().findSend({3})->tapPoint ==
                  routing::SendTapPoint::postFaderPostPan,
          "a stopped Bus Send must retarget destination and tap transactionally");
    const auto planBeforeRejectedBusSend = audio.liveSpecification;
    check(dispatcher.dispatch(commands::SetSendRoute{
              {3}, busA, routing::SendTapPoint::preFaderPrePan}).status ==
              commands::CommandStatus::rejected &&
              dispatcher.dispatch(commands::AddBusSend{
                  busB, busA, routing::SendTapPoint::postFaderPostPan, {}}).status ==
                  commands::CommandStatus::rejected &&
              app.project().findSend({3})->destination == busC &&
              audio.liveSpecification.sends.size() ==
                  planBeforeRejectedBusSend.sends.size(),
          "self-routes and mixed output/send cycles must preserve the prior model and plan");
    audio.rejectNextStructuralPreparation = true;
    check(dispatcher.dispatch(commands::SetSendRoute{
              {3}, busB, routing::SendTapPoint::preFaderPrePan}).status ==
              commands::CommandStatus::rejected &&
              app.project().findSend({3})->destination == busC &&
              app.project().findSend({3})->tapPoint ==
                  routing::SendTapPoint::postFaderPostPan,
          "failed Bus Send preparation must leave editable and RT state unchanged");

    const auto structuralBeforeBusMix = audio.structuralPrepareRequests;
    check(dispatcher.dispatch(commands::SetBusGain{
              busA, mixer::GainDb{-6.0F}}).status ==
              commands::CommandStatus::accepted &&
              dispatcher.dispatch(commands::SetBusPan{
                  busA, mixer::Pan{0.25F}}).status ==
                  commands::CommandStatus::accepted &&
              dispatcher.dispatch(commands::SetBusMute{busA, true}).status ==
                  commands::CommandStatus::accepted &&
              dispatcher.dispatch(commands::SetBusSolo{busA, true}).status ==
                  commands::CommandStatus::accepted &&
              audio.busMixRequests == 4 &&
              audio.structuralPrepareRequests == structuralBeforeBusMix &&
              app.project().findBus(busA)->mix ==
                  mixer::BusMixState{{-6.0F}, {0.25F}, true, true} &&
              audio.lastAudibility.trackIsAudible(0) &&
              audio.lastAudibility.busIsAudible(0) &&
              audio.lastAudibility.busIsAudible(1) &&
              !audio.lastAudibility.trackIsAudible(1) &&
              !audio.lastAudibility.trackIsAudible(2),
          "bus parameters must update model, RT state and resolved solo without rebuilding");
    check(dispatcher.dispatch(commands::SetBusGain{
              busA, mixer::GainDb{
                        std::numeric_limits<float>::infinity()}}).status ==
              commands::CommandStatus::rejected &&
              app.project().findBus(busA)->mix.gain == mixer::GainDb{-6.0F},
          "invalid bus parameters must preserve both model and RT state");
    check(dispatcher.dispatch(commands::SetBusMute{{999}, true}).status ==
              commands::CommandStatus::rejected &&
              audio.busMixRequests == 4,
          "an unknown BusId must fail before publishing a parameter command");
    audio.acceptMixerRequests = false;
    check(dispatcher.dispatch(commands::SetBusMute{busA, false}).status ==
              commands::CommandStatus::rejected &&
              app.project().findBus(busA)->mix.muted,
          "a rejected bus parameter publication must preserve the model");
    audio.acceptMixerRequests = true;
    check(dispatcher.dispatch(commands::SetBusMute{busA, false}).status ==
              commands::CommandStatus::accepted &&
              dispatcher.dispatch(commands::SetBusSolo{busA, false}).status ==
                  commands::CommandStatus::accepted,
          "bus mute and solo must be independently reversible");

    check(dispatcher.dispatch(commands::SetTrackGain{
              emptyBetween, mixer::GainDb{-6.0F}}).status ==
              commands::CommandStatus::accepted &&
              app.project().findTrack(emptyBetween)->mix.gain ==
                  mixer::GainDb{-6.0F} &&
              audio.trackMixRequests == 1,
          "an empty track should update its prepared mixer state without a WAV");
    check(dispatcher.dispatch(commands::SetTrackPan{
              emptyBetween,
              mixer::Pan{std::numeric_limits<float>::quiet_NaN()}}).status ==
              commands::CommandStatus::rejected &&
              app.project().findTrack(emptyBetween)->mix.pan == mixer::Pan{},
          "invalid portable mixer values must not mutate project state");

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

    check(dispatcher.dispatch(commands::SetTrackPan{
              first, mixer::Pan{-0.5F}}).status ==
              commands::CommandStatus::accepted &&
              dispatcher.dispatch(commands::SetTrackMute{first, true}).status ==
                  commands::CommandStatus::accepted &&
              dispatcher.dispatch(commands::SetTrackSolo{first, true}).status ==
                  commands::CommandStatus::accepted &&
              audio.trackMixRequests == 4 &&
              app.project().findTrack(first)->mix ==
                  mixer::TrackMixState{{}, {-0.5F}, true, true},
          "loaded track commands must update RT before committing the model");
    check(dispatcher.dispatch(commands::SetTrackSolo{emptyBetween, true}).status ==
              commands::CommandStatus::accepted &&
              audio.lastAudibility.trackIsAudible(1),
          "solo on an empty track must still update global RT solo eligibility");
    check(dispatcher.dispatch(commands::SetMasterGain{
              mixer::GainDb{-3.0F}}).status ==
              commands::CommandStatus::accepted &&
              audio.masterMixRequests == 1 &&
              app.project().masterMix().gain == mixer::GainDb{-3.0F},
          "master gain command must flow through the audio engine and model");
    audio.acceptMixerRequests = false;
    check(dispatcher.dispatch(commands::SetTrackGain{
              first, mixer::GainDb{-12.0F}}).status ==
              commands::CommandStatus::rejected &&
              app.project().findTrack(first)->mix.gain == mixer::GainDb{},
          "a rejected RT parameter update must preserve project mixer state");
    audio.acceptMixerRequests = true;

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
    const auto preparationsWhilePlaying = audio.structuralPrepareRequests;
    check(dispatcher.dispatch(commands::SetBusGain{
              busA, mixer::GainDb{-3.0F}}).status ==
              commands::CommandStatus::accepted &&
              audio.structuralPrepareRequests == preparationsWhilePlaying,
          "bus parameters must remain available during playback without a graph rebuild");
    check(dispatcher.dispatch(commands::SetSendLevel{
              {1}, mixer::GainDb{-12.0F}}).status ==
              commands::CommandStatus::accepted &&
              audio.structuralPrepareRequests == preparationsWhilePlaying,
          "send level must remain available during playback without rebuilding");
    check(dispatcher.dispatch(commands::AddTrackSend{
              first, busA, routing::SendTapPoint::preFaderPrePan, {}}).status ==
              commands::CommandStatus::rejected &&
              dispatcher.dispatch(commands::AddBusSend{
                  busA, busC, routing::SendTapPoint::preFaderPrePan, {}}).status ==
                  commands::CommandStatus::rejected &&
              dispatcher.dispatch(commands::SetSendRoute{
                  {3}, busB, routing::SendTapPoint::preFaderPrePan}).status ==
                  commands::CommandStatus::rejected &&
              dispatcher.dispatch(commands::RemoveSend{{1}}).status ==
                  commands::CommandStatus::rejected &&
              app.project().routing().sends().size() == 2,
          "send topology changes must be rejected during playback");
    check(dispatcher.dispatch(commands::SetTrackOutputDestination{
              first, routing::TrackOutputDestination::master()}).status ==
              commands::CommandStatus::rejected &&
              audio.structuralPrepareRequests == preparationsWhilePlaying &&
              app.project().routing().findTrackRoute(first)->destination ==
                  routing::TrackOutputDestination::toBus(busA),
          "routing changes during playback must be rejected before preparation");
    check(dispatcher.dispatch(commands::SetBusOutputDestination{
              busA, routing::OutputDestination::master()}).status ==
              commands::CommandStatus::rejected &&
              app.project().findBus(busA)->outputDestination ==
                  routing::OutputDestination::toBus(busB),
          "Bus output changes must also be rejected while playing");
    audio.publishProgress(96000, true);
    app.synchroniseTransport();
    check(app.transport().playback == transport::PlaybackState::playing,
          "shorter tracks ending must not stop the global transport");

    check(dispatcher.dispatch(commands::Stop{}).status ==
              commands::CommandStatus::accepted &&
              app.transport().position.value == 96000,
          "first Stop should preserve the master clock");
    const auto navigationToken = app.history().currentStateToken();
    const auto navigationCursor = app.history().cursor();
    check(dispatcher.dispatch(commands::SeekToProjectFrame{{48000}}).status ==
              commands::CommandStatus::accepted &&
              app.transport().position.value == 48000,
          "Seek while Stopped should update the application mirror");
    check(dispatcher.dispatch(commands::Play{}).status ==
              commands::CommandStatus::accepted &&
              dispatcher.dispatch(commands::SeekToProjectFrame{{24000}}).error ==
                  commands::CommandError::seekRejectedWhilePlaying,
          "Seek while Playing must be rejected explicitly");
    check(dispatcher.dispatch(commands::Pause{}).status ==
              commands::CommandStatus::accepted &&
              app.transport().playback == transport::PlaybackState::paused &&
              dispatcher.dispatch(commands::SeekToProjectFrame{{24000}}).status ==
                  commands::CommandStatus::accepted &&
              app.transport().position.value == 24000,
          "Pause and Seek should preserve the Paused state");
    check(dispatcher.dispatch(commands::Stop{}).status ==
              commands::CommandStatus::accepted &&
              app.transport().position.value == 24000 &&
              dispatcher.dispatch(commands::Stop{}).status ==
                  commands::CommandStatus::accepted &&
              app.transport().position.value == 0,
          "first Stop preserves position and second Stop rewinds");
    check(dispatcher.dispatch(commands::GoToEnd{}).status ==
              commands::CommandStatus::accepted &&
              app.transport().position.value == app.transport().duration.value &&
              dispatcher.dispatch(commands::GoToStart{}).status ==
                  commands::CommandStatus::accepted &&
              app.transport().position.value == 0,
          "GoToEnd and GoToStart must use project-frame Seek");
    check(app.history().currentStateToken() == navigationToken &&
              app.history().cursor() == navigationCursor,
          "transport navigation must not alter Undo or dirty state");
    const auto sharedSource = app.project().findTrack(first)->clips.front().source;
    const auto clipsBeforeFailure = app.project().findTrack(first)->clips.size();
    audio.rejectNextStructuralPreparation = true;
    check(dispatcher.dispatch(commands::AddClip{
              first, sharedSource, {0}, {100.0}, {0.0}}).status ==
              commands::CommandStatus::rejected &&
              app.project().findTrack(first)->clips.size() == clipsBeforeFailure,
          "failed AddClip preparation must preserve model and active plan");
    check(dispatcher.dispatch(commands::AddClip{
              first, sharedSource, {0}, {100.0}, {0.0}}).status ==
              commands::CommandStatus::accepted &&
              app.project().findTrack(first)->clips.size() ==
                  clipsBeforeFailure + 1,
          "AddClip must reuse a prepared Source without decoding media");
    const auto addedClip = app.project().findTrack(first)->clips.back().id;
    check(dispatcher.dispatch(commands::RemoveClip{addedClip}).status ==
              commands::CommandStatus::accepted &&
              app.project().findSource(sharedSource) != nullptr,
          "RemoveClip must retain its AudioSource");
    const auto editedOriginal = app.project().findTrack(first)->clips.front().id;
    const auto loadsBeforeEditing = audio.loadRequests;
    const auto sourcesBeforeEditing = app.project().sources().size();
    check(dispatcher.dispatch(commands::DuplicateClip{
              editedOriginal, {96000}}).status ==
              commands::CommandStatus::accepted,
          "DuplicateClip must flow through dispatcher and structural commit");
    const auto duplicatedClip =
        std::max_element(app.project().findTrack(first)->clips.begin(),
                         app.project().findTrack(first)->clips.end(),
                         [](const auto& left, const auto& right) {
                             return left.id < right.id;
                         })->id;
    check(dispatcher.dispatch(commands::MoveClip{
              duplicatedClip, {72000}}).status ==
              commands::CommandStatus::accepted &&
              dispatcher.dispatch(commands::SplitClip{
                  duplicatedClip, {96000}}).status ==
                  commands::CommandStatus::accepted,
          "MoveClip and SplitClip must rebuild and commit stopped timelines");
    const auto splitRight =
        std::max_element(app.project().findTrack(first)->clips.begin(),
                         app.project().findTrack(first)->clips.end(),
                         [](const auto& left, const auto& right) {
                             return left.id < right.id;
                         })->id;
    check(dispatcher.dispatch(commands::TrimClipLeft{
              splitRight, {108000}}).status ==
              commands::CommandStatus::accepted &&
              dispatcher.dispatch(commands::TrimClipRight{
                  editedOriginal, {24000}}).status ==
                  commands::CommandStatus::accepted &&
              dispatcher.dispatch(commands::DeleteClip{splitRight}).status ==
                  commands::CommandStatus::accepted &&
              audio.loadRequests == loadsBeforeEditing &&
              app.project().sources().size() == sourcesBeforeEditing,
          "trim/delete edits must preserve Source cache and never decode again");
    const auto originalBeforeFailedMove = *app.project().findClip(editedOriginal);
    audio.rejectNextStructuralPreparation = true;
    const auto failedMove = dispatcher.dispatch(commands::MoveClip{
        editedOriginal, {1000}});
    check(failedMove.status == commands::CommandStatus::rejected &&
              failedMove.error == commands::CommandError::preparationFailed &&
              *app.project().findClip(editedOriginal) == originalBeforeFailedMove,
          "failed edit preparation must preserve model and active plan");
    check(dispatcher.dispatch(commands::SetTrackOutputDestination{
              first, routing::TrackOutputDestination::master()}).status ==
              commands::CommandStatus::accepted,
          "a stopped project should accept a routing change");
    audio.rejectNextStructuralPreparation = true;
    check(dispatcher.dispatch(commands::SetTrackOutputDestination{
              first, routing::TrackOutputDestination::toBus(busA)}).status ==
              commands::CommandStatus::rejected &&
              app.project().routing().findTrackRoute(first)->destination ==
                  routing::TrackOutputDestination::master() &&
              audio.liveSpecification.tracks[0].destination ==
                  routing::TrackOutputDestination::master(),
          "failed routing preparation must preserve model and published plan");
    audio.rejectNextStructuralCommit = true;
    check(dispatcher.dispatch(commands::SetTrackOutputDestination{
              first, routing::TrackOutputDestination::toBus(busA)}).status ==
              commands::CommandStatus::rejected &&
              app.project().routing().findTrackRoute(first)->destination ==
                  routing::TrackOutputDestination::master() &&
              audio.liveSpecification.tracks[0].destination ==
                  routing::TrackOutputDestination::master(),
          "failed routing commit must preserve model and published plan");
    check(dispatcher.dispatch(commands::Play{}).status ==
              commands::CommandStatus::accepted,
          "Play should restart after Stop");
    const auto clipCountWhilePlaying = app.project().findTrack(first)->clips.size();
    const auto editWhilePlaying = dispatcher.dispatch(commands::DuplicateClip{
        editedOriginal, {120000}});
    check(editWhilePlaying.status == commands::CommandStatus::rejected &&
              editWhilePlaying.error ==
                  commands::CommandError::transportMustBeStopped &&
              app.project().findTrack(first)->clips.size() ==
                  clipCountWhilePlaying,
          "timeline edits must be rejected before preparation while Playing");

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

    FakeAudioEngine sessionAudio;
    application::DawApplication sessionApp{sessionAudio,
                                            timeline::SampleRate{48000}};
    commands::CommandDispatcher sessionDispatcher{sessionApp};
    const auto initialToken = sessionApp.history().currentStateToken();
    check(sessionDispatcher.dispatch(commands::SetLoopRangeMusical{
              {4*musical::ppq},{12*musical::ppq}}).status ==
              commands::CommandStatus::accepted &&
          sessionApp.project().loopRange().has_value() &&
          sessionApp.history().currentStateToken() != initialToken,
          "loop range is a documentary undoable edit");
    const auto loopToken = sessionApp.history().currentStateToken();
    check(sessionDispatcher.dispatch(commands::SetLoopEnabled{true}).status ==
              commands::CommandStatus::accepted,
          "loop enabled is a session command");
    sessionApp.synchroniseTransport();
    check(sessionApp.loopEnabled() &&
          sessionApp.history().currentStateToken() == loopToken,
          "applied loop state is observable without dirtying history");
    const auto readModelA = sessionApp.timelineSnapshot().loop;
    sessionAudio.rejectNextTemporalCommit = true;
    const auto rejectedLoopB = sessionDispatcher.dispatch(
        commands::SetLoopRangeMusical{{6 * musical::ppq},
                                      {14 * musical::ppq}});
    const auto readModelAfterFailure = sessionApp.timelineSnapshot().loop;
    check(rejectedLoopB.status == commands::CommandStatus::rejected &&
              sessionApp.project().loopRange() == readModelA.documentRange &&
              readModelAfterFailure == readModelA &&
              sessionApp.loopEnabled(),
          "failed loop candidate preserves document, prepared view, revision and enabled policy atomically");
    check(sessionDispatcher.dispatch(commands::SetMetronomeEnabled{true}).status ==
              commands::CommandStatus::accepted &&
          sessionDispatcher.dispatch(commands::SetMetronomeLevel{{-18.0F}}).status ==
              commands::CommandStatus::accepted,
          "metronome session controls dispatch");
    sessionApp.synchroniseTransport();
    check(sessionApp.metronomeEnabled() &&
          sessionApp.metronomeLevel().value == -18.0F &&
          sessionApp.history().currentStateToken() == loopToken,
          "metronome applied state is observable and non-documentary");
    const auto metronomeA = sessionApp.metronomeReadModel();
    const auto documentA = sessionApp.project().musicalTime();
    const auto preparedA = sessionAudio.temporal->documentMap;
    const auto revisionA = sessionAudio.temporal->revision;
    const auto playbackA = sessionAudio.transportSnapshot().playback;
    const auto runUntilStopA = sessionAudio.runUntilStop;
    sessionAudio.rejectNextTemporalCommit = true;
    const auto rejectedTempoB = sessionDispatcher.dispatch(
        commands::SetTempo{{1}, {123.0}});
    sessionApp.synchroniseTransport();
    check(rejectedTempoB.status == commands::CommandStatus::rejected &&
              sessionApp.project().musicalTime() == documentA &&
              sessionAudio.temporal->documentMap == preparedA &&
              sessionAudio.temporal->revision == revisionA &&
              sessionAudio.transportSnapshot().temporalRevision == revisionA &&
              sessionApp.metronomeReadModel() == metronomeA &&
              sessionApp.timelineSnapshot().metronome == metronomeA &&
              metronomeA.temporalRevision == revisionA &&
              sessionApp.metronomeEnabled() == metronomeA.enabled &&
              sessionApp.metronomeLevel() == metronomeA.level &&
              sessionAudio.transportSnapshot().playback == playbackA &&
              sessionAudio.runUntilStop == runUntilStopA,
          "failed temporal commit preserves semantic context, revision, metronome read model and playback policy");
    check(sessionDispatcher.dispatch(commands::Undo{}).status ==
              commands::CommandStatus::accepted &&
          !sessionApp.project().loopRange().has_value(),
          "Undo removes the loop range");

    FakeAudioEngine capacityAudio;
    application::DawApplication capacityApp{capacityAudio,
                                             timeline::SampleRate{48000}};
    commands::CommandDispatcher capacityDispatcher{capacityApp};
    check(capacityDispatcher.dispatch(commands::SetTempo{{1},{123.432}}).status ==
              commands::CommandStatus::accepted &&
          capacityDispatcher.dispatch(commands::AddTempoChange{{1000},{96.1425}}).status ==
              commands::CommandStatus::accepted,
          "representable exact tempo segments commit");
    const auto beforeCapacityFailure=capacityApp.project().musicalTime();
    const auto temporalRevisionBefore=
        capacityAudio.transportSnapshot().temporalRevision;
    const auto capacityFailure=capacityDispatcher.dispatch(
        commands::AddTempoChange{{2000},{105.55}});
    check(capacityFailure.status==commands::CommandStatus::rejected &&
          capacityApp.project().musicalTime()==beforeCapacityFailure &&
          capacityAudio.transportSnapshot().temporalRevision==temporalRevisionBefore,
          "uncertifiable exact anchor rejects without changing model or RT context");

    std::cout << "All command-flow tests passed\n";
    return EXIT_SUCCESS;
}
