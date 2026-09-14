#include "vitadaw/audio/RealtimeAudioEngine.h"
#include "vitadaw/project/ProjectState.h"

#include <array>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <new>
#include <string_view>
#include <type_traits>
#include <vector>

namespace {

std::atomic<bool> countAllocations{};
std::atomic<std::size_t> allocationCount{};

void check(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

class CountingProcessor final : public vitadaw::processors::IAudioProcessor {
public:
    explicit CountingProcessor(int& calls) : calls_(calls) {}
    bool prepare(const vitadaw::processors::ProcessingFormat& format) override {
        channels_ = format.channelCount();
        return format.isValid();
    }
    void reset() noexcept override {}
    void applyParameter(vitadaw::processors::PreparedParameterEvent) noexcept override {}
    vitadaw::processors::ProcessStatus processBlock(
        const vitadaw::processors::ProcessorProcessContext& context,
        vitadaw::audio::ConstAudioBlockView input,
        vitadaw::audio::AudioBlockView output) noexcept override {
        ++calls_;
        for (std::size_t channel = 0; channel < channels_; ++channel) {
            for (std::size_t frame = 0; frame < context.frameCount; ++frame) {
                output.channels[channel][frame] =
                    input.channels[channel][frame] * 2.0F;
            }
        }
        return vitadaw::processors::ProcessStatus::processed;
    }
    vitadaw::processors::ProcessingFrameCount latency() const noexcept override {
        return {};
    }
    vitadaw::processors::TailInfo tail() const noexcept override { return {}; }
    vitadaw::processors::ProcessorCapabilities capabilities() const noexcept override {
        return {false, true, true, true};
    }
    std::size_t runtimeMemoryBytes() const noexcept override { return 0; }
private:
    int& calls_;
    std::size_t channels_{};
};

class CountingFactory final : public vitadaw::processors::IAudioProcessorFactory {
public:
    explicit CountingFactory(int& calls) : calls_(calls) {}
    std::unique_ptr<vitadaw::processors::IAudioProcessor> create(
        const vitadaw::processors::ProcessorState&) const override {
        return std::make_unique<CountingProcessor>(calls_);
    }
private:
    int& calls_;
};

vitadaw::audio::ProcessingPlanSpecification base(double rate = 8.0) {
    vitadaw::audio::ProcessingPlanSpecification specification;
    specification.projectSampleRate = vitadaw::timeline::SampleRate{rate};
    specification.processingSampleRate = vitadaw::timeline::SampleRate{rate};
    return specification;
}

} // namespace

void* operator new(std::size_t size) {
    if (countAllocations.load(std::memory_order_relaxed)) {
        allocationCount.fetch_add(1, std::memory_order_relaxed);
    }
    if (auto* memory = std::malloc(size)) {
        return memory;
    }
    throw std::bad_alloc{};
}

void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }

int main() {
    using namespace vitadaw;

    static_assert(!std::is_convertible_v<media::SourceId, std::uint64_t>);
    static_assert(!std::is_convertible_v<clips::ClipId, std::uint64_t>);

    project::ProjectState project{timeline::SampleRate{8.0}, "Foundation"};
    check(project.settings().name == "Foundation" &&
              project.settings().sampleRate == timeline::SampleRate{8.0},
          "project settings must own name and logical sample rate");
    const auto mono = project.addAudioTrack("Mono", media::AudioChannelLayout::mono);
    const auto stereo = project.addAudioTrack("Stereo", media::AudioChannelLayout::stereo);
    const auto imported = project.importAudioToTrack(
        mono, media::MediaReference{"tone.wav", {}}, {8},
        timeline::SampleRate{8.0}, media::AudioChannelLayout::mono);
    check(imported.source == media::SourceId{1} &&
              imported.clip == clips::ClipId{1} &&
              project.sources().size() == 1 &&
              project.findClip(imported.clip) != nullptr,
          "import must create one stable source and one stable clip");
    const auto later = project.addClip(mono, imported.source, {12}, {3.0}, {2.0});
    const auto sameStart = project.addClip(mono, imported.source, {12}, {2.0}, {0.0});
    check(project.findTrack(mono)->clips[1].id == later &&
              project.findTrack(mono)->clips[2].id == sameStart,
          "canonical clip order must be project start then ClipId");
    check(project.duration().value == 15,
          "project duration must use the maximum exclusive clip end");
    check(!project.removeSource(imported.source),
          "a referenced source must not be removable");
    check(project.removeClip(later) && project.findSource(imported.source) != nullptr,
          "removing a clip must retain its source metadata");
    bool rejectedMismatch{};
    try {
        static_cast<void>(project.addClip(stereo, imported.source, {0}, {1.0}));
    } catch (const std::invalid_argument&) {
        rejectedMismatch = true;
    }
    check(rejectedMismatch, "track/source channel-layout mismatch must be rejected");
    bool rejectedBounds{};
    try {
        static_cast<void>(project.addClip(mono, imported.source, {0}, {4.0}, {6.0}));
    } catch (const std::invalid_argument&) {
        rejectedBounds = true;
    }
    check(rejectedBounds, "clip source bounds must be validated precisely");
    check(project.moveClip(sameStart, {4}) &&
              project.findClip(sameStart)->projectStart.value == 4,
          "moving a clip must preserve identity and restore canonical order");

    project::ProjectState duplicateImport{timeline::SampleRate{8.0}};
    const auto a = duplicateImport.addAudioTrack("A");
    const auto b = duplicateImport.addAudioTrack("B");
    const auto firstImport = duplicateImport.importAudioToTrack(
        a, media::MediaReference{"same.wav", {}}, {8},
        timeline::SampleRate{8.0}, media::AudioChannelLayout::mono);
    const auto secondImport = duplicateImport.importAudioToTrack(
        b, media::MediaReference{"same.wav", {}}, {8},
        timeline::SampleRate{8.0}, media::AudioChannelLayout::mono);
    check(firstImport.source != secondImport.source,
          "separate imports of the same path must not deduplicate implicitly");

    project::ProjectState monotonic{timeline::SampleRate{8.0}};
    const auto reusableTrack = monotonic.addAudioTrack("Reusable");
    const auto removedImport = monotonic.importAudioToTrack(
        reusableTrack, media::MediaReference{"old.wav", {}}, {8},
        timeline::SampleRate{8.0}, media::AudioChannelLayout::mono);
    check(monotonic.removeClip(removedImport.clip) &&
              monotonic.removeSource(removedImport.source),
          "an unused source may be removed explicitly");
    const auto newImport = monotonic.importAudioToTrack(
        reusableTrack, media::MediaReference{"new.wav", {}}, {8},
        timeline::SampleRate{8.0}, media::AudioChannelLayout::mono);
    check(newImport.source == media::SourceId{2} &&
              newImport.clip == clips::ClipId{2},
          "source and clip identities must remain monotonic after removal");

    std::vector<float> samples(8, 1.0F);
    audio::PreparedSourceView source{{1}, {{samples.data(), nullptr}}, 1, {8},
                                     timeline::SampleRate{8.0},
                                     media::AudioChannelLayout::mono};
    auto specification = base();
    specification.sources.push_back(
        {{1}, {8}, timeline::SampleRate{8.0}, media::AudioChannelLayout::mono});
    processors::ProcessorState processor;
    processor.id = {1};
    processor.type = {"test.counting"};
    audio::ProcessingPlanTrackSpecification track;
    track.id = {1};
    track.destination = routing::OutputDestination::master();
    track.layout = media::AudioChannelLayout::mono;
    track.clips = {
        {{1}, {1}, {0}, {4.0}, {0.0}},
        {{2}, {1}, {2}, {4.0}, {0.0}},
        {{3}, {1}, {2}, {2.0}, {4.0}}};
    track.inserts.processors.push_back(processor);
    specification.tracks.push_back(track);
    int processCalls{};
    CountingFactory factory{processCalls};
    auto prepared = audio::prepareProcessingPlanFromSources(
        specification, std::span{&source, 1}, 4,
        audio::defaultProcessingMemoryBudgetBytes, &factory);
    check(prepared.success() && prepared.prepared->plan.duration.value == 6,
          "precise clips must prepare into a valid exclusive project duration");
    const auto candidates = audio::findPreparedClipCandidates(
        prepared.prepared->plan, prepared.prepared->plan.tracks[0], 2.0, 4.0);
    check(candidates.count == 3,
          "temporal index must return every overlapping candidate");

    audio::RealtimeAudioEngine engine;
    engine.configure(prepared.prepared->plan, prepared.prepared->runtime);
    engine.deviceInitialising();
    std::array<float*, 0> noOutput{};
    engine.processBlock({noOutput.data(), 0, 0}, timeline::SampleRate{8.0});
    check(engine.tryRequestPlay().accepted, "prepared clip project must play");
    std::array<float, 6> left{}, right{};
    std::array<float*, 2> output{left.data(), right.data()};
    allocationCount.store(0, std::memory_order_relaxed);
    countAllocations.store(true, std::memory_order_relaxed);
    engine.processBlock({output.data(), output.size(), left.size()},
                        timeline::SampleRate{8.0});
    countAllocations.store(false, std::memory_order_relaxed);
    check(allocationCount.load(std::memory_order_relaxed) == 0,
          "multi-clip processBlock must not allocate");
    check(processCalls == 2,
          "one track insert chain must run once per prepared subblock, not per clip");
    check(std::abs(left[0] - 1.41421356F) < 1.0e-5F &&
              std::abs(left[2] - 4.24264068F) < 1.0e-5F &&
              std::abs(left[5] - 1.41421356F) < 1.0e-5F,
          "gaps and overlaps must sum before one track insert/mix pass");
    check(engine.transportSnapshot().position.value == 6 &&
              !engine.transportSnapshot().playing,
          "natural end must remain governed by the global project clock");

    auto mixedRate = base(8.0);
    std::vector<float> slow{0.0F, 1.0F, 2.0F, 3.0F};
    audio::PreparedSourceView slowSource{
        {9}, {{slow.data(), nullptr}}, 1, {4}, timeline::SampleRate{4.0},
        media::AudioChannelLayout::mono};
    mixedRate.sources.push_back(
        {{9}, {4}, timeline::SampleRate{4.0}, media::AudioChannelLayout::mono});
    audio::ProcessingPlanTrackSpecification mixedTrack;
    mixedTrack.id = {9};
    mixedTrack.destination = routing::OutputDestination::master();
    mixedTrack.clips.push_back({{9}, {9}, {2}, {4.0}, {1.0}});
    mixedRate.tracks.push_back(mixedTrack);
    auto mixedPrepared = audio::prepareProcessingPlanFromSources(
        mixedRate, std::span{&slowSource, 1}, 8);
    check(mixedPrepared.success(),
          "source/project rate conversion with a source offset must prepare");
    const auto& mixedClip = mixedPrepared.prepared->plan.clips.front();
    check(std::abs(audio::renderPreparedClipAtProjectPosition(
                       mixedPrepared.prepared->plan, mixedClip, {4.0}).left -
                   2.0F) < 1.0e-6F &&
              std::abs(audio::renderPreparedClipAtProjectPosition(
                           mixedPrepared.prepared->plan, mixedClip, {3.0}).left -
                       1.5F) < 1.0e-6F,
          "random-access rendering must derive source position independently in either order");

    auto shared = base();
    shared.sources.push_back(
        {{77}, {8}, timeline::SampleRate{8.0}, media::AudioChannelLayout::mono});
    audio::ProcessingPlanTrackSpecification sharedTrack;
    sharedTrack.id = {77};
    sharedTrack.destination = routing::OutputDestination::master();
    for (std::uint64_t id = 1; id <= 10; ++id) {
        sharedTrack.clips.push_back({{id}, {77},
                                     {static_cast<std::int64_t>(id - 1)},
                                     {1.0}, {0.0}});
    }
    shared.tracks.push_back(sharedTrack);
    audio::PreparedSourceView sharedSource{
        {77}, {{samples.data(), nullptr}}, 1, {8}, timeline::SampleRate{8.0},
        media::AudioChannelLayout::mono};
    auto sharedPrepared = audio::prepareProcessingPlanFromSources(
        shared, std::span{&sharedSource, 1}, 64);
    check(sharedPrepared.success() &&
              sharedPrepared.prepared->plan.sources.size() == 1 &&
              sharedPrepared.prepared->plan.clips.size() == 10,
          "ten clips must share exactly one dense PreparedSource/PCM view");

    auto scale = base(48000.0);
    std::vector<audio::PreparedSourceView> scaleSources;
    scaleSources.reserve(100);
    for (std::uint64_t id = 1; id <= 100; ++id) {
        scale.sources.push_back({{id}, {8}, timeline::SampleRate{48000.0},
                                 media::AudioChannelLayout::mono});
        scaleSources.push_back({{id}, {{samples.data(), nullptr}}, 1, {8},
                                timeline::SampleRate{48000.0},
                                media::AudioChannelLayout::mono});
    }
    std::uint64_t clipId{1};
    for (std::uint64_t trackId = 1; trackId <= 64; ++trackId) {
        audio::ProcessingPlanTrackSpecification scaleTrack;
        scaleTrack.id = {trackId};
        scaleTrack.destination = routing::OutputDestination::master();
        for (int clipIndex = 0; clipIndex < (trackId <= 40 ? 16 : 15);
             ++clipIndex) {
            scaleTrack.clips.push_back(
                {{clipId++}, {(clipId % 100) + 1}, {clipIndex * 16},
                 {8.0}, {0.0}});
        }
        scale.tracks.push_back(std::move(scaleTrack));
    }
    auto scaled = audio::prepareProcessingPlanFromSources(
        scale, scaleSources, 1024);
    check(clipId == 1001 && scaled.success(),
          "64 tracks, 100 sources and 1000 clips must prepare within limits");
    audio::RealtimeAudioEngine scaleEngine;
    scaleEngine.configure(scaled.prepared->plan, scaled.prepared->runtime);
    scaleEngine.deviceInitialising();
    scaleEngine.processBlock({noOutput.data(), 0, 0},
                             timeline::SampleRate{48000.0});
    check(scaleEngine.tryRequestPlay().accepted,
          "scaled prepared project must remain playable");
    std::array<float, 300> scaleLeft{}, scaleRight{};
    std::array<float*, 2> scaleOutput{scaleLeft.data(), scaleRight.data()};
    allocationCount.store(0, std::memory_order_relaxed);
    countAllocations.store(true, std::memory_order_relaxed);
    scaleEngine.processBlock(
        {scaleOutput.data(), scaleOutput.size(), scaleLeft.size()},
        timeline::SampleRate{48000.0});
    countAllocations.store(false, std::memory_order_relaxed);
    check(allocationCount.load(std::memory_order_relaxed) == 0,
          "the 1000-clip scale session must not allocate in processBlock");

    std::vector<float> longSamples(2048, 0.25F);
    for (const auto capacity : {64U, 128U, 256U, 512U, 1024U}) {
        auto subblocks = base(48000.0);
        subblocks.sources.push_back({{1}, {2048}, timeline::SampleRate{48000.0},
                                     media::AudioChannelLayout::mono});
        audio::ProcessingPlanTrackSpecification subblockTrack;
        subblockTrack.id = {1};
        subblockTrack.destination = routing::OutputDestination::master();
        subblockTrack.clips.push_back(
            {{1}, {1}, {0}, {2048.0}, {0.0}});
        subblocks.tracks.push_back(subblockTrack);
        audio::PreparedSourceView longSource{
            {1}, {{longSamples.data(), nullptr}}, 1, {2048},
            timeline::SampleRate{48000.0}, media::AudioChannelLayout::mono};
        auto subblockPrepared = audio::prepareProcessingPlanFromSources(
            subblocks, std::span{&longSource, 1}, capacity);
        check(subblockPrepared.success(),
              "each required prepared subblock capacity must be accepted");
        audio::RealtimeAudioEngine subblockEngine;
        subblockEngine.configure(subblockPrepared.prepared->plan,
                                 subblockPrepared.prepared->runtime);
        subblockEngine.deviceInitialising();
        subblockEngine.processBlock({noOutput.data(), 0, 0},
                                    timeline::SampleRate{48000.0});
        check(subblockEngine.tryRequestPlay().accepted,
              "subblock test project must play");
        std::vector<float> subLeft(capacity + 17), subRight(capacity + 17);
        std::array<float*, 2> subOutput{subLeft.data(), subRight.data()};
        subblockEngine.processBlock(
            {subOutput.data(), subOutput.size(), subLeft.size()},
            timeline::SampleRate{48000.0});
        check(std::all_of(subLeft.begin(), subLeft.end(), [](float value) {
                  return std::abs(value - 0.176776695F) < 1.0e-6F;
              }),
              "callbacks larger than capacity must remain continuous without scratch residue");
    }

    std::cout << "Source/clip/track foundation tests passed\n";
}
