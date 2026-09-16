#include "vitadaw/audio/PreparedTemporalContext.h"
#include "vitadaw/audio/RealtimeAudioEngine.h"
#include "vitadaw/persistence/ProjectPersistence.h"
#include "vitadaw/project/ProjectState.h"

#include <array>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <new>
#include <string_view>
#include <vector>

namespace {
using namespace vitadaw;
std::atomic<bool> countRealtimeAllocations{};
std::atomic<std::size_t> realtimeAllocations{};
void check(bool value, std::string_view message) {
    if (!value) { std::cerr << "FAILED: " << message << '\n'; std::exit(1); }
}
void enterOperational(audio::RealtimeAudioEngine& engine, double rate) {
    check(engine.prepareLegacyDeviceRate(timeline::SampleRate{rate}), "prepare exact device context outside RT");
    engine.deviceInitialising();
    std::array<float*, 0> none{};
    engine.processBlock({none.data(), 0, 0}, timeline::SampleRate{rate});
}
void process(audio::RealtimeAudioEngine& engine, std::vector<float>& left,
             std::vector<float>& right, double rate) {
    std::array<float*,2> channels{left.data(),right.data()};
    engine.processBlock({channels.data(),2,left.size()},timeline::SampleRate{rate});
}
void processRange(audio::RealtimeAudioEngine& engine, std::vector<float>& left,
                  std::vector<float>& right, std::size_t offset,
                  std::size_t count, double rate) {
    std::array<float*,2> channels{left.data()+offset,right.data()+offset};
    engine.processBlock({channels.data(),2,count},timeline::SampleRate{rate});
}

struct MetronomeRender {
    std::vector<float> left;
    std::vector<float> right;
};

void makeDiagnosticClicks(audio::PreparedTemporalContext& context) {
    context.clicks.normal.fill(0.0F);
    context.clicks.accent.fill(0.0F);
    context.clicks.normal[0] = 1.0F;
    context.clicks.accent[0] = 2.0F;
    context.clicks.frameCount = 1;
}

MetronomeRender renderMetronomeGrid(
    const musical::MusicalTimeMap& map, double rate, std::size_t frames,
    std::span<const std::size_t> partitions, bool diagnostic,
    float levelDb = 0.0F) {
    auto temporal = audio::prepareTemporalContext(
        map, std::nullopt, timeline::SampleRate{rate},
        timeline::SampleRate{rate}, 71);
    check(temporal.success(), "metronome matrix context prepares");
    if (diagnostic) makeDiagnosticClicks(*temporal.prepared);
    audio::RealtimeAudioEngine engine;
    engine.configure({timeline::SampleRate{rate}, {0},
                      std::span<const audio::PreparedTrackView>{}});
    check(engine.configureTemporalContext(temporal.prepared.get()),
          "metronome matrix context configures");
    enterOperational(engine, rate);
    check(engine.trySetMetronomeEnabled(true).accepted &&
              engine.trySetMetronomeLevel({levelDb}).accepted,
          "metronome matrix controls enqueue");
    std::vector<float> empty;
    process(engine, empty, empty, rate);
    std::vector<float> settle(512), settleRight(512);
    process(engine, settle, settleRight, rate);
    check(engine.tryRequestPlay().accepted, "metronome matrix Play");
    MetronomeRender result{std::vector<float>(frames),
                           std::vector<float>(frames)};
    if (partitions.empty()) {
        process(engine, result.left, result.right, rate);
    } else {
        std::size_t offset{}, partition{};
        while (offset < frames) {
            const auto count = std::min(partitions[partition % partitions.size()],
                                        frames - offset);
            check(count != 0, "metronome partition must be non-zero");
            processRange(engine, result.left, result.right, offset, count, rate);
            offset += count;
            ++partition;
        }
    }
    return result;
}

std::size_t exactTriggerFrame(const audio::PreparedTemporalContext& context,
                              std::int64_t tick) {
    const auto position = context.musicalTime->exactProjectFrameAtTick({tick});
    check(bool(position), "expected musical trigger is representable");
    const auto floor = audio::exact::floorPosition(position.value);
    return static_cast<std::size_t>(floor.frame) +
        (audio::exact::zero(audio::exact::wide(floor.remainder)) ? 0U : 1U);
}

void checkDiagnosticEvents(const std::vector<float>& rendered,
                           std::span<const std::pair<std::size_t, float>> expected,
                           std::string_view message) {
    std::size_t event{};
    for (std::size_t frame = 0; frame < rendered.size(); ++frame) {
        if (rendered[frame] == 0.0F) continue;
        check(event < expected.size() && expected[event].first == frame &&
                  expected[event].second == rendered[frame], message);
        ++event;
    }
    check(event == expected.size(), message);
}

void preparationAndPersistence() {
    musical::MusicalTimeMap map;
    auto golden = audio::prepareTemporalContext(map,
        musical::MusicalLoopRange{{4*musical::ppq},{12*musical::ppq}},
        timeline::SampleRate{48000}, timeline::SampleRate{48000}, 7);
    check(golden.success(), "golden loop prepares");
    check(golden.prepared->loop->start.value == 96000.0 &&
          golden.prepared->loop->end.value == 288000.0,
          "bars 2-4 compile to exact golden frames");
    check(golden.prepared->revision == 7 &&
          golden.prepared->loop->musicalMapRevision == 7,
          "map and loop share one revision");
    check(!audio::prepareTemporalContext(map,
        musical::MusicalLoopRange{{0},{1023}}, timeline::SampleRate{48000},
        timeline::SampleRate{48000},1).success(),
        "short musical loop rejected, not clamped");

    project::ProjectState project{timeline::SampleRate{48000}};
    auto data = project.documentData();
    data.loopRange = musical::MusicalLoopRange{{4*musical::ppq},
                                               {12*musical::ppq}};
    auto restored = project::ProjectState::fromDocumentData(std::move(data));
    check(bool(restored), "documentary loop validates");
    const auto saved = persistence::serializeProject(
        *restored, "/tmp/vitadaw-loop-test.vitadaw");
    check(saved.result.success() && saved.bytes.find("\"schemaVersion\": 3") != std::string::npos,
          "schema v3 is emitted");
    const auto savedAgain = persistence::serializeProject(
        *restored, "/tmp/vitadaw-loop-test.vitadaw");
    check(saved.bytes == savedAgain.bytes, "v3 save is deterministic");
    const auto loaded = persistence::deserializeProject(saved.bytes);
    check(loaded.result.success() && loaded.project->loopRange() == restored->loopRange(),
          "loop ticks round-trip exactly");

    auto changedMap = map;
    changedMap.tempo.events.push_back({{2},{4*musical::ppq},{60.0}});
    changedMap.tempo.nextId = {3};
    changedMap.signatures.events.push_back({{2},{1},{3,4}});
    changedMap.signatures.nextId = {3};
    const auto changed = audio::prepareTemporalContext(changedMap,
        restored->loopRange(), timeline::SampleRate{48000},
        timeline::SampleRate{48000}, 8);
    check(changed.success() && changed.prepared->loop->musical == restored->loopRange(),
          "tempo/meter recompilation preserves authoritative loop ticks");
    check(changed.prepared->loop->end.value == 480000.0 &&
          changed.prepared->loop->end.value != golden.prepared->loop->end.value,
          "tempo recompilation atomically changes prepared frame endpoints");

    musical::MusicalTimeMap maximumMap;
    maximumMap.tempo.events.reserve(musical::maximumEvents);
    for (std::size_t index=1; index<musical::maximumEvents; ++index) {
        maximumMap.tempo.events.push_back({{index+1},
            {static_cast<std::int64_t>(index)*musical::ppq},{120.0}});
    }
    maximumMap.tempo.nextId = {musical::maximumEvents+1};
    check(audio::prepareTemporalContext(maximumMap,std::nullopt,
              timeline::SampleRate{48000},timeline::SampleRate{48000},10).success(),
          "maximum-size musical map prepares temporal context off RT");

    musical::MusicalTimeMap fractional;
    fractional.tempo.events[0].bpm={123.0};
    auto exactLoop=audio::prepareTemporalContext(fractional,
        musical::MusicalLoopRange{{0},{musical::ppq}},
        timeline::SampleRate{48000},timeline::SampleRate{48000},11);
    check(exactLoop.success(),"non-dyadic musical loop prepares");
    check(audio::exact::comparePositions(
              audio::exact::boundaryPosition(
                  exactLoop.prepared->loop->clockBounds.exactEnd),
              {23414,{{26,0},{41,0},false}})==0,
          "loop end preserves exact 960000/41 boundary");
    const auto& view = *exactLoop.prepared->loopView;
    static_assert(noexcept(view.contains(view.exactStart())));
    static_assert(noexcept(view.distanceToEnd(view.exactStart())));
    static_assert(noexcept(view.positionAfterWrap(view.exactEnd())));
    const audio::DspFramePosition justBefore{{23415}, {{16,0}, {41,0}, true}};
    const audio::DspFramePosition exact{{23415}, {{15,0}, {41,0}, true}};
    const audio::DspFramePosition justAfter{{23415}, {{14,0}, {41,0}, true}};
    check(view.contains(view.exactStart()) && view.contains(justBefore) &&
              !view.contains(exact) && !view.contains(justAfter),
          "prepared loop view preserves exact half-open boundaries");
    const auto distance = view.distanceToEnd(view.exactStart());
    check(distance && audio::exact::comparePositions(
              distance->value.exactPosition(), view.exactEnd().exactPosition()) == 0,
          "prepared loop view returns exact distance to end");
    const auto wrappedExact = view.positionAfterWrap(exact);
    const auto wrappedAfter = view.positionAfterWrap(justAfter);
    const auto repeatedDistance = view.distanceToEnd(view.exactStart());
    const auto repeatedWrap = view.positionAfterWrap(justAfter);
    check(wrappedExact && wrappedAfter &&
              audio::exact::comparePositions(wrappedExact->exactPosition(),
                                             view.exactStart().exactPosition()) == 0 &&
              audio::exact::comparePositions(wrappedAfter->exactPosition(),
                  audio::exact::Position{0, {{1,0}, {41,0}, false}}) == 0,
          "prepared loop view wraps exact end and rational overshoot purely");
    check(repeatedDistance && repeatedWrap &&
              audio::exact::comparePositions(repeatedDistance->value.exactPosition(),
                                             distance->value.exactPosition()) == 0 &&
              audio::exact::comparePositions(repeatedWrap->exactPosition(),
                                             wrappedAfter->exactPosition()) == 0,
          "prepared loop view is deterministic for repeated identical inputs");
}

void oneWrapPerDeviceFrameInvariant() {
    musical::MusicalTimeMap map;
    map.tempo.events[0].bpm = {400.0};
    auto temporal = audio::prepareTemporalContext(map,
        musical::MusicalLoopRange{{0}, {1024}}, timeline::SampleRate{1000},
        timeline::SampleRate{100}, 31);
    check(temporal.success(), "one-device-frame minimum loop prepares");
    const auto& loop = *temporal.prepared->loop;
    check(loop.start.value == 0.0 && loop.end.value == 10.0,
          "1024 ticks at 400 BPM exercises the exact 10 ms minimum");
    audio::RealtimeProjectClock clock;
    clock.prepare({0});
    check(clock.prepareFormat(temporal.prepared->exactClock),
          "minimum loop clock format installs");
    clock.setPlaybackPolicy(loop.clockBounds, false);
    check(clock.play(), "minimum loop makes empty clock playable");
    clock.advanceDeviceFrame();
    check(clock.consumeWrapped() &&
              audio::exact::comparePositions(clock.renderPosition().exactPosition(),
                  audio::exact::boundaryPosition(loop.clockBounds.exactStart)) == 0,
          "a device-frame-sized loop wraps exactly once");
    clock.advanceDeviceFrame();
    check(clock.consumeWrapped(),
          "the equality bound produces one boolean wrap per later device frame");
}

void loopAnchorsAtTempoAndMeterChanges() {
    musical::MusicalTimeMap tempoMap;
    tempoMap.tempo.events = {{{1}, {0}, {120.0}},
                             {{2}, {musical::ppq}, {123.0}},
                             {{3}, {2 * musical::ppq}, {60.0}}};
    tempoMap.tempo.nextId = {4};
    auto tempo = audio::prepareTemporalContext(tempoMap,
        musical::MusicalLoopRange{{musical::ppq}, {2 * musical::ppq}},
        timeline::SampleRate{48000}, timeline::SampleRate{96000}, 33);
    check(tempo.success(), "loop exactly bounded by tempo changes prepares");
    const auto start = tempo.prepared->musicalTime->exactProjectFrameAtTick(
        {musical::ppq});
    const auto end = tempo.prepared->musicalTime->exactProjectFrameAtTick(
        {2 * musical::ppq});
    check(start && end &&
              audio::exact::comparePositions(
                  tempo.prepared->loopView->exactStart().exactPosition(), start.value) == 0 &&
              audio::exact::comparePositions(
                  tempo.prepared->loopView->exactEnd().exactPosition(), end.value) == 0,
          "loop start/end consume authoritative exact tempo-change anchors");

    musical::MusicalTimeMap meterMap;
    meterMap.signatures.events = {{{1}, {0}, {3, 4}},
                                  {{2}, {1}, {7, 8}}};
    meterMap.signatures.nextId = {3};
    auto meter = audio::prepareTemporalContext(meterMap,
        musical::MusicalLoopRange{{0}, {8 * musical::ppq}},
        timeline::SampleRate{44100}, timeline::SampleRate{48000}, 34);
    check(meter.success() && meter.prepared->beats.size() >= 2,
          "3/4 to 7/8 meter change inside loop prepares one exact beat grid");
}

void fractionalClock() {
    audio::RealtimeProjectClock clock;
    clock.prepare({100});
    clock.setPlaybackPolicy(audio::RealtimeProjectClock::LoopBounds{0.25,5.75},false);
    check(clock.play(), "loop makes clock playable");
    constexpr double step = 480.0/441.0;
    long double oracle = 0.0L;
    for (int n=0;n<200000;++n) {
        clock.advance({step});
        oracle += static_cast<long double>(step);
        if (oracle >= 5.75L)
            oracle = 0.25L + std::fmod(oracle-0.25L,5.5L);
    }
    check(std::abs(clock.position().value-static_cast<double>(oracle)) < 2e-9,
          "fractional residual does not drift over many wraps");
}

void loopRenderAndMultipleWraps() {
    std::vector<float> samples(100);
    for (std::size_t i=0;i<samples.size();++i) samples[i]=static_cast<float>(i)/100.0F;
    audio::PreparedTrackView track{{1},{{samples.data(),nullptr}},1,
        {static_cast<std::uint64_t>(samples.size())},
        timeline::SampleRate{480}, {0},{100},{0},{}};
    audio::RealtimeAudioEngine engine;
    engine.configure(audio::PreparedProjectView{
        timeline::SampleRate{480},{100},std::span{&track,1}});
    auto temporal = audio::prepareTemporalContext({},
        musical::MusicalLoopRange{{0},{musical::ppq}},
        timeline::SampleRate{480},timeline::SampleRate{441},1);
    check(temporal.success(), "fractional-rate loop context prepares");
    engine.configureTemporalContext(temporal.prepared.get());
    enterOperational(engine,441);
    check(engine.tryRequestSeek({200}).accepted,
          "documentary loop end extends navigation beyond audible content");
    std::vector<float> empty;
    process(engine,empty,empty,441);
    check(engine.transportSnapshot().position.value==200,
          "Seek outside content does not require enabling the loop");
    check(engine.trySetLoopEnabled(true).accepted, "loop enable enqueues");
    process(engine,empty,empty,441);
    check(engine.tryRequestPlay().accepted, "loop accepts Play");
    std::vector<float> left(1000),right(1000);
    realtimeAllocations.store(0,std::memory_order_relaxed);
    countRealtimeAllocations.store(true,std::memory_order_relaxed);
    process(engine,left,right,441);
    countRealtimeAllocations.store(false,std::memory_order_relaxed);
    check(realtimeAllocations.load(std::memory_order_relaxed)==0,
          "loop wrapping processBlock performs no allocation");
    check(engine.transportSnapshot().playback==transport::PlaybackState::playing,
          "multiple wraps keep transport Playing");
    check(engine.transportSnapshot().position.value < 240,
          "published position is inside loop after multiple wraps");
    check(std::any_of(left.begin(),left.end(),[](float x){return x!=0.0F;}),
          "segmented loop renders audio without a gap-only callback");
}

void projectedLoopAdmission() {
    std::vector<float> samples(48000, 0.25F);
    audio::PreparedTrackView track{{1}, {{samples.data(), nullptr}}, 1,
        {samples.size()}, timeline::SampleRate{48000}, {0}, {48000}, {0}, {}};
    audio::RealtimeAudioEngine engine;
    engine.configure({timeline::SampleRate{48000}, {48000}, std::span{&track, 1}});
    auto temporal = audio::prepareTemporalContext({},
        musical::MusicalLoopRange{{0}, {musical::ppq}},
        timeline::SampleRate{48000}, timeline::SampleRate{48000}, 32);
    check(temporal.success() && engine.configureTemporalContext(temporal.prepared.get()),
          "projected-admission context configures");
    enterOperational(engine, 48000);

    check(engine.tryRequestPlay().accepted,
          "projected-admission Play is pending");
    check(!engine.trySetLoopEnabled(true).accepted,
          "pending Play rejects loop enable against projected Playing");
    check(engine.tryRequestStop().accepted &&
              engine.trySetLoopEnabled(true).accepted,
          "pending Stop permits loop enable against projected Stopped");
    std::vector<float> empty;
    process(engine, empty, empty, 48000);
    check(engine.transportSnapshot().playback == transport::PlaybackState::stopped &&
              engine.transportSnapshot().loopEnabled,
          "ordered Play Stop Enable is applied coherently");

    check(engine.tryRequestPlay().accepted,
          "enabled-loop Play is pending");
    check(!engine.trySetLoopEnabled(false).accepted,
          "pending Play rejects loop disable against projected Playing");
    process(engine, empty, empty, 48000);
    check(engine.tryRequestStop().accepted &&
              engine.trySetLoopEnabled(false).accepted,
          "pending Stop permits loop disable against projected Stopped");
    process(engine, empty, empty, 48000);
    check(!engine.transportSnapshot().loopEnabled,
          "ordered Stop Disable is applied coherently");
}

std::vector<float> renderPartitionedLoop(std::span<const std::size_t> partitions) {
    std::vector<float> samples(100);
    for (std::size_t i=0;i<samples.size();++i)
        samples[i]=static_cast<float>(i)/100.0F;
    audio::PreparedTrackView track{{1},{{samples.data(),nullptr}},1,
        {static_cast<std::uint64_t>(samples.size())},timeline::SampleRate{480},
        {0},{100},{0},{}};
    audio::RealtimeAudioEngine engine;
    engine.configure({timeline::SampleRate{480},{100},std::span{&track,1}});
    auto temporal=audio::prepareTemporalContext({},
        musical::MusicalLoopRange{{0},{musical::ppq}},
        timeline::SampleRate{480},timeline::SampleRate{441},2);
    engine.configureTemporalContext(temporal.prepared.get());
    enterOperational(engine,441);
    check(engine.trySetLoopEnabled(true).accepted,"partition loop enable");
    std::vector<float> empty;
    process(engine,empty,empty,441);
    check(engine.tryRequestPlay().accepted,"partition loop Play");
    std::size_t total{};
    for (const auto count:partitions) total+=count;
    std::vector<float> left(total),right(total);
    std::size_t offset{};
    for (const auto count:partitions) {
        processRange(engine,left,right,offset,count,441);
        offset+=count;
    }
    return left;
}

void partitionInvariance() {
    const std::array<std::size_t,1> single{1000};
    const std::array<std::size_t,8> split{64,128,256,7,201,128,128,88};
    check(renderPartitionedLoop(single)==renderPartitionedLoop(split),
          "loop render and fractional phase are callback-partition invariant");
}

std::vector<float> renderMusicalRatePair(double projectRate, double deviceRate,
    std::span<const std::size_t> partitions) {
    musical::MusicalTimeMap map;
    map.tempo.events[0].bpm={400.0};
    auto temporal=audio::prepareTemporalContext(map,
        musical::MusicalLoopRange{{0},{musical::ppq}},
        timeline::SampleRate{projectRate},timeline::SampleRate{deviceRate},21);
    check(temporal.success(),"rate-matrix temporal context prepares");
    const auto beat=temporal.prepared->musicalTime->exactProjectFrameAtTick(
        {musical::ppq});
    check(beat&&audio::exact::compareBoundary(
              beat.value,temporal.prepared->loop->clockBounds.exactEnd)==0,
          "loop and metronome share exact musical anchor");

    audio::RealtimeAudioEngine engine;
    engine.configure({timeline::SampleRate{projectRate},{0},
        std::span<const audio::PreparedTrackView>{}});
    check(engine.configureTemporalContext(temporal.prepared.get()),
          "rate-matrix temporal context installs");
    enterOperational(engine,deviceRate);
    check(engine.trySetLoopEnabled(true).accepted&&
          engine.trySetMetronomeEnabled(true).accepted,
          "rate-matrix loop and metronome enable");
    check(engine.trySetMetronomeLevel({0.0F}).accepted,
          "rate-matrix metronome unity");
    std::vector<float> empty;
    process(engine,empty,empty,deviceRate);
    std::vector<float> settle(512),settleRight(512);
    process(engine,settle,settleRight,deviceRate);
    check(engine.tryRequestPlay().accepted,"rate-matrix Play");
    std::size_t total{};for(auto count:partitions)total+=count;
    std::vector<float> left(total),right(total);
    std::size_t offset{};
    for(auto count:partitions){
        processRange(engine,left,right,offset,count,deviceRate);offset+=count;
    }
    check(std::any_of(left.begin(),left.end(),[](float sample){return sample!=0.0F;}),
          "rate-matrix metronome produces clicks");
    return left;
}

void musicalRateMatrixRegression() {
    const std::array<std::size_t,1> single{30000};
    const std::array<std::size_t,8> split{64,128,256,512,1024,4096,8192,15728};
    for(const auto rates:{std::pair{44100.0,48000.0},
                          std::pair{48000.0,96000.0},
                          std::pair{96000.0,44100.0}})
        check(renderMusicalRatePair(rates.first,rates.second,single)==
              renderMusicalRatePair(rates.first,rates.second,split),
              "musical anchors and click samples are rate/partition invariant");
}

std::vector<float> renderExactMusicalLoop(
    std::span<const std::size_t> partitions) {
    std::vector<float> samples(30000);
    for (std::size_t index=0; index<samples.size(); ++index)
        samples[index]=static_cast<float>(index+1)/30001.0F;
    audio::PreparedTrackView track{{1},{{samples.data(),nullptr}},1,
        {static_cast<std::uint64_t>(samples.size())},
        timeline::SampleRate{48000},{0},{30000},{0},{}};
    audio::RealtimeAudioEngine engine;
    engine.configure({timeline::SampleRate{48000},{30000},
        std::span{&track,1}});
    musical::MusicalTimeMap map;
    map.tempo.events[0].bpm={123.0};
    auto temporal=audio::prepareTemporalContext(map,
        musical::MusicalLoopRange{{0},{musical::ppq}},
        timeline::SampleRate{48000},timeline::SampleRate{48000},12);
    check(temporal.success(),"exact 123 BPM runtime loop prepares");
    check(engine.configureTemporalContext(temporal.prepared.get()),
          "exact runtime loop clock is certified with renderer");
    enterOperational(engine,48000);
    check(engine.trySetLoopEnabled(true).accepted,"exact loop enable");
    std::vector<float> empty;
    process(engine,empty,empty,48000);
    check(engine.tryRequestPlay().accepted,"exact loop Play");
    std::size_t total{};
    for (const auto count:partitions) total+=count;
    std::vector<float> left(total),right(total);
    std::size_t offset{};
    realtimeAllocations.store(0,std::memory_order_relaxed);
    countRealtimeAllocations.store(true,std::memory_order_relaxed);
    for (const auto count:partitions) {
        processRange(engine,left,right,offset,count,48000);
        offset+=count;
    }
    countRealtimeAllocations.store(false,std::memory_order_relaxed);
    check(realtimeAllocations.load(std::memory_order_relaxed)==0,
          "exact non-dyadic loop processing allocates nothing in RT");
    check(left[23414]>left[23415]*1000.0F && left[23415]>0.0F,
          "last interior sample renders and first exterior sample wraps");
    check(engine.transportSnapshot().position.value==1,
          "fractional musical loop wraps on the exact device sample");
    return left;
}

void crossedLoopStartMetronome() {
    const auto render = [](std::size_t partition) {
        musical::MusicalTimeMap map;
        map.tempo.events[0].bpm = {123.0};
        auto temporal = audio::prepareTemporalContext(map,
            musical::MusicalLoopRange{{0}, {musical::ppq}},
            timeline::SampleRate{48000}, timeline::SampleRate{48000}, 1);
        check(temporal.success(), "crossed-event loop prepares");
        audio::RealtimeAudioEngine engine;
        engine.configure({timeline::SampleRate{48000}, {0},
                          std::span<const audio::PreparedTrackView>{}});
        check(engine.configureTemporalContext(temporal.prepared.get()), "crossed-event context");
        enterOperational(engine, 48000);
        check(engine.trySetLoopEnabled(true).accepted && engine.trySetMetronomeEnabled(true).accepted,
              "simultaneous loop/metronome");
        check(engine.trySetMetronomeLevel({0.0F}).accepted, "unity metronome");
        std::vector<float> empty;
        process(engine, empty, empty, 48000);
        std::vector<float> settle(512), settleRight(512);
        process(engine, settle, settleRight, 48000); // settle level while Stopped
        check(engine.tryRequestPlay().accepted, "crossed-event Play");
        constexpr std::size_t frames = 100000;
        std::vector<float> result(frames), right(frames);
        realtimeAllocations.store(0);
        for (std::size_t offset = 0; offset < frames;) {
            const auto count = std::min(partition, frames - offset);
            std::array<float*, 2> outputs{result.data()+offset, right.data()+offset};
            countRealtimeAllocations.store(true);
            engine.processBlock({outputs.data(), 2, count}, timeline::SampleRate{48000});
            countRealtimeAllocations.store(false);
            offset += count;
        }
        check(realtimeAllocations.load() == 0, "crossed-event scheduling allocates nothing");
        // Independent integer oracle: the first device sample in lap k is
        // ceil(k * 960000 / 41). Tables give an audio oracle (including silence
        // between voices); duplicate events would change the summed waveform.
        for (std::size_t frame = 0; frame < frames; ++frame) {
            float expected = 0;
            for (std::size_t lap = 0; lap <= 4; ++lap) {
                const auto trigger = (lap * 960000 + 40) / 41;
                if (frame >= trigger && frame-trigger < temporal.prepared->clicks.frameCount)
                    expected += temporal.prepared->clicks.accent[frame-trigger];
            }
            check(std::abs(result[frame]-expected) < 1.0e-6F,
                  "one audible loop-start event per lap, including fractional overshoot");
        }
        return result;
    };
    const auto reference = render(100000);
    check(reference == render(127) && reference == render(1024),
          "loop-start events are callback-partition independent");
}

void pendingLoopStartSurvivesHardRebuild() {
    musical::MusicalTimeMap map;
    map.tempo.events[0].bpm = {123.0};
    auto temporal = audio::prepareTemporalContext(map,
        musical::MusicalLoopRange{{0}, {musical::ppq}},
        timeline::SampleRate{48000}, timeline::SampleRate{48000}, 41);
    check(temporal.success(), "pending-boundary context prepares");
    audio::RealtimeAudioEngine engine;
    engine.configure({timeline::SampleRate{48000}, {0},
                      std::span<const audio::PreparedTrackView>{}});
    check(engine.configureTemporalContext(temporal.prepared.get()),
          "pending-boundary context configures");
    enterOperational(engine, 48000);
    check(engine.trySetLoopEnabled(true).accepted &&
              engine.trySetMetronomeEnabled(true).accepted &&
              engine.trySetMetronomeLevel({0.0F}).accepted,
          "pending-boundary session controls enqueue");
    std::vector<float> empty;
    process(engine, empty, empty, 48000);
    std::vector<float> settle(512), settleRight(512);
    process(engine, settle, settleRight, 48000);
    check(engine.tryRequestPlay().accepted, "pending-boundary Play");
    std::vector<float> lap(23415), lapRight(23415);
    process(engine, lap, lapRight, 48000);

    engine.deviceInitialisingPreservingTransport();
    const auto checkpoint = engine.temporalCheckpoint();
    check(checkpoint.pendingMetronomeBoundary.crossedLoopStart,
          "wrap at callback end leaves one logical boundary obligation");
    check(engine.configureTemporalContext(temporal.prepared.get()) &&
              engine.restoreTemporalCheckpoint(checkpoint),
          "hard rebuild restores the logical boundary obligation");

    std::vector<float> after(256), afterRight(256);
    process(engine, after, afterRight, 48000);
    for (std::size_t frame = 0; frame < after.size(); ++frame)
        check(after[frame] == temporal.prepared->clicks.accent[frame],
              "restored loop-start click is emitted exactly once");
    check(!engine.temporalCheckpoint().pendingMetronomeBoundary.crossedLoopStart &&
              !engine.temporalCheckpoint().pendingMetronomeBoundary.eventPending,
          "restored musical obligation is consumed once");
}

void pendingBeatSurvivesHardRebuild(bool accent) {
    musical::MusicalTimeMap map;
    map.tempo.events[0].bpm = {123.0};
    auto temporal = audio::prepareTemporalContext(
        map, std::nullopt, timeline::SampleRate{48000},
        timeline::SampleRate{48000}, accent ? 43 : 42);
    check(temporal.success(), "pending beat rebuild context prepares");
    makeDiagnosticClicks(*temporal.prepared);
    audio::RealtimeAudioEngine engine;
    engine.configure({timeline::SampleRate{48000}, {0},
                      std::span<const audio::PreparedTrackView>{}});
    check(engine.configureTemporalContext(temporal.prepared.get()),
          "pending beat rebuild context configures");
    enterOperational(engine, 48000);
    check(engine.trySetMetronomeEnabled(true).accepted &&
              engine.trySetMetronomeLevel({0.0F}).accepted,
          "pending beat rebuild controls enqueue");
    std::vector<float> empty;
    process(engine, empty, empty, 48000);
    std::vector<float> settle(512), settleRight(512);
    process(engine, settle, settleRight, 48000);
    if (accent) {
        const auto bar = temporal.prepared->musicalTime->exactProjectFrameAtTick(
            {4 * musical::ppq});
        check(bool(bar), "pending accent bar is representable");
        const auto floor = audio::exact::floorPosition(bar.value);
        check(!audio::exact::zero(audio::exact::wide(floor.remainder)) &&
                  engine.tryRequestSeek({floor.frame}).accepted,
              "pending accent seeks immediately before fractional downbeat");
        process(engine, empty, empty, 48000);
    }
    check(engine.tryRequestPlay().accepted, "pending beat rebuild Play");
    std::vector<float> before(accent ? 1 : 23415);
    std::vector<float> beforeRight(before.size());
    process(engine, before, beforeRight, 48000);
    const auto pending = engine.temporalCheckpoint();
    check(pending.pendingMetronomeBoundary.eventPending &&
              pending.pendingMetronomeBoundary.accent == accent,
          "real scheduler leaves the expected normal/accent obligation");

    engine.deviceInitialisingPreservingTransport();
    check(engine.configureTemporalContext(temporal.prepared.get()) &&
              engine.restoreTemporalCheckpoint(pending),
          "hard rebuild restores pending normal/accent obligation");
    std::vector<float> after(2), afterRight(2);
    process(engine, after, afterRight, 48000);
    check(after[0] == (accent ? 2.0F : 1.0F) && after[1] == 0.0F &&
              !engine.temporalCheckpoint()
                   .pendingMetronomeBoundary.eventPending,
          "hard rebuild emits pending normal/accent exactly once");
}

void exactMusicalLoopAndMetronome() {
    const std::array<std::size_t,1> single{23416};
    const std::array<std::size_t,5> split{511,1024,8192,10000,3689};
    check(renderExactMusicalLoop(single)==renderExactMusicalLoop(split),
          "non-dyadic musical loop is callback-partition invariant");

    musical::MusicalTimeMap map;
    map.tempo.events={{{1},{0},{120.0}},{{2},{musical::ppq},{123.0}}};
    map.tempo.nextId={3};
    auto temporal=audio::prepareTemporalContext(map,std::nullopt,
        timeline::SampleRate{48000},timeline::SampleRate{48000},13);
    check(temporal.success(),"exact metronome tempo-change context prepares");
    for (const auto& segment:temporal.prepared->beats) {
        if (segment.firstTick>=segment.endTick) continue;
        const auto authoritative=temporal.prepared->musicalTime->exactProjectFrameAtTick(
            {segment.firstTick});
        check(authoritative && audio::exact::comparePositions(
                  authoritative.value,segment.positionAt(segment.firstTick))==0,
              "meter-derived beat ticks use the authoritative exact tempo mapping");
    }
    audio::RealtimeAudioEngine engine;
    engine.configure({timeline::SampleRate{48000},{0},
        std::span<const audio::PreparedTrackView>{}});
    check(engine.configureTemporalContext(temporal.prepared.get()),
          "exact metronome context configures");
    enterOperational(engine,48000);
    check(engine.trySetMetronomeEnabled(true).accepted,
          "exact metronome enable");
    std::vector<float> empty;
    process(engine,empty,empty,48000);
    check(engine.tryRequestPlay().accepted,"exact metronome Play");
    std::vector<float> left(47418),right(47418);
    process(engine,left,right,48000);
    // First post-change beat is 24000 + 960000/41 = 47414 + 26/41.
    check(left[47414]==0.0F && left[47415]==0.0F &&
          std::abs(left[47416])>0.0F,
          "metronome event starts at first device sample at/after exact beat");

    musical::MusicalTimeMap projectionMap;
    projectionMap.tempo.events[0].bpm={123.0};
    auto projectionTemporal=audio::prepareTemporalContext(projectionMap,
        musical::MusicalLoopRange{{musical::ppq},{2*musical::ppq}},
        timeline::SampleRate{48000},timeline::SampleRate{48000},14);
    check(projectionTemporal.success(),"fractional projection loop prepares");
    audio::RealtimeAudioEngine projection;
    projection.configure({timeline::SampleRate{48000},{0},
        std::span<const audio::PreparedTrackView>{}});
    check(projection.configureTemporalContext(projectionTemporal.prepared.get()),
          "fractional projection context configures");
    enterOperational(projection,48000);
    check(projection.trySetLoopEnabled(true).accepted,"projection loop enable");
    process(projection,empty,empty,48000);
    check(projection.tryRequestSeek({60000}).accepted,"seek beyond loop while stopped");
    process(projection,empty,empty,48000);
    check(projection.tryRequestPlay().accepted,"projection loop Play");
    const auto projected=projection.projectedTransportSnapshot();
    check(projected.position.value==23415,
          "projection rounds the exact prepared loop start");
    process(projection,empty,empty,48000);
    check(projection.transportSnapshot().position.value==23415 &&
          projection.transportSnapshot().position==projected.position,
          "RT and projection consume the same exact loop boundary");
}

void temporalReregistrationPreservesClock() {
    audio::RealtimeAudioEngine engine;
    engine.configure({timeline::SampleRate{48000},{0},
        std::span<const audio::PreparedTrackView>{}});
    auto first=audio::prepareTemporalContext({},std::nullopt,
        timeline::SampleRate{48000},timeline::SampleRate{48000},1);
    engine.configureTemporalContext(first.prepared.get());
    enterOperational(engine,48000);
    check(engine.trySetMetronomeEnabled(true).accepted,"lifecycle metronome enable");
    std::vector<float> empty;
    process(engine,empty,empty,48000);
    check(engine.tryRequestPlay().accepted,"lifecycle Play");
    std::vector<float> left(128),right(128);
    process(engine,left,right,48000);
    check(engine.tryRequestPause().accepted,"lifecycle Pause");
    process(engine,empty,empty,48000);
    const auto checkpoint=engine.temporalCheckpoint();
    check(checkpoint.clock.position.value==128,
          "checkpoint captures stopped-thread musical position");

    auto replacement=audio::prepareTemporalContext({},std::nullopt,
        timeline::SampleRate{48000},timeline::SampleRate{48000},2);
    engine.deviceStopped();
    engine.configureTemporalContext(replacement.prepared.get());
    engine.restoreTemporalCheckpoint(checkpoint);
    engine.deviceInitialisingPreservingTransport();
    process(engine,empty,empty,48000);
    const auto restored=engine.transportSnapshot();
    check(restored.position.value==128 &&
          restored.playback==transport::PlaybackState::paused &&
          restored.temporalRevision==2,
          "controlled callback re-registration preserves clock and publishes new revision");
}

void metronomeAndEmptyPolicy() {
    audio::RealtimeAudioEngine engine;
    engine.configure(audio::PreparedProjectView{timeline::SampleRate{48000},{0},
        std::span<const audio::PreparedTrackView>{}});
    auto temporal = audio::prepareTemporalContext({},std::nullopt,
        timeline::SampleRate{48000},timeline::SampleRate{48000},9);
    check(temporal.success(), "metronome context prepares");
    engine.configureTemporalContext(temporal.prepared.get());
    enterOperational(engine,48000);
    check(!engine.tryRequestPlay().accepted,"empty project rejects Play by default");
    check(engine.trySetMetronomeEnabled(true).accepted,"metronome enable enqueues");
    std::vector<float> zero;
    process(engine,zero,zero,48000);
    check(engine.tryRequestPlay().accepted,"metronome makes empty project playable");
    std::vector<float> left(25000),right(25000);
    process(engine,left,right,48000);
    check(engine.transportSnapshot().playing &&
          engine.transportSnapshot().metronomeEnabled,
          "metronome-only transport runs until Stop");
    float firstPeak{},secondPeak{};
    for(std::size_t i=0;i<200;++i) firstPeak=std::max(firstPeak,std::abs(left[i]));
    for(std::size_t i=24000;i<24200;++i) secondPeak=std::max(secondPeak,std::abs(left[i]));
    check(firstPeak>secondPeak && secondPeak>0.01F,
          "bar-start accent and normal beat are audible at exact boundaries");
    check(engine.meterSnapshot().master.left>0.0F,
          "metronome is included in master meter");
    check(engine.tryRequestPause().accepted,"pause enqueues");
    process(engine,zero,zero,48000);
    const auto paused=engine.transportSnapshot().position;
    std::vector<float> silent(128),silentR(128);
    process(engine,silent,silentR,48000);
    check(engine.transportSnapshot().position==paused &&
          std::all_of(silent.begin(),silent.end(),[](float x){return x==0.0F;}),
          "Pause emits no new click and does not advance");

    check(engine.tryRequestStop().accepted,"first Stop after Pause enqueues");
    process(engine,zero,zero,48000);
    check(engine.transportSnapshot().position==paused &&
          engine.transportSnapshot().playback==transport::PlaybackState::stopped,
          "first Stop preserves position and clears active voices");
    check(engine.tryRequestStop().accepted,"second Stop enqueues");
    process(engine,zero,zero,48000);
    check(engine.transportSnapshot().position.value==0,
          "second Stop rewinds metronome transport");

    audio::RealtimeAudioEngine boundaryEngine;
    boundaryEngine.configure({timeline::SampleRate{48000},{0},
        std::span<const audio::PreparedTrackView>{}});
    boundaryEngine.configureTemporalContext(temporal.prepared.get());
    enterOperational(boundaryEngine,48000);
    check(boundaryEngine.trySetMetronomeEnabled(true).accepted,
          "boundary metronome enable");
    process(boundaryEngine,zero,zero,48000);
    check(boundaryEngine.tryRequestPlay().accepted,"boundary metronome Play");
    std::vector<float> exact(24000),exactR(24000);
    process(boundaryEngine,exact,exactR,48000);
    std::vector<float> next(2),nextR(2);
    process(boundaryEngine,next,nextR,48000);
    check(next[0]==0.0F && std::abs(next[1])>0.0F,
          "beat on callback boundary is emitted once by the following block");
}

void meterAccentMatrix() {
    const auto checkMeter = [](unsigned numerator, unsigned denominator,
                               double bpm) {
        musical::MusicalTimeMap map;
        map.tempo.events[0].bpm = {bpm};
        map.signatures.events[0].signature = {numerator, denominator};
        auto oracle = audio::prepareTemporalContext(
            map, std::nullopt, timeline::SampleRate{48000},
            timeline::SampleRate{48000}, 72);
        check(oracle.success(), "meter oracle prepares");
        const auto ticksPerBeat = musical::ppq * 4 / denominator;
        std::vector<std::pair<std::size_t, float>> expected;
        for (unsigned beat = 0; beat < numerator; ++beat) {
            expected.push_back({exactTriggerFrame(*oracle.prepared,
                    static_cast<std::int64_t>(beat) * ticksPerBeat),
                beat == 0 ? 2.0F : 1.0F});
        }
        const auto frames = expected.back().first + 2;
        const auto rendered = renderMetronomeGrid(map, 48000, frames, {}, true);
        checkDiagnosticEvents(rendered.left, expected,
                              "meter emits one accent then denominator-unit beats");
        if (numerator == 7 && denominator == 8) {
            check(std::count_if(rendered.left.begin(), rendered.left.end(),
                                [](float value) { return value == 2.0F; }) == 1,
                  "7/8 produces one accent and no implicit 2+2+3 grouping");
        }
    };
    checkMeter(4, 4, 120.0);
    checkMeter(3, 4, 120.0);
    checkMeter(3, 4, 123.0);
    checkMeter(7, 8, 120.0);
    checkMeter(7, 8, 123.0);

    musical::MusicalTimeMap changes;
    changes.signatures.events = {{{1}, {0}, {4, 4}},
                                  {{2}, {1}, {3, 4}},
                                  {{3}, {2}, {7, 8}}};
    changes.signatures.nextId = {4};
    auto oracle = audio::prepareTemporalContext(
        changes, std::nullopt, timeline::SampleRate{48000},
        timeline::SampleRate{48000}, 73);
    check(oracle.success(), "meter-change oracle prepares");
    const std::array accentTicks{std::int64_t{0},
                                 std::int64_t{4 * musical::ppq},
                                 std::int64_t{7 * musical::ppq}};
    std::vector<std::pair<std::size_t, float>> expected;
    for (const auto tick : accentTicks)
        expected.push_back({exactTriggerFrame(*oracle.prepared, tick), 2.0F});
    const auto rendered = renderMetronomeGrid(
        changes, 48000, expected.back().first + 2, {}, true);
    for (const auto [frame, value] : expected)
        check(rendered.left[frame] == value,
              "new time-signature bar boundary is accented");
}

void tempoRateAndPartitionMatrix() {
    const std::array<double, 4> tempos{
        120.0, 123.0, std::nextafter(123.0, INFINITY),
        std::nextafter(123.0, -INFINITY)};
    for (const auto rate : {44100.0, 48000.0, 96000.0}) {
        for (const auto bpm : tempos) {
            musical::MusicalTimeMap map;
            map.tempo.events[0].bpm = {bpm};
            auto oracle = audio::prepareTemporalContext(
                map, std::nullopt, timeline::SampleRate{rate},
                timeline::SampleRate{rate}, 74);
            check(oracle.success(), "tempo/rate oracle prepares");
            std::vector<std::pair<std::size_t, float>> expected;
            for (std::int64_t beat = 0; beat < 3; ++beat)
                expected.push_back({exactTriggerFrame(
                    *oracle.prepared, beat * musical::ppq),
                    beat == 0 ? 2.0F : 1.0F});
            const auto rendered = renderMetronomeGrid(
                map, rate, expected.back().first + 2, {}, true);
            checkDiagnosticEvents(rendered.left, expected,
                                  "tempo/rate trigger equals rational ceil");
        }
    }

    musical::MusicalTimeMap changes;
    changes.tempo.events = {{{1}, {0}, {120.0}},
                             {{2}, {musical::ppq}, {123.0}},
                             {{3}, {musical::ppq + musical::ppq / 2},
                                   {std::nextafter(123.0, INFINITY)}}};
    changes.tempo.nextId = {4};
    auto oracle = audio::prepareTemporalContext(
        changes, std::nullopt, timeline::SampleRate{48000},
        timeline::SampleRate{48000}, 75);
    check(oracle.success(), "on/off-beat tempo changes prepare");
    std::vector<std::pair<std::size_t, float>> expected;
    for (std::int64_t beat = 0; beat < 4; ++beat)
        expected.push_back({exactTriggerFrame(*oracle.prepared,
                            beat * musical::ppq),
                            beat == 0 ? 2.0F : 1.0F});
    const auto changed = renderMetronomeGrid(
        changes, 48000, expected.back().first + 2, {}, true);
    checkDiagnosticEvents(changed.left, expected,
                          "tempo changes retain exact beat triggers");

    musical::MusicalTimeMap partitionMap;
    partitionMap.tempo.events[0].bpm = {123.0};
    const auto frames = std::size_t{72000};
    const std::array<std::size_t, 1> one{1};
    const std::array<std::size_t, 1> p127{127};
    const std::array<std::size_t, 1> p256{256};
    const std::array<std::size_t, 1> p1024{1024};
    const std::array<std::size_t, 7> irregular{3, 127, 19, 256, 1, 1024, 71};
    const auto pcm = renderMetronomeGrid(partitionMap, 48000, frames, {}, false);
    const auto triggers = renderMetronomeGrid(partitionMap, 48000, frames, {}, true);
    for (const auto partitions : {
             std::span<const std::size_t>{one},
             std::span<const std::size_t>{p127},
             std::span<const std::size_t>{p256},
             std::span<const std::size_t>{p1024},
             std::span<const std::size_t>{irregular}}) {
        check(renderMetronomeGrid(partitionMap, 48000, frames,
                                  partitions, false).left == pcm.left,
              "real click PCM is callback-partition invariant");
        check(renderMetronomeGrid(partitionMap, 48000, frames,
                                  partitions, true).left == triggers.left,
              "logical 0/1/2 triggers are callback-partition invariant");
    }
}

void pauseResumeMetronomeContract() {
    const auto prepare = [](double bpm, bool diagnostic) {
        musical::MusicalTimeMap map;
        map.tempo.events[0].bpm = {bpm};
        auto result = audio::prepareTemporalContext(
            map, std::nullopt, timeline::SampleRate{48000},
            timeline::SampleRate{48000}, 76);
        check(result.success(), "pause context prepares");
        if (diagnostic) makeDiagnosticClicks(*result.prepared);
        return std::move(result.prepared);
    };
    const auto configure = [](audio::RealtimeAudioEngine& engine,
                              const audio::PreparedTemporalContext* temporal) {
        engine.configure({timeline::SampleRate{48000}, {0},
                          std::span<const audio::PreparedTrackView>{}});
        check(engine.configureTemporalContext(temporal),
              "pause context configures");
        enterOperational(engine, 48000);
        check(engine.trySetMetronomeEnabled(true).accepted &&
                  engine.trySetMetronomeLevel({0.0F}).accepted,
              "pause controls enqueue");
        std::vector<float> empty;
        process(engine, empty, empty, 48000);
        std::vector<float> settle(512), settleRight(512);
        process(engine, settle, settleRight, 48000);
        check(engine.tryRequestPlay().accepted, "pause fixture Play");
    };

    auto real = prepare(120.0, false);
    audio::RealtimeAudioEngine voiceEngine;
    configure(voiceEngine, real.get());
    std::vector<float> firstHalf(96), firstHalfRight(96);
    process(voiceEngine, firstHalf, firstHalfRight, 48000);
    check(std::any_of(firstHalf.begin(), firstHalf.end(),
                      [](float value) { return value != 0.0F; }),
          "pause fixture starts a real four-millisecond click");
    check(voiceEngine.tryRequestPause().accepted, "pause mid-voice");
    std::vector<float> empty;
    process(voiceEngine, empty, empty, 48000);
    check(voiceEngine.tryRequestPlay().accepted, "resume after mid-voice pause");
    std::vector<float> afterResume(192), afterResumeRight(192);
    process(voiceEngine, afterResume, afterResumeRight, 48000);
    check(std::all_of(afterResume.begin(), afterResume.end(),
                      [](float value) { return value == 0.0F; }),
          "Resume does not continue the cancelled click tail");

    auto diagnostic = prepare(123.0, true);
    audio::RealtimeAudioEngine pendingEngine;
    configure(pendingEngine, diagnostic.get());
    std::vector<float> toFractionalBeat(23415), beatRight(23415);
    process(pendingEngine, toFractionalBeat, beatRight, 48000);
    const auto beforePause = pendingEngine.temporalCheckpoint();
    check(beforePause.pendingMetronomeBoundary.eventPending &&
              !beforePause.pendingMetronomeBoundary.accent,
          "real scheduler creates a legitimate fractional pending beat");
    check(pendingEngine.tryRequestPause().accepted,
          "Pause with legitimate pending event");
    process(pendingEngine, empty, empty, 48000);
    check(pendingEngine.temporalCheckpoint().pendingMetronomeBoundary ==
              beforePause.pendingMetronomeBoundary,
          "Pause preserves only the legitimate pending obligation");
    check(pendingEngine.tryRequestPlay().accepted,
          "Resume with legitimate pending event");
    std::vector<float> consumed(2), consumedRight(2);
    process(pendingEngine, consumed, consumedRight, 48000);
    check(consumed[0] == 1.0F && consumed[1] == 0.0F &&
              !pendingEngine.temporalCheckpoint()
                   .pendingMetronomeBoundary.eventPending,
          "Resume consumes a legitimate pending event exactly once");

    audio::RealtimeAudioEngine noPendingEngine;
    configure(noPendingEngine, diagnostic.get());
    std::vector<float> awayFromBeat(128), awayRight(128);
    process(noPendingEngine, awayFromBeat, awayRight, 48000);
    check(!noPendingEngine.temporalCheckpoint()
               .pendingMetronomeBoundary.eventPending,
          "no-pending fixture is between beats");
    check(noPendingEngine.tryRequestPause().accepted, "Pause without pending");
    process(noPendingEngine, empty, empty, 48000);
    check(noPendingEngine.tryRequestPlay().accepted, "Resume without pending");
    std::vector<float> noInvented(128), noInventedRight(128);
    process(noPendingEngine, noInvented, noInventedRight, 48000);
    check(std::all_of(noInvented.begin(), noInvented.end(),
                      [](float value) { return value == 0.0F; }),
          "Resume without pending does not invent a click");

    audio::RealtimeAudioEngine stopEngine;
    configure(stopEngine, diagnostic.get());
    std::vector<float> stopPending(23415), stopPendingRight(23415);
    process(stopEngine, stopPending, stopPendingRight, 48000);
    check(stopEngine.temporalCheckpoint()
              .pendingMetronomeBoundary.eventPending,
          "Stop fixture obtains pending from the real scheduler");
    check(stopEngine.tryRequestStop().accepted,
          "Stop with pending obligation is accepted");
    process(stopEngine, empty, empty, 48000);
    check(stopEngine.temporalCheckpoint().pendingMetronomeBoundary ==
              audio::RealtimeAudioEngine::PendingMetronomeBoundaryState{},
          "Stop clears voices and every pending obligation");

    audio::RealtimeAudioEngine seekPendingEngine;
    configure(seekPendingEngine, diagnostic.get());
    std::vector<float> seekPending(23415), seekPendingRight(23415);
    process(seekPendingEngine, seekPending, seekPendingRight, 48000);
    check(seekPendingEngine.tryRequestPause().accepted,
          "Seek-pending fixture pauses");
    process(seekPendingEngine, empty, empty, 48000);
    check(seekPendingEngine.tryRequestSeek({100}).accepted,
          "Seek after Pause with pending is accepted");
    process(seekPendingEngine, empty, empty, 48000);
    check(seekPendingEngine.temporalCheckpoint().pendingMetronomeBoundary ==
              audio::RealtimeAudioEngine::PendingMetronomeBoundaryState{},
          "Seek clears voices and every pending obligation");
}

void loopBoundaryMetronomeMatrix() {
    const auto render = [](musical::MusicalLoopRange loop,
                           std::size_t frames) {
        auto temporal = audio::prepareTemporalContext(
            {}, loop, timeline::SampleRate{48000},
            timeline::SampleRate{48000}, 81);
        check(temporal.success(), "loop-boundary metronome prepares");
        makeDiagnosticClicks(*temporal.prepared);
        audio::RealtimeAudioEngine engine;
        engine.configure({timeline::SampleRate{48000}, {0},
                          std::span<const audio::PreparedTrackView>{}});
        check(engine.configureTemporalContext(temporal.prepared.get()),
              "loop-boundary metronome configures");
        enterOperational(engine, 48000);
        check(engine.trySetLoopEnabled(true).accepted &&
                  engine.trySetMetronomeEnabled(true).accepted &&
                  engine.trySetMetronomeLevel({0.0F}).accepted,
              "loop-boundary controls enqueue");
        std::vector<float> empty;
        process(engine, empty, empty, 48000);
        std::vector<float> settle(512), settleRight(512);
        process(engine, settle, settleRight, 48000);
        check(engine.tryRequestPlay().accepted, "loop-boundary Play");
        std::vector<float> left(frames), right(frames);
        process(engine, left, right, 48000);
        return left;
    };

    const auto normalStart = render(
        {{musical::ppq}, {2 * musical::ppq}}, 48002);
    check(normalStart[0] == 2.0F && normalStart[24000] == 1.0F &&
              normalStart[48000] == 1.0F,
          "loopStart on a normal beat emits that normal beat once per lap");

    const auto nonBeatStart = render(
        {{musical::ppq / 2}, {musical::ppq + musical::ppq / 2}},
        36002);
    check(nonBeatStart[0] == 2.0F && nonBeatStart[24000] == 1.0F &&
              nonBeatStart[36000] == 0.0F,
          "loopStart off-grid invents no click at wrap");

    const auto beatEnd = render({{0}, {musical::ppq}}, 24002);
    check(beatEnd[0] == 2.0F && beatEnd[24000] == 2.0F,
          "loopEnd beat is excluded and shared loopStart downbeat emits once");
}

void metronomeTransportAndLevelMatrix() {
    musical::MusicalTimeMap map;
    map.tempo.events[0].bpm = {123.0};
    auto temporal = audio::prepareTemporalContext(
        map, std::nullopt, timeline::SampleRate{48000},
        timeline::SampleRate{48000}, 77);
    check(temporal.success(), "transport matrix context prepares");
    makeDiagnosticClicks(*temporal.prepared);
    audio::RealtimeAudioEngine engine;
    engine.configure({timeline::SampleRate{48000}, {0},
                      std::span<const audio::PreparedTrackView>{}});
    check(engine.configureTemporalContext(temporal.prepared.get()),
          "transport matrix context configures");
    enterOperational(engine, 48000);
    check(engine.trySetMetronomeEnabled(true).accepted &&
              engine.trySetMetronomeEnabled(false).accepted &&
              engine.trySetMetronomeEnabled(true).accepted,
          "metronome enable/disable is accepted while Stopped");
    std::vector<float> empty;
    process(engine, empty, empty, 48000);
    check(engine.tryRequestPlay().accepted, "transport matrix Play");
    std::vector<float> opening(23415), openingRight(23415);
    process(engine, opening, openingRight, 48000);
    check(engine.temporalCheckpoint().pendingMetronomeBoundary.eventPending,
          "transport matrix reaches a real pending beat");
    check(engine.tryRequestPause().accepted, "transport matrix Pause");
    process(engine, empty, empty, 48000);
    check(engine.trySetMetronomeEnabled(false).accepted &&
              engine.trySetMetronomeEnabled(true).accepted,
          "metronome enable/disable is accepted while Paused");
    process(engine, empty, empty, 48000);
    check(!engine.temporalCheckpoint().pendingMetronomeBoundary.eventPending,
          "metronome-off clears pending scheduling while Paused");
    check(engine.tryRequestPlay().accepted, "transport matrix Resume");
    process(engine, empty, empty, 48000);
    check(engine.trySetMetronomeEnabled(false).accepted,
          "metronome disable is accepted while Playing");
    process(engine, empty, empty, 48000);
    const auto open = engine.temporalCheckpoint();
    check(open.clock.playback == transport::PlaybackState::playing &&
              open.runUntilStop && !open.metronomeEnabled,
          "disable during open playback neither Stops nor clears runUntilStop");
    check(engine.tryRequestStop().accepted, "Stop ends open playback");
    process(engine, empty, empty, 48000);
    check(engine.transportSnapshot().playback ==
              transport::PlaybackState::stopped &&
              !engine.tryRequestPlay().accepted,
          "later empty Play is rejected while metronome is disabled");
    check(engine.tryRequestStop().accepted, "second Stop is accepted");
    process(engine, empty, empty, 48000);
    check(engine.transportSnapshot().position.value == 0,
          "second Stop rewinds to zero");

    auto seekTemporal = audio::prepareTemporalContext(
        {}, std::nullopt, timeline::SampleRate{48000},
        timeline::SampleRate{48000}, 78);
    check(seekTemporal.success(), "seek metronome context prepares");
    makeDiagnosticClicks(*seekTemporal.prepared);
    audio::RealtimeAudioEngine seekEngine;
    seekEngine.configure({timeline::SampleRate{48000}, {0},
                          std::span<const audio::PreparedTrackView>{}});
    check(seekEngine.configureTemporalContext(seekTemporal.prepared.get()),
          "seek metronome context configures");
    enterOperational(seekEngine, 48000);
    check(seekEngine.trySetMetronomeEnabled(true).accepted &&
              seekEngine.trySetMetronomeLevel({0.0F}).accepted,
          "seek metronome enable");
    process(seekEngine, empty, empty, 48000);
    std::vector<float> seekSettle(512), seekSettleRight(512);
    process(seekEngine, seekSettle, seekSettleRight, 48000);
    const auto renderFrom = [&](std::int64_t position, std::size_t frames) {
        check(seekEngine.tryRequestSeek({position}).accepted,
              "Stopped metronome Seek accepted");
        process(seekEngine, empty, empty, 48000);
        check(seekEngine.tryRequestPlay().accepted, "Play after metronome Seek");
        std::vector<float> left(frames), right(frames);
        process(seekEngine, left, right, 48000);
        check(seekEngine.tryRequestStop().accepted, "Stop after metronome Seek");
        process(seekEngine, empty, empty, 48000);
        return left;
    };
    check(renderFrom(24000, 2)[0] == 1.0F,
          "Seek exactly to beat emits that beat once");
    const auto before = renderFrom(23999, 3);
    check(before[0] == 0.0F && before[1] == 1.0F,
          "Seek just before beat reaches the beat causally");
    const auto after = renderFrom(24001, 3);
    check(std::all_of(after.begin(), after.end(),
                      [](float value) { return value == 0.0F; }),
          "Seek just after beat does not replay it");

    check(seekEngine.tryRequestSeek({100}).accepted,
          "Paused-seek fixture positions while Stopped");
    process(seekEngine, empty, empty, 48000);
    check(seekEngine.tryRequestPlay().accepted, "Paused-seek fixture Play");
    process(seekEngine, empty, empty, 48000);
    check(seekEngine.tryRequestPause().accepted, "Paused-seek fixture Pause");
    process(seekEngine, empty, empty, 48000);
    check(seekEngine.tryRequestSeek({24000}).accepted,
          "Seek is accepted while Paused");
    check(seekEngine.tryRequestPlay().accepted,
          "Play can follow pending Paused Seek");
    check(!seekEngine.tryRequestSeek({0}).accepted,
          "Seek is rejected against projected Playing");
    process(seekEngine, empty, empty, 48000);

    check(!seekEngine.trySetMetronomeLevel(
              {std::numeric_limits<float>::quiet_NaN()}).accepted &&
              !seekEngine.trySetMetronomeLevel(
                  {std::numeric_limits<float>::infinity()}).accepted &&
              !seekEngine.trySetMetronomeLevel({-100.1F}).accepted &&
              !seekEngine.trySetMetronomeLevel({0.1F}).accepted,
          "invalid metronome levels are rejected");

    const auto zero = renderMetronomeGrid({}, 48000, 2, {}, true, -100.0F);
    const auto nominal = renderMetronomeGrid({}, 48000, 2, {}, true, -12.0F);
    const auto unity = renderMetronomeGrid({}, 48000, 2, {}, true, 0.0F);
    check(zero.left[0] == 0.0F &&
              std::abs(nominal.left[0] -
                       2.0F * audio::prepareMetronomeLevel({-12.0F})) < 1.0e-6F &&
              unity.left[0] == 2.0F,
          "metronome levels -100/-12/0 use the prepared gain contract");
}

void metronomeLargeAndDensePreparation() {
    musical::MusicalTimeMap dense;
    dense.tempo.events.clear();
    dense.signatures.events.clear();
    dense.tempo.events.reserve(musical::maximumEvents);
    dense.signatures.events.reserve(musical::maximumEvents);
    for (std::size_t index = 0; index < musical::maximumEvents; ++index) {
        dense.tempo.events.push_back({{index + 1},
            {static_cast<std::int64_t>(index) * 960},
            {index % 2 ? 123.0 : 120.0}});
        dense.signatures.events.push_back({{index + 1},
            {static_cast<std::int64_t>(index)}, {1, 64}});
    }
    dense.tempo.nextId = {musical::maximumEvents + 1};
    dense.signatures.nextId = {musical::maximumEvents + 1};
    const auto prepared = audio::prepareTemporalContext(
        dense, std::nullopt, timeline::SampleRate{96000},
        timeline::SampleRate{44100}, 79);
    check(prepared.success() && !prepared.prepared->beats.empty() &&
              prepared.prepared->beats.size() <=
                  2 * musical::maximumEvents - 1,
          "maximum dense tempo/meter maps remain prepared and bounded");

    musical::MusicalTimeMap large;
    large.tempo.events[0].bpm = {400.0};
    auto largeContext = audio::prepareTemporalContext(
        large, std::nullopt, timeline::SampleRate{48000},
        timeline::SampleRate{48000}, 80);
    check(largeContext.success(), "large-position metronome prepares");
    makeDiagnosticClicks(*largeContext.prepared);
    const auto tick = musical::maximumCoordinate -
        musical::maximumCoordinate % musical::ppq;
    const auto exact = largeContext.prepared->musicalTime
                           ->exactProjectFrameAtTick({tick});
    check(bool(exact), "large musical beat has an exact project position");
    const auto floor = audio::exact::floorPosition(exact.value);
    const auto triggerOffset = audio::exact::zero(
        audio::exact::wide(floor.remainder)) ? 0U : 1U;
    audio::RealtimeAudioEngine engine;
    engine.configure({timeline::SampleRate{48000}, {0},
                      std::span<const audio::PreparedTrackView>{}});
    check(engine.configureTemporalContext(largeContext.prepared.get()),
          "large-position metronome configures");
    enterOperational(engine, 48000);
    check(engine.trySetMetronomeEnabled(true).accepted &&
              engine.trySetMetronomeLevel({0.0F}).accepted,
          "large-position metronome enable");
    std::vector<float> empty;
    process(engine, empty, empty, 48000);
    std::vector<float> settle(512), settleRight(512);
    process(engine, settle, settleRight, 48000);
    check(engine.tryRequestSeek({floor.frame}).accepted,
          "large supported metronome position seeks exactly");
    process(engine, empty, empty, 48000);
    check(engine.tryRequestPlay().accepted, "large-position Play");
    std::vector<float> left(2), right(2);
    process(engine, left, right, 48000);
    check((left[triggerOffset] == 1.0F || left[triggerOffset] == 2.0F) &&
              left[1U - triggerOffset] == 0.0F,
          "large-position scheduler emits the exact prepared beat");
}
}

void* operator new(std::size_t size) {
    if (countRealtimeAllocations.load(std::memory_order_relaxed))
        realtimeAllocations.fetch_add(1,std::memory_order_relaxed);
    if (auto* memory=std::malloc(size)) return memory;
    throw std::bad_alloc{};
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete(void* memory,std::size_t) noexcept { std::free(memory); }
void operator delete[](void* memory) noexcept { std::free(memory); }
void operator delete[](void* memory,std::size_t) noexcept { std::free(memory); }

int main() {
    preparationAndPersistence();
    oneWrapPerDeviceFrameInvariant();
    loopAnchorsAtTempoAndMeterChanges();
    fractionalClock();
    loopRenderAndMultipleWraps();
    projectedLoopAdmission();
    partitionInvariance();
    musicalRateMatrixRegression();
    exactMusicalLoopAndMetronome();
    crossedLoopStartMetronome();
    pendingLoopStartSurvivesHardRebuild();
    pendingBeatSurvivesHardRebuild(false);
    pendingBeatSurvivesHardRebuild(true);
    temporalReregistrationPreservesClock();
    metronomeAndEmptyPolicy();
    meterAccentMatrix();
    tempoRateAndPartitionMatrix();
    pauseResumeMetronomeContract();
    loopBoundaryMetronomeMatrix();
    metronomeTransportAndLevelMatrix();
    metronomeLargeAndDensePreparation();
    std::cout << "Loop and metronome tests passed\n";
}
