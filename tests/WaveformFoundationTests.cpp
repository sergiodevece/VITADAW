#include "vitadaw/project/ProjectState.h"
#include "vitadaw/ui/timeline/TimelineModel.h"
#include "vitadaw/ui/timeline/WaveformView.h"
#include "vitadaw/waveform/WaveformCache.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <memory>
#include <vector>

namespace {
using namespace vitadaw;

void check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

waveform::PreparationResult prepare(
    const std::vector<float>& left,
    const std::vector<float>* right = nullptr,
    double rate = 48000.0,
    std::size_t budget = waveform::defaultCacheBudgetBytes) {
    waveform::PcmView view;
    view.channels[0] = left.data();
    view.channels[1] = right == nullptr ? nullptr : right->data();
    view.channelCount = right == nullptr ? 1U : 2U;
    view.frameCount = {left.size()};
    view.sampleRate = timeline::SampleRate{rate};
    return waveform::prepareWaveform(view, budget);
}

void extremaAndAggregation() {
    std::vector<float> left(385, 0.0F), right(385, 0.0F);
    left[0] = -0.75F; left[127] = 0.5F;
    left[128] = -0.25F; left[255] = 0.9F;
    right[4] = -0.4F; right[200] = 0.7F;
    const auto stereo = prepare(left, &right);
    check(stereo.success() && stereo.prepared->channelCount == 2 &&
              stereo.prepared->levels.size() == 3 &&
              stereo.prepared->levels[0].channels[0].size() == 4,
          "stereo source creates independent multiresolution levels");
    const auto& base = stereo.prepared->levels[0];
    check(base.channels[0][0] == waveform::PeakPair{-0.75F, 0.5F} &&
              base.channels[0][1] == waveform::PeakPair{-0.25F, 0.9F} &&
              base.channels[1][0] == waveform::PeakPair{-0.4F, 0.0F} &&
              base.channels[1][1] == waveform::PeakPair{0.0F, 0.7F},
          "mono/stereo extrema are exact and channel independent");
    const auto& aggregate = stereo.prepared->levels[1];
    check(aggregate.framesPerBucket == 256 &&
              aggregate.channels[0][0] == waveform::PeakPair{-0.75F, 0.9F} &&
              aggregate.channels[1][0] == waveform::PeakPair{-0.4F, 0.7F},
          "higher levels aggregate children without rereading PCM");

    std::vector<float> mono(128, 0.25F);
    const auto monoWaveform = prepare(mono);
    check(monoWaveform.success() && monoWaveform.prepared->channelCount == 1 &&
              monoWaveform.prepared->levels[0].channels[0][0] ==
                  waveform::PeakPair{0.25F, 0.25F} &&
              monoWaveform.prepared->levels[0].channels[1].empty(),
          "mono stores one peak pair per bucket");
}

void validationAndBudget() {
    std::vector<float> values(129, 0.0F);
    values[7] = std::numeric_limits<float>::quiet_NaN();
    check(!prepare(values).success(), "NaN PCM is diagnosed rather than clamped");
    values[7] = std::numeric_limits<float>::infinity();
    check(!prepare(values).success(), "infinite PCM is diagnosed rather than clamped");
    values[7] = 0.0F;
    check(!prepare(values, nullptr, 48000.0, 0).success(),
          "injected zero budget fails preparation deterministically");

    float dummy{};
    waveform::PcmView longSource{{&dummy, nullptr}, 1,
        {std::numeric_limits<std::uint64_t>::max() - 64U},
        timeline::SampleRate{48000}};
    const auto rejected = waveform::prepareWaveform(longSource, 1024);
    check(!rejected.success() && !rejected.diagnostic.empty(),
          "64-bit long-source estimate rejects before indexing or overflow");

    std::vector<float> small(128, 0.0F);
    const auto prepared = prepare(small);
    waveform::WaveformCache cache(prepared.prepared->approximateBytes);
    check(cache.store({1}, prepared.prepared) == waveform::WaveformCache::StoreResult::stored &&
              cache.store({2}, prepared.prepared) == waveform::WaveformCache::StoreResult::budgetExceeded &&
              cache.size() == 1,
          "cache enforces its total budget without disturbing existing entries");
    cache.markError({2}, "budget");
    check(cache.state({1}) == waveform::WaveformCache::State::ready &&
              cache.state({2}) == waveform::WaveformCache::State::error &&
              cache.state({3}) == waveform::WaveformCache::State::missing &&
              cache.diagnostic({2}) == "budget",
          "cache distinguishes missing, ready and diagnosed error states");
}

void cacheIdentityAndEdits() {
    std::vector<float> samples(1024, 0.1F);
    const auto prepared = prepare(samples);
    waveform::WaveformCache cache;
    check(cache.store({1}, prepared.prepared) == waveform::WaveformCache::StoreResult::stored,
          "one source waveform stored");
    const auto identity = cache.find({1});
    project::ProjectState project{timeline::SampleRate{48000}};
    const auto first = project.addAudioTrack("A");
    const auto second = project.addAudioTrack("B");
    const auto imported = project.importAudioToTrack(
        first, media::MediaReference{"/tmp/a.wav", {}, {}}, {1024},
        timeline::SampleRate{48000}, media::AudioChannelLayout::mono, {0});
    for (int i = 1; i < 10; ++i)
        check(project.addClip(first, imported.source, {i * 100}, {200}, {0}).isValid(),
              "ten clips share one source");
    const auto duplicated = project.duplicateClip(imported.clip, {1500});
    check(duplicated.succeeded(), "duplicate uses same source");
    check(project.splitClip(imported.clip, {100}).succeeded(), "split uses source windows");
    check(project.moveClip(duplicated.createdClip, second, {1700}).succeeded(),
          "cross-track move keeps source identity");
    check(project.trimClipLeft(duplicated.createdClip, {1750}).succeeded() &&
              project.trimClipRight(duplicated.createdClip, {1800}).succeeded(),
          "both trims remain non-destructive source-window edits");
    check(project.deleteClip(duplicated.createdClip).succeeded(),
          "clip deletion succeeds without deleting derived source data");
    check(cache.size() == 1 && cache.find({1}) == identity,
          "duplicate/split/trim/move/delete never rebuild or evict source waveform");
}

void mappingSelectionAndCulling() {
    std::vector<float> samples(44100, 0.0F);
    samples[22050] = 1.0F;
    const auto prepared = prepare(samples, nullptr, 44100.0);
    check(prepared.success(), "44.1 kHz impulse waveform prepared");
    std::size_t impulseColumn = std::numeric_limits<std::size_t>::max();
    const auto visit = ui::timeline::visitWaveformColumns(
        *prepared.prepared, {0}, {48000.0}, timeline::SampleRate{48000},
        480, 200, 100,
        [&](const ui::timeline::WaveformColumn& column) {
            if (column.channels[0].maximum > 0.9F) impulseColumn = column.pixel;
        });
    check(impulseColumn >= 239 && impulseColumn <= 241 && visit.columns == 100,
          "project-to-source mapping uses 44.1/48 kHz ratio and visible pixels only");
    check(visit.bucketsVisited <= visit.columns * 3,
          "selected level bounds low-zoom work near visible pixel count");

    const auto coarse = ui::timeline::selectWaveformLevel(*prepared.prepared, 10000.0);
    const auto fine = ui::timeline::selectWaveformLevel(*prepared.prepared, 1.0);
    check(coarse > fine && fine == 0,
          "zoom chooses multiresolution level and documents base-level ceiling");

    std::size_t callbacks{};
    const auto narrow = ui::timeline::visitWaveformColumns(
        *prepared.prepared, {1000}, {48000.0}, timeline::SampleRate{48000},
        1000, 731, 17, [&](const auto&) { ++callbacks; });
    check(callbacks == 17 && narrow.columns == 17 && narrow.bucketsVisited <= 51,
          "a 1000-pixel clip visits only its 17 visible waveform columns");
}

void scaleAndSessionIsolation() {
    std::vector<float> samples(128, 0.0F);
    const auto a = prepare(samples);
    samples[0] = -1.0F;
    const auto b = prepare(samples);
    waveform::WaveformCache projectA, projectB;
    check(projectA.store({1}, a.prepared) == waveform::WaveformCache::StoreResult::stored &&
              projectB.store({1}, b.prepared) == waveform::WaveformCache::StoreResult::stored &&
              projectA.find({1}) != projectB.find({1}),
          "SourceId collision is isolated by session cache ownership");
    waveform::WaveformCache scale;
    for (std::uint64_t id = 1; id <= 1000; ++id)
        check(scale.store({id}, a.prepared) == waveform::WaveformCache::StoreResult::stored,
              "1000 source handles fit without copying peak arrays");
    check(scale.size() == 1000 && scale.find({64}) == a.prepared &&
              scale.find({1000}) == a.prepared,
          "64/1000 source lookup preserves immutable shared identities");

    project::ProjectState manyClips{timeline::SampleRate{48000}};
    const auto track = manyClips.addAudioTrack("Scale");
    const auto source = manyClips.importAudioToTrack(
        track, media::MediaReference{"/tmp/scale.wav", {}, {}}, {128},
        timeline::SampleRate{48000}, media::AudioChannelLayout::mono, {0});
    for (int i = 1; i < 1000; ++i)
        check(manyClips.addClip(track, source.source, {i * 480}, {128}, {0}).isValid(),
              "1000 clip model fixture");
    const auto snapshot = ui::timeline::makeTimelineSnapshot(manyClips, 1);
    std::size_t visible{};
    const std::int64_t firstVisible = 100 * 480;
    const std::int64_t lastVisible = 110 * 480;
    for (const auto& clip : snapshot.tracks[0].clips) {
        const auto start = clip.projectStart.value;
        const auto end = start + static_cast<std::int64_t>(std::ceil(clip.duration.value));
        if (start < lastVisible && end > firstVisible) ++visible;
    }
    check(snapshot.tracks[0].clips.size() == 1000 && visible <= 11,
          "1000 clips retain deterministic horizontal culling to the visible range");
}
}

int main() {
    extremaAndAggregation();
    validationAndBudget();
    cacheIdentityAndEdits();
    mappingSelectionAndCulling();
    scaleAndSessionIsolation();
    std::cout << "Waveform foundation tests passed\n";
}
