#include "vitadaw/audio/RealtimeAudioEngine.h"
#include "vitadaw/audio/StereoAccumulator.h"
#include "vitadaw/audio/TrackMixerProcessing.h"
#include "vitadaw/audio/BusMixerProcessing.h"

#include <algorithm>
#include <cmath>
#include <type_traits>

namespace vitadaw::audio {

void RealtimeAudioEngine::configure(PreparedProjectView project) noexcept {
    deviceUnavailable();
    projectSampleRate_ = project.projectSampleRate;
    projectDuration_ = project.duration;
    trackMixCount_ = std::min(project.tracks.size(), maximumTrackCount);
    for (std::size_t index = 0; index < trackMixCount_; ++index) {
        legacyTracks_[index] = {};
        legacyTracks_[index].source = project.tracks[index];
        legacyTracks_[index].id = project.tracks[index].id;
        legacyTracks_[index].layout =
            project.tracks[index].channelCount == 2
                ? media::AudioChannelLayout::stereo
                : media::AudioChannelLayout::mono;
        legacyTracks_[index].destinationBusIndex = masterDestinationIndex;
        legacyOrder_[index] = {ProcessingStepKind::track, index};
        trackMix_[index].reset(project.tracks[index].mix.isValid()
                                   ? project.tracks[index].mix
                                   : mixer::PreparedTrackMixState{});
        meterTrackIds_[index] = project.tracks[index].id;
    }
    legacyOrder_[trackMixCount_] = {ProcessingStepKind::master, 0};
    tracks_ = {legacyTracks_.data(), trackMixCount_};
    buses_ = {};
    sends_ = {};
    sendIndexById_ = {};
    processors_ = {};
    processorIndexById_ = {};
    order_ = {legacyOrder_.data(), trackMixCount_ + 1};
    masterInserts_ = {};
    processingFormat_ = {project.projectSampleRate,
                         defaultProcessingBlockCapacity,
                         processors::ChannelLayout::stereo,
                         processors::ProcessingMode::realtime};
    runtime_ = nullptr;
    plan_ = nullptr;
    blockCapacity_ = defaultProcessingBlockCapacity;
    masterMix_.reset(project.masterMix.isValid()
                         ? project.masterMix
                         : mixer::PreparedMasterMixState{});
    audibility_ = {};
    for (std::size_t index = 0; index < trackMixCount_; ++index) {
        if (!project.anySolo || project.tracks[index].mix.solo) {
            audibility_.setTrack(index);
            audibility_.setTrackMeter(index);
        }
    }
    busMixCount_ = 0;
    sendMixCount_ = 0;
    parameterReadIndex_.store(0, std::memory_order_relaxed);
    parameterWriteIndex_.store(0, std::memory_order_relaxed);
    planGeneration_.fetch_add(1, std::memory_order_acq_rel);
    clock_.prepare(project.duration);
    if (!prepareLegacyDeviceRate(project.projectSampleRate)) {
        deviceError();
        return;
    }
    if (!configureTemporalContext(temporalContext_))
        static_cast<void>(configureTemporalContext(nullptr));
    clearMeters();
    publishMeters();
    publishTransport();
}

void RealtimeAudioEngine::configure(const PreparedProcessingPlan& plan,
                                    ProcessingPlanRuntime& runtime) noexcept {
    deviceUnavailable();
    projectSampleRate_ = plan.projectSampleRate;
    projectDuration_ = plan.duration;
    tracks_ = plan.tracks;
    buses_ = plan.buses;
    sends_ = plan.sends;
    sendIndexById_ = plan.sendIndexById;
    processors_ = plan.processors;
    processorIndexById_ = plan.processorIndexById;
    order_ = plan.order;
    masterInserts_ = plan.masterInserts;
    processingFormat_ = plan.stereoProcessingFormat;
    runtime_ = &runtime;
    plan_ = &plan;
    blockCapacity_ = plan.blockCapacity;
    trackMixCount_ = std::min(tracks_.size(), maximumTrackCount);
    for (std::size_t index = 0; index < trackMixCount_; ++index) {
        trackMix_[index].reset(tracks_[index].source.mix.isValid()
                                   ? tracks_[index].source.mix
                                   : mixer::PreparedTrackMixState{});
        meterTrackIds_[index] = tracks_[index].id;
    }
    for (std::size_t index = 0;
         index < std::min(buses_.size(), maximumBusCount); ++index) {
        meterBusIds_[index] = buses_[index].id;
        busMix_[index].reset(buses_[index].mix.isValid()
                                 ? buses_[index].mix
                                 : mixer::PreparedBusMixState{});
    }
    busMixCount_ = std::min(buses_.size(), maximumBusCount);
    sendMixCount_ = std::min(sends_.size(), maximumPreparedSends);
    for (std::size_t index = 0; index < sendMixCount_ &&
                                index < runtime.sendMix.size(); ++index) {
        runtime.sendMix[index].reset(sends_[index].mix);
    }
    masterMix_.reset(plan.masterMix.isValid()
                         ? plan.masterMix
                         : mixer::PreparedMasterMixState{});
    audibility_ = plan.audibility;
    parameterReadIndex_.store(0, std::memory_order_relaxed);
    parameterWriteIndex_.store(0, std::memory_order_relaxed);
    planGeneration_.fetch_add(1, std::memory_order_acq_rel);
    resetProcessors();
    clock_.prepare(plan.duration);
    baseClockFormat_ = plan.exactClock;
    static_cast<void>(clock_.prepareFormat(plan.exactClock));
    if (!configureTemporalContext(temporalContext_))
        static_cast<void>(configureTemporalContext(nullptr));
    clearMeters();
    publishMeters();
    publishTransport();
}

bool RealtimeAudioEngine::canConfigureTemporalContext(
    const PreparedTemporalContext* context) const noexcept {
    const auto format = context ? context->exactClock : baseClockFormat_;
    if (!format.valid) return false;
    if (context && (context->projectSampleRate != projectSampleRate_ ||
                    context->deviceSampleRate != processingFormat_.sampleRate))
        return false;
    if (plan_ && !processingPlanSupportsClock(*plan_, format)) return false;
    if (!plan_) {
        for (std::size_t index = 0; index < trackMixCount_; ++index) {
            const auto& source = legacyTracks_[index].source;
            if (source.isAvailable() &&
                !exact::sourceMappingSupportsPhaseDenominator(
                    source.exactSource, format.denominator)) return false;
        }
    }
    exact::ProjectPhase certificate;
    if (!certificate.installPreparedFormat(format)) return false;
    const auto checkpoint = clock_.checkpoint();
    return certificate.restore({{checkpoint.position.value, checkpoint.phase}});
}

bool RealtimeAudioEngine::configureTemporalContext(
    const PreparedTemporalContext* context) noexcept {
    if (!plan_ && context &&
        processingFormat_.sampleRate != context->deviceSampleRate &&
        !prepareLegacyDeviceRate(context->deviceSampleRate)) return false;
    if (!canConfigureTemporalContext(context)) return false;
    const auto format = context ? context->exactClock : baseClockFormat_;
    if (!clock_.prepareFormat(format)) return false;
    temporalContext_ = context;
    loopEnabled_ = loopEnabled_ && temporalContext_ != nullptr &&
                   temporalContext_->loop.has_value();
    const auto loop = loopEnabled_ && temporalContext_->loop
        ? std::optional<RealtimeProjectClock::LoopBounds>{temporalContext_->loop->clockBounds}
        : std::nullopt;
    clock_.setPlaybackPolicy(loop, metronomeEnabled_ && !loop);
    clearMetronomeRuntime();
    metronomeLevelSmoother_.reset(prepareMetronomeLevel(metronomeLevel_));
    publishTransport();
    return true;
}

bool RealtimeAudioEngine::prepareLegacyDeviceRate(timeline::SampleRate deviceRate) noexcept {
    if (runtime_ != nullptr) return processingFormat_.sampleRate == deviceRate;
    const auto baseFormat = exact::clockForPreparation(projectSampleRate_.hertz(), deviceRate.hertz());
    if (!baseFormat.valid) return false;
    const auto format = temporalContext_ &&
                                temporalContext_->projectSampleRate == projectSampleRate_ &&
                                temporalContext_->deviceSampleRate == deviceRate
                            ? temporalContext_->exactClock : baseFormat;
    if (!format.valid) return false;
    std::array<exact::SourceMapping, maximumTrackCount> mappings;
    for (std::size_t i = 0; i < trackMixCount_; ++i) {
        const auto& track = legacyTracks_[i].source;
        if (!track.isAvailable()) continue;
        mappings[i] = exact::sourceForPreparation(track.sourceSampleRate.hertz(), projectSampleRate_.hertz(),
            static_cast<double>(track.clipDuration.value), static_cast<double>(track.sourceOffset.value), format);
        if (!mappings[i].valid) return false;
    }
    if (!clock_.prepareFormat(format)) return false;
    baseClockFormat_ = baseFormat;
    for (std::size_t i = 0; i < trackMixCount_; ++i) legacyTracks_[i].source.exactSource = mappings[i];
    processingFormat_.sampleRate = deviceRate;
    return true;
}

RealtimeAudioEngine::TemporalCheckpoint
RealtimeAudioEngine::temporalCheckpoint() const noexcept {
    return {clock_.checkpoint(), loopEnabled_, metronomeEnabled_,
            clock_.isRunUntilStop(),
            metronomeLevel_};
}

bool RealtimeAudioEngine::restoreTemporalCheckpoint(
    TemporalCheckpoint checkpoint) noexcept {
    if (!clock_.restoreQuiescentCheckpoint(checkpoint.clock)) return false;
    loopEnabled_ = checkpoint.loopEnabled && temporalContext_ != nullptr &&
                   temporalContext_->loop.has_value();
    metronomeEnabled_ = checkpoint.metronomeEnabled;
    metronomeLevel_ = checkpoint.metronomeLevel;
    const auto loop = loopEnabled_ && temporalContext_->loop
        ? std::optional<RealtimeProjectClock::LoopBounds>{temporalContext_->loop->clockBounds}
        : std::nullopt;
    clock_.setPlaybackPolicy(loop, checkpoint.runUntilStop);
    metronomeLevelSmoother_.reset(prepareMetronomeLevel(metronomeLevel_));
    clearMetronomeRuntime();
    publishTransport();
    return true;
}

void RealtimeAudioEngine::resetTemporalSessionState() noexcept {
    loopEnabled_ = false;
    metronomeEnabled_ = false;
    metronomeLevel_ = {};
    clock_.setPlaybackPolicy(std::nullopt, false);
    metronomeLevelSmoother_.reset(prepareMetronomeLevel(metronomeLevel_));
    clearMetronomeRuntime();
    publishTransport();
}

void RealtimeAudioEngine::releasePreparedReferences() noexcept {
    // Lifecycle publication/reset may still consult the old views here.
    deviceUnavailable();
    temporalContext_ = nullptr;
    configure({projectSampleRate_, {}, {}, {}, false});
}

void RealtimeAudioEngine::deviceErrorPreservingTransport() noexcept {
    const auto closure = lifecycleGate_.close(DeviceProcessingState::error);
    resolveCommandsThrough(closure.cancellationWatermark);
    resetProcessors();
    clearMetronomeRuntime();
    publishTransport();
}

void RealtimeAudioEngine::deviceInitialising() noexcept {
    transitionAwayFromOperational(DeviceProcessingState::initializing);
}

void RealtimeAudioEngine::deviceInitialisingPreservingTransport() noexcept {
    const auto closure = lifecycleGate_.close(DeviceProcessingState::initializing);
    resolveCommandsThrough(closure.cancellationWatermark);
    resetProcessors();
    clearMetronomeRuntime();
    publishTransport();
}

void RealtimeAudioEngine::deviceConsumerStarted() noexcept {
    lifecycleGate_.consumerStarted();
}

void RealtimeAudioEngine::deviceStopped() noexcept {
    transitionAwayFromOperational(DeviceProcessingState::stopped);
}

void RealtimeAudioEngine::deviceError() noexcept {
    transitionAwayFromOperational(DeviceProcessingState::error);
}

void RealtimeAudioEngine::deviceUnavailable() noexcept {
    transitionAwayFromOperational(DeviceProcessingState::unavailable);
}

DeviceProcessingState RealtimeAudioEngine::deviceState() const noexcept {
    return lifecycleGate_.state();
}

void RealtimeAudioEngine::transitionAwayFromOperational(
    DeviceProcessingState state) noexcept {
    const auto closure = lifecycleGate_.close(state);
    resolveCommandsThrough(closure.cancellationWatermark);
    clock_.stopAndRewind();
    resetProcessors();
    clearMetronomeRuntime();
    publishTransport();
}

AudioControlRequestResult RealtimeAudioEngine::tryRequestPlay() noexcept {
    refreshTransportProjection();
    if (deviceState() != DeviceProcessingState::operational ||
        (!hasPreparedAudio() &&
         !projectedLoopEnabled_ && !projectionBase_.metronomeEnabled)) {
        return {false, 0, AudioControlRejection::unavailable,
                transport::PlaybackState::stopped, {}, false};
    }
    const auto reduction = transport::reduceTransport(
        projectedTransport_, {transport::TransportActionKind::play, {}},
        transportReductionPolicy());
    const auto request = enqueue(CommandType::play);
    if (!request.accepted) return request;
    commitTransportProjection(reduction, request.sequence);
    return {true, request.sequence, AudioControlRejection::none,
            reduction.state.playback, reduction.state.position, true};
}

AudioControlRequestResult RealtimeAudioEngine::trySetLoopEnabled(bool enabled) noexcept {
    refreshTransportProjection();
    if (deviceState() != DeviceProcessingState::operational ||
        transportExchange_.snapshot().playback !=
            transport::PlaybackState::stopped ||
        (enabled && (temporalContext_ == nullptr || !temporalContext_->loop))) return {};
    return enqueue(CommandType::setLoopEnabled, {}, enabled ? 1.0F : 0.0F);
}

AudioControlRequestResult RealtimeAudioEngine::trySetMetronomeEnabled(bool enabled) noexcept {
    refreshTransportProjection();
    if (deviceState() != DeviceProcessingState::operational) return {};
    return enqueue(CommandType::setMetronomeEnabled, {}, enabled ? 1.0F : 0.0F);
}

AudioControlRequestResult RealtimeAudioEngine::trySetMetronomeLevel(
    MetronomeLevelDb level) noexcept {
    refreshTransportProjection();
    if (deviceState() != DeviceProcessingState::operational || !level.isValid()) return {};
    return enqueue(CommandType::setMetronomeLevel, {}, level.value);
}

AudioControlRequestResult RealtimeAudioEngine::tryRequestPause() noexcept {
    if (deviceState() != DeviceProcessingState::operational)
        return {false, 0, AudioControlRejection::unavailable,
                transport::PlaybackState::stopped, {}, false};
    refreshTransportProjection();
    const auto reduction = transport::reduceTransport(
        projectedTransport_, {transport::TransportActionKind::pause, {}});
    const auto request = enqueue(CommandType::pause);
    if (!request.accepted) return request;
    commitTransportProjection(reduction, request.sequence);
    return {true, request.sequence, AudioControlRejection::none,
            reduction.state.playback, reduction.state.position, true};
}

AudioControlRequestResult RealtimeAudioEngine::tryRequestStop() noexcept {
    if (deviceState() != DeviceProcessingState::operational) {
        const auto generation = lifecycleGate_.generation();
        const auto snapshot = transportExchange_.snapshot();
        if (snapshot.commandGeneration != generation ||
            generation != lifecycleGate_.generation() ||
            deviceState() == DeviceProcessingState::operational ||
            snapshot.playback != transport::PlaybackState::stopped ||
            snapshot.position.value != 0) {
            return {false, 0, AudioControlRejection::unavailable,
                    transport::PlaybackState::stopped, {}, false,
                    AudioControlDisposition::rejected};
        }
        projectedTransport_.synchronise(snapshot.playback, snapshot.position,
                                        snapshot.duration);
        projectedTransportSequence_ = snapshot.lastProcessedCommandSequence;
        return {true, snapshot.lastProcessedCommandSequence,
                AudioControlRejection::none, snapshot.playback,
                snapshot.position, true, AudioControlDisposition::alreadySatisfied};
    }
    refreshTransportProjection();
    const auto reduction = transport::reduceTransport(
        projectedTransport_, {transport::TransportActionKind::stop, {}});
    const auto request = enqueue(CommandType::stop);
    if (!request.accepted) return request;
    commitTransportProjection(reduction, request.sequence);
    return {true, request.sequence, AudioControlRejection::none,
            reduction.state.playback, reduction.state.position, true};
}

AudioControlRequestResult RealtimeAudioEngine::tryRequestSeek(
    timeline::ProjectFramePosition position) noexcept {
    if (deviceState() != DeviceProcessingState::operational)
        return {false, 0, AudioControlRejection::unavailable,
                transport::PlaybackState::stopped, {}, false};
    refreshTransportProjection();
    const auto reduction = transport::reduceTransport(
        projectedTransport_, {transport::TransportActionKind::seek, position});
    if (!reduction.accepted) {
        const auto invalid =
            !timeline::isSupportedProjectFramePosition(position);
        return {false, 0, invalid ? AudioControlRejection::invalidPosition
                                 : AudioControlRejection::disallowedState,
                transport::PlaybackState::stopped, {}, false};
    }
    const auto request = enqueue(CommandType::seek, position);
    if (!request.accepted) return request;
    commitTransportProjection(reduction, request.sequence);
    return {true, request.sequence, AudioControlRejection::none,
            reduction.state.playback, reduction.state.position, true};
}

bool RealtimeAudioEngine::tryUpdateTrackMix(
    tracks::TrackId track, mixer::PreparedTrackMixState mix,
    PreparedAudibilityState audibility) noexcept {
    const auto exists = std::any_of(
        tracks_.begin(), tracks_.begin() + trackMixCount_,
        [track](const auto& candidate) { return candidate.id == track; });
    if (!track.isValid() || !exists || !mix.isValid()) {
        return false;
    }
    const auto found = std::find_if(
        tracks_.begin(), tracks_.begin() + trackMixCount_,
        [track](const auto& candidate) { return candidate.id == track; });
    return enqueueParameter(TrackMixCommand{
        static_cast<std::size_t>(found - tracks_.begin()), mix, audibility});
}

bool RealtimeAudioEngine::tryUpdateBusMix(
    routing::BusId bus, mixer::PreparedBusMixState mix,
    PreparedAudibilityState audibility) noexcept {
    if (!bus.isValid() || !mix.isValid()) {
        return false;
    }
    const auto found = std::find_if(
        buses_.begin(), buses_.begin() + busMixCount_,
        [bus](const auto& candidate) { return candidate.id == bus; });
    if (found == buses_.begin() + busMixCount_) {
        return false;
    }
    return enqueueParameter(BusMixCommand{
        static_cast<std::size_t>(found - buses_.begin()), mix, audibility});
}

bool RealtimeAudioEngine::tryUpdateSendMix(
    routing::SendId send, mixer::PreparedSendMixState mix) noexcept {
    if (!send.isValid() || !mix.isValid() || runtime_ == nullptr) {
        return false;
    }
    const auto found = std::lower_bound(
        sendIndexById_.begin(), sendIndexById_.end(), send,
        [](const auto& candidate, const auto value) {
            return candidate.id < value;
        });
    if (found == sendIndexById_.end() || found->id != send ||
        found->denseIndex >= sendMixCount_) {
        return false;
    }
    return enqueueParameter(SendMixCommand{found->denseIndex, mix});
}

bool RealtimeAudioEngine::tryUpdateMasterMix(
    mixer::PreparedMasterMixState mix) noexcept {
    return mix.isValid() && enqueueParameter(MasterMixCommand{mix});
}

bool RealtimeAudioEngine::tryUpdateProcessorBypass(
    processors::ProcessorInstanceId processor, bool bypassed) noexcept {
    if (!processor.isValid() || runtime_ == nullptr) {
        return false;
    }
    const auto found = std::lower_bound(
        processorIndexById_.begin(), processorIndexById_.end(), processor,
        [](const auto& candidate, const auto value) {
            return candidate.id < value;
        });
    if (found == processorIndexById_.end() || found->id != processor ||
        found->denseIndex >= runtime_->processors.size()) {
        return false;
    }
    return enqueueParameter(ProcessorBypassCommand{
        planGeneration_.load(std::memory_order_acquire), found->denseIndex,
        bypassed});
}

bool RealtimeAudioEngine::tryUpdateProcessorParameter(
    processors::ProcessorInstanceId processor,
    processors::ParameterId parameter, float preparedValue,
    std::uint32_t frameOffset) noexcept {
    if (!processor.isValid() || !parameter.isValid() ||
        !std::isfinite(preparedValue) || frameOffset != 0 ||
        runtime_ == nullptr) {
        return false;
    }
    const auto found = std::lower_bound(
        processorIndexById_.begin(), processorIndexById_.end(), processor,
        [](const auto& candidate, const auto value) {
            return candidate.id < value;
        });
    if (found == processorIndexById_.end() || found->id != processor ||
        found->denseIndex >= runtime_->processors.size()) {
        return false;
    }
    return enqueueParameter(ProcessorParameterCommand{
        planGeneration_.load(std::memory_order_acquire), found->denseIndex,
        parameter, preparedValue, frameOffset});
}

RealtimeTransportSnapshot RealtimeAudioEngine::transportSnapshot() const noexcept {
    return transportExchange_.snapshot();
}

RealtimeTransportSnapshot RealtimeAudioEngine::projectedTransportSnapshot() noexcept {
    refreshTransportProjection();
    auto result = projectionBase_;
    result.playback = projectedTransport_.playback;
    result.playing = result.playback == transport::PlaybackState::playing;
    result.position = projectedTransport_.position;
    result.duration = projectedTransport_.duration;
    result.projectedThroughTicket = projectedTransportSequence_;
    result.loopEnabled = projectedLoopEnabled_;
    result.beforeContentEnd = projectedBoundaries_.beforeContentEnd;
    result.beforeLoopEnd = projectedBoundaries_.beforeLoopEnd;
    return result;
}

mixer::MeterSnapshot RealtimeAudioEngine::meterSnapshot() const noexcept {
    return meterExchange_.snapshot();
}

void RealtimeAudioEngine::processBlock(AudioBlockView output,
                                       timeline::SampleRate deviceSampleRate) noexcept {
    for (std::size_t channel = 0; channel < output.channelCount; ++channel) {
        if (output.channels != nullptr && output.channels[channel] != nullptr) {
            std::fill_n(output.channels[channel], output.frameCount, 0.0F);
        }
    }

    if (deviceSampleRate.isValid()) {
        // Entry into the real callback (or its faithful offline equivalent) is
        // direct evidence that an initialising device has an active consumer.
        deviceConsumerStarted();
    }

    consumeCommands();
    consumeParameterCommands(deviceSampleRate);
    clearMeters();
    if (deviceState() != DeviceProcessingState::operational ||
        !projectSampleRate_.isValid() || blockCapacity_ == 0 ||
        !deviceSampleRate.isValid() ||
        processingFormat_.sampleRate != deviceSampleRate) {
        publishMeters();
        publishTransport();
        return;
    }

    if (!clock_.isPlaying()) {
        advanceSmoothers(output.frameCount);
        publishMeters();
        publishTransport();
        return;
    }

    for (std::size_t offset = 0; offset < output.frameCount;) {
        // Temporal scheduling has its own fixed upper bound even if a future
        // prepared DSP plan chooses a larger scratch block.
        constexpr std::size_t maximumTemporalSubBlockFrames = 1024;
        const auto capacityCount = std::min(
            {blockCapacity_, output.frameCount - offset,
             maximumTemporalSubBlockFrames});
        const auto count = clock_.continuousFramesAvailable(capacityCount);
        processSubBlock(output, offset, count);
        offset += count;
        if (clock_.consumeWrapped()) {
            pendingMetronomeWrap_ = metronomeEnabled_;
            processorDiscontinuity_ =
                processors::TemporalDiscontinuity::loopWrap;
        }
        if (!clock_.isPlaying()) {
            break;
        }
    }
    publishMeters();
    publishTransport();
}

bool RealtimeAudioEngine::enqueueParameter(ParameterCommand command) noexcept {
    const auto write = parameterWriteIndex_.load(std::memory_order_relaxed);
    const auto nextWrite = (write + 1) % parameterCommandCapacity;
    if (nextWrite == parameterReadIndex_.load(std::memory_order_acquire)) {
        return false;
    }
    parameterCommands_[write] = command;
    parameterWriteIndex_.store(nextWrite, std::memory_order_release);
    return true;
}

void RealtimeAudioEngine::consumeParameterCommands(
    timeline::SampleRate deviceSampleRate) noexcept {
    auto read = parameterReadIndex_.load(std::memory_order_relaxed);
    const auto write = parameterWriteIndex_.load(std::memory_order_acquire);
    while (read != write) {
        const auto command = parameterCommands_[read];
        std::visit(
            [this, deviceSampleRate](const auto& value) noexcept {
                using T = std::decay_t<decltype(value)>;
                if constexpr (std::is_same_v<T, TrackMixCommand>) {
                    if (value.trackIndex < trackMixCount_) {
                        trackMix_[value.trackIndex].setTarget(value.mix,
                                                              deviceSampleRate);
                        audibility_ = value.audibility;
                    }
                } else if constexpr (std::is_same_v<T, BusMixCommand>) {
                    if (value.busIndex < busMixCount_) {
                        busMix_[value.busIndex].setTarget(value.mix,
                                                          deviceSampleRate);
                        audibility_ = value.audibility;
                    }
                } else if constexpr (std::is_same_v<T, MasterMixCommand>) {
                    masterMix_.setTarget(value.mix, deviceSampleRate);
                } else if constexpr (std::is_same_v<T, SendMixCommand>) {
                    if (runtime_ != nullptr &&
                        value.sendIndex < sendMixCount_ &&
                        value.sendIndex < runtime_->sendMix.size()) {
                        runtime_->sendMix[value.sendIndex].setTarget(
                            value.mix, deviceSampleRate);
                    }
                } else if constexpr (
                    std::is_same_v<T, ProcessorBypassCommand>) {
                    if (runtime_ != nullptr &&
                        value.planGeneration ==
                            planGeneration_.load(std::memory_order_acquire) &&
                        value.processorIndex < runtime_->processors.size()) {
                        runtime_->processors[value.processorIndex].bypassed =
                            value.bypassed;
                    }
                } else if constexpr (
                    std::is_same_v<T, ProcessorParameterCommand>) {
                    if (runtime_ != nullptr &&
                        value.planGeneration ==
                            planGeneration_.load(std::memory_order_acquire) &&
                        value.processorIndex < runtime_->processors.size()) {
                        runtime_->processors[value.processorIndex]
                            .instance->applyParameter(
                                {value.parameter, value.preparedValue,
                                 value.frameOffset});
                        if (!clock_.isPlaying()) {
                            runtime_->processors[value.processorIndex]
                                .instance->reset();
                        }
                    }
                }
            },
            command);
        read = (read + 1) % parameterCommandCapacity;
    }
    parameterReadIndex_.store(read, std::memory_order_release);
}

void RealtimeAudioEngine::clearMeters() noexcept {
    std::fill(trackPeaks_.begin(), trackPeaks_.begin() + trackMixCount_,
              mixer::StereoPeak{});
    masterPeak_ = {};
    std::fill(busPeaks_.begin(),
              busPeaks_.begin() + std::min(buses_.size(), maximumBusCount),
              mixer::StereoPeak{});
}

void RealtimeAudioEngine::publishMeters() noexcept {
    meterExchange_.publish(
        {meterTrackIds_.data(), trackMixCount_},
        {trackPeaks_.data(), trackMixCount_},
        {meterBusIds_.data(), std::min(buses_.size(), maximumBusCount)},
        {busPeaks_.data(), std::min(buses_.size(), maximumBusCount)},
        masterPeak_);
}

void RealtimeAudioEngine::processSubBlock(
    AudioBlockView output, std::size_t outputOffset, std::size_t frameCount) noexcept {
    auto* masterLeft = runtime_ != nullptr ? runtime_->master.left.data()
                                           : legacyMasterLeft_.data();
    auto* masterRight = runtime_ != nullptr ? runtime_->master.right.data()
                                            : legacyMasterRight_.data();
    auto* positions = runtime_ != nullptr ? runtime_->projectPositions.data()
                                          : legacyPositions_.data();
    std::fill_n(masterLeft, frameCount, 0.0F);
    std::fill_n(masterRight, frameCount, 0.0F);
    if (runtime_ != nullptr) {
        for (std::size_t index = 0; index < buses_.size(); ++index) {
            std::fill_n(runtime_->buses[index].left.data(), frameCount, 0.0F);
            std::fill_n(runtime_->buses[index].right.data(), frameCount, 0.0F);
        }
    }

    std::size_t validFrames{};
    while (validFrames < frameCount && clock_.isPlaying()) {
        positions[validFrames] = clock_.renderPosition();
        ++validFrames;
        clock_.advanceDeviceFrame();
    }

    if (validFrames == 0) {
        return;
    }

    prepareMetronomeEvents(positions, validFrames);

    if (runtime_ == nullptr) {
        processLegacySubBlock(output, outputOffset, validFrames, positions);
        return;
    }

    const processors::ProcessorProcessContext context{
        {positions[0].approximate()}, processingFormat_.sampleRate, validFrames,
        processingFormat_.mode, true, processorDiscontinuity_};

    for (const auto& step : order_) {
        if (step.kind == ProcessingStepKind::track) {
            const auto& route = tracks_[step.index];
            auto& scratch =
                runtime_->processorScratch[route.inserts.scratchIndex];
            std::fill_n(scratch.first.left.data(), validFrames, 0.0F);
            std::fill_n(scratch.first.right.data(), validFrames, 0.0F);
            if (route.clips.count != 0) {
                const auto candidates = findPreparedClipCandidates(
                    *plan_, route, positions[0], positions[validFrames - 1]);
                for (std::size_t candidate = 0;
                     candidate < candidates.count; ++candidate) {
                    const auto& clip =
                        plan_->clips[candidates.first + candidate];
                    for (std::size_t frame = 0; frame < validFrames; ++frame) {
                        const auto rendered = renderPreparedClipAtDspPosition(
                            *plan_, clip, positions[frame]);
                        scratch.first.left[frame] += rendered.left;
                        scratch.first.right[frame] += rendered.right;
                    }
                }
            } else {
                for (std::size_t frame = 0; frame < validFrames; ++frame) {
                    const auto rendered = renderTrackAtDspPosition(
                        route.source, positions[frame], projectSampleRate_);
                    scratch.first.left[frame] = rendered.left;
                    scratch.first.right[frame] = rendered.right;
                }
            }
            const auto processed = processInsertChain(route.inserts, context);
            for (std::size_t frame = 0; frame < validFrames; ++frame) {
                const StereoSample rendered{
                    processed.left[frame],
                    processed.channelCount == 1 ? processed.left[frame]
                                                : processed.right[frame]};
                const auto mix = trackMix_[step.index].next();
                const auto preTap = makeTrackPreFaderPrePanTap(
                    rendered, static_cast<std::uint32_t>(processed.channelCount));
                const auto postTap = makeTrackPostFaderPostPanTap(
                    rendered, static_cast<std::uint32_t>(processed.channelCount),
                    mix);
                const auto contribution = audibility_.trackIsAudible(step.index)
                                              ? postTap
                                              : StereoSample{};
                const auto meterSample =
                    audibility_.trackMeterIsAudible(step.index)
                        ? postTap
                        : StereoSample{};
                trackPeaks_[step.index].left = std::max(
                    trackPeaks_[step.index].left, std::abs(meterSample.left));
                trackPeaks_[step.index].right = std::max(
                    trackPeaks_[step.index].right, std::abs(meterSample.right));
                if (route.destinationBusIndex == masterDestinationIndex) {
                    masterLeft[frame] += contribution.left;
                    masterRight[frame] += contribution.right;
                } else {
                    auto& destination = runtime_->buses[route.destinationBusIndex];
                    destination.left[frame] += contribution.left;
                    destination.right[frame] += contribution.right;
                }
                distributeSends(route.preFaderSends, preTap, frame);
                distributeSends(route.postFaderSends, postTap, frame);
            }
        } else if (step.kind == ProcessingStepKind::bus) {
            const auto bufferIndex = buses_[step.index].bufferIndex;
            const auto& bus = runtime_->buses[bufferIndex];
            auto& scratch = runtime_->processorScratch[
                buses_[step.index].inserts.scratchIndex];
            std::copy_n(bus.left.data(), validFrames,
                        scratch.first.left.data());
            std::copy_n(bus.right.data(), validFrames,
                        scratch.first.right.data());
            const auto processed =
                processInsertChain(buses_[step.index].inserts, context);
            for (std::size_t frame = 0; frame < validFrames; ++frame) {
                const StereoSample input{processed.left[frame],
                                         processed.right[frame]};
                const auto mix = busMix_[step.index].next();
                const auto preTap = makeBusPreFaderPreBalanceTap(input);
                const auto postTap =
                    makeBusPostFaderPostBalanceTap(input, mix);
                const auto contribution = audibility_.busIsAudible(step.index)
                                              ? postTap
                                              : StereoSample{};
                const auto meterSample =
                    audibility_.busMeterIsAudible(step.index)
                        ? postTap
                        : StereoSample{};
                busPeaks_[step.index].left = std::max(
                    busPeaks_[step.index].left, std::abs(meterSample.left));
                busPeaks_[step.index].right = std::max(
                    busPeaks_[step.index].right, std::abs(meterSample.right));
                distributeSends(buses_[step.index].preFaderSends, preTap,
                                frame);
                distributeSends(buses_[step.index].postFaderSends, postTap,
                                frame);
                const auto destination = buses_[step.index].destinationBusIndex;
                if (destination == masterDestinationIndex) {
                    masterLeft[frame] += contribution.left;
                    masterRight[frame] += contribution.right;
                } else {
                    auto& destinationBuffer = runtime_->buses[
                        buses_[destination].bufferIndex];
                    destinationBuffer.left[frame] += contribution.left;
                    destinationBuffer.right[frame] += contribution.right;
                }
            }
        } else {
            auto& scratch =
                runtime_->processorScratch[masterInserts_.scratchIndex];
            std::copy_n(masterLeft, validFrames, scratch.first.left.data());
            std::copy_n(masterRight, validFrames, scratch.first.right.data());
            const auto processed = processInsertChain(masterInserts_, context);
            for (std::size_t frame = 0; frame < validFrames; ++frame) {
                StereoSample mixed{processed.left[frame],
                                   processed.right[frame]};
                const auto click = renderMetronomeSample(frame);
                mixed.left += click;
                mixed.right += click;
                applyMasterGain(mixed, masterMix_.next());
                masterPeak_.left = std::max(masterPeak_.left,
                                            std::abs(mixed.left));
                masterPeak_.right = std::max(masterPeak_.right,
                                             std::abs(mixed.right));
                if (output.channels != nullptr && output.channelCount > 0 &&
                    output.channels[0] != nullptr) {
                    output.channels[0][outputOffset + frame] = mixed.left;
                }
                if (output.channels != nullptr && output.channelCount > 1 &&
                    output.channels[1] != nullptr) {
                    output.channels[1][outputOffset + frame] = mixed.right;
                }
            }
        }
    }

    if (!clock_.isPlaying()) {
        resetProcessors();
    } else {
        processorDiscontinuity_ = processors::TemporalDiscontinuity::continuous;
    }
}

void RealtimeAudioEngine::processLegacySubBlock(
    AudioBlockView output, std::size_t outputOffset, std::size_t validFrames,
    const DspFramePosition* positions) noexcept {
    auto* masterLeft = legacyMasterLeft_.data();
    auto* masterRight = legacyMasterRight_.data();
    for (const auto& step : order_) {
        if (step.kind == ProcessingStepKind::track) {
            const auto& route = tracks_[step.index];
            for (std::size_t frame = 0; frame < validFrames; ++frame) {
                const auto rendered = renderTrackAtDspPosition(
                    route.source, positions[frame], projectSampleRate_);
                const auto mix = trackMix_[step.index].next();
                const auto postTap = makeTrackPostFaderPostPanTap(
                    rendered, route.source.channelCount, mix);
                const auto contribution = audibility_.trackIsAudible(step.index)
                                              ? postTap
                                              : StereoSample{};
                const auto meterSample =
                    audibility_.trackMeterIsAudible(step.index)
                        ? postTap
                        : StereoSample{};
                trackPeaks_[step.index].left = std::max(
                    trackPeaks_[step.index].left, std::abs(meterSample.left));
                trackPeaks_[step.index].right = std::max(
                    trackPeaks_[step.index].right, std::abs(meterSample.right));
                masterLeft[frame] += contribution.left;
                masterRight[frame] += contribution.right;
            }
        } else if (step.kind == ProcessingStepKind::master) {
            for (std::size_t frame = 0; frame < validFrames; ++frame) {
                StereoSample mixed{masterLeft[frame], masterRight[frame]};
                const auto click = renderMetronomeSample(frame);
                mixed.left += click;
                mixed.right += click;
                applyMasterGain(mixed, masterMix_.next());
                masterPeak_.left =
                    std::max(masterPeak_.left, std::abs(mixed.left));
                masterPeak_.right =
                    std::max(masterPeak_.right, std::abs(mixed.right));
                if (output.channels != nullptr && output.channelCount > 0 &&
                    output.channels[0] != nullptr) {
                    output.channels[0][outputOffset + frame] = mixed.left;
                }
                if (output.channels != nullptr && output.channelCount > 1 &&
                    output.channels[1] != nullptr) {
                    output.channels[1][outputOffset + frame] = mixed.right;
                }
            }
        }
    }
}

RealtimeAudioEngine::ProcessedNodeBlock
RealtimeAudioEngine::processInsertChain(
    PreparedInsertRange range,
    const processors::ProcessorProcessContext& context) noexcept {
    auto& scratch = runtime_->processorScratch[range.scratchIndex];
    ProcessedNodeBlock current{
        scratch.first.left.data(), scratch.first.right.data(),
        static_cast<std::size_t>(range.layout)};
    bool currentIsFirst = true;
    for (std::size_t offset = 0; offset < range.count; ++offset) {
        const auto processorIndex = range.first + offset;
        auto& processor = runtime_->processors[processorIndex];
        auto& outputBuffer = currentIsFirst ? scratch.second : scratch.first;
        const std::array<const float*, 2> inputChannels{current.left,
                                                        current.right};
        const std::array<float*, 2> outputChannels{outputBuffer.left.data(),
                                                   outputBuffer.right.data()};
        const audio::ConstAudioBlockView input{
            inputChannels.data(), current.channelCount, context.frameCount};
        const audio::AudioBlockView output{
            outputChannels.data(), current.channelCount, context.frameCount};
        processors::ProcessStatus status;
        if (processors_[processorIndex].capabilities.supportsOutOfPlace) {
            status = processor.instance->processBlock(context, input, output);
        } else {
            for (std::size_t channel = 0; channel < current.channelCount;
                 ++channel) {
                std::copy_n(inputChannels[channel], context.frameCount,
                            outputChannels[channel]);
            }
            const std::array<const float*, 2> aliasedInputChannels{
                outputChannels[0], outputChannels[1]};
            status = processor.instance->processBlock(
                context,
                {aliasedInputChannels.data(), current.channelCount,
                 context.frameCount},
                output);
        }
        if (status == processors::ProcessStatus::failed) {
            for (std::size_t channel = 0; channel < current.channelCount;
                 ++channel) {
                std::fill_n(outputChannels[channel], context.frameCount, 0.0F);
            }
        }
        processor.bypassDelay.process(input, output, processor.bypassed);
        current = {outputBuffer.left.data(), outputBuffer.right.data(),
                   current.channelCount};
        currentIsFirst = !currentIsFirst;
    }
    return current;
}

void RealtimeAudioEngine::resetProcessors() noexcept {
    if (runtime_ != nullptr) {
        for (auto& processor : runtime_->processors) {
            processor.instance->reset();
            processor.bypassDelay.reset();
        }
    }
    processorDiscontinuity_ = processors::TemporalDiscontinuity::hardDiscontinuity;
    clearMetronomeRuntime();
}

void RealtimeAudioEngine::distributeSends(PreparedSendRange range,
                                          StereoSample tap,
                                          std::size_t frame) noexcept {
    for (std::size_t offset = 0; offset < range.count; ++offset) {
        const auto sendIndex = range.first + offset;
        const auto& send = sends_[sendIndex];
        const auto sendMix = runtime_->sendMix[send.runtimeIndex].next();
        if (sendMix.muted ||
            !audibility_.sendIsAudible(send.audibilityIndex)) {
            continue;
        }
        auto& destination = runtime_->buses[send.destinationBufferIndex];
        destination.left[frame] += tap.left * sendMix.linearGain;
        destination.right[frame] += tap.right * sendMix.linearGain;
    }
}

void RealtimeAudioEngine::advanceSmoothers(std::size_t frameCount) noexcept {
    for (std::size_t frame = 0; frame < frameCount; ++frame) {
        for (std::size_t index = 0; index < trackMixCount_; ++index) {
            static_cast<void>(trackMix_[index].next());
        }
        for (std::size_t index = 0; index < busMixCount_; ++index) {
            static_cast<void>(busMix_[index].next());
        }
        if (runtime_ != nullptr) {
            for (std::size_t index = 0; index < sendMixCount_; ++index) {
                static_cast<void>(runtime_->sendMix[index].next());
            }
        }
        static_cast<void>(masterMix_.next());
        static_cast<void>(metronomeLevelSmoother_.next());
    }
}

void RealtimeAudioEngine::clearMetronomeRuntime() noexcept {
    for (auto& voice : metronomeVoices_) voice = {};
    metronomeEventCount_ = 0;
    pendingMetronomeEvent_ = false;
    pendingMetronomeAccent_ = false;
    pendingMetronomeWrap_ = false;
}

void RealtimeAudioEngine::prepareMetronomeEvents(
    const DspFramePosition* positions, std::size_t frameCount) noexcept {
    metronomeEventCount_ = 0;
    if (!metronomeEnabled_ || temporalContext_ == nullptr || frameCount == 0) return;
    const auto appendEvent = [this](std::size_t frame, bool accent) noexcept {
        if (metronomeEventCount_ && metronomeEvents_[metronomeEventCount_ - 1].frame == frame) {
            metronomeEvents_[metronomeEventCount_ - 1].accent |= accent;
            return;
        }
        if (metronomeEventCount_ < metronomeEvents_.size())
            metronomeEvents_[metronomeEventCount_++] = {frame, accent};
    };
    if (pendingMetronomeEvent_) {
        appendEvent(0, pendingMetronomeAccent_);
        pendingMetronomeEvent_ = pendingMetronomeAccent_ = false;
    }
    auto start = positions[0].exactPosition();
    // The device sample after a wrap can lie strictly beyond loopStart. Keep
    // the crossed interval explicitly, including its first event, rather than
    // inferring it from the post-wrap locator. Placement below maps any crossed
    // beat to frame zero and coalesces equal-frame events exactly once.
    if (pendingMetronomeWrap_ && loopEnabled_ && temporalContext_->loop)
        start = exact::boundaryPosition(temporalContext_->loop->clockBounds.exactStart);
    pendingMetronomeWrap_ = false;
    if (exact::floorPosition(start).frame > musical::maximumCoordinate) return;
    exact::ProjectPhase next{clock_.exactFormat(), positions[frameCount - 1].exactPosition()};
    next.advance(std::nullopt);
    auto exclusive = next.position();
    const auto* loop = loopEnabled_ && temporalContext_->loop ? &*temporalContext_->loop : nullptr;
    if (loop && exact::compareBoundary(exclusive, loop->clockBounds.exactEnd) > 0)
        exclusive = exact::boundaryPosition(loop->clockBounds.exactEnd);
    if (!clock_.isPlaying() && !loop) {
        const exact::Position end{clock_.duration().value, {}};
        if (exact::comparePositions(exclusive, end) > 0) exclusive = end;
    }
    const auto firstFloor = exact::floorPosition(start).frame;
    const auto lastFloor = exact::floorPosition(exclusive).frame;
    // Prepared segment count <= 8191. With certified device rate >= 1 Hz,
    // tempo <= 400 and denominator <= 64, <= 108 beats/second plus the bounded
    // signature anchors can fall in a subblock. No search over project duration.
    for (const auto& segment : temporalContext_->beats) {
        // A loop endpoint at the same document tick overrides the segment's
        // prepared anchor below. Its conservative range must include that
        // endpoint too; the ordinary segment-only range cannot cull it.
        if (!loop && (segment.lastFrame <= firstFloor || segment.firstFrame > lastFloor)) continue;
        const auto beatCount = static_cast<std::uint64_t>(
            (segment.endTick - 1 - segment.firstTick) / segment.ticksPerBeat + 1);
        const auto beatPosition = [&](std::uint64_t index) noexcept {
            const auto tick = segment.firstTick + static_cast<std::int64_t>(index) * segment.ticksPerBeat;
            // Identical document ticks share their prepared loop boundary.
            if (loop && tick == loop->musical.start.value) return exact::boundaryPosition(loop->clockBounds.exactStart);
            if (loop && tick == loop->musical.end.value) return exact::boundaryPosition(loop->clockBounds.exactEnd);
            return segment.positionAt(tick);
        };
        std::uint64_t low = 0, high = beatCount;
        while (low < high) {
            const auto middle = low + (high - low) / 2;
            if (exact::comparePositions(beatPosition(middle), start) < 0) low = middle + 1;
            else high = middle;
        }
        for (auto index = low; index < beatCount; ++index) {
            const auto beat = beatPosition(index);
            if (exact::comparePositions(beat, exclusive) >= 0 ||
                exact::floorPosition(beat).frame > musical::maximumCoordinate) break;
            std::size_t begin = 0, end = frameCount;
            while (begin < end) {
                const auto middle = begin + (end - begin) / 2;
                if (exact::comparePositions(positions[middle].exactPosition(), beat) < 0) begin = middle + 1;
                else end = middle;
            }
            const auto tick = segment.firstTick + static_cast<std::int64_t>(index) * segment.ticksPerBeat;
            const bool accent = (tick - segment.signatureTick) % segment.ticksPerBar == 0;
            if (begin == frameCount) {
                pendingMetronomeEvent_ = true;
                pendingMetronomeAccent_ |= accent;
            } else appendEvent(begin, accent);
        }
    }
}

float RealtimeAudioEngine::renderMetronomeSample(std::size_t frame) noexcept {
    if (temporalContext_ == nullptr) {
        static_cast<void>(metronomeLevelSmoother_.next());
        return 0.0F;
    }
    for (std::size_t index = 0; index < metronomeEventCount_; ++index) {
        if (metronomeEvents_[index].frame != frame) continue;
        auto* selected = &metronomeVoices_[0];
        for (auto& voice : metronomeVoices_) {
            if (!voice.active) { selected = &voice; break; }
            if (voice.frame > selected->frame) selected = &voice;
        }
        *selected = {true, metronomeEvents_[index].accent, 0};
    }
    float sample{};
    for (auto& voice : metronomeVoices_) {
        if (!voice.active) continue;
        const auto& table = voice.accent ? temporalContext_->clicks.accent
                                         : temporalContext_->clicks.normal;
        sample += table[voice.frame++];
        if (voice.frame >= temporalContext_->clicks.frameCount) voice.active = false;
    }
    return sample * metronomeLevelSmoother_.next();
}

AudioControlRequestResult RealtimeAudioEngine::enqueue(
    CommandType type, timeline::ProjectFramePosition target, float value) noexcept {
    if (pendingCommandCount_ == pendingCommandCapacity) {
        return {false, 0, AudioControlRejection::queueFull,
                transport::PlaybackState::stopped, {}, false};
    }
    const auto claim = lifecycleGate_.tryClaim();
    if (!claim.active) {
        return {false, 0, AudioControlRejection::unavailable,
                transport::PlaybackState::stopped, {}, false};
    }

    // Admission was computed from this confirmed generation plus pure replay.
    // A close/reopen before claim must not accept old-context semantics. A close
    // after this check still invalidates the same token at tryAccept().
    if (claim.generation != projectionBase_.commandGeneration) {
        lifecycleGate_.reject(claim);
        return {false, 0, AudioControlRejection::unavailable,
                transport::PlaybackState::stopped, {}, false};
    }

    const auto write = commandWriteIndex_.load(std::memory_order_relaxed);
    const auto nextWrite = (write + 1) % commandCapacity;
    if (nextWrite == commandReadIndex_.load(std::memory_order_acquire)) {
        lifecycleGate_.reject(claim);
        return {false, 0, AudioControlRejection::queueFull,
                transport::PlaybackState::stopped, {}, false};
    }

    const auto sequence = lifecycleGate_.reserveSequence(claim);
    if (sequence == 0) {
        lifecycleGate_.reject(claim);
        return {false, 0, AudioControlRejection::unavailable,
                transport::PlaybackState::stopped, {}, false};
    }
    commands_[write] = {type, sequence, claim.generation, target, value};

    if (!lifecycleGate_.tryAccept(claim)) {
        return {false, 0, AudioControlRejection::unavailable,
                transport::PlaybackState::stopped, {}, false};
    }
    // The command becomes visible to RT only after its acceptance point.
    commandWriteIndex_.store(nextWrite, std::memory_order_release);
    // Space was secured before acceptance. Only this producer touches the log.
    pendingCommands_[pendingCommandCount_++] = commands_[write];
    return {true, sequence, AudioControlRejection::none,
            transport::PlaybackState::stopped, {}, false};
}

void RealtimeAudioEngine::consumeCommands() noexcept {
    auto read = commandReadIndex_.load(std::memory_order_relaxed);
    const auto write = commandWriteIndex_.load(std::memory_order_acquire);
    const auto generation = lifecycleGate_.generation();
    while (read != write) {
        const auto queued = commands_[read];
        read = (read + 1) % commandCapacity;
        if (queued.generation == generation) {
            if (queued.type == CommandType::stop) {
                clock_.stop();
                resetProcessors();
                clearMetronomeRuntime();
            } else if (queued.type == CommandType::pause) {
                clock_.pause();
            } else if (queued.type == CommandType::seek) {
                if (clock_.seek(queued.target)) {
                    processorDiscontinuity_ =
                        processors::TemporalDiscontinuity::seek;
                    clearMetronomeRuntime();
                }
            } else if (queued.type == CommandType::play) {
                static_cast<void>(clock_.play());
            } else if (queued.type == CommandType::setLoopEnabled) {
                loopEnabled_ = queued.value != 0.0F;
                const auto loop = loopEnabled_ && temporalContext_ != nullptr &&
                                          temporalContext_->loop
                    ? std::optional<RealtimeProjectClock::LoopBounds>{temporalContext_->loop->clockBounds}
                    : std::nullopt;
                clock_.setPlaybackPolicy(loop, metronomeEnabled_ && !loop);
            } else if (queued.type == CommandType::setMetronomeEnabled) {
                const auto wasUnbounded = clock_.isRunUntilStop();
                metronomeEnabled_ = queued.value != 0.0F;
                if (!metronomeEnabled_) clearMetronomeRuntime();
                const auto loop = loopEnabled_ && temporalContext_ != nullptr &&
                                          temporalContext_->loop
                    ? std::optional<RealtimeProjectClock::LoopBounds>{temporalContext_->loop->clockBounds}
                    : std::nullopt;
                const auto preserveUnboundedPlayback =
                    clock_.isPlaying() && !loop && wasUnbounded;
                clock_.setPlaybackPolicy(loop,
                    !loop && (metronomeEnabled_ || preserveUnboundedPlayback));
            } else if (queued.type == CommandType::setMetronomeLevel) {
                metronomeLevel_ = {queued.value};
                metronomeLevelSmoother_.setTarget(
                    prepareMetronomeLevel(metronomeLevel_),
                    temporalContext_ ? temporalContext_->clicks.deviceSampleRate
                                     : projectSampleRate_,
                    metronomeSmoothingSeconds);
            }
        }
        resolveCommandsThrough(queued.sequence);
    }
    commandReadIndex_.store(read, std::memory_order_release);
}

void RealtimeAudioEngine::refreshTransportProjection() noexcept {
    const auto snapshot = transportExchange_.snapshot();
    projectionBase_ = snapshot;
    projectedTransport_.synchronise(snapshot.playback, snapshot.position,
                                    snapshot.duration);
    projectedBoundaries_ = {snapshot.beforeContentEnd, snapshot.beforeLoopEnd};
    projectedLoopEnabled_ = snapshot.loopEnabled;
    projectedTransportSequence_ = snapshot.lastProcessedCommandSequence;
    std::size_t retained{};
    for (std::size_t index = 0; index < pendingCommandCount_; ++index) {
        const auto command = pendingCommands_[index];
        if (command.sequence <= snapshot.lastProcessedCommandSequence) continue;
        // A newer coherent generation cancels older history even if the late
        // reservation was not observed in the closure's watermark.
        if (command.generation < snapshot.commandGeneration) continue;
        pendingCommands_[retained++] = command;
        if (command.type == CommandType::setLoopEnabled) {
            projectedLoopEnabled_ = command.value != 0.0F;
        } else if (command.type == CommandType::setMetronomeEnabled) {
            projectionBase_.metronomeEnabled = command.value != 0.0F;
        } else if (command.type == CommandType::setMetronomeLevel) {
            projectionBase_.metronomeLevelDb = command.value;
        } else if (command.type == CommandType::play || command.type == CommandType::pause ||
                   command.type == CommandType::stop || command.type == CommandType::seek) {
            const auto action = command.type == CommandType::play ? transport::TransportActionKind::play :
                command.type == CommandType::pause ? transport::TransportActionKind::pause :
                command.type == CommandType::stop ? transport::TransportActionKind::stop :
                                                   transport::TransportActionKind::seek;
            const auto policy = transportReductionPolicy();
            const auto reduction = transport::reduceTransport(projectedTransport_,
                {action, command.target}, policy);
            commitTransportProjection(reduction, command.sequence);
        }
        projectedTransportSequence_ = command.sequence;
    }
    pendingCommandCount_ = retained;
}

transport::TransportReductionPolicy
RealtimeAudioEngine::transportReductionPolicy() const noexcept {
    transport::TransportReductionPolicy result;
    result.boundaries = projectedBoundaries_;
    if (projectedLoopEnabled_ &&
        temporalContext_ != nullptr && temporalContext_->loop) {
        result.loop = transport::TransportReductionPolicy::Loop{
            temporalContext_->loop->clockBounds};
    }
    return result;
}

void RealtimeAudioEngine::commitTransportProjection(
    const transport::TransportReduction& reduction,
    AudioCommandSequence sequence) noexcept {
    auto boundaryPolicy = transportReductionPolicy();
    // Preserve the prepared loop relation while disabled, so a later pending
    // enable never has to infer the residue from the public integer.
    if (temporalContext_ && temporalContext_->loop)
        boundaryPolicy.loop = transport::TransportReductionPolicy::Loop{
            temporalContext_->loop->clockBounds};
    projectedBoundaries_ = transport::boundariesAfterReduction(
        reduction, projectedBoundaries_, boundaryPolicy);
    projectedTransport_ = reduction.state;
    projectedTransportSequence_ = sequence;
}

void RealtimeAudioEngine::publishTransport() noexcept {
    transportExchange_.publish({clock_.isPlaying(), clock_.publicPosition(),
                                clock_.duration(),
                                lastResolvedCommandSequence_.load(
                                    std::memory_order_acquire),
                                clock_.playback(), loopEnabled_,
                                metronomeEnabled_, metronomeLevel_.value,
                                temporalContext_ ? temporalContext_->revision : 0,
                                lifecycleGate_.generation(),
                                clock_.boundaryFacts().beforeContentEnd,
                                temporalContext_ && temporalContext_->loop &&
                                    clock_.isBefore(temporalContext_->loop->clockBounds.exactEnd)});
}

void RealtimeAudioEngine::resolveCommandsThrough(
    AudioCommandSequence sequence) noexcept {
    auto resolved = lastResolvedCommandSequence_.load(std::memory_order_relaxed);
    while (resolved < sequence &&
           !lastResolvedCommandSequence_.compare_exchange_weak(
               resolved, sequence, std::memory_order_release,
               std::memory_order_relaxed)) {
    }
}

bool RealtimeAudioEngine::hasPreparedAudio() const noexcept {
    return std::any_of(tracks_.begin(), tracks_.begin() + trackMixCount_,
                       [this](const auto& track) {
                           return track.source.isAvailable() ||
                                  (plan_ != nullptr && track.clips.count != 0);
                       });
}

} // namespace vitadaw::audio
