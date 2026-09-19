#include "vitadaw/audio/RealtimeCapture.h"

#include <algorithm>
#include <new>

namespace vitadaw::audio {

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

    const auto write = writeFrame_.load(std::memory_order_relaxed);
    const auto read = readFrame_.load(std::memory_order_acquire);
    const auto used = write - read;
    if (used > capacity_ || input.frameCount > capacity_ - used) {
        fail(RecordingFailure::overflow);
        return;
    }

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
    const auto count = static_cast<std::size_t>(
        std::min<std::uint64_t>(write - read, output.frameCount));
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
    return count;
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
