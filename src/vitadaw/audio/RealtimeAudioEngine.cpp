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
        legacyTracks_[index] = {project.tracks[index], masterDestinationIndex};
        legacyOrder_[index] = {ProcessingStepKind::track, index};
        trackMix_[index].reset(project.tracks[index].mix.isValid()
                                   ? project.tracks[index].mix
                                   : mixer::PreparedTrackMixState{});
        meterTrackIds_[index] = project.tracks[index].id;
    }
    legacyOrder_[trackMixCount_] = {ProcessingStepKind::master, 0};
    tracks_ = {legacyTracks_.data(), trackMixCount_};
    buses_ = {};
    order_ = {legacyOrder_.data(), trackMixCount_ + 1};
    runtime_ = nullptr;
    blockCapacity_ = defaultProcessingBlockCapacity;
    masterMix_.reset(project.masterMix.isValid()
                         ? project.masterMix
                         : mixer::PreparedMasterMixState{});
    audibility_ = {};
    for (std::size_t index = 0; index < trackMixCount_; ++index) {
        if (!project.anySolo || project.tracks[index].mix.solo) {
            audibility_.setTrack(index);
        }
    }
    busMixCount_ = 0;
    parameterReadIndex_.store(0, std::memory_order_relaxed);
    parameterWriteIndex_.store(0, std::memory_order_relaxed);
    clock_.prepare(project.duration);
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
    order_ = plan.order;
    runtime_ = &runtime;
    blockCapacity_ = plan.blockCapacity;
    trackMixCount_ = std::min(tracks_.size(), maximumTrackCount);
    for (std::size_t index = 0; index < trackMixCount_; ++index) {
        trackMix_[index].reset(tracks_[index].source.mix.isValid()
                                   ? tracks_[index].source.mix
                                   : mixer::PreparedTrackMixState{});
        meterTrackIds_[index] = tracks_[index].source.id;
    }
    for (std::size_t index = 0;
         index < std::min(buses_.size(), maximumBusCount); ++index) {
        meterBusIds_[index] = buses_[index].id;
        busMix_[index].reset(buses_[index].mix.isValid()
                                 ? buses_[index].mix
                                 : mixer::PreparedBusMixState{});
    }
    busMixCount_ = std::min(buses_.size(), maximumBusCount);
    masterMix_.reset(plan.masterMix.isValid()
                         ? plan.masterMix
                         : mixer::PreparedMasterMixState{});
    audibility_ = plan.audibility;
    parameterReadIndex_.store(0, std::memory_order_relaxed);
    parameterWriteIndex_.store(0, std::memory_order_relaxed);
    clock_.prepare(plan.duration);
    clearMeters();
    publishMeters();
    publishTransport();
}

void RealtimeAudioEngine::deviceInitialising() noexcept {
    transitionAwayFromOperational(DeviceProcessingState::initializing);
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
    publishTransport();
}

AudioControlRequestResult RealtimeAudioEngine::tryRequestPlay() noexcept {
    if (deviceState() != DeviceProcessingState::operational || !hasPreparedAudio()) {
        return {};
    }
    return enqueue(CommandType::play);
}

AudioControlRequestResult RealtimeAudioEngine::tryRequestStop() noexcept {
    if (deviceState() != DeviceProcessingState::operational) {
        // The lifecycle transition has already stopped and published the clock.
        // Stop remains a valid idempotent application action without creating
        // a command that would need a missing RT consumer.
        return {true, transportExchange_.snapshot().lastProcessedCommandSequence};
    }
    return enqueue(CommandType::stop);
}

bool RealtimeAudioEngine::tryUpdateTrackMix(
    tracks::TrackId track, mixer::PreparedTrackMixState mix,
    PreparedAudibilityState audibility) noexcept {
    const auto exists = std::any_of(
        tracks_.begin(), tracks_.begin() + trackMixCount_,
        [track](const auto& candidate) { return candidate.source.id == track; });
    if (!track.isValid() || !exists || !mix.isValid()) {
        return false;
    }
    const auto found = std::find_if(
        tracks_.begin(), tracks_.begin() + trackMixCount_,
        [track](const auto& candidate) { return candidate.source.id == track; });
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

bool RealtimeAudioEngine::tryUpdateMasterMix(
    mixer::PreparedMasterMixState mix) noexcept {
    return mix.isValid() && enqueueParameter(MasterMixCommand{mix});
}

RealtimeTransportSnapshot RealtimeAudioEngine::transportSnapshot() const noexcept {
    return transportExchange_.snapshot();
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
        !deviceSampleRate.isValid()) {
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

    const auto projectFramesPerDeviceFrame = timeline::projectFramesForDeviceFrames(
        {1}, projectSampleRate_, deviceSampleRate);
    for (std::size_t offset = 0; offset < output.frameCount;
         offset += blockCapacity_) {
        const auto count = std::min(blockCapacity_, output.frameCount - offset);
        processSubBlock(output, offset, count, projectFramesPerDeviceFrame);
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
    AudioBlockView output, std::size_t outputOffset, std::size_t frameCount,
    timeline::ProjectFrameDuration projectFramesPerDeviceFrame) noexcept {
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
        positions[validFrames] = clock_.position().value;
        ++validFrames;
        clock_.advance(projectFramesPerDeviceFrame);
    }

    for (const auto& step : order_) {
        if (step.kind == ProcessingStepKind::track) {
            const auto& route = tracks_[step.index];
            for (std::size_t frame = 0; frame < validFrames; ++frame) {
                const auto rendered = renderTrackAtProjectPosition(
                    route.source, {positions[frame]}, projectSampleRate_);
                const auto contribution = applyTrackMixResolved(
                    rendered, route.source.channelCount,
                    trackMix_[step.index].next(),
                    audibility_.trackIsAudible(step.index));
                trackPeaks_[step.index].left = std::max(
                    trackPeaks_[step.index].left, std::abs(contribution.left));
                trackPeaks_[step.index].right = std::max(
                    trackPeaks_[step.index].right, std::abs(contribution.right));
                if (route.destinationBusIndex == masterDestinationIndex) {
                    masterLeft[frame] += contribution.left;
                    masterRight[frame] += contribution.right;
                } else {
                    auto& destination = runtime_->buses[route.destinationBusIndex];
                    destination.left[frame] += contribution.left;
                    destination.right[frame] += contribution.right;
                }
            }
        } else if (step.kind == ProcessingStepKind::bus) {
            const auto bufferIndex = buses_[step.index].bufferIndex;
            const auto& bus = runtime_->buses[bufferIndex];
            for (std::size_t frame = 0; frame < validFrames; ++frame) {
                const auto contribution = applyBusMix(
                    {bus.left[frame], bus.right[frame]},
                    busMix_[step.index].next(),
                    audibility_.busIsAudible(step.index));
                busPeaks_[step.index].left = std::max(
                    busPeaks_[step.index].left, std::abs(contribution.left));
                busPeaks_[step.index].right = std::max(
                    busPeaks_[step.index].right, std::abs(contribution.right));
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
            for (std::size_t frame = 0; frame < validFrames; ++frame) {
                StereoSample mixed{masterLeft[frame], masterRight[frame]};
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
}

void RealtimeAudioEngine::advanceSmoothers(std::size_t frameCount) noexcept {
    for (std::size_t frame = 0; frame < frameCount; ++frame) {
        for (std::size_t index = 0; index < trackMixCount_; ++index) {
            static_cast<void>(trackMix_[index].next());
        }
        for (std::size_t index = 0; index < busMixCount_; ++index) {
            static_cast<void>(busMix_[index].next());
        }
        static_cast<void>(masterMix_.next());
    }
}

AudioControlRequestResult RealtimeAudioEngine::enqueue(CommandType type) noexcept {
    const auto claim = lifecycleGate_.tryClaim();
    if (!claim.active) {
        return {};
    }

    const auto write = commandWriteIndex_.load(std::memory_order_relaxed);
    const auto nextWrite = (write + 1) % commandCapacity;
    if (nextWrite == commandReadIndex_.load(std::memory_order_acquire)) {
        lifecycleGate_.reject(claim);
        return {};
    }

    const auto sequence = lifecycleGate_.reserveSequence(claim);
    if (sequence == 0) {
        lifecycleGate_.reject(claim);
        return {};
    }
    commands_[write] = {type, sequence, claim.generation};

    if (!lifecycleGate_.tryAccept(claim)) {
        return {};
    }
    // The command becomes visible to RT only after its acceptance point.
    commandWriteIndex_.store(nextWrite, std::memory_order_release);
    return {true, sequence};
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
                clock_.stopAndRewind();
            } else {
                static_cast<void>(clock_.play());
            }
        }
        resolveCommandsThrough(queued.sequence);
    }
    commandReadIndex_.store(read, std::memory_order_release);
}

void RealtimeAudioEngine::publishTransport() noexcept {
    transportExchange_.publish({clock_.isPlaying(), clock_.publicPosition(),
                                clock_.duration(),
                                lastResolvedCommandSequence_.load(
                                    std::memory_order_acquire)});
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
                       [](const auto& track) {
                           return track.source.isAvailable();
                       });
}

} // namespace vitadaw::audio
