#include "vitadaw/audio/OfflineRenderer.h"
#include "vitadaw/audio/RealtimeAudioEngine.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <string_view>
#include <vector>

namespace {

using namespace vitadaw;

void check(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

void checkNear(float actual, float expected, std::string_view message) {
    check(std::abs(actual - expected) < 1.0e-6F, message);
}

audio::PreparedSourceView monoSource(media::SourceId id,
                                     const std::vector<float>& samples,
                                     double rate = 8.0) {
    return {id, {{samples.data(), nullptr}}, 1,
            {static_cast<std::uint64_t>(samples.size())},
            timeline::SampleRate{rate}, media::AudioChannelLayout::mono};
}

audio::PreparedSourceView stereoSource(media::SourceId id,
                                       const std::vector<float>& left,
                                       const std::vector<float>& right,
                                       double rate = 8.0) {
    return {id, {{left.data(), right.data()}}, 2,
            {static_cast<std::uint64_t>(left.size())},
            timeline::SampleRate{rate}, media::AudioChannelLayout::stereo};
}

audio::ProcessingPlanSpecification baseSpecification(double rate = 8.0) {
    audio::ProcessingPlanSpecification specification;
    specification.projectSampleRate = timeline::SampleRate{rate};
    return specification;
}

void addSourceSpecification(audio::ProcessingPlanSpecification& specification,
                            const audio::PreparedSourceView& source) {
    specification.sources.push_back(
        {source.id, source.frameCount, source.sampleRate, source.layout});
}

void addTrack(audio::ProcessingPlanSpecification& specification,
              tracks::TrackId id, media::AudioChannelLayout layout,
              std::vector<clips::AudioClip> clips,
              float linearGain = 1.0F) {
    audio::ProcessingPlanTrackSpecification track;
    track.id = id;
    track.layout = layout;
    track.destination = routing::OutputDestination::master();
    track.mix = mixer::prepareLinear(linearGain, {});
    track.clips = std::move(clips);
    specification.tracks.push_back(std::move(track));
}

audio::OfflineRenderResult render(
    audio::ProcessingPlanSpecification specification,
    std::span<const audio::PreparedSourceView> sources,
    std::int64_t start, std::int64_t end, std::size_t blockSize = 4,
    double outputRate = 8.0, std::uint32_t channels = 2,
    audio::OfflineRenderCallbacks callbacks = {}) {
    return audio::renderOffline(
        std::move(specification), sources,
        {{start}, {end}, timeline::SampleRate{outputRate}, channels, blockSize},
        callbacks);
}

std::vector<std::vector<float>> renderRealtimeEquivalent(
    audio::ProcessingPlanSpecification specification,
    std::span<const audio::PreparedSourceView> sources,
    std::int64_t start, std::int64_t end, std::size_t blockSize) {
    specification.processingSampleRate = specification.projectSampleRate;
    specification.processingMode = processors::ProcessingMode::realtime;
    auto prepared = audio::prepareProcessingPlanFromSources(
        specification, sources, blockSize);
    check(prepared.success(), "realtime comparison plan must prepare");
    audio::RealtimeAudioEngine engine;
    engine.configure(prepared.prepared->plan, prepared.prepared->runtime);
    engine.deviceInitialising();
    engine.deviceConsumerStarted();
    check(engine.tryRequestSeek({start}).accepted,
          "realtime comparison seek must be accepted");
    check(engine.tryRequestPlay().accepted,
          "realtime comparison play must be accepted");
    const auto frames = static_cast<std::size_t>(end - start);
    std::vector<std::vector<float>> result(2, std::vector<float>(frames));
    for (std::size_t offset = 0; offset < frames;) {
        const auto count = std::min(blockSize, frames - offset);
        std::array<float*, 2> output{result[0].data() + offset,
                                     result[1].data() + offset};
        engine.processBlock({output.data(), output.size(), count},
                            specification.projectSampleRate);
        offset += count;
    }
    return result;
}

struct CancellationProbe {
    std::uint64_t completed{};
    std::uint64_t total{};
};

struct StreamingProbe {
    std::uint64_t frames{};
    std::size_t blocks{};
    std::size_t largestBlock{};
    bool silenceOnly{true};
};

bool cancelAfterFirstBlock(void* context) noexcept {
    return static_cast<CancellationProbe*>(context)->completed >= 3;
}

void captureProgress(void* context, timeline::DeviceFrameCount completed,
                     timeline::DeviceFrameCount total) noexcept {
    auto& probe = *static_cast<CancellationProbe*>(context);
    probe.completed = completed.value;
    probe.total = total.value;
}

bool consumeStreaming(void* context, audio::ConstAudioBlockView block) noexcept {
    auto& probe = *static_cast<StreamingProbe*>(context);
    if (!block.isValid()) return false;
    probe.frames += block.frameCount;
    ++probe.blocks;
    probe.largestBlock = std::max(probe.largestBlock, block.frameCount);
    for (std::size_t channel = 0; channel < block.channelCount; ++channel) {
        for (std::size_t frame = 0; frame < block.frameCount; ++frame) {
            if (block.channels[channel][frame] != 0.0F) probe.silenceOnly = false;
        }
    }
    return true;
}

} // namespace

int main() {
    constexpr float centre = 0.70710678F;

    // A: no plan content still has an explicit render range and yields silence.
    auto empty = render(baseSpecification(), {}, 0, 7, 4);
    check(empty.success() && empty.renderedFrames.value == 7 &&
              empty.channels.size() == 2 &&
              std::all_of(empty.channels[0].begin(), empty.channels[0].end(),
                          [](float sample) { return sample == 0.0F; }),
          "empty project must render the requested silence without a device");

    const std::vector<float> monoSamples{1, 2, 3, 4, 5, 6, 7, 8};
    const auto mono = monoSource({1}, monoSamples);
    auto monoSpec = baseSpecification();
    addSourceSpecification(monoSpec, mono);
    addTrack(monoSpec, {1}, media::AudioChannelLayout::mono,
             {{{1}, {1}, {0}, {8.0}, {0.0}}});

    // B: mono project output follows the production equal-power centre law.
    auto oneMono = render(monoSpec, std::span{&mono, 1}, 0, 8, 4);
    check(oneMono.success() && oneMono.channels[0].size() == 8,
          "one mono clip must render");
    for (std::size_t frame = 0; frame < monoSamples.size(); ++frame) {
        checkNear(oneMono.channels[0][frame], monoSamples[frame] * centre,
                  "mono clip left sample");
        checkNear(oneMono.channels[1][frame], monoSamples[frame] * centre,
                  "mono clip right sample");
    }

    const std::vector<float> stereoLeft{1, 2, 3, 4};
    const std::vector<float> stereoRight{4, 3, 2, 1};
    const auto stereo = stereoSource({2}, stereoLeft, stereoRight);
    auto stereoSpec = baseSpecification();
    addSourceSpecification(stereoSpec, stereo);
    addTrack(stereoSpec, {2}, media::AudioChannelLayout::stereo,
             {{{2}, {2}, {0}, {4.0}, {0.0}}});
    auto oneStereo = render(stereoSpec, std::span{&stereo, 1}, 0, 4, 3);
    check(oneStereo.success() && oneStereo.channels[0] == stereoLeft &&
              oneStereo.channels[1] == stereoRight,
          "one stereo clip must preserve channels");

    // C/D: tracks sum independently and overlapping clips use the same additive
    // semantics as realtime playback. Track gain is part of that shared path.
    const std::vector<float> half(8, 0.5F);
    const auto second = monoSource({2}, half);
    auto summedSpec = baseSpecification();
    addSourceSpecification(summedSpec, mono);
    addSourceSpecification(summedSpec, second);
    addTrack(summedSpec, {1}, media::AudioChannelLayout::mono,
             {{{1}, {1}, {0}, {8.0}, {0.0}},
              {{2}, {1}, {2}, {4.0}, {0.0}}},
             0.5F);
    addTrack(summedSpec, {2}, media::AudioChannelLayout::mono,
             {{{3}, {2}, {0}, {8.0}, {0.0}}});
    const std::array sources{mono, second};
    auto summed = render(summedSpec, sources, 0, 8, 8);
    check(summed.success(), "multiple and overlapping clips must render");
    checkNear(summed.channels[0][1], (1.0F + 0.5F) * centre,
              "multiple tracks sum outside overlap");
    checkNear(summed.channels[0][2], (3.0F * 0.5F + 1.0F * 0.5F + 0.5F) * centre,
              "overlapping clips sum on their track before track gain");

    // E/H: the clip begins and ends inside one processing block.
    auto shiftedSpec = baseSpecification();
    addSourceSpecification(shiftedSpec, mono);
    addTrack(shiftedSpec, {1}, media::AudioChannelLayout::mono,
             {{{1}, {1}, {3}, {4.0}, {1.0}}});
    auto shifted = render(shiftedSpec, std::span{&mono, 1}, 0, 9, 9);
    check(shifted.success(), "shifted clip must render in one block");
    check(shifted.channels[0][0] == 0.0F &&
              shifted.channels[0][2] == 0.0F &&
              shifted.channels[0][7] == 0.0F,
          "range before and after a clip must remain silent");
    for (std::size_t frame = 0; frame < 4; ++frame) {
        checkNear(shifted.channels[0][3 + frame],
                  monoSamples[1 + frame] * centre,
                  "clip boundary inside block must be sample accurate");
    }

    // F/G: range boundaries inside the clip map to the matching source samples.
    auto insideStart = render(shiftedSpec, std::span{&mono, 1}, 5, 7, 8);
    check(insideStart.success(), "range beginning inside clip must render");
    checkNear(insideStart.channels[0][0], monoSamples[3] * centre,
              "inside start first sample");
    checkNear(insideStart.channels[0][1], monoSamples[4] * centre,
              "inside start second sample");
    auto insideEnd = render(shiftedSpec, std::span{&mono, 1}, 3, 5, 8);
    check(insideEnd.success() && insideEnd.channels[0].size() == 2,
          "range ending inside clip must be exclusive");
    checkNear(insideEnd.channels[0][1], monoSamples[2] * centre,
              "inside end last sample");

    // I/J and realtime equivalence: a tail block is exact and repeated jobs do
    // not retain runtime/processor/transport state.
    auto tailBlock = render(monoSpec, std::span{&mono, 1}, 0, 8, 3);
    auto repeated = render(monoSpec, std::span{&mono, 1}, 0, 8, 3);
    check(tailBlock.success() && tailBlock.renderedFrames.value == 8 &&
              tailBlock.channels == repeated.channels,
          "non-divisible tail and repeated render must be deterministic");
    const auto realtime = renderRealtimeEquivalent(
        monoSpec, std::span{&mono, 1}, 0, 8, 3);
    check(tailBlock.channels == realtime,
          "offline and realtime must share identical production rendering");

    // The streaming primitive keeps only processing-sized buffers regardless
    // of the requested duration. It reports a real partial final block and
    // never materialises an output vector for this consumer.
    StreamingProbe streaming;
    auto streamed = audio::renderOfflineBlocks(
        baseSpecification(), {},
        {{0}, {200003}, timeline::SampleRate{8.0}, 2, 257},
        {&streaming, consumeStreaming});
    check(streamed.success() && streamed.renderedFrames.value == 200003 &&
              streaming.frames == 200003 && streaming.blocks > 1 &&
              streaming.largestBlock == 257 && streaming.silenceOnly,
          "streaming render must consume bounded silent blocks including a tail");

    // The requested output rate uses the same exact device/project clock ratio.
    auto halfRate = render(monoSpec, std::span{&mono, 1}, 0, 8, 3, 4.0);
    check(halfRate.success() && halfRate.renderedFrames.value == 4,
          "output sample rate must determine the exact result frame count");
    for (std::size_t frame = 0; frame < 4; ++frame) {
        checkNear(halfRate.channels[0][frame], monoSamples[frame * 2] * centre,
                  "different output rate must follow the shared exact clock");
    }

    auto monoOutput = render(stereoSpec, std::span{&stereo, 1}, 0, 4, 3, 8.0, 1);
    check(monoOutput.success() && monoOutput.channels.size() == 1 &&
              monoOutput.channels[0] == stereoLeft,
          "mono output configuration must use the existing first-channel semantics");

    // K: an unrelated installed realtime transport is unchanged by the job.
    auto livePreparation = audio::prepareProcessingPlanFromSources(
        monoSpec, std::span{&mono, 1}, 4);
    check(livePreparation.success(), "live transport fixture must prepare");
    audio::RealtimeAudioEngine liveEngine;
    liveEngine.configure(livePreparation.prepared->plan,
                         livePreparation.prepared->runtime);
    liveEngine.deviceInitialising();
    liveEngine.deviceConsumerStarted();
    check(liveEngine.tryRequestPlay().accepted,
          "live transport fixture must play");
    std::array<float, 2> liveLeft{}, liveRight{};
    std::array<float*, 2> liveOutput{liveLeft.data(), liveRight.data()};
    liveEngine.processBlock({liveOutput.data(), 2, 2},
                            timeline::SampleRate{8.0});
    const auto before = liveEngine.transportSnapshot();
    const auto recordingBefore = liveEngine.recordingSnapshot();
    auto isolated = render(monoSpec, std::span{&mono, 1}, 2, 6, 2);
    const auto after = liveEngine.transportSnapshot();
    const auto recordingAfter = liveEngine.recordingSnapshot();
    check(isolated.success() && before.playback == after.playback &&
              before.position == after.position &&
              before.loopEnabled == after.loopEnabled &&
              before.monitoringEnabled == after.monitoringEnabled &&
              recordingBefore.phase == recordingAfter.phase,
          "offline rendering must not mutate installed transport, loop, monitoring or recording state");

    // L: malformed ranges/configurations fail before producing audio.
    check(render(monoSpec, std::span{&mono, 1}, 4, 4).status ==
              audio::OfflineRenderStatus::invalidRequest,
          "empty range must fail");
    check(render(monoSpec, std::span{&mono, 1}, -1, 4).status ==
              audio::OfflineRenderStatus::invalidRequest,
          "negative start must fail");
    check(render(monoSpec, std::span{&mono, 1}, 0, 4, 0).status ==
              audio::OfflineRenderStatus::invalidRequest,
          "zero processing block must fail");
    check(render(monoSpec, std::span{&mono, 1}, 0, 4, 4, 0.0).status ==
              audio::OfflineRenderStatus::invalidRequest,
          "invalid output rate must fail");
    check(render(monoSpec, std::span{&mono, 1}, 0, 4, 4, 8.0, 3).status ==
              audio::OfflineRenderStatus::invalidRequest,
          "unsupported output channel count must fail");

    CancellationProbe probe;
    auto cancelled = render(
        monoSpec, std::span{&mono, 1}, 0, 8, 3, 8.0, 2,
        {&probe, cancelAfterFirstBlock, captureProgress});
    check(cancelled.status == audio::OfflineRenderStatus::cancelled &&
              cancelled.renderedFrames.value == 3 && probe.completed == 3 &&
              probe.total == 8,
          "cancellation and progress must be observable between blocks");

    std::cout << "All offline renderer tests passed\n";
    return EXIT_SUCCESS;
}
