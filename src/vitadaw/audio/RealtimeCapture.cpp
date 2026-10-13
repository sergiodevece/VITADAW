#include "vitadaw/audio/RealtimeCapture.h"

#include <algorithm>
#include <new>

namespace vitadaw::audio {

// Counters are zeroed with the ring: by prepare() and reset() while no take is
// active, and by begin() while it holds "starting", a phase in which neither
// capture() nor drain() touches them. A failure or a stale callback that
// overlaps it can only touch atomics.
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

void RealtimeCapture::resetRingIndices() noexcept {
    writeFrame_.store(0, std::memory_order_relaxed);
    readFrame_.store(0, std::memory_order_relaxed);
    acceptedFrames_.store(0, std::memory_order_relaxed);
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
    resetRingIndices();
    resetTelemetryCounters();
    track_.store({}, std::memory_order_relaxed);
    placement_ = {};
    layout_.store(media::AudioChannelLayout::mono, std::memory_order_relaxed);
    projectStart_.store(0, std::memory_order_relaxed);
    deviceSampleRate_.store(0.0, std::memory_order_relaxed);
    status_.store(encode(StatusPhase::idle, RecordingFailure::none, 0),
                  std::memory_order_release);
    return true;
}

void RealtimeCapture::reset() noexcept {
    // Only an inactive ring (idle or terminal) is reset. No other thread can
    // change such a status: begin() needs "prepared", stop() needs "capturing",
    // fail() and failSession() need an active phase. The plain store of the
    // status is therefore never lost. placement_ is read only by this thread;
    // a begin() that lost its claim to the failure may still be reading track_
    // and layout_, which are atomics.
    if (isActive()) return;
    resetRingIndices();
    resetTelemetryCounters();
    track_.store({}, std::memory_order_relaxed);
    placement_ = {};
    projectStart_.store(0, std::memory_order_relaxed);
    deviceSampleRate_.store(0.0, std::memory_order_relaxed);
    status_.store(encode(StatusPhase::idle, RecordingFailure::none, 0),
                  std::memory_order_release);
}

bool RealtimeCapture::prepareRequest(const RecordingRequest& request) noexcept {
    if (!request.session || request.session > maximumSession ||
        !request.track.isValid() || capacity_ == 0 ||
        (request.layout != media::AudioChannelLayout::mono &&
         request.layout != media::AudioChannelLayout::stereo)) return false;
    auto expected = status_.load(std::memory_order_acquire);
    if (statusPhase(expected) != StatusPhase::idle) return false;
    // The ring is idle: no other thread writes request metadata, and the
    // callback reads it only after acquiring "prepared" from the release CAS
    // below (a stale begin() still reading track_ or layout_ reads atomics).
    track_.store(request.track, std::memory_order_relaxed);
    placement_ = request.placement;
    layout_.store(request.layout, std::memory_order_relaxed);
    projectStart_.store(0, std::memory_order_relaxed);
    deviceSampleRate_.store(0.0, std::memory_order_relaxed);
    return status_.compare_exchange_strong(
        expected, encode(StatusPhase::prepared, RecordingFailure::none, request.session),
        std::memory_order_release, std::memory_order_relaxed);
}

void RealtimeCapture::clearPreparedRequest() noexcept {
    // Only a request that the callback has not claimed returns to idle. Its
    // metadata stays in place: it is not read while idle, and the next
    // prepareRequest() overwrites it.
    auto expected = status_.load(std::memory_order_acquire);
    if (statusPhase(expected) != StatusPhase::prepared) return;
    static_cast<void>(status_.compare_exchange_strong(
        expected, encode(StatusPhase::idle, RecordingFailure::none, 0),
        std::memory_order_release, std::memory_order_relaxed));
}

bool RealtimeCapture::begin(const RecordingRequest& request,
                            timeline::ProjectFramePosition start,
                            timeline::SampleRate deviceRate) noexcept {
    if (!request.session || request.session > maximumSession || !deviceRate.isValid())
        return false;
    // 1. Claim. Succeeds only if this exact session is prepared right now. An
    //    idle ring (cleanup ran), a terminal take (cancelled or failed), another
    //    prepared or capturing session: all fail here, before anything is
    //    written. Acquire: the request metadata published by prepareRequest()
    //    is visible from here on.
    auto expected = encode(StatusPhase::prepared, RecordingFailure::none, request.session);
    if (!status_.compare_exchange_strong(
            expected, encode(StatusPhase::starting, RecordingFailure::none, request.session),
            std::memory_order_acquire, std::memory_order_relaxed)) return false;

    // While "starting" is held, nothing else writes the metadata: the control
    // thread writes it only into an inactive ring, and capture() and drain()
    // ignore this phase. If a failure ends the take meanwhile, cleanup may
    // rewrite track_ and layout_ under these reads - both are atomics, and the
    // publication below then fails. A command that does not carry the request
    // this session was prepared with gives the claim back.
    if (track_.load(std::memory_order_relaxed) != request.track ||
        layout_.load(std::memory_order_relaxed) != request.layout) {
        expected = encode(StatusPhase::starting, RecordingFailure::none, request.session);
        static_cast<void>(status_.compare_exchange_strong(
            expected, encode(StatusPhase::prepared, RecordingFailure::none, request.session),
            std::memory_order_release, std::memory_order_relaxed));
        return false;
    }
    resetRingIndices();
    resetTelemetryCounters();
    projectStart_.store(start.value, std::memory_order_relaxed);
    deviceSampleRate_.store(deviceRate.hertz(), std::memory_order_relaxed);

    // 2. Publish. A fail() or cancel that landed while "starting" was held has
    //    already replaced it with "failed"; this CAS then fails and that
    //    terminal state stands. Release: the begin values and the zeroed ring
    //    are visible to anyone who acquires "capturing".
    expected = encode(StatusPhase::starting, RecordingFailure::none, request.session);
    return status_.compare_exchange_strong(
        expected, encode(StatusPhase::capturing, RecordingFailure::none, request.session),
        std::memory_order_release, std::memory_order_relaxed);
}

void RealtimeCapture::capture(ConstAudioBlockView input) noexcept {
    // One read decides whether, and for which session, this block is captured.
    // A failure found below terminalizes only that session, never a take that
    // the control thread prepared after this read.
    const auto state = status_.load(std::memory_order_acquire);
    if (statusPhase(state) != StatusPhase::capturing || input.frameCount == 0) return;
    const auto session = sessionTag(state);
    const auto channels = static_cast<std::size_t>(
        media::channelCount(layout_.load(std::memory_order_relaxed)));
    if (input.channels == nullptr || input.channelCount < channels) {
        static_cast<void>(failSession(session, RecordingFailure::missingInput));
        return;
    }
    for (std::size_t channel = 0; channel < channels; ++channel) {
        if (input.channels[channel] == nullptr) {
            static_cast<void>(failSession(session, RecordingFailure::missingInput));
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
        static_cast<void>(failSession(session, RecordingFailure::overflow));
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
    // Stop and failure race by CAS; whichever terminal transition wins stands.
    auto expected = status_.load(std::memory_order_acquire);
    while (statusPhase(expected) == StatusPhase::capturing) {
        if (status_.compare_exchange_weak(
                expected,
                encode(StatusPhase::complete, RecordingFailure::none, sessionTag(expected)),
                std::memory_order_release, std::memory_order_acquire)) return;
    }
}

void RealtimeCapture::fail(RecordingFailure reason) noexcept {
    if (reason == RecordingFailure::none) return;
    auto expected = status_.load(std::memory_order_acquire);
    while (active(statusPhase(expected))) {
        if (status_.compare_exchange_weak(
                expected, encode(StatusPhase::failed, reason, sessionTag(expected)),
                std::memory_order_release, std::memory_order_acquire)) return;
    }
}

bool RealtimeCapture::failSession(RecordingSessionId session,
                                  RecordingFailure reason) noexcept {
    if (reason == RecordingFailure::none || session == 0 || session > maximumSession)
        return false;
    auto expected = status_.load(std::memory_order_acquire);
    while (active(statusPhase(expected)) && sessionTag(expected) == session) {
        if (status_.compare_exchange_weak(
                expected, encode(StatusPhase::failed, reason, session),
                std::memory_order_release, std::memory_order_acquire)) return true;
    }
    return false;
}

std::size_t RealtimeCapture::drain(AudioBlockView output) noexcept {
    if (output.channels == nullptr || output.frameCount == 0 || capacity_ == 0)
        return 0;
    const auto state = statusPhase(status_.load(std::memory_order_acquire));
    if (state == StatusPhase::idle || state == StatusPhase::prepared ||
        state == StatusPhase::starting)
        return 0;
    const auto channels = static_cast<std::size_t>(
        media::channelCount(layout_.load(std::memory_order_relaxed)));
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
    // Control thread only. One acquire load decides the phase and the session;
    // the request metadata it then reads was published before that phase and
    // is written only by this same thread while idle.
    RecordingSnapshot result;
    const auto state = status_.load(std::memory_order_acquire);
    result.phase = publicPhase(statusPhase(state));
    result.failure = failure(state);
    if (result.phase == RecordingPhase::idle) {
        return result;
    }
    result.session = sessionTag(state);
    result.track = track_.load(std::memory_order_relaxed);
    result.layout = layout_.load(std::memory_order_relaxed);
    result.placement = placement_;
    // A prepared take has no begin values and no frames, even while begin()
    // is writing them ("starting" reads as prepared). Once the acquired phase
    // is capturing, the release CAS that published it also published them.
    if (result.phase == RecordingPhase::prepared) {
        return result;
    }
    result.projectStart = {projectStart_.load(std::memory_order_relaxed)};
    result.acceptedDeviceFrames = {
        acceptedFrames_.load(std::memory_order_acquire)};
    result.deviceSampleRate = timeline::SampleRate{
        deviceSampleRate_.load(std::memory_order_relaxed)};
    return result;
}

} // namespace vitadaw::audio
