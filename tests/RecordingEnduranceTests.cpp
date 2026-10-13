// VitaDAW 0.8.3-B - Recording Endurance.
//
// Deterministic endurance harness for the recording producer/ring/writer path.
// The audio callback is driven block by block through the production
// RealtimeAudioEngine; the writer is serviced from a simulated clock measured
// in device frames, so jitter, consumer pauses and the recovery that follows
// are reproducible without wall-clock sleeps or real hardware.
//
// Scenarios LR-1..LR-6 are documented in
// docs/audit-recording-endurance-0.8.3B.md.

#include "vitadaw/application/DawApplication.h"
#include "vitadaw/audio/RealtimeAudioEngine.h"
#include "vitadaw/audio/RealtimeCapture.h"
#include "vitadaw/platform/files/RecordingMediaIO.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace {
using namespace vitadaw;

// Allocation accounting. realtimeRegion marks the exact span of a production
// audio callback; allocationCount covers every allocation in the process and
// is used to prove that steady-state endurance neither allocates nor leaks.
thread_local bool realtimeRegion{};
std::atomic<std::size_t> realtimeAllocations{};
std::atomic<std::size_t> allocationCount{};

void check(bool value, const char* message) {
    if (!value) {
        std::cerr << "FAILED: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

// ---------------------------------------------------------------------------
// Deterministic stimulus
// ---------------------------------------------------------------------------
// Every captured frame carries its own stream index. The values are dyadic
// rationals, so they stay exact in binary32 however long the run is. The
// pattern repeats every 8192 frames, so a value comparison alone detects any
// loss, duplication or reordering that is not an exact multiple of the period;
// losses that are an exact multiple are covered by the exact produced ==
// consumed frame-count identity every scenario also asserts.
constexpr std::uint64_t patternPeriod = 8192;

[[nodiscard]] float pattern(std::uint64_t index, std::size_t channel) noexcept {
    const auto phase = static_cast<float>(index % patternPeriod) /
                       static_cast<float>(patternPeriod);
    return channel == 0 ? phase - 0.5F : 0.5F - phase;
}

// Mirrors the cadence at which the host platform services the writer (the
// JUCE application timer, 30 Hz). The test models that cadence; it does not
// read it from Core, which deliberately knows nothing about it.
constexpr double nominalServiceRateHz = 30.0;

[[nodiscard]] std::uint64_t environmentSeconds(const char* name,
                                               std::uint64_t fallback) {
    const char* value = std::getenv(name);
    if (value == nullptr || *value == '\0') return fallback;
    char* end{};
    const auto parsed = std::strtoull(value, &end, 10);
    if (end == value || parsed == 0) return fallback;
    return parsed;
}

// ---------------------------------------------------------------------------
// Simulated recording writer
// ---------------------------------------------------------------------------
// Mirrors JuceAudioDeviceAdapter::serviceRecording: it drains the ring in
// fixed chunks until the ring is empty and stops at the first write failure.
// Optionally it writes the drained PCM through the production
// RecordingFileHandle, including its bounded short-write loop, so the bytes
// that reach a real descriptor are verified too.
class ServiceWriter {
public:
    static constexpr std::size_t chunkFrames = 8192; // recordingDrainBuffer_

    ServiceWriter(std::size_t channels, const std::filesystem::path& file,
                  std::size_t maximumWriteChunk)
        : channels_(channels),
          buffers_(channels, std::vector<float>(chunkFrames)),
          pointers_(channels),
          interleaved_(chunkFrames * channels) {
        for (std::size_t channel = 0; channel < channels_; ++channel)
            pointers_[channel] = buffers_[channel].data();
        if (file.empty()) return;
        faults_.maximumWriteChunk = maximumWriteChunk;
        path_ = file;
#if defined(_WIN32)
        const auto descriptor = ::_open(
            path_.string().c_str(), _O_CREAT | _O_TRUNC | _O_WRONLY | _O_BINARY,
            _S_IREAD | _S_IWRITE);
#else
        const auto descriptor = ::open(path_.c_str(), O_CREAT | O_TRUNC | O_WRONLY, 0600);
#endif
        check(descriptor >= 0, "endurance writer opens its temporary file");
        file_ = std::make_unique<platform::files::RecordingFileHandle>(
            descriptor, &faults_);
    }

    // Drain(AudioBlockView) -> frames, matching RealtimeAudioEngine::drainRecording.
    template <class Drain>
    std::size_t service(Drain&& drain) {
        std::size_t total{};
        for (;;) {
            const auto frames = drain(audio::AudioBlockView{
                pointers_.data(), channels_, chunkFrames});
            if (frames == 0) break;
            for (std::size_t channel = 0; channel < channels_; ++channel)
                for (std::size_t index = 0; index < frames; ++index)
                    if (buffers_[channel][index] != pattern(expected_ + index, channel))
                        ++mismatches_;
            if (file_ != nullptr) {
                for (std::size_t index = 0; index < frames; ++index)
                    for (std::size_t channel = 0; channel < channels_; ++channel)
                        interleaved_[index * channels_ + channel] =
                            buffers_[channel][index];
                if (!file_->write(interleaved_.data(),
                                  frames * channels_ * sizeof(float), false)) {
                    writeFailed_ = true;
                    break;
                }
            }
            expected_ += frames;
            total += frames;
        }
        return total;
    }

    [[nodiscard]] bool finish() {
        if (file_ == nullptr) return true;
        const auto result = file_->sync() && file_->close();
        file_.reset();
        return result;
    }

    // Verifies the bytes a real descriptor received, independently of the
    // in-flight comparison above.
    [[nodiscard]] bool fileMatchesStream() const {
        if (path_.empty()) return true;
        std::ifstream input(path_, std::ios::binary);
        if (!input) return false;
        std::vector<float> block(chunkFrames * channels_);
        std::uint64_t index{};
        while (index < expected_) {
            const auto frames = static_cast<std::size_t>(
                std::min<std::uint64_t>(chunkFrames, expected_ - index));
            input.read(reinterpret_cast<char*>(block.data()),
                       static_cast<std::streamsize>(frames * channels_ * sizeof(float)));
            if (!input) return false;
            for (std::size_t frame = 0; frame < frames; ++frame)
                for (std::size_t channel = 0; channel < channels_; ++channel)
                    if (block[frame * channels_ + channel] !=
                        pattern(index + frame, channel))
                        return false;
            index += frames;
        }
        input.peek();
        return input.eof();
    }

    [[nodiscard]] std::uint64_t frames() const noexcept { return expected_; }
    [[nodiscard]] std::uint64_t mismatches() const noexcept { return mismatches_; }
    [[nodiscard]] bool writeFailed() const noexcept { return writeFailed_; }

private:
    std::size_t channels_{};
    std::vector<std::vector<float>> buffers_;
    std::vector<float*> pointers_;
    std::vector<float> interleaved_;
    std::uint64_t expected_{};
    std::uint64_t mismatches_{};
    bool writeFailed_{};
    std::filesystem::path path_;
    platform::files::RecordingIoFaultInjection faults_{};
    std::unique_ptr<platform::files::RecordingFileHandle> file_;
};

// ---------------------------------------------------------------------------
// Endurance rig
// ---------------------------------------------------------------------------
struct RigConfig {
    media::AudioChannelLayout layout{media::AudioChannelLayout::mono};
    timeline::SampleRate deviceRate{48000.0};
    double ringSeconds{audio::recordingCaptureRingSeconds};
    double serviceRateHz{nominalServiceRateHz};
    std::size_t blockFrames{512};
    std::uint64_t takeFrames{};              // reserved transport duration
    std::filesystem::path writerFile{};      // empty: verify in memory only
    std::size_t maximumWriteChunk{};
};

class Rig {
public:
    explicit Rig(const RigConfig& config)
        : config_(config),
          channels_(static_cast<std::size_t>(media::channelCount(config.layout))),
          capacity_(static_cast<std::size_t>(
              std::ceil(config.deviceRate.hertz() * config.ringSeconds))),
          servicePeriod_(static_cast<std::uint64_t>(
              std::ceil(config.deviceRate.hertz() / config.serviceRateHz))),
          stimulus_(channels_, std::vector<float>(config.blockFrames)),
          inputs_(channels_),
          writer_(channels_, config.writerFile, config.maximumWriteChunk) {
        for (std::size_t channel = 0; channel < channels_; ++channel)
            inputs_[channel] = stimulus_[channel].data();
        const auto duration = static_cast<std::int64_t>(
            config.takeFrames + static_cast<std::uint64_t>(config.deviceRate.hertz()) * 4);
        engine_.configure({config.deviceRate, {duration}, {}, {}, false});
        check(engine_.prepareLegacyDeviceRate(config.deviceRate),
              "endurance rig prepares the device rate");
        check(engine_.prepareRecordingCapture(capacity_),
              "endurance rig prepares the capture ring");
        // Same budget the adapter installs: one service period plus one callback.
        engine_.setRecordingServiceBudgetFrames(
            static_cast<std::size_t>(servicePeriod_) + config.blockFrames);
        engine_.deviceInitialising();
        engine_.processBlock({nullptr, 0, 0}, config.deviceRate);
    }

    void beginTake(audio::RecordingSessionId session, tracks::TrackId track) {
        check(engine_.tryRequestRecord({session, track, config_.layout}).accepted,
              "endurance take is accepted by the prepared engine");
        nextService_ = servicePeriod_;
    }

    void setJitter(std::uint64_t maximumExtraFrames) noexcept {
        jitterMaximum_ = maximumExtraFrames;
    }
    // Suspends the writer for the rest of the run.
    void suspendService() noexcept { serviceSuspended_ = true; }
    void resumeService() noexcept {
        serviceSuspended_ = false;
        nextService_ = clock_ + servicePeriod_;
    }

    // Advances the simulation by whole device callbacks and returns the exact
    // number of frames produced, so every assertion can use real quantities
    // instead of a rounded request.
    std::uint64_t run(std::uint64_t frames) {
        const auto blocks = frames / config_.blockFrames;
        for (std::uint64_t block = 0; block < blocks; ++block) {
            produce(config_.blockFrames);
            clock_ += config_.blockFrames;
            while (!serviceSuspended_ && clock_ >= nextService_) {
                service();
                nextService_ += servicePeriod_ + jitter();
            }
        }
        return blocks * config_.blockFrames;
    }

    // Stop, consume the RT command, then drain what the producer published.
    void stopTake() {
        check(engine_.tryRequestStop().accepted, "endurance take stops");
        engine_.processBlock({nullptr, 0, 0}, config_.deviceRate);
        serviceSuspended_ = false;
        service();
        check(writer_.finish(), "endurance writer syncs and closes");
    }

    std::size_t service() {
        return writer_.service([this](audio::AudioBlockView view) {
            return engine_.drainRecording(view);
        });
    }

    [[nodiscard]] audio::RecordingCaptureTelemetry telemetry() const noexcept {
        return engine_.recordingTelemetry();
    }
    [[nodiscard]] audio::RecordingSnapshot snapshot() const noexcept {
        return engine_.recordingSnapshot();
    }
    [[nodiscard]] const ServiceWriter& writer() const noexcept { return writer_; }
    [[nodiscard]] ServiceWriter& writer() noexcept { return writer_; }
    [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }
    [[nodiscard]] std::uint64_t servicePeriod() const noexcept { return servicePeriod_; }
    [[nodiscard]] std::uint64_t offeredFrames() const noexcept { return offered_; }
    [[nodiscard]] audio::RealtimeAudioEngine& engine() noexcept { return engine_; }

private:
    void produce(std::size_t count) {
        for (std::size_t channel = 0; channel < channels_; ++channel)
            for (std::size_t index = 0; index < count; ++index)
                stimulus_[channel][index] = pattern(offered_ + index, channel);
        realtimeRegion = true;
        engine_.processBlock({inputs_.data(), channels_, count},
                             {nullptr, 0, count}, config_.deviceRate);
        realtimeRegion = false;
        offered_ += count;
    }

    [[nodiscard]] std::uint64_t jitter() noexcept {
        if (jitterMaximum_ == 0) return 0;
        random_ = random_ * 6364136223846793005ULL + 1442695040888963407ULL;
        return (random_ >> 33U) % (jitterMaximum_ + 1);
    }

    RigConfig config_;
    std::size_t channels_{};
    std::size_t capacity_{};
    std::uint64_t servicePeriod_{};
    std::vector<std::vector<float>> stimulus_;
    std::vector<const float*> inputs_;
    ServiceWriter writer_;
    audio::RealtimeAudioEngine engine_;
    std::uint64_t clock_{};
    std::uint64_t nextService_{};
    std::uint64_t offered_{};
    std::uint64_t jitterMaximum_{};
    std::uint64_t random_{0x9E3779B97F4A7C15ULL};
    bool serviceSuspended_{};
};

struct Report {
    std::string scenario;
    audio::RecordingCaptureTelemetry telemetry;
    double simulatedSeconds{};
    std::string note;
};
std::vector<Report> reports;

void record(std::string scenario, const audio::RecordingCaptureTelemetry& telemetry,
            timeline::SampleRate rate, std::string note = {}) {
    reports.push_back({std::move(scenario), telemetry,
                       static_cast<double>(telemetry.producedFrames) / rate.hertz(),
                       std::move(note)});
}

// Invariants every scenario must satisfy regardless of its stress profile.
void checkCoherentCounters(const audio::RecordingCaptureTelemetry& telemetry,
                           std::size_t capacity) {
    check(telemetry.ringCapacityFrames == capacity, "telemetry reports the ring capacity");
    check(telemetry.peakOccupancyFrames <= capacity, "peak occupancy never exceeds capacity");
    check(telemetry.minimumHeadroomFrames + telemetry.peakOccupancyFrames == capacity,
          "minimum headroom is the exact complement of the peak occupancy");
    check(telemetry.consumedFrames <= telemetry.producedFrames,
          "the consumer never reports more frames than the producer accepted");
    check(telemetry.occupancyFrames ==
              telemetry.producedFrames - telemetry.consumedFrames,
          "instantaneous occupancy equals produced minus consumed");
    check(telemetry.emptyDrainCalls <= telemetry.drainCalls,
          "empty drain calls are a subset of drain calls");
    check(telemetry.lateServiceEvents <= telemetry.drainCalls,
          "late service events are a subset of drain calls");
}

// ---------------------------------------------------------------------------
// LR-1 nominal
// ---------------------------------------------------------------------------
void lr1Nominal(const std::filesystem::path& directory) {
    const timeline::SampleRate rate{48000.0};
    constexpr std::uint64_t seconds = 30;
    RigConfig config;
    config.deviceRate = rate;
    config.blockFrames = 512;
    config.takeFrames = static_cast<std::uint64_t>(rate.hertz()) * seconds;
    config.writerFile = directory / "lr1-nominal.pcm";
    config.maximumWriteChunk = 4096; // force the production short-write loop
    Rig rig{config};
    const auto allocationsBefore = realtimeAllocations.load();
    rig.beginTake(1, tracks::TrackId{7});
    const auto produced = rig.run(config.takeFrames);
    const auto steady = rig.telemetry();
    rig.stopTake();
    const auto telemetry = rig.telemetry();
    checkCoherentCounters(telemetry, rig.capacity());

    const auto nominalBound = rig.servicePeriod() + config.blockFrames;
    check(telemetry.lossless() && telemetry.producedFrames == produced &&
              telemetry.consumedFrames == produced && telemetry.occupancyFrames == 0,
          "LR-1 nominal service captures and drains every frame without loss");
    check(steady.peakOccupancyFrames <= nominalBound &&
              telemetry.peakServiceBacklogFrames <= nominalBound &&
              telemetry.lateServiceEvents == 0,
          "LR-1 occupancy stays inside one service period plus one callback");
    check(telemetry.captureCalls == produced / config.blockFrames &&
              telemetry.drainCalls >= produced / rig.servicePeriod(),
          "LR-1 producer and service call counts are coherent");
    check(rig.snapshot().phase == audio::RecordingPhase::complete &&
              rig.snapshot().acceptedDeviceFrames.value == produced,
          "LR-1 take completes with the exact accepted frame count");
    check(rig.writer().mismatches() == 0 && !rig.writer().writeFailed() &&
              rig.writer().frames() == produced &&
              rig.writer().fileMatchesStream(),
          "LR-1 every drained and written frame is bit-exact and in order");
    check(realtimeAllocations.load() == allocationsBefore,
          "LR-1 instrumented capture callbacks perform no allocation");
    record("LR-1 nominal", telemetry, rate, "30 s mono, 512-frame callbacks, file-backed writer");
}

// ---------------------------------------------------------------------------
// LR-2 moderate jitter
// ---------------------------------------------------------------------------
void lr2Jitter(const std::filesystem::path& directory) {
    const timeline::SampleRate rate{48000.0};
    constexpr std::uint64_t seconds = 30;
    RigConfig config;
    config.layout = media::AudioChannelLayout::stereo;
    config.deviceRate = rate;
    config.blockFrames = 512;
    config.takeFrames = static_cast<std::uint64_t>(rate.hertz()) * seconds;
    config.writerFile = directory / "lr2-jitter.pcm";
    config.maximumWriteChunk = 7000;
    Rig rig{config};
    const auto allocationsBefore = realtimeAllocations.load();
    rig.beginTake(2, tracks::TrackId{8});
    // Service periods between 1x and 4x the nominal 30 Hz timer period.
    rig.setJitter(rig.servicePeriod() * 3);
    const auto produced = rig.run(config.takeFrames);
    const auto stressed = rig.telemetry();
    rig.stopTake();
    const auto telemetry = rig.telemetry();
    checkCoherentCounters(telemetry, rig.capacity());

    const auto jitterBound = rig.servicePeriod() * 4 + config.blockFrames;
    check(telemetry.lossless() && telemetry.producedFrames == produced &&
              telemetry.consumedFrames == produced && telemetry.occupancyFrames == 0,
          "LR-2 the ring absorbs service jitter with zero loss");
    check(stressed.lateServiceEvents > 0,
          "LR-2 the injected jitter really exceeds the nominal service budget");
    check(telemetry.peakOccupancyFrames <= jitterBound &&
              telemetry.peakOccupancyFrames < rig.capacity() / 4,
          "LR-2 jitter occupancy stays far below the ring capacity");
    check(rig.writer().mismatches() == 0 && !rig.writer().writeFailed() &&
              rig.writer().frames() == produced && rig.writer().fileMatchesStream(),
          "LR-2 stereo channels stay separate and in order under jitter");
    check(realtimeAllocations.load() == allocationsBefore,
          "LR-2 jittered service does not make the callback allocate");
    record("LR-2 jitter", telemetry, rate,
           "30 s stereo, service period 1x-4x nominal");
}

// ---------------------------------------------------------------------------
// LR-3 long but recoverable consumer pause
// ---------------------------------------------------------------------------
void lr3RecoverablePause() {
    const timeline::SampleRate rate{48000.0};
    RigConfig config;
    config.deviceRate = rate;
    config.blockFrames = 512;
    config.takeFrames = static_cast<std::uint64_t>(rate.hertz()) * 12;
    Rig rig{config};
    const auto allocationsBefore = realtimeAllocations.load();
    rig.beginTake(3, tracks::TrackId{9});
    rig.run(static_cast<std::uint64_t>(rate.hertz()) * 2);
    const auto beforePause = rig.telemetry();
    // 1.5 s of writer silence: well above any plausible UI stall, still inside
    // the 2 s ring.
    rig.suspendService();
    const auto pauseFrames = rig.run(static_cast<std::uint64_t>(rate.hertz() * 1.5));
    const auto duringPause = rig.telemetry();
    rig.resumeService();
    rig.run(static_cast<std::uint64_t>(rate.hertz()) * 2);
    const auto afterRecovery = rig.telemetry();
    rig.stopTake();
    const auto telemetry = rig.telemetry();
    checkCoherentCounters(telemetry, rig.capacity());

    check(duringPause.occupancyFrames >= pauseFrames &&
              duringPause.peakOccupancyFrames >= pauseFrames &&
              duringPause.occupancyFrames > beforePause.occupancyFrames,
          "LR-3 occupancy grows for the whole consumer pause");
    check(telemetry.lossless() && telemetry.peakOccupancyFrames < rig.capacity(),
          "LR-3 a 1.5 s pause stays inside the ring without overflow");
    check(afterRecovery.occupancyFrames <= rig.servicePeriod() + config.blockFrames,
          "LR-3 the writer drains the backlog once it resumes");
    check(telemetry.occupancyFrames == 0 &&
              telemetry.consumedFrames == telemetry.producedFrames &&
              rig.writer().mismatches() == 0 &&
              rig.writer().frames() == telemetry.producedFrames,
          "LR-3 the recovered stream is complete and in order");
    check(duringPause.lateServiceEvents == beforePause.lateServiceEvents,
          "LR-3 a suspended writer records no service call at all");
    check(afterRecovery.lateServiceEvents > duringPause.lateServiceEvents,
          "LR-3 the first service after the pause is reported as late");
    check(realtimeAllocations.load() == allocationsBefore,
          "LR-3 a stalled consumer does not make the callback allocate");
    record("LR-3 recoverable pause", telemetry, rate,
           "1.5 s writer pause inside a 2 s ring");
}

// ---------------------------------------------------------------------------
// LR-4 capacity limit: measure the real absorbable burst per callback size
// ---------------------------------------------------------------------------
struct BurstMeasurement {
    std::size_t blockFrames{};
    std::size_t capacity{};
    std::uint64_t absorbedFrames{};
    std::size_t minimumHeadroom{};
    std::uint64_t droppedFrames{};
};

BurstMeasurement measureAbsorbableBurst(std::size_t blockFrames,
                                        timeline::SampleRate rate) {
    RigConfig config;
    config.deviceRate = rate;
    config.blockFrames = blockFrames;
    config.takeFrames = static_cast<std::uint64_t>(rate.hertz()) * 4;
    Rig rig{config};
    rig.beginTake(4, tracks::TrackId{10});
    // The writer never runs: the ring must absorb the entire burst alone. The
    // first callback is also what consumes the begin command, so the loop is
    // bounded by the reserved take instead of the initial phase.
    rig.suspendService();
    const auto limit = config.takeFrames / blockFrames;
    std::uint64_t callbacks{};
    while (!rig.snapshot().terminal() && callbacks++ < limit)
        rig.run(blockFrames);
    const auto telemetry = rig.telemetry();
    const auto failed = rig.snapshot();
    check(failed.phase == audio::RecordingPhase::failed &&
              failed.failure == audio::RecordingFailure::overflow,
          "LR-4 an unserviced ring terminalizes with overflow, never silently");
    check(telemetry.producedFrames == failed.acceptedDeviceFrames.value &&
              telemetry.peakOccupancyFrames == telemetry.producedFrames &&
              telemetry.minimumHeadroomFrames < blockFrames,
          "LR-4 the measured burst fills the ring to within one callback");
    check(telemetry.producedFrames % blockFrames == 0 &&
              telemetry.droppedFrames == blockFrames &&
              telemetry.overflowEvents == 1,
          "LR-4 overflow rejects exactly one whole callback");
    // Everything accepted before the overflow must still be readable in order.
    rig.resumeService();
    const auto drained = rig.service();
    check(static_cast<std::uint64_t>(drained) == telemetry.producedFrames &&
              rig.writer().mismatches() == 0,
          "LR-4 the ring content at the capacity limit is uncorrupted");
    return {blockFrames, rig.capacity(), telemetry.producedFrames,
            telemetry.minimumHeadroomFrames, telemetry.droppedFrames};
}

std::vector<BurstMeasurement> lr4CapacityLimit() {
    const timeline::SampleRate rate{48000.0};
    std::vector<BurstMeasurement> measurements;
    for (const std::size_t block : {64U, 128U, 256U, 512U, 1024U}) {
        const auto measurement = measureAbsorbableBurst(block, rate);
        const auto expected = (measurement.capacity / block) * block;
        check(measurement.absorbedFrames == expected,
              "LR-4 the absorbable burst is capacity rounded down to a callback");
        measurements.push_back(measurement);
    }
    return measurements;
}

// ---------------------------------------------------------------------------
// LR-5 forced overflow: ring behaviour and application policy
// ---------------------------------------------------------------------------
void lr5ForcedOverflowAtRing(const std::filesystem::path& directory) {
    const timeline::SampleRate rate{48000.0};
    RigConfig config;
    config.deviceRate = rate;
    config.blockFrames = 512;
    config.takeFrames = static_cast<std::uint64_t>(rate.hertz()) * 8;
    config.writerFile = directory / "lr5-overflow.pcm";
    Rig rig{config};
    rig.beginTake(5, tracks::TrackId{11});
    rig.run(static_cast<std::uint64_t>(rate.hertz()));
    const auto serviced = rig.telemetry();
    check(serviced.lossless() && serviced.consumedFrames > 0,
          "LR-5 the take is healthy before the forced stall");

    // A 3 s writer outage against a 2 s ring: loss is unavoidable by design.
    rig.suspendService();
    rig.run(static_cast<std::uint64_t>(rate.hertz()) * 3);
    const auto failed = rig.snapshot();
    const auto atFailure = rig.telemetry();
    checkCoherentCounters(atFailure, rig.capacity());
    check(failed.phase == audio::RecordingPhase::failed &&
              failed.failure == audio::RecordingFailure::overflow,
          "LR-5 a consumer outage beyond the ring fails explicitly as overflow");
    check(atFailure.overflowEvents == 1 &&
              atFailure.droppedFrames == config.blockFrames,
          "LR-5 exactly one callback is rejected and counted, with no partial write");
    check(atFailure.producedFrames == failed.acceptedDeviceFrames.value &&
              atFailure.peakOccupancyFrames <= rig.capacity(),
          "LR-5 the accepted frame count is the exact pre-overflow prefix");

    // Further callbacks are inert: capture is terminal, so nothing more is
    // accepted, counted or dropped.
    rig.run(static_cast<std::uint64_t>(rate.hertz()));
    const auto afterFailure = rig.telemetry();
    check(afterFailure.producedFrames == atFailure.producedFrames &&
              afterFailure.droppedFrames == atFailure.droppedFrames &&
              afterFailure.captureCalls == atFailure.captureCalls &&
              afterFailure.overflowEvents == 1,
          "LR-5 callbacks after a terminal overflow change nothing");

    rig.resumeService();
    rig.service();
    check(rig.writer().finish(), "LR-5 the writer closes after the overflow");
    const auto drainedTelemetry = rig.telemetry();
    check(drainedTelemetry.consumedFrames == drainedTelemetry.producedFrames &&
              drainedTelemetry.occupancyFrames == 0 &&
              rig.writer().mismatches() == 0 &&
              rig.writer().frames() == drainedTelemetry.producedFrames &&
              rig.writer().fileMatchesStream(),
          "LR-5 the pre-overflow PCM is drained intact: no silent corruption");
    check(rig.engine().tryRequestStop().accepted,
          "LR-5 transport can still be stopped after a capture overflow");
    record("LR-5 forced overflow", drainedTelemetry, rate,
           "3 s writer outage against a 2 s ring");
}

// --- Application-level overflow policy -------------------------------------
class MemoryFiles final : public platform::files::IProjectFileIO {
public:
    std::map<std::filesystem::path, std::string> content;

    platform::files::ReadResult read(const std::filesystem::path& path,
                                     std::size_t maximum) override {
        const auto found = content.find(path);
        if (found == content.end())
            return {{persistence::PersistenceCode::fileNotFound,
                     persistence::PersistencePhase::read}, {}};
        if (found->second.size() > maximum)
            return {{persistence::PersistenceCode::fileTooLarge,
                     persistence::PersistencePhase::read}, {}};
        const auto bytes = std::as_bytes(
            std::span{found->second.data(), found->second.size()});
        return {{}, {bytes.begin(), bytes.end()}};
    }

    persistence::PersistenceResult replace(const std::filesystem::path& path,
                                           std::string_view value) override {
        content[path] = value;
        return {};
    }
};

// A hardware-free engine backed by the real capture ring. Only the failure
// policy is modelled: an overflowed take never reaches finalization, so this
// fake deliberately cannot commit media.
class RingBackedEngine final : public audio::IAudioEngineControl {
public:
    struct Plan final : audio::PreparedProcessingPlanChange {};

    audio::RealtimeCapture capture;
    audio::RealtimeTransportSnapshot transport;
    audio::RecordingRequest prepared;
    audio::RecordingSnapshot lastDiscarded;
    timeline::SampleRate deviceRate{48000.0};
    std::size_t capacityFrames{static_cast<std::size_t>(48000.0 *
        audio::recordingCaptureRingSeconds)};
    std::size_t blockFrames{512};
    std::uint64_t session{1};
    audio::AudioCommandSequence nextSequence{1};
    unsigned discards{};
    unsigned finalizations{};
    ServiceWriter writer{1, {}, 0};

    // Drives the production ring directly from the test, as the callback does.
    void produceUntilOverflow() {
        std::vector<float> block(blockFrames);
        const std::array<const float*, 1> channels{block.data()};
        while (capture.isCapturing()) {
            for (std::size_t index = 0; index < blockFrames; ++index)
                block[index] = pattern(offered_ + index, 0);
            capture.capture({channels.data(), 1, blockFrames});
            offered_ += blockFrames;
        }
    }

    audio::AudioControlRequestResult result(transport::PlaybackState state,
                                            timeline::ProjectFramePosition position) noexcept {
        transport.playback = state;
        transport.playing = state == transport::PlaybackState::playing;
        transport.position = position;
        transport.lastProcessedCommandSequence = nextSequence;
        transport.projectedThroughTicket = nextSequence;
        return {true, nextSequence++, audio::AudioControlRejection::none,
                state, position, true};
    }
    audio::AudioControlRequestResult tryRequestPlay() noexcept override {
        return result(transport::PlaybackState::playing, transport.position);
    }
    audio::AudioControlRequestResult tryRequestStop() noexcept override {
        return result(transport::PlaybackState::stopped, transport.position);
    }
    audio::AudioControlRequestResult tryRequestSeek(
        timeline::ProjectFramePosition position) noexcept override {
        return result(transport::PlaybackState::stopped, position);
    }
    audio::RealtimeTransportSnapshot transportSnapshot() const noexcept override {
        return transport;
    }
    audio::RealtimeTransportSnapshot projectedTransportSnapshot() noexcept override {
        return transport;
    }
    mixer::MeterSnapshot meterSnapshot() const noexcept override { return {}; }
    audio::AudioFilePreparationResult prepareWav(const std::filesystem::path&) override {
        return {nullptr, "Media preparation is not part of the overflow policy fixture"};
    }
    audio::StructuralPlanPreparationResult prepareProcessingPlan(
        const audio::ProcessingPlanSpecification&) override {
        return {std::make_unique<Plan>(), {}};
    }
    bool commitPreparedProcessingPlan(audio::PreparedProcessingPlanChangePtr plan,
                                      audio::AudioFileCommitAction action) noexcept override {
        if (!plan || !action.isValid()) return false;
        action.execute();
        return true;
    }
    bool tryUpdateTrackMix(tracks::TrackId, mixer::PreparedTrackMixState,
                           audio::PreparedAudibilityState) noexcept override { return true; }
    bool tryUpdateBusMix(routing::BusId, mixer::PreparedBusMixState,
                         audio::PreparedAudibilityState) noexcept override { return true; }
    bool tryUpdateSendMix(routing::SendId, mixer::PreparedSendMixState) noexcept override {
        return true;
    }
    bool tryUpdateMasterMix(mixer::PreparedMasterMixState) noexcept override { return true; }

    audio::RecordingPreflightResult prepareRecording(
        const audio::RecordingPreflightRequest& request) override {
        if (!capture.prepare(capacityFrames))
            return {{}, "The realtime recording buffer could not be prepared"};
        capture.setServiceBudgetFrames(
            static_cast<std::size_t>(deviceRate.hertz() / nominalServiceRateHz) +
            blockFrames);
        prepared = {session++, request.track, request.layout, {}};
        prepared.placement.projectSampleRate = request.projectSampleRate;
        prepared.placement.deviceSampleRate = deviceRate;
        prepared.placement.deviceBufferFrames = static_cast<std::uint32_t>(blockFrames);
        return {prepared, {}, {}};
    }
    audio::AudioControlRequestResult tryRequestRecord(
        audio::RecordingRequest request) noexcept override {
        if (request.session != prepared.session || !capture.prepareRequest(request) ||
            !capture.begin(request, transport.position, deviceRate)) return {};
        return result(transport::PlaybackState::playing, transport.position);
    }
    bool tryCancelRecording() noexcept override {
        if (!capture.isActive()) return false;
        capture.cancel();
        return true;
    }
    void serviceRecording() noexcept override {
        static_cast<void>(writer.service([this](audio::AudioBlockView view) {
            return capture.drain(view);
        }));
    }
    audio::RecordingSnapshot recordingSnapshot() const noexcept override {
        return capture.snapshot();
    }
    audio::RecordingFinalizationResult finalizeRecording() override {
        ++finalizations;
        return {{}, capture.snapshot(), {}, "Finalization is not reachable for a failed take", {}};
    }
    bool discardRecording(bool) noexcept override {
        ++discards;
        lastDiscarded = capture.snapshot();
        capture.reset();
        return true;
    }
    audio::RecordingCleanupResult discardRecordingWithDiagnostics(
        bool removePublished, std::string primary = {}) noexcept override {
        static_cast<void>(discardRecording(removePublished));
        audio::RecordingCleanupResult cleanup;
        cleanup.primaryError = std::move(primary);
        return cleanup;
    }
    audio::RecordingCleanupResult shutdownRecording() noexcept override {
        return discardRecordingWithDiagnostics(true, "Recording cancelled during application shutdown");
    }
    void confirmRecordingCommit() noexcept override { capture.reset(); }

private:
    std::uint64_t offered_{};
};

void lr5OverflowApplicationPolicy() {
    MemoryFiles files;
    RingBackedEngine engine;
    application::DawApplication app{engine, timeline::SampleRate{48000.0}, files};
    check(app.handle(commands::AddAudioTrack{"Overflow"}).status ==
              commands::CommandStatus::accepted &&
              app.handle(commands::SaveProjectAs{"/virtual/Overflow.vitadaw"}).status ==
              commands::CommandStatus::accepted &&
              app.handle(commands::SetTrackRecordArmed{{1}, true}).status ==
              commands::CommandStatus::accepted,
          "LR-5 application fixture is armed and saved");
    // Saving cleared dirty and left exactly one history entry for the track.
    // A failed take must leave both of them untouched.
    const auto historyCursorBefore = app.history().cursor();
    const auto historyTokenBefore = app.history().currentStateToken();
    const auto canUndoBefore = app.canUndo();
    const auto canRedoBefore = app.canRedo();
    check(!app.session().dirty(),
          "LR-5 the fixture starts from a saved, non-dirty project");
    check(app.handle(commands::Record{}).status == commands::CommandStatus::accepted &&
              app.recordingPhase() == audio::RecordingPhase::capturing,
          "LR-5 Record is accepted before the overflow");

    engine.produceUntilOverflow();
    const auto telemetry = engine.capture.telemetry();
    check(telemetry.overflowEvents == 1 && telemetry.droppedFrames > 0 &&
              telemetry.peakOccupancyFrames <= telemetry.ringCapacityFrames,
          "LR-5 the overflow is observable through bounded ring telemetry");

    app.synchroniseTransport();
    check(app.recordingPhase() == audio::RecordingPhase::failed &&
              app.recordingError() ==
                  "Recording stopped because the capture buffer overflowed",
          "LR-5 overflow is reported to the application as an explicit failure");
    check(app.project().sources().empty() && app.project().tracks()[0].clips.empty() &&
              engine.finalizations == 0 && engine.discards == 1,
          "LR-5 an overflowed take is never finalized or committed");
    check(app.transport().playback == transport::PlaybackState::stopped &&
              !engine.transport.playing,
          "LR-5 transport is stopped after the overflow");
    check(!app.session().dirty(),
          "LR-5 an overflowed take leaves the project clean");
    check(app.history().cursor() == historyCursorBefore &&
              app.history().currentStateToken() == historyTokenBefore &&
              app.canUndo() == canUndoBefore && app.canRedo() == canRedoBefore,
          "LR-5 an overflowed take adds no Undo/Redo entry and no state token");
    check(engine.writer.mismatches() == 0 &&
              engine.writer.frames() == engine.lastDiscarded.acceptedDeviceFrames.value,
          "LR-5 the frames the writer received before the failure are intact");
    check(app.handle(commands::Record{}).status == commands::CommandStatus::accepted,
          "LR-5 a new take is possible after an overflow failure");
    app.shutdownRecording();
}

// ---------------------------------------------------------------------------
// LR-6 long endurance
// ---------------------------------------------------------------------------
void lr6Endurance() {
    const timeline::SampleRate rate{48000.0};
    // Simulated, not wall-clock: VITADAW_ENDURANCE_SECONDS raises the tier.
    const auto seconds = environmentSeconds("VITADAW_ENDURANCE_SECONDS", 1800);
    RigConfig config;
    config.deviceRate = rate;
    config.blockFrames = 512;
    config.takeFrames = static_cast<std::uint64_t>(rate.hertz()) * seconds;
    Rig rig{config};
    rig.beginTake(6, tracks::TrackId{12});
    rig.setJitter(rig.servicePeriod());
    // Warm up, then prove the steady state allocates nothing at all.
    rig.run(static_cast<std::uint64_t>(rate.hertz()));
    const auto allocationsBefore = allocationCount.load();
    const auto realtimeBefore = realtimeAllocations.load();

    // One punctual stall every 300 s, or four over a shorter tier, so every
    // duration exercises the pause/recovery cycle.
    const auto stallEvery = std::min<std::uint64_t>(
        static_cast<std::uint64_t>(rate.hertz()) * 300, config.takeFrames / 4);
    const auto stallFrames = static_cast<std::uint64_t>(rate.hertz());
    std::uint64_t remaining = config.takeFrames -
        static_cast<std::uint64_t>(rate.hertz());
    std::size_t stalls{};
    std::size_t samples{};
    std::size_t worstOccupancy{};
    while (remaining >= config.blockFrames) {
        const auto segment = std::min(stallEvery, remaining);
        remaining -= rig.run(segment);
        worstOccupancy = std::max(worstOccupancy, rig.telemetry().occupancyFrames);
        ++samples;
        if (remaining > stallFrames * 2) {
            // A punctual 1 s consumer pause, well inside the ring.
            rig.suspendService();
            remaining -= rig.run(stallFrames);
            rig.resumeService();
            ++stalls;
        }
    }
    check(allocationCount.load() == allocationsBefore,
          "LR-6 steady-state endurance performs no allocation at all");
    check(realtimeAllocations.load() == realtimeBefore,
          "LR-6 no capture callback allocates during the long run");
    rig.stopTake();
    const auto telemetry = rig.telemetry();
    checkCoherentCounters(telemetry, rig.capacity());

    check(stalls > 0 && samples > 1, "LR-6 the long run exercised punctual stalls");
    check(telemetry.lossless() && telemetry.producedFrames == rig.offeredFrames() &&
              telemetry.consumedFrames == telemetry.producedFrames &&
              telemetry.occupancyFrames == 0,
          "LR-6 a long take loses nothing and leaves no backlog");
    // Worst-case backlog of an injected pause: the jittered backlog already in
    // the ring when the writer stops (up to two service periods plus one
    // callback), the pause itself, and the first jittered period after the
    // writer resumes.
    const auto pauseBound = stallFrames + rig.servicePeriod() * 4 +
                            config.blockFrames * 2;
    check(telemetry.peakOccupancyFrames < rig.capacity() &&
              telemetry.peakOccupancyFrames <= pauseBound,
          "LR-6 occupancy does not accumulate across the run");
    check(worstOccupancy <= telemetry.peakOccupancyFrames,
          "LR-6 sampled occupancy never exceeds the reported high-water mark");
    check(telemetry.captureCalls == telemetry.producedFrames / config.blockFrames &&
              telemetry.drainCalls > 0 &&
              telemetry.emptyDrainCalls < telemetry.drainCalls,
          "LR-6 counters stay coherent over a long run");
    check(rig.writer().mismatches() == 0 &&
              rig.writer().frames() == telemetry.producedFrames,
          "LR-6 the whole simulated session is bit-exact and in order");
    record("LR-6 endurance", telemetry, rate,
           std::to_string(seconds) + " s simulated, jitter plus 1 s stalls every 300 s");
}

// ---------------------------------------------------------------------------
// Instrumentation RT-safety
// ---------------------------------------------------------------------------
void instrumentationRealtimeSafety() {
    const timeline::SampleRate rate{48000.0};
    RigConfig config;
    config.deviceRate = rate;
    config.blockFrames = 256;
    config.takeFrames = static_cast<std::uint64_t>(rate.hertz());
    Rig rig{config};
    rig.beginTake(7, tracks::TrackId{13});
    rig.run(config.blockFrames * 4);
    const auto before = allocationCount.load();
    // The telemetry reader is non-RT by contract, but it must still be a
    // bounded, allocation-free snapshot of plain integer counters.
    realtimeRegion = true;
    const auto telemetry = rig.telemetry();
    const auto snapshot = rig.snapshot();
    realtimeRegion = false;
    check(allocationCount.load() == before && realtimeAllocations.load() == 0,
          "telemetry and snapshot reads allocate nothing");
    check(telemetry.ringCapacityFrames == rig.capacity() &&
              telemetry.producedFrames == snapshot.acceptedDeviceFrames.value,
          "telemetry agrees with the capture snapshot");
    rig.stopTake();
}

void printReport(const std::vector<BurstMeasurement>& bursts) {
    std::cout << "\n--- 0.8.3-B recording endurance metrics ---\n";
    std::cout << std::left << std::setw(24) << "scenario"
              << std::right << std::setw(12) << "sim s"
              << std::setw(11) << "produced"
              << std::setw(11) << "consumed"
              << std::setw(9) << "peak"
              << std::setw(9) << "headrm"
              << std::setw(9) << "backlog"
              << std::setw(10) << "drains"
              << std::setw(9) << "late"
              << std::setw(7) << "drops" << '\n';
    for (const auto& report : reports) {
        std::cout << std::left << std::setw(24) << report.scenario
                  << std::right << std::fixed << std::setprecision(1)
                  << std::setw(12) << report.simulatedSeconds
                  << std::setw(11) << report.telemetry.producedFrames
                  << std::setw(11) << report.telemetry.consumedFrames
                  << std::setw(9) << report.telemetry.peakOccupancyFrames
                  << std::setw(9) << report.telemetry.minimumHeadroomFrames
                  << std::setw(9) << report.telemetry.peakServiceBacklogFrames
                  << std::setw(10) << report.telemetry.drainCalls
                  << std::setw(9) << report.telemetry.lateServiceEvents
                  << std::setw(7) << report.telemetry.droppedFrames << '\n';
    }
    std::cout << "\nLR-4 absorbable burst per callback size (48 kHz, 2 s ring)\n";
    std::cout << std::right << std::setw(8) << "block" << std::setw(11) << "capacity"
              << std::setw(11) << "absorbed" << std::setw(10) << "seconds"
              << std::setw(10) << "headroom" << '\n';
    for (const auto& burst : bursts)
        std::cout << std::setw(8) << burst.blockFrames << std::setw(11) << burst.capacity
                  << std::setw(11) << burst.absorbedFrames
                  << std::setw(10) << std::fixed << std::setprecision(4)
                  << static_cast<double>(burst.absorbedFrames) / 48000.0
                  << std::setw(10) << burst.minimumHeadroom << '\n';
}

} // namespace

void* operator new(std::size_t size) {
    allocationCount.fetch_add(1, std::memory_order_relaxed);
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
    const auto directory = std::filesystem::temp_directory_path() /
                           "vitadaw-recording-endurance-tests";
    std::error_code ignored;
    std::filesystem::remove_all(directory, ignored);
    std::filesystem::create_directories(directory, ignored);
    check(!ignored, "endurance temporary directory created");

    instrumentationRealtimeSafety();
    lr1Nominal(directory);
    lr2Jitter(directory);
    lr3RecoverablePause();
    const auto bursts = lr4CapacityLimit();
    lr5ForcedOverflowAtRing(directory);
    lr5OverflowApplicationPolicy();
    lr6Endurance();

    std::filesystem::remove_all(directory, ignored);
    printReport(bursts);
    check(realtimeAllocations.load() == 0,
          "no capture callback allocated in any endurance scenario");
    std::cout << "\nRecording endurance tests passed\n";
}
