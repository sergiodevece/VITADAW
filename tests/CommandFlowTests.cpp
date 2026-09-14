#include "vitadaw/application/DawApplication.h"
#include "vitadaw/commands/CommandDispatcher.h"

#include <algorithm>
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
            vitadaw::audio::ProcessingPlanSpecification candidate)
            : specification(std::move(candidate)) {}

        vitadaw::audio::ProcessingPlanSpecification specification;
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
        vitadaw::timeline::SampleRate projectSampleRate,
        vitadaw::mixer::PreparedTrackMixState trackMix) override {
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

    vitadaw::audio::StructuralPlanPreparationResult prepareProcessingPlan(
        const vitadaw::audio::ProcessingPlanSpecification& specification) override {
        ++structuralPrepareRequests;
        if (rejectNextStructuralPreparation) {
            rejectNextStructuralPreparation = false;
            return {nullptr, "Injected routing preparation failure"};
        }
        const auto validation = vitadaw::audio::prepareProcessingPlan(
            specification, {});
        if (!validation.success()) {
            return {nullptr, validation.errorMessage};
        }
        return {std::make_unique<PreparedPlan>(specification), {}};
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
        liveSpecification = std::move(candidate->specification);
        snapshot.playing = false;
        snapshot.position = {0};
        modelCommit.execute();
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

    vitadaw::mixer::MeterSnapshot meterSnapshot() const noexcept override {
        return {};
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
    vitadaw::audio::ProcessingPlanSpecification liveSpecification;
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
              app.transport().position.value == 0,
          "Stop should rewind the master clock");
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
