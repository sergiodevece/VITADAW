#include "vitadaw/audio/PreparedProcessingPlan.h"
#include "vitadaw/history/UndoManager.h"
#include "vitadaw/processors/GainProcessor.h"
#include "vitadaw/project/ProjectState.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <functional>
#include <iomanip>
#include <iostream>
#include <memory>
#include <string_view>
#include <vector>

namespace {
using namespace vitadaw;
using Clock = std::chrono::steady_clock;

struct Fixture {
    project::ProjectState project{timeline::SampleRate{48000.0}, "Arrange benchmark"};
    std::vector<float> pcm;
    std::vector<audio::PreparedSourceView> sources;
    std::vector<clips::ClipId> sampleClips;
};

audio::ProcessingPlanSpecification specification(
    const project::ProjectState& project) {
    audio::ProcessingPlanSpecification result;
    result.projectSampleRate = project.sampleRate();
    result.processingSampleRate = project.sampleRate();
    result.masterMix = mixer::prepare(project.masterMix());
    result.masterInserts = project.masterInserts();
    for (const auto& source : project.sources())
        result.sources.push_back({source.id, source.frameCount,
                                  source.sampleRate, source.layout});
    for (const auto& track : project.tracks()) {
        const auto* route = project.routing().findTrackRoute(track.id);
        result.tracks.push_back({track.id, mixer::prepare(track.mix),
            route->destination, track.inserts, track.layout, track.clips});
    }
    for (const auto& bus : project.routing().buses())
        result.buses.push_back({bus.id, mixer::prepare(bus.mix),
            bus.outputDestination, bus.inserts});
    for (const auto& send : project.routing().sends())
        result.sends.push_back({send.id, send.source, send.destination,
            send.tapPoint, mixer::prepare(send.mix)});
    return result;
}

Fixture fixture(std::size_t trackCount, std::size_t clipCount) {
    Fixture result;
    std::vector<tracks::TrackId> tracks;
    tracks.reserve(trackCount);
    for (std::size_t index = 0; index < trackCount; ++index)
        tracks.push_back(result.project.addAudioTrack(
            "Track " + std::to_string(index + 1)));
    const auto imported = result.project.importAudioToTrack(
        tracks.front(), {"/benchmark/shared.wav", {}}, {48000},
        timeline::SampleRate{48000.0}, media::AudioChannelLayout::mono, {0});
    for (std::size_t index = 1; index < clipCount; ++index) {
        const auto clip = result.project.addClip(
            tracks[index % tracks.size()], imported.source,
            {static_cast<std::int64_t>(index * 64)}, {32.0}, {0.0});
        if (!clip.isValid()) std::abort();
    }
    for (std::size_t index = 0;
         index < std::min<std::size_t>(trackCount, 128); ++index) {
        const auto processor = result.project.addProcessor(
            processors::InsertTarget{tracks[index]},
            processors::ProcessorType{processors::internalGainProcessorType});
        if (!processor.isValid()) std::abort();
    }
    const auto busA = result.project.addBus("Benchmark A");
    const auto busB = result.project.addBus("Benchmark B");
    for (std::size_t index = 0; index < trackCount; ++index) {
        if (!result.project.addSend(
                tracks[index], index % 2 == 0 ? busA : busB,
                routing::SendTapPoint::postFaderPostPan, {}).isValid())
            std::abort();
    }
    if (!result.project.addSend(busA, busB,
            routing::SendTapPoint::postFaderPostPan, {}).isValid())
        std::abort();
    result.pcm.assign(48000, 0.125F);
    result.sources.push_back({imported.source, {{result.pcm.data(), nullptr}},
        1, {result.pcm.size()}, timeline::SampleRate{48000.0},
        media::AudioChannelLayout::mono});
    for (const auto& track : result.project.tracks())
        for (const auto& clip : track.clips)
            if (result.sampleClips.size() < 256)
                result.sampleClips.push_back(clip.id);
    return result;
}

enum class Operation { moveClip, moveClips, duplicateClips, deleteClips,
                       addTrack, deleteTrack, reorderTrack, undo, redo };

const char* name(Operation operation) {
    switch (operation) {
    case Operation::moveClip: return "MoveClip";
    case Operation::moveClips: return "MoveClips";
    case Operation::duplicateClips: return "DuplicateClips";
    case Operation::deleteClips: return "DeleteClips";
    case Operation::addTrack: return "AddTrack";
    case Operation::deleteTrack: return "DeleteTrack";
    case Operation::reorderTrack: return "ReorderTrack";
    case Operation::undo: return "Undo";
    case Operation::redo: return "Redo";
    }
    return "Unknown";
}

bool mutate(project::ProjectState& project, Operation operation,
            std::span<const clips::ClipId> clips) {
    switch (operation) {
    case Operation::moveClip: {
        const auto* clip = project.findClip(clips.front());
        return clip && static_cast<bool>(
            project.moveClip(clip->id, {clip->projectStart.value + 1}));
    }
    case Operation::moveClips:
        return static_cast<bool>(project.moveClips(
            clips.first(std::min<std::size_t>(64, clips.size())), 1));
    case Operation::duplicateClips:
        return static_cast<bool>(project.duplicateClips(
            clips.first(std::min<std::size_t>(64, clips.size())), 1000000));
    case Operation::deleteClips:
        return static_cast<bool>(project.deleteClips(
            clips.first(std::min<std::size_t>(64, clips.size()))));
    case Operation::addTrack:
        return project.addAudioTrack("Added").isValid();
    case Operation::deleteTrack:
        return project.removeAudioTrack(project.tracks().back().id).has_value();
    case Operation::reorderTrack:
        return static_cast<bool>(project.reorderAudioTrack(
            project.tracks().back().id, project.tracks().front().id, false));
    case Operation::undo:
    case Operation::redo: {
        auto moved = project;
        const auto edit = moved.moveClips(
            clips.first(std::min<std::size_t>(64, clips.size())), 1);
        if (!edit) return false;
        history::UndoableOperation historyOperation;
        historyOperation.payload = history::MoveClips{edit.before, edit.after};
        if (operation == Operation::undo) {
            project.swap(moved);
            return historyOperation.apply(project, false);
        }
        return historyOperation.apply(project, true);
    }
    }
    return false;
}

template <typename Function>
double medianMilliseconds(int repetitions, Function&& function) {
    std::vector<double> values;
    values.reserve(static_cast<std::size_t>(repetitions));
    function(); // warm-up
    for (int index = 0; index < repetitions; ++index) {
        const auto start = Clock::now();
        function();
        const auto end = Clock::now();
        values.push_back(std::chrono::duration<double, std::milli>(end-start).count());
    }
    std::sort(values.begin(), values.end());
    return values[values.size() / 2];
}

template <typename Function>
double medianMeasuredMilliseconds(int repetitions, Function&& function) {
    std::vector<double> values;
    values.reserve(static_cast<std::size_t>(repetitions));
    static_cast<void>(function()); // warm-up
    for (int index = 0; index < repetitions; ++index)
        values.push_back(function());
    std::sort(values.begin(), values.end());
    return values[values.size() / 2];
}

void runScenario(std::size_t tracks, std::size_t clips, int repetitions) {
    auto data = fixture(tracks, clips);
    constexpr Operation operations[]{Operation::moveClip, Operation::moveClips,
        Operation::duplicateClips, Operation::deleteClips, Operation::addTrack,
        Operation::deleteTrack, Operation::reorderTrack, Operation::undo,
        Operation::redo};
    std::cout << "scenario,operation,model_ms,prepare_ms,commit_ms,total_ms\n";
    for (const auto operation : operations) {
        const auto model = medianMeasuredMilliseconds(repetitions, [&] {
            auto candidate = data.project;
            const auto start = Clock::now();
            if (!mutate(candidate, operation, data.sampleClips)) std::abort();
            const auto end = Clock::now();
            return std::chrono::duration<double, std::milli>(end-start).count();
        });
        auto candidate = data.project;
        if (!mutate(candidate, operation, data.sampleClips)) std::abort();
        const auto spec = specification(candidate);
        const auto prepare = medianMeasuredMilliseconds(repetitions, [&] {
            const auto start = Clock::now();
            auto prepared = audio::prepareProcessingPlanFromSources(
                spec, data.sources, 512);
            const auto end = Clock::now();
            if (!prepared.success()) std::abort();
            return std::chrono::duration<double, std::milli>(end-start).count();
        });
        const auto commit = medianMeasuredMilliseconds(repetitions, [&] {
            auto document = data.project;
            auto next = candidate;
            auto prepared = audio::prepareProcessingPlanFromSources(
                spec, data.sources, 512);
            if (!prepared.success()) std::abort();
            std::unique_ptr<audio::PreparedProcessingBundle> active;
            const auto start = Clock::now();
            document.swap(next);
            active.swap(prepared.prepared);
            const auto end = Clock::now();
            return std::chrono::duration<double, std::milli>(end-start).count();
        });
        const auto total = medianMilliseconds(repetitions, [&] {
            auto document = data.project;
            if (!mutate(document, operation, data.sampleClips)) std::abort();
            auto fullSpec = specification(document);
            auto prepared = audio::prepareProcessingPlanFromSources(
                fullSpec, data.sources, 512);
            if (!prepared.success()) std::abort();
            std::unique_ptr<audio::PreparedProcessingBundle> active;
            active.swap(prepared.prepared);
        });
        std::cout << tracks << "x" << clips << ',' << name(operation) << ','
                  << std::fixed << std::setprecision(6) << model << ','
                  << prepare << ',' << commit << ',' << total << '\n';
    }
}
}

int main() {
    runScenario(64, 1000, 5);
    runScenario(128, 10000, 3);
}
