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
    fractionalClock();
    loopRenderAndMultipleWraps();
    partitionInvariance();
    musicalRateMatrixRegression();
    exactMusicalLoopAndMetronome();
    crossedLoopStartMetronome();
    temporalReregistrationPreservesClock();
    metronomeAndEmptyPolicy();
    std::cout << "Loop and metronome tests passed\n";
}
