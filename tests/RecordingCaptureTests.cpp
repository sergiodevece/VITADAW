#include "vitadaw/audio/RealtimeAudioEngine.h"
#include "vitadaw/audio/RealtimeCapture.h"
#include "vitadaw/project/ProjectState.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <new>
#include <thread>
#include <vector>

namespace {
using namespace vitadaw;

thread_local bool realtimeRegion{};
std::atomic<std::size_t> realtimeAllocations{};

void check(bool value, const char* message) {
    if (!value) {
        std::cerr << "FAILED: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

struct Harness {
    audio::RealtimeAudioEngine engine;
    timeline::SampleRate projectRate{48000.0};
    timeline::SampleRate deviceRate{48000.0};

    explicit Harness(std::size_t capacity = 32,
                     timeline::SampleRate device = timeline::SampleRate{48000.0})
        : deviceRate(device) {
        engine.configure({projectRate, {1000}, {}, {}, false});
        check(engine.prepareLegacyDeviceRate(deviceRate), "prepare device rate");
        check(engine.prepareRecordingCapture(capacity), "prepare capture ring");
        engine.deviceInitialising();
        engine.processBlock({nullptr, 0, 0}, deviceRate);
    }

    void input(std::span<const float> left,
               std::span<const float> right = {}) {
        std::array<const float*, 2> channels{
            left.empty() ? nullptr : left.data(),
            right.empty() ? nullptr : right.data()};
        realtimeRegion = true;
        engine.processBlock({channels.data(), right.empty() ? 1U : 2U,
                             left.size()}, {nullptr, 0, left.size()}, deviceRate);
        realtimeRegion = false;
    }

    std::vector<std::vector<float>> drain(std::size_t channels,
                                          std::size_t maximum = 256) {
        std::vector<std::vector<float>> result(channels,
                                               std::vector<float>(maximum));
        std::array<float*, 2> pointers{};
        for (std::size_t channel = 0; channel < channels; ++channel)
            pointers[channel] = result[channel].data();
        const auto count = engine.drainRecording(
            {pointers.data(), channels, maximum});
        for (auto& channel : result) channel.resize(count);
        return result;
    }
};

void startStopAndIntegrity() {
    Harness h;
    check(h.engine.tryRequestSeek({123}).accepted, "non-zero seek accepted");
    h.engine.processBlock({nullptr, 0, 0}, h.deviceRate);
    check(h.engine.tryRequestRecord({1, {77}, media::AudioChannelLayout::mono}).accepted,
          "armed recording request accepted by prepared engine");
    const std::array<float, 3> a{1, 2, 3};
    const std::array<float, 2> b{4, 5};
    h.input(a);
    h.input(b);
    auto snapshot = h.engine.recordingSnapshot();
    check(snapshot.phase == audio::RecordingPhase::capturing &&
              snapshot.track == tracks::TrackId{77} &&
              snapshot.projectStart == timeline::ProjectFramePosition{123} &&
              snapshot.acceptedDeviceFrames.value == 5,
          "capture publishes exact start, TrackId and frame count");
    check(h.engine.tryRequestStop().accepted, "recording Stop accepted");
    const std::array<float, 4> excluded{6, 7, 8, 9};
    h.input(excluded);
    snapshot = h.engine.recordingSnapshot();
    check(snapshot.phase == audio::RecordingPhase::complete &&
              snapshot.acceptedDeviceFrames.value == 5 &&
              h.engine.transportSnapshot().playback ==
                  transport::PlaybackState::stopped &&
              !h.engine.transportSnapshot().playing,
          "Stop excludes its callback and leaves the productive transport stopped");
    const auto pcm = h.drain(1);
    check(pcm[0] == std::vector<float>({1, 2, 3, 4, 5}),
          "multiple and partial callbacks preserve sample order");
}

void stereoAndInputFailures() {
    {
        Harness h;
        check(h.engine.tryRequestRecord(
                  {2, {9}, media::AudioChannelLayout::stereo}).accepted,
              "stereo recording accepted");
        const std::array<float, 3> left{1, 2, 3}, right{-1, -2, -3};
        h.input(left, right);
        check(h.engine.tryRequestStop().accepted, "stereo stop");
        h.engine.processBlock({nullptr, 0, 0}, h.deviceRate);
        const auto pcm = h.drain(2);
        check(pcm[0] == std::vector<float>({1, 2, 3}) &&
                  pcm[1] == std::vector<float>({-1, -2, -3}),
              "stereo channels remain independent");
    }
    for (const bool nullInput : {false, true}) {
        Harness h;
        check(h.engine.tryRequestRecord(
                  {3, {11}, media::AudioChannelLayout::stereo}).accepted,
              "failure fixture starts");
        std::array<float, 2> mono{1, 2};
        std::array<const float*, 2> channels{
            nullInput ? nullptr : mono.data(), nullptr};
        h.engine.processBlock({channels.data(), nullInput ? 2U : 1U, mono.size()},
                              {nullptr, 0, mono.size()}, h.deviceRate);
        check(h.engine.recordingSnapshot().failure ==
                  audio::RecordingFailure::missingInput,
              "null or insufficient input fails explicitly");
    }
    {
        Harness h;
        check(h.engine.tryRequestRecord({67, {10}, media::AudioChannelLayout::mono}).accepted,
              "writer terminal fixture starts");
        check(!h.engine.failRecording(999, audio::RecordingFailure::writerFailed) &&
                  h.engine.failRecording(67, audio::RecordingFailure::writerFailed),
              "stale writer failure cannot terminalize a newer session");
        const auto failed = h.engine.recordingSnapshot();
        check(failed.phase == audio::RecordingPhase::failed &&
                  failed.failure == audio::RecordingFailure::writerFailed,
              "writer failure terminalizes directly without a cancel command");
        check(h.engine.tryRequestStop().accepted, "application may stop transport after writer failure");
        h.engine.processBlock({nullptr, 0, 0}, h.deviceRate);
        h.engine.resetRecordingCapture();
        check(h.engine.tryRequestRecord({68, {10}, media::AudioChannelLayout::mono}).accepted,
              "capture can be prepared again after direct writer terminalization");
    }
}

void wrapAndOverflow() {
    Harness h{5};
    check(h.engine.tryRequestRecord({4, {1}, media::AudioChannelLayout::mono}).accepted,
          "wrap fixture starts");
    const std::array<float, 4> first{1, 2, 3, 4};
    h.input(first);
    check(h.drain(1, 3)[0] == std::vector<float>({1, 2, 3}),
          "consumer advances ring read index");
    const std::array<float, 4> wrapped{5, 6, 7, 8};
    h.input(wrapped);
    check(h.drain(1)[0] == std::vector<float>({4, 5, 6, 7, 8}),
          "ring wrap preserves ordering");

    Harness overflow{4};
    check(overflow.engine.tryRequestRecord(
              {5, {1}, media::AudioChannelLayout::mono}).accepted,
          "overflow fixture starts");
    overflow.input(first);
    const std::array<float, 2> rejected{9, 10};
    overflow.input(rejected);
    const auto state = overflow.engine.recordingSnapshot();
    check(state.phase == audio::RecordingPhase::failed &&
              state.failure == audio::RecordingFailure::overflow &&
              state.acceptedDeviceFrames.value == 4 &&
              overflow.drain(1)[0] == std::vector<float>({1, 2, 3, 4}),
          "overflow rejects the entire callback without overwrite");
}

std::vector<float> partitioned(std::span<const std::size_t> blocks,
                               timeline::SampleRate deviceRate) {
    Harness h{64, deviceRate};
    check(h.engine.tryRequestRecord({8, {4}, media::AudioChannelLayout::mono}).accepted,
          "partition fixture starts");
    std::array<float, 12> source{};
    for (std::size_t i = 0; i < source.size(); ++i)
        source[i] = static_cast<float>(i + 1);
    std::size_t offset{};
    for (const auto block : blocks) {
        h.input(std::span<const float>{source}.subspan(offset, block));
        offset += block;
    }
    check(h.engine.tryRequestStop().accepted, "partition stop");
    h.engine.processBlock({nullptr, 0, 0}, h.deviceRate);
    const auto snapshot = h.engine.recordingSnapshot();
    check(snapshot.deviceSampleRate == deviceRate &&
              snapshot.acceptedDeviceFrames.value == source.size(),
          "device-rate metadata and exact count retained");
    return h.drain(1)[0];
}

void ratePartitionLifecycleAndRealtime() {
    const std::array<std::size_t, 1> one{12};
    const std::array<std::size_t, 4> many{1, 4, 2, 5};
    check(partitioned(one, timeline::SampleRate{48000}) ==
              partitioned(many, timeline::SampleRate{48000}),
          "callback partition does not alter captured PCM");
    check(partitioned(many, timeline::SampleRate{44100}).size() == 12,
          "unequal project/device rates capture device frames unchanged");

    Harness lost;
    check(lost.engine.tryRequestRecord(
              {9, {3}, media::AudioChannelLayout::mono}).accepted,
          "device-loss fixture starts");
    const std::array<float, 1> sample{1};
    lost.input(sample);
    lost.engine.deviceStopped();
    const auto lostTransport = lost.engine.transportSnapshot();
    check(lost.engine.recordingSnapshot().failure ==
              audio::RecordingFailure::deviceLost &&
              lostTransport.playback == transport::PlaybackState::stopped &&
              lostTransport.position == timeline::ProjectFramePosition{1},
          "device loss invalidates the generation and stops without rewinding");

    Harness allocation;
    check(allocation.engine.tryRequestRecord(
              {10, {2}, media::AudioChannelLayout::mono}).accepted,
          "allocation fixture starts");
    const auto before = realtimeAllocations.load();
    allocation.input(sample);
    check(realtimeAllocations.load() == before,
          "recording callback performs no allocation");
}

void concurrentProducerConsumer() {
    constexpr std::size_t frameCount = 20000;
    audio::RealtimeCapture capture;
    check(capture.prepare(frameCount), "concurrent SPSC ring prepared");
    check(capture.begin({42, {8}, media::AudioChannelLayout::mono},
                        {31}, timeline::SampleRate{48000}),
          "concurrent SPSC session begins");
    std::thread producer([&] {
        std::array<float, 37> block{};
        std::size_t produced{};
        while (produced < frameCount) {
            const auto count = std::min(block.size(), frameCount - produced);
            for (std::size_t index = 0; index < count; ++index)
                block[index] = static_cast<float>(produced + index);
            const std::array<const float*, 1> channels{block.data()};
            capture.capture({channels.data(), 1, count});
            produced += count;
            std::this_thread::yield();
        }
        capture.stop();
    });

    std::vector<float> received;
    received.reserve(frameCount);
    std::array<float, 23> block{};
    const std::array<float*, 1> channels{block.data()};
    while (received.size() < frameCount) {
        const auto count = capture.drain({channels.data(), 1, block.size()});
        received.insert(received.end(), block.begin(), block.begin() + count);
        if (count == 0) std::this_thread::yield();
    }
    producer.join();
    const auto snapshot = capture.snapshot();
    check(snapshot.phase == audio::RecordingPhase::complete &&
              snapshot.acceptedDeviceFrames.value == frameCount,
          "concurrent SPSC producer completes without loss");
    for (std::size_t index = 0; index < received.size(); ++index)
        check(received[index] == static_cast<float>(index),
              "concurrent SPSC preserves exact ordering");
}

void preparedLifecycleAndTerminalConsistency() {
    {
        Harness h;
        check(h.engine.tryRequestRecord({61, {5}, media::AudioChannelLayout::mono}).accepted,
              "prepared device-loss fixture starts");
        h.engine.deviceStopped();
        const auto failed = h.engine.recordingSnapshot();
        check(failed.phase == audio::RecordingPhase::failed &&
                  failed.failure == audio::RecordingFailure::deviceLost &&
                  failed.session == 61,
              "device loss terminalizes a prepared request before its callback");
        h.engine.deviceInitialising();
        h.engine.processBlock({nullptr, 0, 0}, h.deviceRate);
        check(h.engine.recordingSnapshot().phase == audio::RecordingPhase::failed,
              "stale beginRecord cannot start after device recovery");
    }
    {
        Harness h;
        check(h.engine.tryRequestRecord({62, {6}, media::AudioChannelLayout::mono}).accepted,
              "prepared restart fixture starts");
        h.engine.deviceInitialisingPreservingTransport();
        h.engine.processBlock({nullptr, 0, 0}, h.deviceRate);
        const auto failed = h.engine.recordingSnapshot();
        check(failed.phase == audio::RecordingPhase::failed &&
                  failed.failure == audio::RecordingFailure::deviceLost &&
                  failed.session == 62,
              "device restart terminalizes a prepared request before its callback");
    }
    {
        Harness h;
        check(h.engine.tryRequestRecord({63, {7}, media::AudioChannelLayout::mono}).accepted &&
                  h.engine.tryCancelRecording(),
              "prepared request can be cancelled without a callback");
        const auto cancelled = h.engine.recordingSnapshot();
        check(cancelled.phase == audio::RecordingPhase::failed &&
                  cancelled.failure == audio::RecordingFailure::cancelled &&
                  cancelled.session == 63,
              "prepared cancellation has a deterministic terminal snapshot");
        h.engine.processBlock({nullptr, 0, 0}, h.deviceRate);
        check(h.engine.recordingSnapshot().phase == audio::RecordingPhase::failed,
              "cancelled prepared beginRecord remains inert when later consumed");
    }
    {
        audio::RealtimeCapture capture;
        check(capture.prepare(8) && capture.begin(
                  {64, {8}, media::AudioChannelLayout::mono}, {}, timeline::SampleRate{48000}),
              "terminal-race fixture begins");
        capture.stop();
        capture.fail(audio::RecordingFailure::deviceLost);
        const auto stopWins = capture.snapshot();
        check(stopWins.phase == audio::RecordingPhase::complete &&
                  stopWins.failure == audio::RecordingFailure::none,
              "Stop wins terminal CAS without a stale failure reason");
        capture.reset();
        check(capture.prepare(8) && capture.begin(
                  {65, {8}, media::AudioChannelLayout::mono}, {}, timeline::SampleRate{48000}),
              "failure-wins fixture begins");
        capture.fail(audio::RecordingFailure::deviceLost);
        capture.stop();
        const auto failureWins = capture.snapshot();
        check(failureWins.phase == audio::RecordingPhase::failed &&
                  failureWins.failure == audio::RecordingFailure::deviceLost,
              "failure wins terminal CAS and repeated terminal requests are stable");
    }
}

void unequalRateClipExtent() {
    constexpr std::uint64_t sourceFrames = 1001;
    const timeline::SampleRate projectRate{48000.0};
    const timeline::SampleRate deviceRate{44117.0};
    const std::array<std::size_t, 4> partitionA{101, 300, 7, 593};
    const std::array<std::size_t, 3> partitionB{500, 499, 2};
    const auto capture = [&](std::span<const std::size_t> partitions) {
        Harness h{2048, deviceRate};
        check(h.engine.tryRequestSeek({17}).accepted, "unequal-rate seek accepted");
        h.engine.processBlock({nullptr, 0, 0}, deviceRate);
        check(h.engine.tryRequestRecord({66, {9}, media::AudioChannelLayout::mono}).accepted,
              "unequal-rate capture accepted");
        std::vector<float> source(sourceFrames);
        source.back() = -1.0F;
        std::size_t offset{};
        for (const auto count : partitions) {
            h.input(std::span<const float>{source}.subspan(offset, count));
            offset += count;
        }
        check(offset == source.size() && h.engine.tryRequestStop().accepted,
              "unequal-rate capture has exact partition total and Stop");
        h.engine.processBlock({nullptr, 0, 0}, deviceRate);
        return std::pair{h.engine.recordingSnapshot(), h.drain(1, source.size())[0]};
    };
    const auto [snapshot, pcm] = capture(partitionA);
    const auto [partitionedSnapshot, partitionedPcm] = capture(partitionB);
    check(snapshot.phase == audio::RecordingPhase::complete &&
              snapshot.acceptedDeviceFrames.value == sourceFrames &&
              snapshot.deviceSampleRate == deviceRate && pcm.size() == sourceFrames &&
              pcm.back() == -1.0F &&
              partitionedSnapshot.acceptedDeviceFrames == snapshot.acceptedDeviceFrames &&
              partitionedPcm == pcm,
          "unequal-rate recording retains exact frames, final sample and partition-invariant PCM");
    project::ProjectState project{projectRate};
    const auto track = project.addAudioTrack("Recorded", media::AudioChannelLayout::mono);
    const auto imported = project.importAudioToTrack(
        track, {"/virtual/unequal.wav", {}, {}}, snapshot.acceptedDeviceFrames,
        snapshot.deviceSampleRate, media::AudioChannelLayout::mono, snapshot.projectStart);
    const auto* clip = project.findClip(imported.clip);
    check(clip != nullptr &&
              project.sources().size() == 1 &&
              project.sources()[0].sampleRate == deviceRate &&
              project.sources()[0].frameCount == timeline::SourceFrameCount{sourceFrames} &&
              std::abs(clip->duration.value -
                       static_cast<double>(sourceFrames) * projectRate.hertz() /
                           deviceRate.hertz()) < 1.0e-12 &&
              project.duration().value == 17 +
                  timeline::sourceFramesToProjectDuration(
                      {sourceFrames}, deviceRate, projectRate).value,
          "unequal non-integer recording retains source metadata, precise duration and exclusive end");

    audio::PreparedTrackView playable{{9}, {{pcm.data(), nullptr}}, 1, {pcm.size()}, deviceRate,
                                      {17}, timeline::sourceFramesToProjectDuration(
                                                  {pcm.size()}, deviceRate, projectRate), {0}};
    audio::RealtimeAudioEngine playback;
    playback.configure({projectRate, {project.duration().value + 2}, {&playable, 1}});
    check(playback.prepareLegacyDeviceRate(deviceRate),
          "unequal-rate playback prepares the device-rate mapping");
    playback.deviceInitialising();
    playback.processBlock({nullptr, 0, 0}, deviceRate);
    check(playback.tryRequestPlay().accepted, "unequal-rate recorded source is playable");
    std::array<float, 1200> left{}, right{};
    std::array<float*, 2> output{left.data(), right.data()};
    playback.processBlock({output.data(), output.size(), left.size()}, deviceRate);
    constexpr float centreGain = 0.7071067811865476F;
    check(std::any_of(left.begin(), left.end(), [&](float sample) {
              return sample < pcm.back() * centreGain * 0.98F;
          }),
          "unequal-rate playback maps the final recorded sample");
}

void recordedPcmPlayback() {
    Harness capture;
    check(capture.engine.tryRequestRecord(
              {51, {12}, media::AudioChannelLayout::mono}).accepted,
          "playback fixture recording starts");
    const std::array<float, 4> samples{0.25F, -0.5F, 0.75F, -1.0F};
    capture.input(samples);
    check(capture.engine.tryRequestStop().accepted,
          "playback fixture recording stops");
    capture.engine.processBlock({nullptr, 0, 0}, capture.deviceRate);
    const auto pcm = capture.drain(1)[0];

    audio::PreparedTrackView track{
        {12}, {{pcm.data(), nullptr}}, 1, {pcm.size()}, capture.deviceRate,
        {0}, timeline::sourceFramesToProjectDuration(
                 {pcm.size()}, capture.deviceRate, capture.projectRate),
        {0}};
    audio::RealtimeAudioEngine playback;
    playback.configure({capture.projectRate, {4}, {&track, 1}});
    playback.deviceInitialising();
    playback.processBlock({nullptr, 0, 0}, capture.deviceRate);
    check(playback.tryRequestPlay().accepted,
          "recorded source playback accepts Play");
    std::array<float, 4> left{}, right{};
    std::array<float*, 2> output{left.data(), right.data()};
    playback.processBlock({output.data(), output.size(), left.size()},
                          capture.deviceRate);
    constexpr float centreGain = 0.7071067811865476F;
    for (std::size_t index = 0; index < samples.size(); ++index)
        check(std::abs(left[index] - samples[index] * centreGain) < 1.0e-6F &&
                  std::abs(right[index] - samples[index] * centreGain) < 1.0e-6F,
              "captured PCM plays through the production render path");
}

} // namespace

void* operator new(std::size_t size) {
    if (realtimeRegion) ++realtimeAllocations;
    if (auto* value = std::malloc(size)) return value;
    throw std::bad_alloc{};
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* value) noexcept { std::free(value); }
void operator delete[](void* value) noexcept { std::free(value); }
void operator delete(void* value, std::size_t) noexcept { std::free(value); }
void operator delete[](void* value, std::size_t) noexcept { std::free(value); }

int main() {
    startStopAndIntegrity();
    stereoAndInputFailures();
    wrapAndOverflow();
    ratePartitionLifecycleAndRealtime();
    concurrentProducerConsumer();
    preparedLifecycleAndTerminalConsistency();
    unequalRateClipExtent();
    recordedPcmPlayback();
    std::cout << "Recording capture tests passed\n";
}
