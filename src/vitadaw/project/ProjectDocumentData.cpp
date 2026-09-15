#include "vitadaw/project/ProjectState.h"
#include "vitadaw/audio/PreparedProcessingPlan.h"
#include "vitadaw/audio/PreparedTemporalContext.h"
#include "vitadaw/processors/GainProcessor.h"
#include <algorithm>
#include <unordered_set>
#include <unordered_map>
#include <functional>

namespace vitadaw::project {
ProjectState::DocumentData ProjectState::documentData() const {
    return {settings_, tracks_, sources_, routing_.documentData(), nextTrackId_,
        nextSourceId_, nextClipId_, nextProcessorId_, masterMix_, masterInserts_, musicalTime_, loopRange_};
}
std::unique_ptr<ProjectState> ProjectState::fromDocumentData(DocumentData data) {
    using namespace audio;
    if (!audio::prepareTemporalContext(data.musicalTime, data.loopRange,
            data.settings.sampleRate, data.settings.sampleRate, 0).success()) return {};
    if (!data.settings.sampleRate.isValid() || data.settings.name.size() > 4096 ||
        data.tracks.size() > maximumTracks || data.sources.size() > maximumSources ||
        data.routing.buses.size() > maximumPreparedBuses ||
        data.routing.sends.size() > maximumPreparedSends ||
        data.routing.trackRoutes.size() != data.tracks.size() ||
        !data.masterMix.gain.isValid()) return {};
    if (!data.nextTrackId.isValid() || !data.nextSourceId.isValid() ||
        !data.nextClipId.isValid() || !data.nextProcessorId.isValid() ||
        !data.routing.nextBusId.isValid() || !data.routing.nextSendId.isValid()) return {};
    std::unordered_set<std::uint64_t> tracks, sources, clips, buses, sends, processors;
    const auto addId = [](auto id, auto next, auto& set) {
        return id.isValid() && id.value < next.value && set.insert(id.value).second;
    };
    const auto layoutValid = [](auto layout) {
        return layout == media::AudioChannelLayout::mono || layout == media::AudioChannelLayout::stereo;
    };
    const auto chainValid = [&](const processors::InsertChain& chain) {
        if (chain.processors.size() > maximumPreparedProcessorsPerChain) return false;
        for (const auto& processor : chain.processors) {
            if (!addId(processor.id, data.nextProcessorId, processors) ||
                processor.type.identifier != processors::internalGainProcessorType ||
                !processor.serializedState.empty() || processor.parameters.size() != 1 ||
                processor.parameters[0].id != processors::gainParameterId ||
                !processors::GainProcessor::isValidGainDb(processor.parameters[0].value)) return false;
        }
        return processors.size() <= maximumPreparedProcessors;
    };
    for (const auto& source : data.sources) {
        if (!source.isValid() || !layoutValid(source.layout) ||
            !addId(source.id, data.nextSourceId, sources)) return {};
    }
    for (const auto& track : data.tracks) {
        if (!addId(track.id, data.nextTrackId, tracks) || track.name.size() > 4096 ||
            !layoutValid(track.layout) || !track.mix.gain.isValid() || !track.mix.pan.isValid() ||
            track.clips.size() > maximumClipsPerTrack || !chainValid(track.inserts)) return {};
    }
    for (const auto& bus : data.routing.buses) {
        if (!addId(bus.id, data.routing.nextBusId, buses) || bus.name.size() > 4096 ||
            !bus.mix.gain.isValid() || !bus.mix.balance.isValid() || !chainValid(bus.inserts)) return {};
    }
    if (!chainValid(data.masterInserts)) return {};
    const auto destinationValid = [&](const routing::OutputDestination& dest) {
        return dest.isValid() && (dest.kind == routing::DestinationKind::master || buses.contains(dest.bus.value));
    };
    std::unordered_set<std::uint64_t> routed;
    for (const auto& route : data.routing.trackRoutes)
        if (!tracks.contains(route.track.value) || !routed.insert(route.track.value).second ||
            !destinationValid(route.destination)) return {};
    std::unordered_map<std::uint64_t,std::vector<std::uint64_t>> edges;
    for (const auto& bus : data.routing.buses) {
        if (!destinationValid(bus.outputDestination)) return {};
        if (bus.outputDestination.kind == routing::DestinationKind::bus)
            edges[bus.id.value].push_back(bus.outputDestination.bus.value);
    }
    std::unordered_map<std::uint64_t,std::size_t> trackSends, busSends;
    for (const auto& send : data.routing.sends) {
        if (!addId(send.id, data.routing.nextSendId, sends) || !buses.contains(send.destination.value) ||
            !routing::isValid(send.tapPoint) || !send.mix.level.isValid()) return {};
        if (const auto* track = std::get_if<tracks::TrackId>(&send.source)) {
            if (!tracks.contains(track->value) || ++trackSends[track->value] > maximumPreparedSendsPerTrack) return {};
        } else {
            const auto bus = std::get<routing::BusId>(send.source);
            if (!buses.contains(bus.value) || ++busSends[bus.value] > maximumPreparedSendsPerBus) return {};
            edges[bus.value].push_back(send.destination.value);
        }
    }
    std::unordered_map<std::uint64_t,int> colour;
    std::function<bool(std::uint64_t)> visit = [&](auto id) {
        if (colour[id] == 1) return false;
        if (colour[id] == 2) return true;
        colour[id] = 1;
        for (auto dest : edges[id]) if (!visit(dest)) return false;
        colour[id] = 2; return true;
    };
    for (const auto& bus : data.routing.buses) if (!visit(bus.id.value)) return {};
    auto result = std::make_unique<ProjectState>(data.settings.sampleRate, std::move(data.settings.name));
    result->sources_ = std::move(data.sources);
    for (auto& track : data.tracks) {
        for (const auto& clip : track.clips) {
            const auto* source = result->findSource(clip.source);
            if (!addId(clip.id, data.nextClipId, clips) || clips.size() > maximumClips || !source ||
                !result->validateClip(track, *source, clip.projectStart, clip.duration, clip.sourceOffset)) return {};
        }
        result->sortTrackClips(track);
    }
    result->tracks_ = std::move(data.tracks);
    result->routing_.buses_ = std::move(data.routing.buses);
    result->routing_.trackRoutes_ = std::move(data.routing.trackRoutes);
    result->routing_.sends_ = std::move(data.routing.sends);
    result->routing_.nextBusId_ = data.routing.nextBusId;
    result->routing_.nextSendId_ = data.routing.nextSendId;
    result->nextTrackId_ = data.nextTrackId; result->nextSourceId_ = data.nextSourceId;
    result->nextClipId_ = data.nextClipId; result->nextProcessorId_ = data.nextProcessorId;
    result->masterMix_ = data.masterMix; result->masterInserts_ = std::move(data.masterInserts);
    result->musicalTime_ = std::move(data.musicalTime);
    result->loopRange_ = data.loopRange;
    return result;
}
} // namespace vitadaw::project
