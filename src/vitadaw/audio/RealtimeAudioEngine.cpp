#include "vitadaw/audio/RealtimeAudioEngine.h"

#include <algorithm>

namespace vitadaw::audio {

void RealtimeAudioEngine::configure(
    RealtimeProjectContext context,
    std::array<PreparedTrackView, tracks::audioTrackCount> tracks) noexcept {
    deviceUnavailable();
    context_ = context;
    tracks_ = tracks;
    clock_.prepare(context_.duration);
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

RealtimeTransportSnapshot RealtimeAudioEngine::transportSnapshot() const noexcept {
    return transportExchange_.snapshot();
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
    if (deviceState() != DeviceProcessingState::operational ||
        !clock_.isPlaying() || !context_.projectSampleRate.isValid() ||
        !deviceSampleRate.isValid()) {
        publishTransport();
        return;
    }

    const auto projectFramesPerDeviceFrame = timeline::projectFramesForDeviceFrames(
        {1}, context_.projectSampleRate, deviceSampleRate);
    for (std::size_t frame = 0; frame < output.frameCount; ++frame) {
        if (!clock_.isPlaying()) {
            break;
        }
        const auto mixed = mixTwoTracksAtProjectPosition(
            tracks_, clock_.position(), context_.projectSampleRate);
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
    publishTransport();
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
    return std::any_of(tracks_.begin(), tracks_.end(),
                       [](const auto& track) { return track.isAvailable(); });
}

} // namespace vitadaw::audio
