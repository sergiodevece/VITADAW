#include "vitadaw/application/DawApplication.h"
#include "vitadaw/audio/RealtimeAudioEngine.h"
#include "vitadaw/commands/CommandDispatcher.h"
#include "vitadaw/persistence/ProjectPersistence.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace {
using namespace vitadaw;

thread_local bool realtimeRegion{};
std::atomic<std::size_t> realtimeAllocations{};
std::atomic<std::size_t> realtimeDestructions{};

void check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

class MemoryFiles final : public platform::files::IProjectFileIO {
public:
    std::map<std::filesystem::path, std::string> content;

    platform::files::ReadResult read(const std::filesystem::path& path,
                                     std::size_t limit) override {
        const auto found = content.find(path);
        if (found == content.end())
            return {{persistence::PersistenceCode::fileNotFound,
                     persistence::PersistencePhase::read}, {}};
        if (found->second.size() > limit)
            return {{persistence::PersistenceCode::fileTooLarge,
                     persistence::PersistencePhase::read}, {}};
        const auto bytes = std::as_bytes(
            std::span{found->second.data(), found->second.size()});
        return {{}, {bytes.begin(), bytes.end()}};
    }

    persistence::PersistenceResult replace(
        const std::filesystem::path& path, std::string_view bytes) override {
        content[path] = bytes;
        return {};
    }
};

// Hardware-free application adapter. It owns synthetic decoded PCM but delegates
// plan compilation, command queues, clock, render and snapshots to production code.
class HardeningEngine final : public audio::IAudioEngineControl {
public:
    struct Pcm {
        std::vector<float> left;
        ~Pcm() { if (realtimeRegion) ++realtimeDestructions; }
    };
    struct File final : audio::PreparedAudioFile {
        std::shared_ptr<Pcm> pcm;
        File(timeline::SampleRate rate, std::size_t frames, float seed)
            : PreparedAudioFile({rate, 1, {frames},
                  {static_cast<double>(frames) / rate.hertz()}}),
              pcm(std::make_shared<Pcm>()) {
            pcm->left.resize(frames);
            for (std::size_t i = 0; i < frames; ++i)
                pcm->left[i] = seed + static_cast<float>(i % 97U) * 0.0001F;
        }
    };
    struct Owner { media::SourceId id; std::shared_ptr<Pcm> pcm; };
    struct Plan final : audio::PreparedProcessingPlanChange {
        std::vector<Owner> owners;
        std::unique_ptr<audio::PreparedProcessingBundle> bundle;
        ~Plan() override { if (realtimeRegion) ++realtimeDestructions; }
    };

    explicit HardeningEngine(MemoryFiles& files) : files_(files) {
        rt_.configure(audio::PreparedProjectView{});
        rt_.deviceInitialising();
        rt_.processBlock({nullptr, 0, 0}, deviceRate_);
    }
    ~HardeningEngine() override {
        rt_.deviceUnavailable();
        rt_.configure(audio::PreparedProjectView{});
        temporal_.reset();
        active_.reset();
    }

    audio::AudioFilePreparationResult prepareWav(
        const std::filesystem::path& path) override {
        const auto read = files_.read(path, 1024);
        if (!read.result.success()) return {nullptr, {}, read.result};
        const auto sourceRate = path.string().find("44100") != std::string::npos
            ? timeline::SampleRate{44100.0} : timeline::SampleRate{48000.0};
        const auto frames = static_cast<std::size_t>(sourceRate.hertz() * 4.0);
        const auto seed = read.bytes.empty() || read.bytes.front() == std::byte{'A'}
            ? 0.1F : 0.2F;
        auto file = std::make_unique<File>(sourceRate, frames, seed);
        file->media = {path, {}, platform::files::fingerprint(read.bytes)};
        return {std::move(file), {}};
    }

    audio::StructuralPlanPreparationResult prepareProcessingPlan(
        const audio::ProcessingPlanSpecification& specification) override {
        return build(specification,
                     active_ ? active_->owners : std::vector<Owner>{});
    }

    audio::StructuralPlanPreparationResult prepareProcessingPlanWithAudio(
        const audio::ProcessingPlanSpecification& specification,
        media::SourceId source, audio::PreparedAudioFilePtr audioFile) override {
        auto owners = active_ ? active_->owners : std::vector<Owner>{};
        auto* file = dynamic_cast<File*>(audioFile.get());
        if (file == nullptr) return {nullptr, "Unexpected prepared audio type"};
        owners.push_back({source, file->pcm});
        return build(specification, std::move(owners));
    }

    audio::StructuralPlanPreparationResult prepareProjectReplacement(
        const audio::ProcessingPlanSpecification& specification,
        std::vector<audio::PreparedSourceAudio> resources) override {
        std::vector<Owner> owners;
        owners.reserve(resources.size());
        for (auto& resource : resources) {
            auto* file = dynamic_cast<File*>(resource.audio.get());
            if (file == nullptr) return {nullptr, "Unexpected replacement audio"};
            owners.push_back({resource.id, file->pcm});
        }
        return build(specification, std::move(owners));
    }

    bool commitPreparedProcessingPlan(
        audio::PreparedProcessingPlanChangePtr prepared,
        audio::AudioFileCommitAction commit) noexcept override {
        return commitPlan(std::move(prepared), commit, false);
    }

    bool commitPreparedProcessingPlanPreservingTransport(
        audio::PreparedProcessingPlanChangePtr prepared,
        audio::AudioFileCommitAction commit) noexcept override {
        return commitPlan(std::move(prepared), commit, true);
    }

    audio::TemporalContextPreparationResult prepareTemporalContext(
        const musical::MusicalTimeMap& map,
        std::optional<musical::MusicalLoopRange> loop,
        timeline::SampleRate projectRate, std::uint64_t revision) override {
        return audio::prepareTemporalContext(map, loop, projectRate,
                                             deviceRate_, revision);
    }

    bool commitPreparedTemporalContext(
        std::unique_ptr<audio::PreparedTemporalContext> prepared,
        audio::AudioFileCommitAction commit) noexcept override {
        if (!prepared || !commit.isValid()) return false;
        const auto checkpoint = rt_.temporalCheckpoint();
        rt_.deviceStopped();
        temporal_.swap(prepared);
        rt_.configureTemporalContext(temporal_.get());
        rt_.restoreTemporalCheckpoint(checkpoint);
        commit.execute();
        restartConsumer();
        return true;
    }

    bool commitPreparedProjectAndTemporalContext(
        audio::PreparedProcessingPlanChangePtr project,
        std::unique_ptr<audio::PreparedTemporalContext> temporal,
        audio::AudioFileCommitAction commit) noexcept override {
        auto* raw = dynamic_cast<Plan*>(project.get());
        if (raw == nullptr || !raw->bundle || !temporal || !commit.isValid())
            return false;
        rt_.deviceStopped();
        std::unique_ptr<Plan> candidate{static_cast<Plan*>(project.release())};
        temporal_.swap(temporal);
        rt_.configure(candidate->bundle->plan, candidate->bundle->runtime);
        rt_.configureTemporalContext(temporal_.get());
        rt_.resetTemporalSessionState();
        active_.swap(candidate);
        commit.execute();
        restartConsumer();
        return true;
    }

    bool tryUpdateTrackMix(tracks::TrackId id, mixer::PreparedTrackMixState mix,
                           audio::PreparedAudibilityState audibility) noexcept override {
        return rt_.tryUpdateTrackMix(id, mix, audibility);
    }
    bool tryUpdateBusMix(routing::BusId id, mixer::PreparedBusMixState mix,
                         audio::PreparedAudibilityState audibility) noexcept override {
        return rt_.tryUpdateBusMix(id, mix, audibility);
    }
    bool tryUpdateSendMix(routing::SendId id,
                          mixer::PreparedSendMixState mix) noexcept override {
        return rt_.tryUpdateSendMix(id, mix);
    }
    bool tryUpdateMasterMix(mixer::PreparedMasterMixState mix) noexcept override {
        return rt_.tryUpdateMasterMix(mix);
    }
    audio::AudioControlRequestResult tryRequestPlay() noexcept override {
        return rt_.tryRequestPlay();
    }
    audio::AudioControlRequestResult tryRequestPause() noexcept override {
        return rt_.tryRequestPause();
    }
    audio::AudioControlRequestResult tryRequestStop() noexcept override {
        return rt_.tryRequestStop();
    }
    audio::AudioControlRequestResult tryRequestSeek(
        timeline::ProjectFramePosition position) noexcept override {
        return rt_.tryRequestSeek(position);
    }
    audio::AudioControlRequestResult trySetLoopEnabled(bool enabled) noexcept override {
        return rt_.trySetLoopEnabled(enabled);
    }
    audio::AudioControlRequestResult trySetMetronomeEnabled(bool enabled) noexcept override {
        return rt_.trySetMetronomeEnabled(enabled);
    }
    audio::AudioControlRequestResult trySetMetronomeLevel(
        audio::MetronomeLevelDb level) noexcept override {
        return rt_.trySetMetronomeLevel(level);
    }
    audio::RealtimeTransportSnapshot transportSnapshot() const noexcept override {
        return rt_.transportSnapshot();
    }
    mixer::MeterSnapshot meterSnapshot() const noexcept override {
        return rt_.meterSnapshot();
    }

    std::vector<float> process(std::size_t frames) {
        std::vector<float> left(frames), right(frames);
        std::array<float*, 2> channels{left.data(), right.data()};
        const auto allocationsBefore = realtimeAllocations.load();
        const auto destructionsBefore = realtimeDestructions.load();
        realtimeRegion = true;
        rt_.processBlock({channels.data(), channels.size(), frames}, deviceRate_);
        realtimeRegion = false;
        check(realtimeAllocations.load() == allocationsBefore,
              "processBlock must not allocate");
        check(realtimeDestructions.load() == destructionsBefore,
              "processBlock must not destroy owned resources");
        return left;
    }

    audio::RealtimeAudioEngine& realtimeEngine() noexcept { return rt_; }

private:
    audio::StructuralPlanPreparationResult build(
        const audio::ProcessingPlanSpecification& specification,
        std::vector<Owner> owners) {
        auto plan = std::make_unique<Plan>();
        plan->owners = std::move(owners);
        std::vector<audio::PreparedSourceView> views;
        views.reserve(plan->owners.size());
        for (const auto& owner : plan->owners) {
            const auto found = std::find_if(
                specification.sources.begin(), specification.sources.end(),
                [&](const auto& source) { return source.id == owner.id; });
            if (found == specification.sources.end()) continue;
            views.push_back({owner.id, {{owner.pcm->left.data(), nullptr}}, 1,
                {owner.pcm->left.size()}, found->sampleRate,
                media::AudioChannelLayout::mono});
        }
        auto next = specification;
        next.processingSampleRate = deviceRate_;
        auto prepared = audio::prepareProcessingPlanFromSources(
            next, views, 128);
        if (!prepared.success()) return {nullptr, std::move(prepared.errorMessage)};
        plan->bundle = std::move(prepared.prepared);
        return {std::move(plan), {}};
    }

    bool commitPlan(audio::PreparedProcessingPlanChangePtr prepared,
                    audio::AudioFileCommitAction commit,
                    bool preserveTransport) noexcept {
        auto* raw = dynamic_cast<Plan*>(prepared.get());
        if (raw == nullptr || !raw->bundle || !commit.isValid()) return false;
        const auto checkpoint = rt_.temporalCheckpoint();
        rt_.deviceStopped();
        std::unique_ptr<Plan> candidate{static_cast<Plan*>(prepared.release())};
        rt_.configure(candidate->bundle->plan, candidate->bundle->runtime);
        if (preserveTransport) rt_.restoreTemporalCheckpoint(checkpoint);
        active_.swap(candidate);
        commit.execute();
        restartConsumer();
        return true;
    }

    void restartConsumer() noexcept {
        rt_.deviceInitialisingPreservingTransport();
        rt_.processBlock({nullptr, 0, 0}, deviceRate_);
    }

    MemoryFiles& files_;
    timeline::SampleRate deviceRate_{48000.0};
    audio::RealtimeAudioEngine rt_;
    std::unique_ptr<Plan> active_;
    std::unique_ptr<audio::PreparedTemporalContext> temporal_;
};

void requireAccepted(commands::CommandDispatcher& dispatcher,
                     commands::Command command, const char* message) {
    const auto result = dispatcher.dispatch(command);
    if (result.status != commands::CommandStatus::accepted)
        std::cerr << "Command failure: " << result.message << '\n';
    check(result.status == commands::CommandStatus::accepted, message);
}

std::string documentaryState(const project::ProjectState& project) {
    auto serialized = persistence::serializeProject(
        project, "/session/state.vitadaw");
    check(serialized.result.success(), "test project serializes canonically");
    return serialized.bytes;
}

struct Fixture {
    MemoryFiles files;
    HardeningEngine engine;
    application::DawApplication application;
    commands::CommandDispatcher dispatcher;

    Fixture()
        : engine(files), application(engine, timeline::SampleRate{48000}, files),
          dispatcher(application) {
        files.content["/session/a-48000.wav"] = "A";
        files.content["/session/b-44100.wav"] = "B";
        requireAccepted(dispatcher,
            commands::AddAudioTrack{"Audio 1", media::AudioChannelLayout::mono},
            "first track setup");
        requireAccepted(dispatcher,
            commands::AddAudioTrack{"Audio 2", media::AudioChannelLayout::mono},
            "second track setup");
        requireAccepted(dispatcher,
            commands::AddAudioTrack{"Empty", media::AudioChannelLayout::mono},
            "empty track setup");
        requireAccepted(dispatcher,
            commands::ImportAudioToTrack{"/session/a-48000.wav", {1}, {0}},
            "first synthetic source import");
    }

    std::vector<float> process(std::size_t frames = 64) {
        auto output = engine.process(frames);
        application.synchroniseTransport();
        return output;
    }
};

void transportCommandMatrix() {
    Fixture f;
    const auto initialHistory = f.application.history().cursor();

    requireAccepted(f.dispatcher, commands::Play{}, "Play accepted");
    requireAccepted(f.dispatcher, commands::Play{}, "repeated Play is idempotent");
    requireAccepted(f.dispatcher, commands::Pause{}, "Pause queued after Play");
    requireAccepted(f.dispatcher, commands::SeekToProjectFrame{{24000}},
                    "Seek queued while application is Paused");
    requireAccepted(f.dispatcher, commands::Play{}, "Play queued after Paused Seek");
    const auto resumed = f.process();
    check(f.application.transport().playback == transport::PlaybackState::playing &&
              f.application.transport().position.value == 24064 &&
              std::any_of(resumed.begin(), resumed.end(), [](float value) {
                  return value != 0.0F;
              }),
          "Play/Pause/Seek/Play FIFO must resume at the requested frame");

    const auto beforeRejectedSeek = f.application.transport();
    const auto rejectedSeek = f.dispatcher.dispatch(
        commands::SeekToProjectFrame{{12000}});
    check(rejectedSeek.error == commands::CommandError::seekRejectedWhilePlaying &&
              f.application.transport().position == beforeRejectedSeek.position,
          "Seek during Playing must remain rejected without mirror mutation");

    requireAccepted(f.dispatcher, commands::Pause{}, "Pause active transport");
    f.process();
    const auto pausedAt = f.application.transport().position;
    const auto silence = f.process(257);
    check(f.application.transport().playback == transport::PlaybackState::paused &&
              f.application.transport().position == pausedAt &&
              std::all_of(silence.begin(), silence.end(), [](float value) {
                  return value == 0.0F;
              }),
          "Pause must remain silent and sample-exact across callbacks");

    for (std::int64_t position = 1; position <= 7; ++position)
        requireAccepted(f.dispatcher, commands::SeekToProjectFrame{{position}},
                        "bounded Paused Seek fills FIFO");
    const auto positionBeforeFull = f.application.transport().position;
    const auto full = f.dispatcher.dispatch(commands::SeekToProjectFrame{{8}});
    check(full.status == commands::CommandStatus::rejected &&
              full.error == commands::CommandError::transportUnavailable &&
              f.application.transport().position == positionBeforeFull,
          "full transport FIFO must reject without changing the application mirror");
    f.process();
    check(f.application.transport().playback == transport::PlaybackState::paused &&
              f.application.transport().position.value == 7,
          "all accepted Paused seeks must resolve FIFO to the final target");

    requireAccepted(f.dispatcher, commands::Play{}, "resume after seek burst");
    f.process(32);
    requireAccepted(f.dispatcher, commands::Stop{}, "first Stop accepted");
    f.process();
    const auto stoppedAt = f.application.transport().position;
    check(stoppedAt.value > 0 &&
              f.application.transport().playback == transport::PlaybackState::stopped,
          "first Stop must preserve the master position");
    requireAccepted(f.dispatcher, commands::Stop{}, "second Stop accepted");
    f.process();
    check(f.application.transport().position.value == 0,
          "second Stop must rewind deterministically");
    requireAccepted(f.dispatcher, commands::Stop{}, "third Stop is idempotent");
    f.process();
    check(f.application.transport().position.value == 0 &&
              f.application.history().cursor() == initialHistory,
          "transport sequences must not affect history or dirty state");

    const auto play = f.engine.realtimeEngine().tryRequestPlay();
    const auto pause = f.engine.realtimeEngine().tryRequestPause();
    check(play.accepted && pause.accepted,
          "lifecycle cancellation fixture queues transport commands");
    f.engine.realtimeEngine().deviceStopped();
    const auto cancelled = f.engine.transportSnapshot();
    check(cancelled.playback == transport::PlaybackState::stopped &&
              cancelled.position.value == 0 &&
              cancelled.lastProcessedCommandSequence >= pause.sequence,
          "lifecycle close resolves all accepted pending commands as cancelled");
    f.application.synchroniseTransport();
    check(f.application.transport().playback == transport::PlaybackState::stopped,
          "application mirror converges after lifecycle command cancellation");
}

std::vector<float> renderLoop(
    const musical::MusicalTimeMap& map, musical::MusicalLoopRange loop,
    std::span<const std::size_t> partitions,
    audio::RealtimeTransportSnapshot* finalSnapshot = nullptr) {
    constexpr std::size_t sourceFrames = 600000;
    std::vector<float> samples(sourceFrames);
    for (std::size_t i = 0; i < samples.size(); ++i)
        samples[i] = static_cast<float>((i % 251U) + 1U) / 251.0F;
    audio::PreparedTrackView track{{1}, {{samples.data(), nullptr}}, 1,
        {samples.size()}, timeline::SampleRate{48000}, {0}, {sourceFrames}, {0}, {}};
    audio::RealtimeAudioEngine engine;
    engine.configure({timeline::SampleRate{48000}, {sourceFrames},
                      std::span{&track, 1}});
    auto temporal = audio::prepareTemporalContext(
        map, loop, timeline::SampleRate{48000}, timeline::SampleRate{44100}, 1);
    check(temporal.success(), "existing minimum-valid loop policy must prepare");
    engine.configureTemporalContext(temporal.prepared.get());
    engine.deviceInitialising();
    engine.processBlock({nullptr, 0, 0}, timeline::SampleRate{44100});
    check(engine.trySetLoopEnabled(true).accepted, "loop enable accepted");
    engine.processBlock({nullptr, 0, 0}, timeline::SampleRate{44100});
    check(engine.tryRequestPlay().accepted, "loop Play accepted");

    std::size_t total{};
    for (const auto count : partitions) total += count;
    std::vector<float> left(total), right(total);
    std::size_t offset{};
    for (const auto count : partitions) {
        std::array<float*, 2> channels{left.data() + offset,
                                       right.data() + offset};
        const auto allocationsBefore = realtimeAllocations.load();
        realtimeRegion = true;
        engine.processBlock({channels.data(), channels.size(), count},
                            timeline::SampleRate{44100});
        realtimeRegion = false;
        check(realtimeAllocations.load() == allocationsBefore,
              "loop processBlock must remain allocation-free");
        offset += count;
    }
    if (finalSnapshot != nullptr) *finalSnapshot = engine.transportSnapshot();
    engine.deviceUnavailable();
    engine.configure(audio::PreparedProjectView{});
    return left;
}

void loopHardening() {
    musical::MusicalTimeMap map;
    const musical::MusicalLoopRange minimumValid{{0}, {1024}};
    const std::array<std::size_t, 1> oneCallback{4096};
    const std::array<std::size_t, 16> partitioned{
        64, 128, 256, 512, 1, 63, 65, 127,
        129, 255, 257, 511, 513, 128, 256, 831};
    check(renderLoop(map, minimumValid, oneCallback) ==
              renderLoop(map, minimumValid, partitioned),
          "minimum-valid loop must be callback-partition invariant");

    map.tempo.events.push_back({{2}, {2 * musical::ppq}, {60.0}});
    map.tempo.nextId = {3};
    map.signatures.events.push_back({{2}, {1}, {7, 8}});
    map.signatures.nextId = {3};
    const musical::MusicalLoopRange crossing{
        {musical::ppq}, {8 * musical::ppq}};
    const std::array<std::size_t, 1> longBlock{150000};
    const std::array<std::size_t, 7> mixedBlocks{
        64, 128, 256, 512, 1024, 32768, 115248};
    audio::RealtimeTransportSnapshot snapshot;
    const auto contiguous = renderLoop(map, crossing, longBlock);
    const auto divided = renderLoop(map, crossing, mixedBlocks, &snapshot);
    check(contiguous == divided &&
              snapshot.playback == transport::PlaybackState::playing,
          "loop crossing tempo and meter changes must preserve audio and clock partition invariance");
}

audio::ProcessingPlanSpecification makeSpecification(
    const project::ProjectState& project) {
    audio::ProcessingPlanSpecification result;
    result.projectSampleRate = project.sampleRate();
    result.processingSampleRate = project.sampleRate();
    for (const auto& source : project.sources())
        result.sources.push_back({source.id, source.frameCount,
                                  source.sampleRate, source.layout});
    for (const auto& track : project.tracks()) {
        const auto* route = project.routing().findTrackRoute(track.id);
        result.tracks.push_back({track.id, mixer::prepare(track.mix),
            route->destination, track.inserts, track.layout, track.clips});
    }
    result.masterMix = mixer::prepare(project.masterMix());
    return result;
}

std::vector<float> renderProject(project::ProjectState project,
                                 std::span<const float> pcm,
                                 std::size_t frames) {
    const auto source = project.sources().front();
    const audio::PreparedSourceView view{source.id, {{pcm.data(), nullptr}}, 1,
        {pcm.size()}, source.sampleRate, media::AudioChannelLayout::mono};
    auto prepared = audio::prepareProcessingPlanFromSources(
        makeSpecification(project), std::span{&view, 1}, 128);
    if (!prepared.success())
        std::cerr << "Plan preparation failure: " << prepared.errorMessage << '\n';
    check(prepared.success(), "edited project plan prepares");
    audio::RealtimeAudioEngine engine;
    engine.configure(prepared.prepared->plan, prepared.prepared->runtime);
    engine.deviceInitialising();
    engine.processBlock({nullptr, 0, 0}, project.sampleRate());
    check(engine.tryRequestPlay().accepted, "edited project Play accepted");
    std::vector<float> left(frames), right(frames);
    std::array<float*, 2> channels{left.data(), right.data()};
    const auto allocationsBefore = realtimeAllocations.load();
    realtimeRegion = true;
    engine.processBlock({channels.data(), channels.size(), frames},
                        project.sampleRate());
    realtimeRegion = false;
    check(realtimeAllocations.load() == allocationsBefore,
          "edited project render must remain allocation-free");
    engine.deviceUnavailable();
    engine.configure(audio::PreparedProjectView{});
    return left;
}

void splitAndBlockBoundaries() {
    constexpr std::array boundaries{63, 64, 65, 127, 128, 129,
                                    255, 256, 257, 511, 512, 513};
    for (const auto boundary : boundaries) {
        project::ProjectState original{timeline::SampleRate{48000}};
        const auto track = original.addAudioTrack("Boundary");
        const auto imported = original.importAudioToTrack(
            track, {"/boundary.wav", {}, {}}, {1024},
            timeline::SampleRate{44100}, media::AudioChannelLayout::mono, {0});
        std::vector<float> pcm(1024);
        for (std::size_t i = 0; i < pcm.size(); ++i)
            pcm[i] = static_cast<float>(i + 1U) / 1024.0F;
        auto split = original;
        check(split.splitClip(imported.clip, {boundary}).succeeded(),
              "interior split around audio block boundary succeeds");
        const auto frames = static_cast<std::size_t>(original.duration().value + 2);
        check(renderProject(original, pcm, frames) ==
                  renderProject(split, pcm, frames),
              "split at block boundary must preserve bit-identical render");
    }

    project::ProjectState edge{timeline::SampleRate{48000}};
    const auto track = edge.addAudioTrack("Edge");
    const auto imported = edge.importAudioToTrack(
        track, {"/edge.wav", {}, {}}, {3}, timeline::SampleRate{44100},
        media::AudioChannelLayout::mono, {100});
    const auto before = *edge.findClip(imported.clip);
    const auto beforeClipCount = edge.findTrack(track)->clips.size();
    check(!edge.splitClip(imported.clip, {100}).succeeded() &&
              !edge.splitClip(imported.clip,
                  {edge.projectContentDuration().value}).succeeded() &&
              *edge.findClip(imported.clip) == before &&
              edge.findTrack(track)->clips.size() == beforeClipCount,
          "split at exact clip endpoints must fail without partial mutation");
    auto lastLegalEdge = edge;
    const auto firstLegal = edge.splitClip(imported.clip, {101});
    check(firstLegal.succeeded(),
          "three-source-frame clip accepts its first legal project-frame split");
    const auto lastLegalPosition =
        lastLegalEdge.projectContentDuration().value - 1;
    check(lastLegalEdge.splitClip(imported.clip, {lastLegalPosition}).succeeded(),
          "three-source-frame clip accepts its last legal project-frame split");
}

void editingHistoryAndActivePolicy() {
    Fixture f;
    const auto original = *f.application.project().findClip({1});
    const auto baseDocument = documentaryState(f.application.project());
    const auto baseData = f.application.project().documentData();

    requireAccepted(f.dispatcher, commands::Play{}, "active edit policy Play");
    f.process();
    const auto playingDocument = documentaryState(f.application.project());
    const auto playingToken = f.application.history().currentStateToken();
    for (const commands::Command command : {
             commands::Command{commands::MoveClip{{1}, {200}}},
             commands::Command{commands::DuplicateClip{{1}, {500}}},
             commands::Command{commands::SplitClip{{1}, {1000}}},
             commands::Command{commands::TrimClipLeft{{1}, {100}}},
             commands::Command{commands::TrimClipRight{{1}, {1000}}},
             commands::Command{commands::DeleteClip{{1}}},
             commands::Command{commands::Undo{}},
             commands::Command{commands::Redo{}}}) {
        const auto result = f.dispatcher.dispatch(command);
        check(result.status == commands::CommandStatus::rejected &&
                  result.error == commands::CommandError::transportMustBeStopped &&
                  documentaryState(f.application.project()) == playingDocument &&
                  f.application.history().currentStateToken() == playingToken,
              "every timeline/history edit during Playing is atomic rejection");
    }

    requireAccepted(f.dispatcher, commands::Pause{}, "pause before paused edits");
    f.process();
    const auto pausedPosition = f.application.transport().position;
    requireAccepted(f.dispatcher, commands::DuplicateClip{{1}, {50000}},
                    "same-track Duplicate remains permitted while Paused");
    check(f.application.transport().playback == transport::PlaybackState::paused &&
              f.application.transport().position == pausedPosition,
          "Paused structural commit preserves transport checkpoint");
    const auto crossTrack = f.dispatcher.dispatch(
        commands::MoveClip{{1}, tracks::TrackId{2}, {200}});
    check(crossTrack.error == commands::CommandError::transportMustBeStopped,
          "cross-track Move remains rejected while Paused");
    requireAccepted(f.dispatcher, commands::Undo{},
                    "Undo remains permitted while Paused");
    requireAccepted(f.dispatcher, commands::Redo{},
                    "Redo remains permitted while Paused");

    requireAccepted(f.dispatcher, commands::Stop{}, "stop paused fixture");
    f.process();
    requireAccepted(f.dispatcher,
        commands::MoveClip{{1}, tracks::TrackId{2}, {200}},
        "cross-track Move succeeds while Stopped");
    requireAccepted(f.dispatcher, commands::SeekToProjectFrame{{1000}},
                    "playhead positioned for exact split");
    f.process();
    requireAccepted(f.dispatcher,
        commands::SplitClip{{1}, f.application.transport().position},
                    "Split around playhead succeeds");
    requireAccepted(f.dispatcher, commands::DuplicateClip{{1}, {60000}},
                    "consecutive Duplicate succeeds");
    const auto duplicateId = f.application.project().tracks()[1].clips.back().id;
    requireAccepted(f.dispatcher, commands::DeleteClip{duplicateId},
                    "Delete after Duplicate succeeds");

    const auto finalDocument = documentaryState(f.application.project());
    const auto historyDepth = f.application.history().cursor();
    check(historyDepth >= 5, "combined edit chain creates a substantial history");
    for (std::size_t i = 0; i < historyDepth; ++i)
        requireAccepted(f.dispatcher, commands::Undo{},
                        "complete edit-chain Undo succeeds");
    auto undoneData = f.application.project().documentData();
    check(undoneData.nextClipId.value >= baseData.nextClipId.value,
          "Undo never rewinds the monotonic ClipId generator");
    undoneData.nextClipId = baseData.nextClipId;
    auto normalisedUndo = project::ProjectState::fromDocumentData(
        std::move(undoneData));
    check(normalisedUndo && documentaryState(*normalisedUndo) == baseDocument,
          "complete Undo restores exact pre-edit content and entity IDs");
    for (std::size_t i = 0; i < historyDepth; ++i)
        requireAccepted(f.dispatcher, commands::Redo{},
                        "complete edit-chain Redo succeeds");
    check(documentaryState(f.application.project()) == finalDocument,
          "complete Redo restores the exact final project");
    for (int cycle = 0; cycle < 8; ++cycle) {
        requireAccepted(f.dispatcher, commands::Undo{}, "long Undo/Redo cycle Undo");
        requireAccepted(f.dispatcher, commands::Redo{}, "long Undo/Redo cycle Redo");
        requireAccepted(f.dispatcher, commands::Undo{}, "long Undo/Redo cycle second Undo");
        requireAccepted(f.dispatcher, commands::Redo{}, "long Undo/Redo cycle restore");
        check(documentaryState(f.application.project()) == finalDocument,
              "Undo/Redo/Undo/Redo must restore exact documentary state");
    }
    check(f.application.project().findClip(original.id) != nullptr &&
              f.application.project().findClip(original.id)->source == original.source,
          "history stress preserves original ClipId and SourceId");
}

void saveLoadEditedState() {
    Fixture f;
    requireAccepted(f.dispatcher,
        commands::ImportAudioToTrack{"/session/b-44100.wav", {2}, {24000}},
        "second-rate source import");
    requireAccepted(f.dispatcher, commands::SplitClip{{1}, {48000}},
                    "persistence fixture split");
    requireAccepted(f.dispatcher, commands::DuplicateClip{{1}, {120000}},
                    "persistence fixture duplicate");
    requireAccepted(f.dispatcher,
        commands::MoveClip{{1}, tracks::TrackId{2}, {1000}},
        "persistence fixture cross-track move");
    requireAccepted(f.dispatcher, commands::DeleteClip{{2}},
                    "persistence fixture delete");
    requireAccepted(f.dispatcher,
        commands::AddTempoChange{{2 * musical::ppq}, {90.0}},
        "persistence fixture tempo");
    requireAccepted(f.dispatcher,
        commands::AddTimeSignatureChange{{2}, {7, 8}},
        "persistence fixture meter");
    requireAccepted(f.dispatcher,
        commands::SetLoopRangeMusical{{musical::ppq}, {5 * musical::ppq}},
        "persistence fixture loop");

    const auto expected = documentaryState(f.application.project());
    requireAccepted(f.dispatcher,
        commands::SaveProjectAs{"/session/hardening.vitadaw"},
        "edited project Save As");
    const auto savedBytes = f.files.content.at("/session/hardening.vitadaw");
    check(!f.application.session().dirty(), "Save marks edited session clean");

    requireAccepted(f.dispatcher, commands::MoveClip{{1}, {3000}},
                    "post-save divergence");
    requireAccepted(f.dispatcher,
        commands::LoadProject{"/session/hardening.vitadaw", true},
        "edited project Load");
    check(documentaryState(f.application.project()) == expected &&
              !f.application.session().dirty() &&
              !f.application.canUndo() && !f.application.canRedo(),
          "Save/Load restores exact project and establishes clean history barrier");
    requireAccepted(f.dispatcher, commands::SaveProject{},
                    "loaded project deterministic Save");
    check(f.files.content.at("/session/hardening.vitadaw") == savedBytes,
          "edited project serialization remains byte deterministic after Load");

    requireAccepted(f.dispatcher, commands::SeekToProjectFrame{{1000}},
                    "loaded project Seek");
    f.process();
    requireAccepted(f.dispatcher, commands::Play{}, "loaded project Play");
    const auto output = f.process(257);
    check(std::any_of(output.begin(), output.end(), [](float value) {
              return value != 0.0F;
          }),
          "loaded edited plan renders through production processBlock");
}

void scaleSanity() {
    project::ProjectState project{timeline::SampleRate{48000}};
    std::vector<tracks::TrackId> tracks;
    for (int i = 0; i < 64; ++i)
        tracks.push_back(project.addAudioTrack("Track " + std::to_string(i + 1)));
    const auto imported = project.importAudioToTrack(
        tracks.front(), {"/scale.wav", {}, {}}, {48000},
        timeline::SampleRate{48000}, media::AudioChannelLayout::mono, {0});
    // Every clip remains strictly positive. Some tracks are intentionally empty,
    // and project positions leave documentary gaps between clip groups.
    for (int i = 1; i < 1000; ++i) {
        const auto target = tracks[static_cast<std::size_t>(i % 56)];
        const auto start = static_cast<std::int64_t>(i * 53 + (i / 40) * 500);
        check(project.addClip(target, imported.source, {start}, {31.0},
                              {static_cast<double>(i % 1000)}).isValid(),
              "positive-duration scale clip added");
    }
    check(std::count_if(project.tracks().begin(), project.tracks().end(),
              [](const auto& track) { return track.clips.empty(); }) >= 8,
          "scale fixture includes empty tracks rather than empty clips");
    std::vector<float> pcm(48000, 0.25F);
    const audio::PreparedSourceView source{imported.source,
        {{pcm.data(), nullptr}}, 1, {pcm.size()}, timeline::SampleRate{48000},
        media::AudioChannelLayout::mono};
    auto prepared = audio::prepareProcessingPlanFromSources(
        makeSpecification(project), std::span{&source, 1}, 128);
    check(prepared.success() && prepared.prepared->plan.tracks.size() == 64 &&
              prepared.prepared->plan.clips.size() == 1000,
          "64-track/1000-positive-clip project prepares within existing limits");
    audio::RealtimeAudioEngine engine;
    engine.configure(prepared.prepared->plan, prepared.prepared->runtime);
    engine.deviceInitialising();
    engine.processBlock({nullptr, 0, 0}, timeline::SampleRate{48000});
    check(engine.tryRequestPlay().accepted, "scale project Play accepted");
    for (const auto count : {64U, 128U, 256U, 512U, 1024U, 1537U}) {
        std::vector<float> left(count), right(count);
        std::array<float*, 2> channels{left.data(), right.data()};
        const auto allocationsBefore = realtimeAllocations.load();
        realtimeRegion = true;
        engine.processBlock({channels.data(), channels.size(), count},
                            timeline::SampleRate{48000});
        realtimeRegion = false;
        check(realtimeAllocations.load() == allocationsBefore,
              "scale callback remains allocation-free at variable block sizes");
    }
    engine.deviceUnavailable();
    engine.configure(audio::PreparedProjectView{});
}
}

void* operator new(std::size_t size) {
    if (realtimeRegion) ++realtimeAllocations;
    if (auto* memory = std::malloc(size == 0 ? 1 : size)) return memory;
    throw std::bad_alloc{};
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* memory) noexcept {
    std::free(memory);
}
void operator delete(void* memory, std::size_t) noexcept {
    ::operator delete(memory);
}
void operator delete[](void* memory) noexcept {
    ::operator delete(memory);
}
void operator delete[](void* memory, std::size_t) noexcept {
    ::operator delete(memory);
}

int main() {
    transportCommandMatrix();
    loopHardening();
    splitAndBlockBoundaries();
    editingHistoryAndActivePolicy();
    saveLoadEditedState();
    scaleSanity();
    std::cout << "Editing and transport hardening tests passed\n";
}
