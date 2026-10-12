#include "vitadaw/audio/RealtimeCapture.h"

#include <algorithm>
#include <new>

namespace vitadaw::audio {

// Counters are zeroed with the ring itself: prepare() and reset() run while
// capture is inactive, and begin() runs in the callback before any consumer
// can observe the session, so no concurrent writer of these fields exists.
void RealtimeCapture::resetTelemetryCounters() noexcept {
    peakOccupancyFrames_.store(0, std::memory_order_relaxed);
    droppedFrames_.store(0, std::memory_order_relaxed);
    captureCalls_.store(0, std::memory_order_relaxed);
    overflowEvents_.store(0, std::memory_order_relaxed);
    consumedFrames_.store(0, std::memory_order_relaxed);
    drainCalls_.store(0, std::memory_order_relaxed);
    emptyDrainCalls_.store(0, std::memory_order_relaxed);
    peakServiceBacklogFrames_.store(0, std::memory_order_relaxed);
    lateServiceEvents_.store(0, std::memory_order_relaxed);
}

bool RealtimeCapture::prepare(std::size_t frameCapacity) {
    if (frameCapacity == 0 || isActive()) return false;
    try {
        std::array<std::vector<float>, 2> next;
        next[0].resize(frameCapacity);
        next[1].resize(frameCapacity);
        samples_.swap(next);
    } catch (const std::bad_alloc&) {
        return false;
    }
    capacity_ = frameCapacity;
    writeFrame_.store(0, std::memory_order_relaxed);
    readFrame_.store(0, std::memory_order_relaxed);
    acceptedFrames_.store(0, std::memory_order_relaxed);
    resetTelemetryCounters();
    session_ = 0;
    track_ = {};
    projectStart_ = {};
    deviceSampleRate_ = {};
    placement_ = {};
    status_.store(encode(RecordingPhase::idle, RecordingFailure::none),
                  std::memory_order_release);
    return true;
}

void RealtimeCapture::reset() noexcept {
    if (isActive()) return;
    writeFrame_.store(0, std::memory_order_relaxed);
    readFrame_.store(0, std::memory_order_relaxed);
    acceptedFrames_.store(0, std::memory_order_relaxed);
    resetTelemetryCounters();
    session_ = 0;
    track_ = {};
    projectStart_ = {};
    deviceSampleRate_ = {};
    placement_ = {};
    status_.store(encode(RecordingPhase::idle, RecordingFailure::none),
                  std::memory_order_release);
}

bool RealtimeCapture::prepareRequest(const RecordingRequest& request) noexcept {
    if (!request.session || !request.track.isValid() || !canPrepareRequest() ||
        (request.layout != media::AudioChannelLayout::mono &&
         request.layout != media::AudioChannelLayout::stereo)) return false;
    session_ = request.session;
    track_ = request.track;
    layout_ = request.layout;
    projectStart_ = {};
    deviceSampleRate_ = {};
    placement_ = request.placement;
    status_.store(encode(RecordingPhase::prepared, RecordingFailure::none),
                  std::memory_order_release);
    return true;
}

void RealtimeCapture::clearPreparedRequest() noexcept {
    if (!isPrepared()) return;
    status_.store(encode(RecordingPhase::idle, RecordingFailure::none),
                  std::memory_order_release);
    session_ = 0;
    track_ = {};
    projectStart_ = {};
    deviceSampleRate_ = {};
    placement_ = {};
}

bool RealtimeCapture::begin(const RecordingRequest& request,
                            timeline::ProjectFramePosition start,
                            timeline::SampleRate deviceRate) noexcept {
    // Direct portable users may begin a freshly allocated ring. The production
    // engine binds the request before enqueueing, so lifecycle closure can see
    // it while it is still prepared.
    if (canPrepareRequest() && !prepareRequest(request)) return false;
    if (!request.session || !request.track.isValid() || !deviceRate.isValid() ||
        capacity_ == 0 || !isPrepared() || session_ != request.session ||
        track_ != request.track || layout_ != request.layout ||
        (request.layout != media::AudioChannelLayout::mono &&
         request.layout != media::AudioChannelLayout::stereo)) {
        return false;
    }
    session_ = request.session;
    track_ = request.track;
    layout_ = request.layout;
    projectStart_ = start;
    deviceSampleRate_ = deviceRate;
    writeFrame_.store(0, std::memory_order_relaxed);
    readFrame_.store(0, std::memory_order_relaxed);
    acceptedFrames_.store(0, std::memory_order_relaxed);
    resetTelemetryCounters();
    status_.store(encode(RecordingPhase::capturing, RecordingFailure::none),
                  std::memory_order_release);
    return true;
}

void RealtimeCapture::capture(ConstAudioBlockView input) noexcept {
    if (!isCapturing() || input.frameCount == 0) return;
    const auto channels = static_cast<std::size_t>(media::channelCount(layout_));
    if (input.channels == nullptr || input.channelCount < channels) {
        fail(RecordingFailure::missingInput);
        return;
    }
    for (std::size_t channel = 0; channel < channels; ++channel) {
        if (input.channels[channel] == nullptr) {
            fail(RecordingFailure::missingInput);
            return;
        }
    }

    captureCalls_.fetch_add(1, std::memory_order_relaxed);
    const auto write = writeFrame_.load(std::memory_order_relaxed);
    const auto read = readFrame_.load(std::memory_order_acquire);
    const auto used = write - read;
    if (used > capacity_ || input.frameCount > capacity_ - used) {
        droppedFrames_.fetch_add(input.frameCount, std::memory_order_relaxed);
        overflowEvents_.fetch_add(1, std::memory_order_relaxed);
        fail(RecordingFailure::overflow);
        return;
    }
    const auto occupancy = used + input.frameCount;
    if (occupancy > peakOccupancyFrames_.load(std::memory_order_relaxed))
        peakOccupancyFrames_.store(occupancy, std::memory_order_relaxed);

    const auto first = static_cast<std::size_t>(write % capacity_);
    const auto firstCount = std::min(input.frameCount, capacity_ - first);
    const auto secondCount = input.frameCount - firstCount;
    for (std::size_t channel = 0; channel < channels; ++channel) {
        std::copy_n(input.channels[channel], firstCount,
                    samples_[channel].data() + first);
        if (secondCount != 0) {
            std::copy_n(input.channels[channel] + firstCount, secondCount,
                        samples_[channel].data());
        }
    }
    acceptedFrames_.fetch_add(input.frameCount, std::memory_order_relaxed);
    writeFrame_.store(write + input.frameCount, std::memory_order_release);
}

void RealtimeCapture::stop() noexcept {
    auto expected = encode(RecordingPhase::capturing, RecordingFailure::none);
    static_cast<void>(status_.compare_exchange_strong(
        expected, encode(RecordingPhase::complete, RecordingFailure::none),
        std::memory_order_release,
        std::memory_order_relaxed));
}

void RealtimeCapture::fail(RecordingFailure reason) noexcept {
    if (reason == RecordingFailure::none) return;
    auto expected = status_.load(std::memory_order_acquire);
    while (phase(expected) == RecordingPhase::prepared ||
           phase(expected) == RecordingPhase::capturing) {
        if (status_.compare_exchange_weak(
                expected, encode(RecordingPhase::failed, reason),
                std::memory_order_release, std::memory_order_acquire)) return;
    }
}

std::size_t RealtimeCapture::drain(AudioBlockView output) noexcept {
    if (output.channels == nullptr || output.frameCount == 0 || capacity_ == 0)
        return 0;
    const auto state = status_.load(std::memory_order_acquire);
    if (phase(state) == RecordingPhase::idle || phase(state) == RecordingPhase::prepared)
        return 0;
    const auto channels = static_cast<std::size_t>(media::channelCount(layout_));
    if (output.channelCount < channels) return 0;
    for (std::size_t channel = 0; channel < channels; ++channel)
        if (output.channels[channel] == nullptr) return 0;

    const auto read = readFrame_.load(std::memory_order_relaxed);
    const auto write = writeFrame_.load(std::memory_order_acquire);
    const auto backlog = write - read;
    drainCalls_.fetch_add(1, std::memory_order_relaxed);
    if (backlog == 0) emptyDrainCalls_.fetch_add(1, std::memory_order_relaxed);
    if (backlog > peakServiceBacklogFrames_.load(std::memory_order_relaxed))
        peakServiceBacklogFrames_.store(backlog, std::memory_order_relaxed);
    const auto budget = serviceBudgetFrames_.load(std::memory_order_relaxed);
    if (budget != 0 && backlog > budget)
        lateServiceEvents_.fetch_add(1, std::memory_order_relaxed);
    const auto count = static_cast<std::size_t>(
        std::min<std::uint64_t>(backlog, output.frameCount));
    const auto first = static_cast<std::size_t>(read % capacity_);
    const auto firstCount = std::min(count, capacity_ - first);
    const auto secondCount = count - firstCount;
    for (std::size_t channel = 0; channel < channels; ++channel) {
        std::copy_n(samples_[channel].data() + first, firstCount,
                    output.channels[channel]);
        if (secondCount != 0) {
            std::copy_n(samples_[channel].data(), secondCount,
                        output.channels[channel] + firstCount);
        }
    }
    readFrame_.store(read + count, std::memory_order_release);
    consumedFrames_.fetch_add(count, std::memory_order_relaxed);
    return count;
}

RecordingCaptureTelemetry RealtimeCapture::telemetry() const noexcept {
    RecordingCaptureTelemetry result;
    result.ringCapacityFrames = capacity_;
    const auto write = writeFrame_.load(std::memory_order_acquire);
    const auto read = readFrame_.load(std::memory_order_acquire);
    result.occupancyFrames = static_cast<std::size_t>(write >= read ? write - read : 0);
    const auto peak = peakOccupancyFrames_.load(std::memory_order_relaxed);
    result.peakOccupancyFrames = static_cast<std::size_t>(peak);
    result.minimumHeadroomFrames = capacity_ > peak
        ? capacity_ - static_cast<std::size_t>(peak) : 0;
    result.peakServiceBacklogFrames = static_cast<std::size_t>(
        peakServiceBacklogFrames_.load(std::memory_order_relaxed));
    result.serviceBudgetFrames = static_cast<std::size_t>(
        serviceBudgetFrames_.load(std::memory_order_relaxed));
    result.producedFrames = acceptedFrames_.load(std::memory_order_relaxed);
    result.consumedFrames = consumedFrames_.load(std::memory_order_relaxed);
    result.droppedFrames = droppedFrames_.load(std::memory_order_relaxed);
    result.captureCalls = captureCalls_.load(std::memory_order_relaxed);
    result.overflowEvents = overflowEvents_.load(std::memory_order_relaxed);
    result.drainCalls = drainCalls_.load(std::memory_order_relaxed);
    result.emptyDrainCalls = emptyDrainCalls_.load(std::memory_order_relaxed);
    result.lateServiceEvents = lateServiceEvents_.load(std::memory_order_relaxed);
    return result;
}

void RealtimeCapture::setServiceBudgetFrames(std::size_t frames) noexcept {
    serviceBudgetFrames_.store(frames, std::memory_order_relaxed);
}

RecordingSnapshot RealtimeCapture::snapshot() const noexcept {
    RecordingSnapshot result;
    const auto state = status_.load(std::memory_order_acquire);
    result.phase = phase(state);
    result.failure = failure(state);
    if (result.phase == RecordingPhase::idle) {
        return result;
    }
    result.session = session_;
    result.track = track_;
    result.layout = layout_;
    result.projectStart = projectStart_;
    result.acceptedDeviceFrames = {
        acceptedFrames_.load(std::memory_order_acquire)};
    result.deviceSampleRate = deviceSampleRate_;
    result.placement = placement_;
    return result;
}

} // namespace vitadaw::audio
