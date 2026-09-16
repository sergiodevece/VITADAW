#include "vitadaw/audio/RealtimeAudioEngine.h"
#include "vitadaw/timeline/Time.h"
#include "vitadaw/transport/TransportReducer.h"
#include "vitadaw/project/ProjectState.h"

#include <array>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <semaphore>
#include <string_view>
#include <thread>
#include <vector>

// Test-only access to existing callback/lifecycle stages. No production hook,
// waiting primitive or alternate DSP implementation is introduced.
namespace vitadaw::audio {
struct TransportTestAccess {
    // Split the existing setter at its post-enqueue tail, without a production
    // scheduling hook. The tail disappears once the auxiliary flags are removed.
    static void enableMetronomeAcrossClosure(RealtimeAudioEngine& engine,
        std::binary_semaphore& accepted, std::binary_semaphore& closed) {
        engine.refreshTransportProjection();
        if (!engine.enqueue(RealtimeAudioEngine::CommandType::setMetronomeEnabled,
                            {}, 1.0F).accepted) std::abort();
        accepted.release();
        closed.acquire();
        staleTail(engine);
    }
    template<class Engine> static void staleTail(Engine& engine) {
        if constexpr (requires { engine.requestedMetronomeEnabled_; })
            engine.requestedMetronomeEnabled_.store(true, std::memory_order_release);
    }
    static void refresh(RealtimeAudioEngine& engine) { engine.refreshTransportProjection(); }
    static AudioControlRequestResult enqueueValidatedPlay(RealtimeAudioEngine& engine) {
        return engine.enqueue(RealtimeAudioEngine::CommandType::play);
    }
    static void consumeWithoutPublishing(RealtimeAudioEngine& engine) {
        engine.consumeCommands();
    }
    static void publish(RealtimeAudioEngine& engine) { engine.publishTransport(); }
    static std::size_t pending(const RealtimeAudioEngine& engine) {
        return engine.pendingCommandCount_;
    }
    static std::size_t fifoWrite(const RealtimeAudioEngine& engine) {
        return engine.commandWriteIndex_.load();
    }
    static void closeBeforeClockPublication(RealtimeAudioEngine& engine) {
        const auto closure = engine.lifecycleGate_.close(DeviceProcessingState::stopped);
        engine.resolveCommandsThrough(closure.cancellationWatermark);
    }
    static void rejectedReservation(RealtimeAudioEngine& engine) {
        const auto claim = engine.lifecycleGate_.tryClaim();
        const auto ticket = engine.lifecycleGate_.reserveSequence(claim);
        const auto closure = engine.lifecycleGate_.close(DeviceProcessingState::error);
        if (engine.lifecycleGate_.tryAccept(claim) || closure.cancellationWatermark < ticket)
            std::abort();
        engine.resolveCommandsThrough(closure.cancellationWatermark);
        engine.publishTransport();
    }
};
}

namespace {
using namespace vitadaw;

void check(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

audio::PreparedTrackView mono(const std::vector<float>& samples,
                              double projectRate) {
    return {{1}, {{samples.data(), nullptr}}, 1,
            {static_cast<std::uint64_t>(samples.size())},
            timeline::SampleRate{projectRate}, {0},
            {static_cast<std::int64_t>(samples.size())}, {0}};
}

void process(audio::RealtimeAudioEngine& engine, std::size_t frames,
             double deviceRate) {
    std::vector<float> left(frames), right(frames);
    std::array<float*, 2> channels{left.data(), right.data()};
    engine.processBlock({channels.data(), channels.size(), frames},
                        timeline::SampleRate{deviceRate});
}

void makeOperational(audio::RealtimeAudioEngine& engine, double rate) {
    check(engine.prepareLegacyDeviceRate(timeline::SampleRate{rate}), "exact device context prepares outside RT");
    engine.deviceInitialising();
    process(engine, 0, rate);
    check(engine.deviceState() == audio::DeviceProcessingState::operational,
          "real processBlock confirms the consumer");
}

void playAvailabilityLifecycle() {
    audio::RealtimeAudioEngine engine;
    engine.configure({timeline::SampleRate{48000}, {}, {}});
    makeOperational(engine, 48000);
    std::binary_semaphore accepted{0}, closed{0};
    std::thread producer([&] {
        audio::TransportTestAccess::enableMetronomeAcrossClosure(engine, accepted, closed);
    });
    accepted.acquire();
    engine.deviceInitialising();
    closed.release();
    producer.join();
    process(engine, 0, 48000);
    check(!engine.transportSnapshot().metronomeEnabled, "cancelled metronome stays disabled in RT");
    const auto play = engine.tryRequestPlay();
    check(!play.accepted &&
          engine.projectedTransportSnapshot().playback == transport::PlaybackState::stopped,
          "cancelled setter tail cannot admit Play in the next generation");
    process(engine, 1, 48000);
    check(engine.transportSnapshot().playback == transport::PlaybackState::stopped,
          "unavailable Play leaves RT stopped");

    check(engine.trySetMetronomeEnabled(true).accepted && engine.tryRequestPlay().accepted,
          "accepted pending metronome legitimately enables Play");
    process(engine, 1, 48000);
    check(engine.transportSnapshot().playback == transport::PlaybackState::playing,
          "pending metronome and Play agree in RT");
}

void playGenerationValidation() {
    audio::RealtimeAudioEngine engine;
    engine.configure({timeline::SampleRate{48000}, {}, {}});
    makeOperational(engine, 48000);
    check(engine.trySetMetronomeEnabled(true).accepted, "availability before validation");
    audio::TransportTestAccess::refresh(engine);
    // Close and reopen after availability validation, before enqueue's claim.
    std::thread lifecycle([&] { engine.deviceInitialising(); process(engine, 0, 48000); });
    lifecycle.join();
    check(!audio::TransportTestAccess::enqueueValidatedPlay(engine).accepted,
          "a validation from an older generation cannot accept in the new one");
    check(!engine.tryRequestPlay().accepted, "reconciled generation has no Play availability");
}

void largeLocalAudio() {
    const auto length = timeline::maximumSupportedProjectFrame().value - 1;
    for (const std::uint64_t sourceOffset : {0U, 3U}) {
        const std::vector<float> signal(sourceOffset + 1, 0.8F);
        const auto rate = timeline::SampleRate{48000.0 / static_cast<double>(length)};
        audio::ProcessingPlanSpecification specification;
        specification.projectSampleRate = timeline::SampleRate{48000};
        specification.processingSampleRate = timeline::SampleRate{96000};
        specification.sources = {{{1}, {sourceOffset + 1}, rate, media::AudioChannelLayout::mono}};
        specification.tracks = {{{1}, {}, {}, {}, media::AudioChannelLayout::mono,
            {{{1}, {1}, {0}, {static_cast<double>(length)}, {static_cast<double>(sourceOffset)}}}}};
        const std::array<audio::PreparedSourceView, 1> sources{{{{1}, {{signal.data(), nullptr}},
            1, {sourceOffset + 1}, rate, media::AudioChannelLayout::mono}}};
        auto bundle = audio::prepareProcessingPlanFromSources(specification, sources);
        check(bundle.success(), "large local coordinate is admitted by real preparation");
        audio::RealtimeAudioEngine engine;
        engine.configure(bundle.prepared->plan, bundle.prepared->runtime);
        makeOperational(engine, 96000);
        check(engine.tryRequestSeek({length - 1}).accepted && engine.tryRequestPlay().accepted,
              "large local real render starts");
        std::array<float, 3> left{}, right{};
        std::array<float*, 2> channels{left.data(), right.data()};
        engine.processBlock({channels.data(), 2, 3}, timeline::SampleRate{96000});
        for (const auto index : {0, 1})
            check(std::abs(left[index] - 0.8F * audio::monoCentreCoefficient) < 1.0e-5F,
                  "large local render preserves both samples before the exclusive end");
        check(left[2] == 0 && engine.transportSnapshot().position.value == length &&
              engine.transportSnapshot().playback == transport::PlaybackState::stopped,
              "large local render stops exactly at the exclusive end");
        // Exercise the compatibility renderer with the same owner and coordinates.
        const std::array<audio::PreparedTrackView, 1> legacy{{{{1}, {{signal.data(), nullptr}},
            1, {sourceOffset + 1}, rate, {0}, {length}, {sourceOffset}}}};
        engine.configure({timeline::SampleRate{48000}, {length}, legacy});
        makeOperational(engine, 96000);
        check(engine.tryRequestSeek({length - 1}).accepted && engine.tryRequestPlay().accepted,
              "large local legacy render starts");
        engine.processBlock({channels.data(), 2, 3}, timeline::SampleRate{96000});
        check(left[0] > 0.5F && left[1] > 0.5F && left[2] == 0,
              "legacy and prepared render share compensated source bounds");
    }
}

void naturalEndBelowBoundary() {
    const std::vector<float> signal{0.8F};
    const std::array track{mono(signal, 48000)};
    audio::RealtimeAudioEngine engine;
    engine.configure({timeline::SampleRate{48000}, {1}, track});
    makeOperational(engine, 48000.0024);
    check(engine.tryRequestPlay().accepted, "n-epsilon Play accepted");
    std::array<float, 1> left{}, right{};
    std::array<float*, 2> channels{left.data(), right.data()};
    engine.processBlock({channels.data(), 2, 1}, timeline::SampleRate{48000.0024});
    check(left[0] > 0.5F && engine.transportSnapshot().playback == transport::PlaybackState::playing,
          "strictly before content end retains audio and Playing despite rounded locator");
    engine.processBlock({channels.data(), 2, 1}, timeline::SampleRate{48000.0024});
    check(left[0] > 0.5F && engine.transportSnapshot().playback == transport::PlaybackState::stopped,
          "last valid device sample plays before crossing natural end");
    engine.processBlock({channels.data(), 2, 1}, timeline::SampleRate{48000.0024});
    check(left[0] == 0 && engine.transportSnapshot().position.value == 1,
          "after crossing natural end output is silent at the content duration");
}

void exactAuditReproducers() {
    constexpr auto length = std::int64_t{9007199254739992};
    const std::array<float, 1> signal{0.8F};
    const auto sourceRate = timeline::SampleRate{0x1.77000000002dcp-38};
    audio::ProcessingPlanSpecification specification;
    specification.projectSampleRate = timeline::SampleRate{48000};
    specification.processingSampleRate = timeline::SampleRate{96000};
    specification.sources = {{{1}, {1}, sourceRate, media::AudioChannelLayout::mono}};
    specification.tracks = {{{1}, {}, {}, {}, media::AudioChannelLayout::mono,
        {{{1}, {1}, {0}, {static_cast<double>(length)}, {0x1.1374bc6af9097p-53}}}}};
    const std::array<audio::PreparedSourceView, 1> sources{{{{1}, {{signal.data(), nullptr}},
        1, {1}, sourceRate, media::AudioChannelLayout::mono}}};
    auto bundle = audio::prepareProcessingPlanFromSources(specification, sources);
    check(bundle.success(), "audit fixture is numerically certified, not rejected to hide error");
    audio::RealtimeAudioEngine engine;
    engine.configure(bundle.prepared->plan, bundle.prepared->runtime);
    makeOperational(engine, 96000);
    check(engine.tryRequestSeek({length - 1}).accepted && engine.tryRequestPlay().accepted, "audit start");
    std::array<float, 3> left{}, right{};
    std::array<float*, 2> channels{left.data(), right.data()};
    engine.processBlock({channels.data(), 2, 3}, timeline::SampleRate{96000});
    check(left[0] > 0.5F && left[1] > 0.5F && left[2] == 0,
          "source 1 - 69/5070602400912917605986812821504000 still renders index zero");
    for (const auto rates : {std::pair{0x1.bbbbbbbbbbbb9p+15, 0x1.4cccccccccccbp+15},
                             std::pair{0x1.cb60000000004p+15, 0x1.5888000000003p+15}}) {
        const std::vector<float> data(4, .8F);
        const std::array tracks{mono(data, rates.first)};
        engine.configure({timeline::SampleRate{rates.first}, {4}, tracks});
        makeOperational(engine, rates.second);
        check(engine.tryRequestPlay().accepted, "non-integral audit rate starts");
        engine.processBlock({channels.data(), 2, 3}, timeline::SampleRate{rates.second});
        const bool strictlyBefore = rates.first == 0x1.bbbbbbbbbbbb9p+15;
        check(engine.transportSnapshot().playing == strictlyBefore, "exact n-epsilon vs exact n transition after three samples");
        engine.processBlock({channels.data(), 2, 1}, timeline::SampleRate{rates.second});
        check((left[0] > .5F) == strictlyBefore && !engine.transportSnapshot().playing,
              "fourth device sample exists iff exact coordinate is still interior");
    }
}

void rateBoundariesAndPartitioning() {
    for (const auto rates : {std::pair{4, 3}, std::pair{44100, 48000},
             std::pair{48000, 44100}, std::pair{48000, 96000}, std::pair{96000, 48000}}) {
        const std::vector<float> signal(static_cast<std::size_t>(rates.first), 0.8F);
        const std::array track{mono(signal, rates.first)};
        const auto render = [&](bool partitioned) {
            audio::RealtimeAudioEngine engine;
            engine.configure({timeline::SampleRate{static_cast<double>(rates.first)},
                              {rates.first}, track});
            makeOperational(engine, rates.second);
            check(engine.tryRequestPlay().accepted, "exact rate boundary Play");
            std::vector<float> left(static_cast<std::size_t>(rates.second) + 1), right(left.size());
            std::size_t offset{};
            constexpr std::array<std::size_t, 5> blocks{1, 63, 128, 511, 1024};
            std::size_t block{};
            while (offset < left.size()) {
                const auto count = std::min(left.size() - offset,
                    partitioned ? blocks[block++ % blocks.size()] : left.size());
                std::array<float*, 2> channels{left.data() + offset, right.data() + offset};
                engine.processBlock({channels.data(), 2, count},
                    timeline::SampleRate{static_cast<double>(rates.second)});
                offset += count;
            }
            check(engine.transportSnapshot().position.value == rates.first &&
                  engine.transportSnapshot().playback == transport::PlaybackState::stopped,
                  "exact rational end stops at content duration");
            for (int index = 0; index < rates.second; ++index)
                check(left[static_cast<std::size_t>(index)] > 0.5F,
                      "every device sample strictly before exact rate boundary is audible");
            check(left.back() == 0, "sample at exact rate boundary is silent, without epsilon");
            return left;
        };
        check(render(false) == render(true), "boundary audio is independent of callback partitioning");
    }
}

void reducerContract() {
    transport::TransportState playing{transport::PlaybackState::playing,
                                      {123}, {1000}};
    const auto first = transport::reduceTransport(
        playing, {transport::TransportActionKind::stop, {}});
    const auto second = transport::reduceTransport(
        first.state, {transport::TransportActionKind::stop, {}});
    check(first.accepted && first.state.position.value == 123 &&
              first.state.playback == transport::PlaybackState::stopped &&
              second.state.position.value == 0,
          "Stop remains stateful under pure ordered reduction");

    transport::TransportState paused{transport::PlaybackState::paused,
                                     {10}, {100}};
    for (const auto target : {20, 30, 40}) {
        const auto reduction = transport::reduceTransport(
            paused, {transport::TransportActionKind::seek, {target}});
        check(reduction.accepted && reduction.seekDiscontinuity,
              "Paused Seek is an explicit accepted discontinuity");
        paused = reduction.state;
    }
    check(paused.position.value == 40 &&
              paused.playback == transport::PlaybackState::paused,
          "Seek A/B/C deterministically leaves C");
    check(!transport::reduceTransport(
               playing, {transport::TransportActionKind::seek, {50}}).accepted,
          "Playing Seek is rejected by transport semantics");

    transport::TransportState beyondContent{transport::PlaybackState::paused,
                                             {150}, {100}};
    const transport::TransportReductionPolicy loopPolicy{
        transport::TransportReductionPolicy::Loop{{20.0}, {200.0}}};
    const auto loopPlay = transport::reduceTransport(
        beyondContent, {transport::TransportActionKind::play, {}},
        loopPolicy);
    check(loopPlay.state.position.value == 150,
          "active loop policy does not confuse content end with playback end");
    beyondContent.position = {200};
    const auto loopRestart = transport::reduceTransport(
        beyondContent, {transport::TransportActionKind::play, {}},
        loopPolicy);
    check(loopRestart.state.position.value == 20,
          "active loop policy restarts at its own boundary");
    beyondContent.position = {10};
    const auto preroll = transport::reduceTransport(
        beyondContent, {transport::TransportActionKind::play, {}}, loopPolicy);
    check(preroll.state.position.value == 10,
          "Play before loop start preserves preroll position");
    beyondContent.position = {201};
    const auto afterLoop = transport::reduceTransport(
        beyondContent, {transport::TransportActionKind::play, {}}, loopPolicy);
    check(afterLoop.state.position.value == 20,
          "Play after loop end restarts at exact loop start");
}

void queuedOrderingAndNavigation() {
    std::vector<float> signal(4096, 0.25F);
    const std::array track{mono(signal, 48000.0)};
    audio::RealtimeAudioEngine engine;
    engine.configure({timeline::SampleRate{48000}, {4096}, track});
    makeOperational(engine, 48000.0);

    check(engine.tryRequestPlay().accepted, "Play accepted");
    process(engine, 64, 48000.0);
    const auto beforeStops = engine.transportSnapshot().position;
    check(beforeStops.value == 64, "Playing advances integer project authority");
    const auto stopA = engine.tryRequestStop();
    const auto stopB = engine.tryRequestStop();
    check(stopA.accepted && stopB.accepted &&
              stopB.sequence == stopA.sequence + 1 &&
              stopA.projectedPosition == beforeStops &&
              stopB.projectedPosition.value == 0,
          "two pre-callback Stops retain ordered projected semantics");
    process(engine, 0, 48000.0);
    auto snapshot = engine.transportSnapshot();
    check(snapshot.playback == transport::PlaybackState::stopped &&
              snapshot.position.value == 0 &&
              snapshot.lastProcessedCommandSequence == stopB.sequence,
          "Playing Stop/Stop in one callback resolves Stopped at zero");

    check(engine.tryRequestPlay().accepted, "second Play accepted");
    process(engine, 32, 48000.0);
    check(engine.tryRequestPause().accepted, "Pause accepted");
    process(engine, 0, 48000.0);
    const auto seekA = engine.tryRequestSeek({100});
    const auto seekB = engine.tryRequestSeek({200});
    const auto seekC = engine.tryRequestSeek({300});
    check(seekA.accepted && seekB.accepted && seekC.accepted &&
              seekC.projectedPosition.value == 300,
          "Paused Seek A/B/C accepted before one callback");
    process(engine, 0, 48000.0);
    snapshot = engine.transportSnapshot();
    check(snapshot.playback == transport::PlaybackState::paused &&
              snapshot.position.value == 300 &&
              snapshot.lastProcessedCommandSequence == seekC.sequence,
          "Paused Seek A/B/C resolves exactly to C");

    const auto beyond = timeline::ProjectFramePosition{100000};
    check(engine.tryRequestSeek(beyond).accepted,
          "position beyond content is a valid locator");
    process(engine, 0, 48000.0);
    check(engine.transportSnapshot().position == beyond &&
              engine.transportSnapshot().duration.value == 4096,
          "navigation does not extend descriptive content duration");
    check(engine.tryRequestPlay().accepted,
          "Play from beyond content preserves provisional restart policy");
    process(engine, 1, 48000.0);
    check(engine.transportSnapshot().position.value == 1,
          "Play at or beyond content restarts from zero");

    const auto sequenceBeforeRejectedSeek =
        engine.transportSnapshot().lastProcessedCommandSequence;
    const auto rejectedPlayingSeek = engine.tryRequestSeek({25});
    check(!rejectedPlayingSeek.accepted &&
              rejectedPlayingSeek.rejection ==
                  audio::AudioControlRejection::disallowedState,
          "Playing Seek is rejected by engine-owned linearized state");
    const auto acceptedStop = engine.tryRequestStop();
    check(acceptedStop.accepted &&
              acceptedStop.sequence == sequenceBeforeRejectedSeek + 1,
          "rejected Seek consumes no sequence");
    process(engine, 0, 48000.0);

    const auto maximum = timeline::maximumSupportedProjectFrame();
    check(engine.tryRequestSeek(maximum).accepted,
          "maximum supported locator is accepted independently of content");
    process(engine, 0, 48000.0);
    check(engine.transportSnapshot().position == maximum,
          "maximum supported locator remains exact");
    check(!engine.tryRequestSeek({maximum.value + 1}).accepted,
          "first unsupported locator is rejected");
}

void queueAndLifecycle() {
    std::vector<float> signal(1024, 0.25F);
    const std::array track{mono(signal, 48000.0)};
    audio::RealtimeAudioEngine engine;
    engine.configure({timeline::SampleRate{48000}, {1024}, track});
    makeOperational(engine, 48000.0);
    audio::AudioCommandSequence last{};
    for (std::size_t index = 0;
         index < audio::RealtimeAudioEngine::commandCapacity - 1; ++index) {
        const auto request = engine.tryRequestStop();
        check(request.accepted, "every usable FIFO slot accepts Stop");
        last = request.sequence;
    }
    const auto rejected = engine.tryRequestPlay();
    check(!rejected.accepted &&
              rejected.rejection == audio::AudioControlRejection::queueFull &&
              !rejected.hasProjection,
          "full FIFO rejects a state-changing command without a projection");
    process(engine, 0, 48000.0);
    check(engine.transportSnapshot().playback ==
              transport::PlaybackState::stopped,
          "queue-full rejection does not alter the RT state");
    const auto afterFull = engine.tryRequestStop();
    check(afterFull.accepted && afterFull.sequence == last + 1,
          "queue-full rejection consumes no sequence across wrap-around");
    const auto pendingPlay = engine.tryRequestPlay();
    check(pendingPlay.accepted, "lifecycle test has a pending command");
    engine.deviceStopped();
    const auto stopped = engine.transportSnapshot();
    check(stopped.playback == transport::PlaybackState::stopped &&
              stopped.position.value == 0 &&
              stopped.lastProcessedCommandSequence >= pendingPlay.sequence,
          "lifecycle closure resolves pending ordered transport commands");
}

timeline::ProjectFramePosition renderPartitioned(
    const std::vector<std::size_t>& blocks, double projectRate,
    double deviceRate, std::size_t totalFrames) {
    std::vector<float> signal(totalFrames * 2U, 0.25F);
    const std::array track{mono(signal, projectRate)};
    audio::RealtimeAudioEngine engine;
    engine.configure({timeline::SampleRate{projectRate},
                      {static_cast<std::int64_t>(signal.size())}, track});
    makeOperational(engine, deviceRate);
    check(engine.tryRequestPlay().accepted, "partition fixture Play accepted");
    std::size_t rendered{};
    std::size_t index{};
    while (rendered < totalFrames) {
        const auto count = std::min(blocks[index++ % blocks.size()],
                                    totalFrames - rendered);
        process(engine, count, deviceRate);
        rendered += count;
    }
    return engine.transportSnapshot().position;
}

timeline::ProjectFramePosition advanceAtRate(double rate,
                                             std::size_t frameCount) {
    std::vector<float> signal(frameCount + 1U, 0.25F);
    const std::array track{mono(signal, rate)};
    audio::RealtimeAudioEngine engine;
    engine.configure({timeline::SampleRate{rate},
                      {static_cast<std::int64_t>(signal.size())}, track});
    makeOperational(engine, rate);
    check(engine.tryRequestPlay().accepted, "same-rate advance Play accepted");
    process(engine, frameCount, rate);
    return engine.transportSnapshot().position;
}

void conversionAndPartitioning() {
    for (const auto rate : {44100.0, 48000.0, 96000.0}) {
        for (const auto frame : {std::int64_t{0}, std::int64_t{1},
                                std::int64_t{63}, std::int64_t{64},
                                std::int64_t{65},
                                static_cast<std::int64_t>(rate) + 1}) {
            const auto converted = timeline::checkedProjectPositionToSeconds(
                {frame}, timeline::SampleRate{rate});
            check(converted && converted->value ==
                  static_cast<double>(frame) / rate,
                  "project-frame presentation conversion is deterministic");
        }
        check(advanceAtRate(rate, 4097).value == 4097,
              "integer transport advances exactly at supported device rates");
    }
    const auto maximum = timeline::maximumSupportedProjectFrame();
    check(timeline::checkedProjectPositionToSeconds(
              maximum, timeline::SampleRate{96000}).has_value() &&
              !timeline::checkedProjectPositionToSeconds(
                  {-1}, timeline::SampleRate{48000}).has_value() &&
              !timeline::checkedProjectPositionToSeconds(
                  {maximum.value + 1}, timeline::SampleRate{48000}).has_value() &&
              !timeline::checkedProjectPositionToSeconds(
                  {0}, timeline::SampleRate{0}).has_value(),
          "checked presentation conversion rejects unsupported coordinates/rates");

    audio::RealtimeProjectClock boundaryClock;
    boundaryClock.prepare({maximum.value});
    check(boundaryClock.seek({maximum.value - 1}) && boundaryClock.play(),
          "clock accepts a locator immediately below the numeric boundary");
    boundaryClock.advance({2.0});
    check(boundaryClock.publicPosition() == maximum &&
              !boundaryClock.isPlaying(),
          "clock stops at the supported boundary without overflow");

    constexpr std::size_t total = 150000;
    const auto oneBlock = renderPartitioned({total}, 48000.0, 44100.0, total);
    const auto mixed = renderPartitioned(
        {1, 64, 127, 256, 511, 1024, 1537}, 48000.0, 44100.0, total);
    const auto expected = static_cast<std::int64_t>(std::llround(
        static_cast<double>(total) * 48000.0 / 44100.0));
    check(oneBlock == mixed && oneBlock.value == expected,
          "integer authority plus bounded phase is callback-partition independent");
}
void fractionalResume() {
    std::vector<float> signal(1000, 0.0F);
    signal[999] = 0.8F;
    const std::array track{mono(signal, 48000.0)};
    audio::RealtimeAudioEngine engine;
    engine.configure({timeline::SampleRate{48000}, {1000}, track});
    makeOperational(engine, 96000.0);
    check(engine.tryRequestSeek({999}).accepted && engine.tryRequestPlay().accepted,
          "prepare final half-project-frame");
    process(engine, 1, 96000.0);
    check(engine.tryRequestPause().accepted, "pause before DSP end");
    process(engine, 0, 96000.0);
    const auto resume = engine.tryRequestPlay();
    check(resume.accepted && resume.projectedPosition.value == 1000,
          "48/96 resume must not project premature rewind");
    std::array<float, 1> left{}, right{};
    std::array<float*, 2> channels{left.data(), right.data()};
    engine.processBlock({channels.data(), 2, 1}, timeline::SampleRate{96000});
    check(std::abs(left[0] - 0.8F * audio::monoCentreCoefficient) < 1.0e-5F,
          "48/96 resume renders last source sample, not silent frame zero");
}

void nonOperationalStop() {
    std::vector<float> signal(1000, 0.2F);
    const std::array track{mono(signal, 48000.0)};
    audio::RealtimeAudioEngine engine;
    engine.configure({timeline::SampleRate{48000}, {1000}, track});
    makeOperational(engine, 48000.0);
    check(engine.tryRequestPlay().accepted, "start before preserving lifecycle");
    process(engine, 10, 48000.0);
    check(engine.tryRequestPause().accepted, "pause before preserving lifecycle");
    process(engine, 0, 48000.0);
    audio::TransportTestAccess::closeBeforeClockPublication(engine);
    check(!engine.tryRequestStop().accepted,
          "closed gate with previous generation snapshot cannot satisfy Stop");
    engine.deviceInitialisingPreservingTransport();
    check(!engine.tryRequestStop().accepted,
          "nonoperational Paused Stop cannot claim satisfaction");
    makeOperational(engine, 48000.0);
    check(engine.tryRequestSeek({20}).accepted, "stopped nonzero locator");
    process(engine, 0, 48000.0);
    engine.deviceInitialisingPreservingTransport();
    check(!engine.tryRequestStop().accepted,
          "nonoperational Stopped at nonzero needs actual rewind");
    engine.deviceStopped();
    const auto satisfied = engine.tryRequestStop();
    check(satisfied.accepted && satisfied.disposition ==
              audio::AudioControlDisposition::alreadySatisfied,
          "confirmed Stopped at zero satisfies Stop without scheduling");
}

void doubleCapacityAndCancelledTicket() {
    std::vector<float> signal(1000, 0.2F);
    const std::array track{mono(signal, 48000.0)};
    audio::RealtimeAudioEngine engine;
    engine.configure({timeline::SampleRate{48000}, {1000}, track});
    makeOperational(engine, 48000.0);
    audio::AudioCommandSequence last{};
    for (std::size_t batch = 0; batch < 2; ++batch) {
        for (std::size_t index = 0; index < audio::RealtimeAudioEngine::commandCapacity - 1; ++index) {
            const auto seek = engine.tryRequestSeek({static_cast<std::int64_t>(last + 1)});
            check(seek.accepted, "accepted Seek fits FIFO and pending journal");
            last = seek.sequence;
        }
        if (batch == 0) {
            const auto before = engine.projectedTransportSnapshot();
            const auto write = audio::TransportTestAccess::fifoWrite(engine);
            check(audio::TransportTestAccess::pending(engine) < audio::RealtimeAudioEngine::pendingCommandCapacity,
                  "FIFO-full test has pending capacity");
            const auto rejected = engine.tryRequestPlay();
            check(!rejected.accepted && rejected.rejection == audio::AudioControlRejection::queueFull &&
                      engine.projectedTransportSnapshot().position == before.position &&
                      engine.projectedTransportSnapshot().playback == before.playback &&
                      audio::TransportTestAccess::fifoWrite(engine) == write,
                  "FIFO full leaves projection and FIFO unchanged");
        }
        audio::TransportTestAccess::consumeWithoutPublishing(engine);
    }
    check(audio::TransportTestAccess::pending(engine) == audio::RealtimeAudioEngine::pendingCommandCapacity,
          "journal full while consumed FIFO has space");
    const auto before = engine.projectedTransportSnapshot();
    const auto write = audio::TransportTestAccess::fifoWrite(engine);
    const auto rejected = engine.tryRequestPlay();
    check(!rejected.accepted && rejected.rejection == audio::AudioControlRejection::queueFull &&
              audio::TransportTestAccess::fifoWrite(engine) == write &&
              engine.projectedTransportSnapshot().position == before.position &&
              engine.projectedTransportSnapshot().playback == before.playback,
          "journal full leaves FIFO and projection unchanged");
    audio::TransportTestAccess::publish(engine);
    const auto next = engine.tryRequestSeek({50});
    check(next.accepted && next.sequence == last + 1,
          "neither capacity rejection reserves a ticket");
    process(engine, 0, 48000.0);
    const auto prior = engine.transportSnapshot();
    audio::TransportTestAccess::rejectedReservation(engine);
    const auto after = engine.transportSnapshot();
    const auto projected = engine.projectedTransportSnapshot();
    check(after.lastProcessedCommandSequence > prior.lastProcessedCommandSequence &&
              after.position == prior.position && after.playback == prior.playback &&
              projected.position == prior.position && projected.playback == prior.playback,
          "cancelled reservation can expose watermark but cannot apply an action");
}

void maximumFraction() {
    const auto end = timeline::maximumSupportedProjectFrame().value - 1;
    audio::RealtimeProjectClock clock;
    clock.prepare({end});
    check(clock.seek({end - 1}) && clock.play(), "large frame fixture");
    clock.advance({0.5});
    check(clock.isPlaying(), "large integer rounding cannot anticipate DSP end");
    clock.pause();
    check(clock.play() && clock.publicPosition().value == end,
          "large fractional resume must preserve locator");
    clock.advance({0.5});
    check(!clock.isPlaying() && clock.publicPosition().value == end,
          "large rational half plus half reaches exact end");
}

void maximumAudio() {
    const auto end = timeline::maximumSupportedProjectFrame().value;
    const auto start = end - 3;
    const std::vector<float> signal{0.2F, 0.6F, 0.8F};
    std::array track{mono(signal, 48000.0)};
    track[0].clipStart = {start};
    audio::RealtimeAudioEngine engine;
    engine.configure({timeline::SampleRate{48000}, {end}, track});
    makeOperational(engine, 96000.0);
    check(engine.tryRequestSeek({start}).accepted && engine.tryRequestPlay().accepted,
          "large-position real render fixture");
    std::array<float, 2> left{}, right{};
    std::array<float*, 2> channels{left.data(), right.data()};
    engine.processBlock({channels.data(), 2, 2}, timeline::SampleRate{96000});
    check(std::abs(left[1] - 0.4F * audio::monoCentreCoefficient) < 1.0e-5F,
          "near maximum render preserves the source half-frame, not an absolute rounded double");

    audio::ProcessingPlanSpecification specification;
    specification.projectSampleRate = timeline::SampleRate{48000};
    specification.processingSampleRate = timeline::SampleRate{96000};
    specification.sources = {{{1}, {3}, timeline::SampleRate{48000}, media::AudioChannelLayout::mono}};
    specification.tracks = {{{1}, {}, {}, {}, media::AudioChannelLayout::mono,
        {{{1}, {1}, {start}, {2.5}, {0.0}}}}};
    const std::array<audio::PreparedSourceView, 1> sources{{{{1}, {{signal.data(), nullptr}},
        1, {3}, timeline::SampleRate{48000}, media::AudioChannelLayout::mono}}};
    auto bundle = audio::prepareProcessingPlanFromSources(specification, sources);
    check(bundle.success() && bundle.prepared->plan.duration.value == end,
          "large fractional clip exclusive duration uses integer start plus local ceil");
    engine.configure(bundle.prepared->plan, bundle.prepared->runtime);
    makeOperational(engine, 96000);
    check(engine.tryRequestSeek({start}).accepted && engine.tryRequestPlay().accepted,
          "large prepared-clip fixture starts");
    engine.processBlock({channels.data(), 2, 2}, timeline::SampleRate{96000});
    check(std::abs(left[1] - 0.4F * audio::monoCentreCoefficient) < 1.0e-5F,
          "prepared clip and legacy view retain the same local half-frame");
    project::ProjectState model{timeline::SampleRate{48000}};
    const auto trackId = model.addAudioTrack("Large boundary");
    const auto imported = model.importAudioToTrack(trackId,
        media::MediaReference{"numeric.wav", {}}, {3}, timeline::SampleRate{48000},
        media::AudioChannelLayout::mono);
    static_cast<void>(model.addClip(trackId, imported.source, {start}, {2.5}, {0.0}));
    check(model.duration().value == bundle.prepared->plan.duration.value,
          "document and prepared content duration agree at the large half-frame boundary");
}

void rationalMaximumClock() {
    const auto base = timeline::maximumSupportedProjectFrame().value - 10000;
    for (const auto ratio : {std::pair{1LL, 2LL}, std::pair{147LL, 160LL}, std::pair{320LL, 147LL}}) {
        audio::RealtimeProjectClock clock;
        clock.prepare(timeline::ProjectFrameCount{timeline::maximumSupportedProjectFrame().value});
        check(clock.seek({base}) && clock.play(), "rational maximum fixture");
        bool positive{}, negative{};
        for (std::int64_t n = 1; n <= 3000; ++n) {
            clock.advance({static_cast<double>(ratio.first) / ratio.second});
            const auto numerator = n * ratio.first;
            const auto rounded = (2 * numerator + ratio.second) / (2 * ratio.second);
            // Non-dyadic floating ratios can land immediately either side of a
            // half-frame tie. The exact DSP distance, not that rounding choice,
            // must agree with the rational oracle.
            const auto actual = clock.checkpoint();
            const auto phase = clock.renderPosition().phase;
            const auto local = static_cast<double>(actual.position.value - base) + phase;
            check(std::abs(local - static_cast<double>(numerator) / ratio.second) < 1.0e-9 &&
                      std::abs(actual.position.value - base - rounded) <= 1 &&
                      std::abs(phase) <= 0.5,
                  "rational near-maximum advance preserves bounded fractional distance");
            positive |= phase > 0.0;
            negative |= phase < 0.0;
        }
        check(negative && (ratio.second == 2 || positive), "oracle exercises residue signs");
    }
    audio::RealtimeProjectClock loop;
    const auto end = timeline::maximumSupportedProjectFrame().value;
    loop.prepare({end});
    loop.setPlaybackPolicy(audio::RealtimeProjectClock::LoopBounds{
        static_cast<double>(end - 1000), static_cast<double>(end)}, false);
    check(loop.seek({end - 1}) && loop.play(), "maximum loop fixture");
    loop.advance({0.5});
    check(!loop.consumeWrapped(), "negative phase does not wrap prematurely");
    loop.advance({0.5});
    check(loop.consumeWrapped() && loop.publicPosition().value == end - 1000,
          "maximum loop wraps on the actual boundary without overflow");
}

void fractionalLoop() {
    audio::RealtimeProjectClock clock;
    clock.prepare({2000});
    clock.setPlaybackPolicy(audio::RealtimeProjectClock::LoopBounds{0.0, 1000.75}, false);
    check(clock.seek({1000}) && clock.play(), "fractional loop fixture");
    clock.advance({0.5});
    clock.pause();
    const transport::TransportState state{clock.playback(), clock.publicPosition(), clock.duration()};
    const auto projected = transport::reduceTransport(state,
        {transport::TransportActionKind::play, {}},
        {{{{0.0}, {1000.75}}}, clock.boundaryFacts()});
    check(clock.play() && projected.state.position == clock.publicPosition(),
          "fractional loop producer and RT must agree");
}
} // namespace

int main(int argc, char** argv) {
    if (argc == 2) {
        const std::string_view name{argv[1]};
        if (name == "resume") fractionalResume();
        else if (name == "stop") nonOperationalStop();
        else if (name == "maximum") maximumFraction();
        else if (name == "maximum-audio") maximumAudio();
        else if (name == "loop") fractionalLoop();
        else if (name == "availability") playAvailabilityLifecycle();
        else if (name == "generation") playGenerationValidation();
        else if (name == "large-local") largeLocalAudio();
        else if (name == "n-epsilon") naturalEndBelowBoundary();
        else if (name == "exact-audit") exactAuditReproducers();
        else if (name == "rate-boundaries") rateBoundariesAndPartitioning();
        else check(false, "unknown focused test");
        return 0;
    }
    reducerContract();
    playAvailabilityLifecycle();
    playGenerationValidation();
    largeLocalAudio();
    naturalEndBelowBoundary();
    exactAuditReproducers();
    rateBoundariesAndPartitioning();
    queuedOrderingAndNavigation();
    queueAndLifecycle();
    conversionAndPartitioning();
    fractionalResume();
    nonOperationalStop();
    maximumFraction();
    maximumAudio();
    rationalMaximumClock();
    fractionalLoop();
    doubleCapacityAndCancelledTicket();
    std::cout << "Transport and timeline foundation tests passed\n";
}
