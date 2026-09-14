#include "vitadaw/audio/RealtimeAudioEngine.h"
#include "vitadaw/audio/TrackRenderer.h"
#include "vitadaw/project/ProjectState.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <new>
#include <string_view>
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

vitadaw::audio::ProcessingPlanSpecification makeSpecification(
    const vitadaw::project::ProjectState& project) {
    using namespace vitadaw;
    audio::ProcessingPlanSpecification result;
    result.projectSampleRate = project.sampleRate();
    result.processingSampleRate = project.sampleRate();
    for (const auto& source : project.sources()) {
        result.sources.push_back(
            {source.id, source.frameCount, source.sampleRate, source.layout});
    }
    for (const auto& track : project.tracks()) {
        const auto* route = project.routing().findTrackRoute(track.id);
        audio::ProcessingPlanTrackSpecification preparedTrack;
        preparedTrack.id = track.id;
        preparedTrack.mix = mixer::prepare(track.mix);
        preparedTrack.destination = route->destination;
        preparedTrack.inserts = track.inserts;
        preparedTrack.layout = track.layout;
        preparedTrack.clips = track.clips;
        result.tracks.push_back(std::move(preparedTrack));
    }
    return result;
}

std::vector<vitadaw::audio::StereoSample> renderTimeline(
    const vitadaw::audio::PreparedProcessingPlan& plan,
    std::size_t frameCount) {
    using namespace vitadaw;
    std::vector<audio::StereoSample> result(frameCount);
    for (std::size_t frame = 0; frame < frameCount; ++frame) {
        for (const auto& clip : plan.clips) {
            const auto sample = audio::renderPreparedClipAtProjectPosition(
                plan, clip,
                timeline::PreciseProjectFramePosition{
                    static_cast<double>(frame)});
            result[frame].left += sample.left;
            result[frame].right += sample.right;
        }
    }
    return result;
}

void checkSplitEquivalence(double projectRate, double sourceRate,
                           vitadaw::media::AudioChannelLayout layout) {
    using namespace vitadaw;
    constexpr std::uint64_t sourceFrames = 441;
    project::ProjectState original{timeline::SampleRate{projectRate}};
    const auto track = original.addAudioTrack("Edit", layout);
    const auto imported = original.importAudioToTrack(
        track, media::MediaReference{"synthetic.wav", {}}, {sourceFrames},
        timeline::SampleRate{sourceRate}, layout);

    std::vector<float> left(sourceFrames), right(sourceFrames);
    for (std::size_t index = 0; index < left.size(); ++index) {
        left[index] = static_cast<float>(index) / 512.0F;
        right[index] = -left[index];
    }
    const audio::PreparedSourceView source{
        imported.source,
        {{left.data(), layout == media::AudioChannelLayout::stereo
                           ? right.data()
                           : nullptr}},
        media::channelCount(layout), {sourceFrames},
        timeline::SampleRate{sourceRate}, layout};
    auto before = audio::prepareProcessingPlanFromSources(
        makeSpecification(original), std::span{&source, 1}, 128);
    check(before.success(), "original split-equivalence plan must prepare");

    auto edited = original;
    const auto duration = edited.findClip(imported.clip)->duration.value;
    const auto splitFrame = static_cast<std::int64_t>(duration * 0.37);
    const auto split = edited.splitClip(imported.clip, {splitFrame});
    check(split && split.createdClip.isValid(),
          "split must retain left identity and create a right identity");
    const auto* leftClip = edited.findClip(imported.clip);
    const auto* rightClip = edited.findClip(split.createdClip);
    check(leftClip != nullptr && rightClip != nullptr &&
              std::abs((leftClip->duration.value + rightClip->duration.value) -
                       duration) < 1.0e-9 &&
              std::abs(rightClip->sourceOffset.value -
                       static_cast<double>(splitFrame) * sourceRate /
                           projectRate) < 1.0e-9,
          "split must partition precise project/source time without a gap");
    auto after = audio::prepareProcessingPlanFromSources(
        makeSpecification(edited), std::span{&source, 1}, 128);
    check(after.success() && after.prepared->plan.sources.size() == 1 &&
              after.prepared->plan.clips.size() == 2,
          "split clips must share one PreparedSource");
    const auto outputFrames = static_cast<std::size_t>(original.duration().value);
    const auto originalRender = renderTimeline(before.prepared->plan, outputFrames);
    const auto splitRender = renderTimeline(after.prepared->plan, outputFrames);
    for (std::size_t frame = 0; frame < outputFrames; ++frame) {
        check(std::abs(originalRender[frame].left - splitRender[frame].left) <
                      1.0e-6F &&
                  std::abs(originalRender[frame].right -
                           splitRender[frame].right) < 1.0e-6F,
              "split render must equal the original at every project frame");
    }
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
    using EditStatus = project::ProjectState::ClipEditStatus;

    project::ProjectState project{timeline::SampleRate{48000.0}, "Editing"};
    const auto track = project.addAudioTrack("Mono");
    const auto imported = project.importAudioToTrack(
        track, media::MediaReference{"ramp.wav", {}}, {48000},
        timeline::SampleRate{48000.0}, media::AudioChannelLayout::mono);
    const auto bus = project.addBus("Edit Aux");
    check(project.setTrackOutputDestination(
              track, routing::OutputDestination::toBus(bus)),
          "editing regression route must be valid");
    const auto send = project.addSend(
        track, bus, routing::SendTapPoint::preFaderPrePan,
        mixer::SendMixState{});
    const auto processor = project.addProcessor(
        processors::InsertTarget{track}, processors::ProcessorType{"test.edit"});
    const auto original = *project.findClip(imported.clip);

    const auto moved = project.moveClip(imported.clip, {48000});
    check(moved && *project.findClip(imported.clip) == clips::AudioClip{
                         original.id, original.source, {48000},
                         original.duration, original.sourceOffset} &&
              project.duration().value == 96000,
          "MoveClip must alter only projectStart and extend duration");
    check(project.moveClip(imported.clip, {0}) &&
              *project.findClip(imported.clip) == original &&
              project.findSend(send) != nullptr &&
              project.findProcessor(processor) != nullptr &&
              project.routing().findTrackRoute(track)->destination ==
                  routing::OutputDestination::toBus(bus),
          "moving back must restore clip state without changing inserts/sends/routing");

    const auto duplicated = project.duplicateClip(imported.clip, {24000});
    check(duplicated && duplicated.createdClip == clips::ClipId{2} &&
              project.findClip(duplicated.createdClip)->source == imported.source &&
              project.sources().size() == 1 && project.duration().value == 72000,
          "DuplicateClip must create only a ClipId and reuse SourceId");
    const auto intersecting = project.clipsIntersectingRange(track, {30000}, {31000});
    check(intersecting.size() == 2 && project.clipsForTrack(track).size() == 2,
          "portable timeline queries must use stable IDs and half-open ranges");

    const auto duplicateBeforeFailure = project.clipsForTrack(track).size();
    check(project.moveClip({9999}, {0}).status == EditStatus::clipNotFound &&
              project.moveClip(imported.clip, {-1}).status ==
                  EditStatus::invalidPosition &&
              project.splitClip(imported.clip, {0}).status ==
                  EditStatus::invalidPosition &&
              project.splitClip(imported.clip, {48000}).status ==
                  EditStatus::invalidPosition &&
              project.trimClipLeft(imported.clip, {48000}).status ==
                  EditStatus::zeroLengthClip &&
              project.trimClipRight(imported.clip, {0}).status ==
                  EditStatus::zeroLengthClip &&
              project.clipsForTrack(track).size() == duplicateBeforeFailure,
          "invalid edits must return explicit status without partial mutation");
    check(project.moveClip(imported.clip,
                           {std::numeric_limits<std::int64_t>::max()})
                  .status == EditStatus::invalidPosition,
          "MoveClip must reject an unrepresentable exclusive end");
    check(project.deleteClip(duplicated.createdClip) &&
              project.findClip(imported.clip) != nullptr &&
              project.findSource(imported.source) != nullptr &&
              project.clipsForTrack(track).size() == 1,
          "deleting one of several clips must preserve siblings and source");

    project::ProjectState trimProject{timeline::SampleRate{48000.0}};
    const auto trimTrack = trimProject.addAudioTrack("Trim");
    const auto trimImport = trimProject.importAudioToTrack(
        trimTrack, media::MediaReference{"trim.wav", {}}, {44100},
        timeline::SampleRate{44100.0}, media::AudioChannelLayout::mono,
        {100});
    check(trimProject.trimClipLeft(trimImport.clip, {24100}).succeeded(),
          "TrimClipLeft must accept an interior boundary");
    const auto* leftTrimmed = trimProject.findClip(trimImport.clip);
    check(leftTrimmed->projectStart.value == 24100 &&
              std::abs(leftTrimmed->sourceOffset.value - 22050.0) < 1.0e-9 &&
              std::abs(leftTrimmed->duration.value - 24000.0) < 1.0e-9,
          "left trim must advance source offset by the exact rate ratio");
    const auto leftTrimmedOffset = leftTrimmed->sourceOffset;
    check(trimProject.trimClipLeft(trimImport.clip, {24100}) &&
              trimProject.trimClipRight(trimImport.clip, {36100}),
          "equal left trim is a documented no-op and right trim may shorten");
    const auto* rightTrimmed = trimProject.findClip(trimImport.clip);
    check(rightTrimmed->projectStart.value == 24100 &&
              rightTrimmed->sourceOffset == leftTrimmedOffset &&
              std::abs(rightTrimmed->duration.value - 12000.0) < 1.0e-9 &&
              trimProject.duration().value == 36100,
          "right trim must change only duration and project content end");

    const auto sourceStillUsed = trimImport.source;
    check(trimProject.deleteClip(trimImport.clip) &&
              trimProject.projectContentDuration().value == 0 &&
              trimProject.findSource(sourceStillUsed) != nullptr &&
              trimProject.clipsForTrack(trimTrack).empty(),
          "DeleteClip must leave source metadata and an empty complete track");

    checkSplitEquivalence(48000.0, 48000.0,
                          media::AudioChannelLayout::mono);
    checkSplitEquivalence(48000.0, 44100.0,
                          media::AudioChannelLayout::mono);
    checkSplitEquivalence(44100.0, 48000.0,
                          media::AudioChannelLayout::mono);
    checkSplitEquivalence(48000.0, 44100.0,
                          media::AudioChannelLayout::stereo);

    project::ProjectState shortProject{timeline::SampleRate{48000.0}};
    const auto shortTrack = shortProject.addAudioTrack("Short");
    const auto shortImport = shortProject.importAudioToTrack(
        shortTrack, media::MediaReference{"short.wav", {}}, {2},
        timeline::SampleRate{44100.0}, media::AudioChannelLayout::mono);
    const auto shortDuration = shortProject.findClip(shortImport.clip)->duration.value;
    const auto shortSplit = shortProject.splitClip(shortImport.clip, {1});
    check(shortSplit &&
              std::abs(shortProject.findClip(shortImport.clip)->duration.value +
                           shortProject.findClip(shortSplit.createdClip)->duration.value -
                       shortDuration) < 1.0e-12,
          "a two-source-frame clip must split without losing precise duration");

    project::ProjectState sharing{timeline::SampleRate{48000.0}};
    const auto sharingTrack = sharing.addAudioTrack("Sharing");
    const auto sharingImport = sharing.importAudioToTrack(
        sharingTrack, media::MediaReference{"shared.wav", {}}, {4096},
        timeline::SampleRate{48000.0}, media::AudioChannelLayout::mono);
    for (std::int64_t index = 1; index < 100; ++index) {
        const auto result = sharing.duplicateClip(sharingImport.clip,
                                                  {index * 8});
        check(result.succeeded(),
              "100 edited clips must fit the prepared sharing model");
    }
    std::vector<float> sharedPcm(4096);
    for (std::size_t index = 0; index < sharedPcm.size(); ++index) {
        sharedPcm[index] = static_cast<float>(index % 37) / 37.0F;
    }
    audio::PreparedSourceView sharedSource{
        sharingImport.source, {{sharedPcm.data(), nullptr}}, 1, {4096},
        timeline::SampleRate{48000.0}, media::AudioChannelLayout::mono};
    auto sharedPlan = audio::prepareProcessingPlanFromSources(
        makeSpecification(sharing), std::span{&sharedSource, 1}, 64);
    check(sharedPlan.success() && sharedPlan.prepared->plan.sources.size() == 1 &&
              sharedPlan.prepared->plan.clips.size() == 100,
          "100 DuplicateClip operations must retain one PCM/PreparedSource");

    std::array<float*, 0> noOutput{};
    for (const auto capacity : {64U, 128U, 256U, 512U, 1024U}) {
        auto subblockPlan = audio::prepareProcessingPlanFromSources(
            makeSpecification(sharing), std::span{&sharedSource, 1}, capacity);
        check(subblockPlan.success(),
              "edited timeline must prepare at every required capacity");
        audio::RealtimeAudioEngine engine;
        engine.configure(subblockPlan.prepared->plan,
                         subblockPlan.prepared->runtime);
        engine.deviceInitialising();
        engine.processBlock({noOutput.data(), 0, 0},
                            timeline::SampleRate{48000.0});
        check(engine.tryRequestPlay().accepted,
              "an edited prepared project must remain playable");
        std::vector<float> left(capacity + 17), right(capacity + 17);
        std::array<float*, 2> output{left.data(), right.data()};
        allocationCount.store(0, std::memory_order_relaxed);
        countAllocations.store(true, std::memory_order_relaxed);
        engine.processBlock({output.data(), output.size(), left.size()},
                            timeline::SampleRate{48000.0});
        countAllocations.store(false, std::memory_order_relaxed);
        check(allocationCount.load(std::memory_order_relaxed) == 0 &&
                  std::any_of(left.begin(), left.end(), [](float value) {
                      return value != 0.0F;
                  }),
              "edited callbacks over capacity must stay continuous and allocation-free");
    }

    project::ProjectState capacity{timeline::SampleRate{48000.0}};
    const auto capacityTrack = capacity.addAudioTrack("Capacity");
    const auto capacityImport = capacity.importAudioToTrack(
        capacityTrack, media::MediaReference{"capacity.wav", {}}, {8192},
        timeline::SampleRate{48000.0}, media::AudioChannelLayout::mono);
    for (std::size_t index = 1;
         index < project::ProjectState::maximumClipsPerTrack; ++index) {
        check(capacity.duplicateClip(capacityImport.clip,
                                     {static_cast<std::int64_t>(index)})
                  .succeeded(),
              "duplicates below per-track capacity must succeed");
    }
    check(capacity.duplicateClip(capacityImport.clip, {0}).status ==
              EditStatus::capacityExceeded &&
              capacity.clipsForTrack(capacityTrack).size() ==
                  project::ProjectState::maximumClipsPerTrack,
          "DuplicateClip must fail atomically at the configured capacity");

    std::cout << "Timeline editing operations tests passed\n";
}
