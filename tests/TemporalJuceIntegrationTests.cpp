#include "vitadaw/platform/juce/JuceAudioDeviceAdapter.h"
#include "vitadaw/application/DawApplication.h"
#include "vitadaw/commands/CommandDispatcher.h"
#include <array>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <string_view>

using namespace vitadaw;
namespace {
void check(bool value, const char* message) {
    if (!value) { std::cerr << message << '\n'; std::exit(1); }
}
void committed(void*) noexcept {}
int token;
const audio::AudioFileCommitAction commit{&token, committed};
}
namespace vitadaw::platform::juce_adapter {
class PersistenceIntegrationAccess {
public:
    // Same preparation entry as native startup, before DawApplication exists.
    // No AudioDeviceManager consumer: the test owns the quiescent/render region.
    static bool open(JuceAudioDeviceAdapter& adapter, double rate) {
        adapter.beginDeviceReinitialisation();
        adapter.deviceSampleRate_ = timeline::SampleRate{rate};
        std::string error;
        if (!adapter.reprepareForCurrentDevice(error)) {
            adapter.realtimeEngine_.deviceErrorPreservingTransport();
            return false;
        }
        // attachAudioCallback(true) invokes audioDeviceAboutToStart before the
        // first consumer callback; reproduce that lifecycle edge hardware-free.
        adapter.realtimeEngine_.deviceInitialisingPreservingTransport();
        return true;
    }
    static void controlledStop(JuceAudioDeviceAdapter& adapter) {
        // JUCE removeAudioCallback emits this notification after serialising
        // with render; no physical device is needed to exercise our handler.
        adapter.preserveTransportDuringRegistration_.store(true);
        adapter.audioDeviceStopped();
        adapter.preserveTransportDuringRegistration_.store(false);
    }
    static auto& engine(JuceAudioDeviceAdapter& adapter) { return adapter.realtimeEngine_; }
    static const void* plan(JuceAudioDeviceAdapter& adapter) { return adapter.preparedProject_.get(); }
    static const auto* context(JuceAudioDeviceAdapter& adapter) { return adapter.preparedTemporalContext_.get(); }
};
}
using Access = platform::juce_adapter::PersistenceIntegrationAccess;
using Adapter = platform::juce_adapter::JuceAudioDeviceAdapter;
void process(Adapter& adapter, std::size_t frames) {
    std::array<float, 256> left{}, right{};
    std::array<float*, 2> output{left.data(), right.data()};
    do {
        const auto count = std::min(frames, left.size());
        Access::engine(adapter).processBlock({output.data(), 2, count}, timeline::SampleRate{48000});
        frames -= count;
    } while (frames);
}
void temporal(Adapter& adapter, double bpm) {
    musical::MusicalTimeMap map;
    map.tempo.events[0].bpm = {bpm};
    auto result = adapter.prepareTemporalContext(map,
        musical::MusicalLoopRange{{0}, {musical::ppq}}, timeline::SampleRate{48000}, 17);
    check(result.success(), "temporal preparation");
    check(adapter.commitPreparedTemporalContext(std::move(result.prepared), commit), "temporal commit");
}
void plan(Adapter& adapter, bool preserve = false) {
    audio::ProcessingPlanSpecification specification;
    specification.projectSampleRate = timeline::SampleRate{48000};
    auto candidate = adapter.prepareProcessingPlan(specification);
    check(candidate.success(), "empty structural plan prepares");
    check(preserve ? adapter.commitPreparedProcessingPlanPreservingTransport(std::move(candidate.prepared), commit)
                   : adapter.commitPreparedProcessingPlan(std::move(candidate.prepared), commit),
          "structural plan commits with exact active phase");
}
bool same(const audio::RealtimeAudioEngine::TemporalCheckpoint& a,
          const audio::RealtimeAudioEngine::TemporalCheckpoint& b) {
    return a.clock.position == b.clock.position && a.clock.playback == b.clock.playback &&
        audio::exact::comparePositions({a.clock.position.value, a.clock.phase},
                                     {b.clock.position.value, b.clock.phase}) == 0 &&
        a.loopEnabled == b.loopEnabled && a.metronomeEnabled == b.metronomeEnabled &&
        a.runUntilStop == b.runUntilStop &&
        a.metronomeLevel == b.metronomeLevel &&
        a.pendingMetronomeBoundary == b.pendingMetronomeBoundary;
}
int main(int argc, char** argv) {
    juce::ScopedJuceInitialiser_GUI initialiseJuce;
    check(argc == 2, "case required");
    const std::string_view test{argv[1]};
    Adapter adapter;
    if (test == "bootstrap") {
        check(Access::open(adapter, 48000), "native order: certify empty device before application");
        application::DawApplication app{adapter, timeline::SampleRate{48000}};
        check(Access::context(adapter) != nullptr, "first musical context installed after device bootstrap");
        Access::engine(adapter).deviceInitialising();
        process(adapter, 0);
        check(Access::engine(adapter).deviceState() == audio::DeviceProcessingState::operational,
              "real processBlock confirms consumer");
    } else {
        application::DawApplication app{adapter, timeline::SampleRate{48000}};
        if (test == "lifetime") {
            check(Access::open(adapter, 44100), "empty project device-rate replacement");
            Access::engine(adapter).deviceUnavailable();
            check(Access::open(adapter, 48000), "empty project reprepare back");
            plan(adapter);
        } else if (test == "metronome_rates") {
            check(Access::open(adapter, 44100),
                  "metronome lifecycle opens 44.1 kHz");
            Access::engine(adapter).deviceConsumerStarted();
            check(adapter.trySetMetronomeEnabled(true).accepted &&
                      adapter.trySetMetronomeLevel({-12.0F}).accepted,
                  "metronome lifecycle establishes session controls");
            process(adapter, 0);
            check(adapter.tryRequestPlay().accepted,
                  "metronome lifecycle starts open playback");
            process(adapter, 64);
            auto previous = Access::engine(adapter).temporalCheckpoint();
            for (const auto rate : {48000.0, 96000.0, 44100.0}) {
                check(Access::open(adapter, rate),
                      "metronome lifecycle reprepares requested sample rate");
                check(Access::context(adapter)->deviceSampleRate ==
                          timeline::SampleRate{rate} &&
                          same(previous,
                               Access::engine(adapter).temporalCheckpoint()),
                      "44.1/48/96 reprepare preserves metronome checkpoint");
                Access::engine(adapter).deviceConsumerStarted();
                process(adapter, 64);
                previous = Access::engine(adapter).temporalCheckpoint();
                check(previous.clock.playback ==
                          transport::PlaybackState::playing &&
                          previous.metronomeEnabled && previous.runUntilStop,
                      "metronome remains active after device-rate rebuild");
            }
            check(adapter.tryRequestStop().accepted,
                  "metronome lifecycle stops after rate matrix");
            process(adapter, 0);
        } else if (test == "metronome_rollback") {
            check(Access::open(adapter, 48000),
                  "metronome rollback opens certified device");
            Access::engine(adapter).deviceConsumerStarted();
            commands::CommandDispatcher dispatcher{app};
            check(dispatcher.dispatch(commands::SetTempo{
                      {1}, {std::nextafter(123.0, INFINITY)}}).status ==
                      commands::CommandStatus::accepted,
                  "metronome rollback commits document A");
            check(dispatcher.dispatch(commands::SetLoopRangeMusical{
                      {0}, {musical::ppq}}).status ==
                      commands::CommandStatus::accepted,
                  "metronome rollback certifies fractional temporal context A");
            check(dispatcher.dispatch(commands::SetMetronomeEnabled{true}).status ==
                      commands::CommandStatus::accepted &&
                      dispatcher.dispatch(commands::SetMetronomeLevel{{-18.0F}}).status ==
                      commands::CommandStatus::accepted,
                  "metronome rollback establishes session A");
            process(adapter, 0);
            app.synchroniseTransport();
            check(dispatcher.dispatch(commands::Play{}).status ==
                      commands::CommandStatus::accepted,
                  "metronome rollback starts open playback A");
            process(adapter, 32);
            app.synchroniseTransport();
            const auto documentA = app.project().musicalTime();
            const auto preparedMapA = Access::context(adapter)->documentMap;
            const auto revisionA = Access::context(adapter)->revision;
            const auto readModelA = app.metronomeReadModel();
            const auto checkpointA = Access::engine(adapter).temporalCheckpoint();
            check(readModelA.enabled && readModelA.level.value == -18.0F &&
                      readModelA.temporalRevision == revisionA &&
                      checkpointA.clock.playback ==
                          transport::PlaybackState::playing &&
                      checkpointA.runUntilStop,
                  "metronome rollback fixture is coherent before candidate B");
            check(!Access::open(adapter, 44100),
                  "metronome rollback rejects incompatible reprepare B");
            app.synchroniseTransport();
            const auto checkpointAfter =
                Access::engine(adapter).temporalCheckpoint();
            check(app.project().musicalTime() == documentA &&
                      Access::context(adapter)->documentMap == preparedMapA &&
                      Access::context(adapter)->revision == revisionA &&
                      Access::context(adapter)->deviceSampleRate ==
                          timeline::SampleRate{48000} &&
                      adapter.transportSnapshot().temporalRevision == revisionA &&
                      app.metronomeReadModel() == readModelA &&
                      same(checkpointA, checkpointAfter) &&
                      checkpointAfter.clock.playback ==
                          transport::PlaybackState::playing &&
                      checkpointAfter.runUntilStop,
                  "failed reprepare preserves semantic context, revision, read model and open playback without a hybrid");
            check(Access::open(adapter, 48000),
                  "metronome rollback recovers compatible device");
        } else if (test == "run_until_stop") {
            temporal(adapter, std::nextafter(123.0, INFINITY));
            plan(adapter);
            Access::engine(adapter).deviceInitialising();
            Access::engine(adapter).deviceConsumerStarted();
            check(adapter.trySetMetronomeEnabled(true).accepted,
                  "enable metronome for open playback");
            process(adapter, 0);
            check(adapter.tryRequestPlay().accepted, "open playback Play");
            process(adapter, 32);
            check(adapter.trySetMetronomeEnabled(false).accepted,
                  "disable metronome while Playing");
            process(adapter, 0);
            const auto before = Access::engine(adapter).temporalCheckpoint();
            const auto* previousContext = Access::context(adapter);
            const auto* previousPlan = Access::plan(adapter);
            const auto revision = adapter.transportSnapshot().temporalRevision;
            check(before.clock.playback == transport::PlaybackState::playing &&
                      !before.loopEnabled && !before.metronomeEnabled &&
                      before.runUntilStop,
                  "fixture is Playing open-ended with loop/metronome disabled");
            check(!Access::open(adapter, 44100),
                  "open-policy reprepare exceeds exact capacity");
            check(same(before, Access::engine(adapter).temporalCheckpoint()) &&
                      Access::engine(adapter).temporalCheckpoint().runUntilStop &&
                      Access::context(adapter) == previousContext &&
                      Access::plan(adapter) == previousPlan &&
                      adapter.transportSnapshot().temporalRevision == revision,
                  "rejected reprepare preserves visible checkpoint and owners");
            check(Access::open(adapter, 48000), "recover compatible device context");
            check(Access::engine(adapter).temporalCheckpoint().runUntilStop,
                  "compatible recovery preserves explicit open policy");
            Access::engine(adapter).deviceConsumerStarted();
            process(adapter, 1);
            check(adapter.transportSnapshot().playback ==
                      transport::PlaybackState::playing,
                  "recovered transport retains run-until-Stop policy");
            check(adapter.tryRequestStop().accepted, "explicit Stop after recovery");
            process(adapter, 0);
            check(adapter.transportSnapshot().playback ==
                      transport::PlaybackState::stopped,
                  "open playback still terminates on explicit Stop");
        } else if (test == "pending_wrap") {
            temporal(adapter, 123.0);
            plan(adapter);
            Access::engine(adapter).deviceInitialising();
            Access::engine(adapter).deviceConsumerStarted();
            check(adapter.trySetLoopEnabled(true).accepted &&
                      adapter.trySetMetronomeEnabled(true).accepted &&
                      adapter.trySetMetronomeLevel({0.0F}).accepted,
                  "pending-wrap session controls");
            process(adapter, 0);
            process(adapter, 512);
            check(adapter.tryRequestPlay().accepted, "pending-wrap Play");
            process(adapter, 23415);
            Access::controlledStop(adapter);
            const auto pending = Access::engine(adapter).temporalCheckpoint();
            check(pending.pendingMetronomeBoundary.crossedLoopStart,
                  "JUCE checkpoint retains crossed loop-start obligation");
            plan(adapter, true);
            Access::engine(adapter).deviceInitialisingPreservingTransport();
            std::array<float, 256> left{}, right{};
            std::array<float*, 2> output{left.data(), right.data()};
            Access::engine(adapter).processBlock(
                {output.data(), 2, left.size()}, timeline::SampleRate{48000});
            for (std::size_t frame = 0; frame < left.size(); ++frame)
                check(left[frame] == Access::context(adapter)->clicks.accent[frame],
                      "JUCE rebuild emits the pending loop-start click once");
            check(!Access::engine(adapter).temporalCheckpoint()
                       .pendingMetronomeBoundary.crossedLoopStart,
                  "JUCE rebuild consumes the pending obligation");
        } else {
            temporal(adapter, test == "rollback" ? std::nextafter(123.0, INFINITY) : 123.0);
            plan(adapter);
            Access::engine(adapter).deviceInitialising();
            Access::engine(adapter).deviceConsumerStarted();
            check(adapter.trySetLoopEnabled(true).accepted, "enable loop");
            process(adapter, 0);
            check(adapter.tryRequestPlay().accepted, "Play");
            process(adapter, 23415);
            check(adapter.tryRequestStop().accepted, "Stop");
            process(adapter, 0);
            const auto checkpoint = Access::engine(adapter).temporalCheckpoint();
            const auto* previousContext = Access::context(adapter);
            const auto* previousPlan = Access::plan(adapter);
            const auto revision = adapter.transportSnapshot().temporalRevision;
            if (test == "checkpoint") {
                check(audio::exact::comparePositions({checkpoint.clock.position.value, checkpoint.clock.phase},
                    {0, {{15,0}, {41,0}, false}}) == 0, "fixture phase is exactly 15/41");
                Access::controlledStop(adapter);
                check(same(checkpoint, Access::engine(adapter).temporalCheckpoint()),
                      "controlled callback removal retains the exact stopped phase");
                plan(adapter, true);
            } else {
                check(!Access::open(adapter, 44100), "133-bit context must reject");
                check(Access::context(adapter) == previousContext && Access::plan(adapter) == previousPlan,
                      "failed reprepare preserves both owners");
                check(Access::engine(adapter).deviceState() == audio::DeviceProcessingState::error,
                      "failed physical reconfiguration cannot render with old format");
            }
            check(same(checkpoint, Access::engine(adapter).temporalCheckpoint()), "checkpoint and policies preserved exactly");
            check(adapter.transportSnapshot().temporalRevision == revision, "revision preserved");
        }
    }
    adapter.shutdown();
    adapter.shutdown(); // effective destruction + idempotent retirement, under ASan
    check(Access::context(adapter) == nullptr && Access::plan(adapter) == nullptr, "owners released on close");
    check(adapter.transportSnapshot().temporalRevision == 0, "no engine view survives owner destruction");
    std::cout << test << " passed\n";
}
