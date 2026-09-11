#include "vitadaw/audio/RealtimeAudioEngine.h"
#include "vitadaw/audio/StereoAccumulator.h"
#include "vitadaw/audio/TrackMixerProcessing.h"

#include <algorithm>
#include <cmath>
#include <type_traits>

namespace vitadaw::audio {

void RealtimeAudioEngine::configure(PreparedProjectView project) noexcept {
    deviceUnavailable();
    project_ = project;
    trackMixCount_ = std::min(project_.tracks.size(), maximumTrackCount);
    for (std::size_t index = 0; index < trackMixCount_; ++index) {
        trackMix_[index].reset(project_.tracks[index].mix.isValid()
                                   ? project_.tracks[index].mix
                                   : mixer::PreparedTrackMixState{});
        meterTrackIds_[index] = project_.tracks[index].id;
    }
    masterMix_.reset(project_.masterMix.isValid()
                         ? project_.masterMix
                         : mixer::PreparedMasterMixState{});
    anySolo_ = project_.anySolo;
    parameterReadIndex_.store(0, std::memory_order_relaxed);
    parameterWriteIndex_.store(0, std::memory_order_relaxed);
    clock_.prepare(project_.duration);
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
    bool anySolo) noexcept {
    const auto exists = std::any_of(
        project_.tracks.begin(), project_.tracks.begin() + trackMixCount_,
        [track](const auto& candidate) { return candidate.id == track; });
    return track.isValid() && exists && mix.isValid() &&
           enqueueParameter(TrackMixCommand{track, mix, anySolo});
}

bool RealtimeAudioEngine::tryUpdateGlobalSolo(bool anySolo) noexcept {
    return enqueueParameter(GlobalSoloCommand{anySolo});
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
        !project_.projectSampleRate.isValid() ||
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
        {1}, project_.projectSampleRate, deviceSampleRate);
    for (std::size_t frame = 0; frame < output.frameCount; ++frame) {
        if (!clock_.isPlaying()) {
            break;
        }
        StereoSample mixed;
        for (std::size_t index = 0; index < trackMixCount_; ++index) {
            const auto& track = project_.tracks[index];
            const auto rendered = renderTrackAtProjectPosition(
                track, clock_.position(), project_.projectSampleRate);
            const auto currentMix = trackMix_[index].next();
            const auto contribution = applyTrackMix(
                rendered, track.channelCount, currentMix, anySolo_);
            trackPeaks_[index].left = std::max(
                trackPeaks_[index].left, std::abs(contribution.left));
            trackPeaks_[index].right = std::max(
                trackPeaks_[index].right, std::abs(contribution.right));
            accumulateTrackContribution(
                mixed, contribution);
        }
        applyMasterGain(mixed, masterMix_.next());
        masterPeak_.left = std::max(masterPeak_.left, std::abs(mixed.left));
        masterPeak_.right = std::max(masterPeak_.right, std::abs(mixed.right));
        if (output.channels != nullptr && output.channelCount > 0 &&
            output.channels[0] != nullptr) {
            output.channels[0][frame] = mixed.left;
        }
        if (output.channels != nullptr && output.channelCount > 1 &&
            output.channels[1] != nullptr) {
            output.channels[1][frame] = mixed.right;
        }
        clock_.advance(projectFramesPerDeviceFrame);
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
                    for (std::size_t index = 0; index < trackMixCount_; ++index) {
                        if (project_.tracks[index].id == value.track) {
                            trackMix_[index].setTarget(value.mix,
                                                       deviceSampleRate);
                            anySolo_ = value.anySolo;
                            break;
                        }
                    }
                } else if constexpr (std::is_same_v<T, MasterMixCommand>) {
                    masterMix_.setTarget(value.mix, deviceSampleRate);
                } else {
                    anySolo_ = value.anySolo;
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
}

void RealtimeAudioEngine::publishMeters() noexcept {
    meterExchange_.publish(
        {meterTrackIds_.data(), trackMixCount_},
        {trackPeaks_.data(), trackMixCount_}, masterPeak_);
}

void RealtimeAudioEngine::advanceSmoothers(std::size_t frameCount) noexcept {
    for (std::size_t frame = 0; frame < frameCount; ++frame) {
        for (std::size_t index = 0; index < trackMixCount_; ++index) {
            static_cast<void>(trackMix_[index].next());
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
    return std::any_of(project_.tracks.begin(),
                       project_.tracks.begin() + trackMixCount_,
                       [](const auto& track) { return track.isAvailable(); });
}

} // namespace vitadaw::audio
