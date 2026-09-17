#pragma once
#include "vitadaw/timeline/Time.h"
#include "vitadaw/transport/PlaybackState.h"
#include "vitadaw/transport/TransportReducer.h"
#include "vitadaw/audio/DspFramePosition.h"
#include "vitadaw/audio/ExactProjectPhase.h"
#include <optional>

namespace vitadaw::audio {
class RealtimeProjectClock {
public:
    using LoopBounds = exact::LoopBounds;
    void prepare(timeline::ProjectFrameCount duration) noexcept {
        duration_ = duration;
        loop_.reset();
        runUntilStop_ = false;
        phase_ = {};
        phase_.installPreparedFormat(exact::clockForPreparation(1, 1));
        stopAndRewind();
    }
    // Quiescent/non-RT. Never prepare or reduce a rate inside advanceDeviceFrame.
    bool prepareFormat(exact::ClockFormat format) noexcept {
        return phase_.installPreparedFormat(format);
    }
    void setPlaybackPolicy(std::optional<LoopBounds> loop, bool runUntilStop) noexcept {
        loop_ = loop && loop->valid ? loop : std::nullopt;
        runUntilStop_ = runUntilStop;
    }
    bool play() noexcept {
        if (duration_.value <= 0 && !loop_ && !runUntilStop_) return false;
        return transition({transport::TransportActionKind::play, {}}).accepted;
    }
    bool record() noexcept {
        runUntilStop_ = true;
        return transition({transport::TransportActionKind::record, {}}).accepted;
    }
    void pause() noexcept { static_cast<void>(transition({transport::TransportActionKind::pause, {}})); }
    void stop() noexcept { static_cast<void>(transition({transport::TransportActionKind::stop, {}})); }
    bool seek(timeline::ProjectFramePosition target) noexcept {
        return transition({transport::TransportActionKind::seek, target}).accepted;
    }
    void stopAndRewind() noexcept { static_cast<void>(transition({transport::TransportActionKind::rewind, {}})); }
    transport::TransportBoundaryFacts boundaryFacts() const noexcept {
        return {phase_.before(integerBoundary(duration_.value)), loop_ && phase_.before(loop_->exactEnd)};
    }
    bool isBefore(exact::Boundary boundary) const noexcept { return phase_.before(boundary); }
    bool isBefore(exact::RationalBoundary boundary) const noexcept { return phase_.before(boundary); }
    transport::TransportReduction transition(transport::TransportAction action) noexcept {
        transport::TransportReductionPolicy policy;
        if (loop_) policy.loop = transport::TransportReductionPolicy::Loop{*loop_};
        policy.boundaries = boundaryFacts();
        const auto result = transport::reduceTransport({playback_, publicPosition(), duration_}, action, policy);
        if (result.accepted) {
            playback_ = result.state.playback;
            if (result.clockEffect == transport::ClockEffect::loopStart) phase_.locate(loop_->exactStart);
            else if (result.clockEffect == transport::ClockEffect::locate) {
                phase_.locate(static_cast<std::uint64_t>(result.state.position.value));
                wrapped_ = false;
            }
        }
        return result;
    }
    // Offline compatibility helper for existing clock tests. Not used by engine.
    void advance(timeline::ProjectFrameDuration frames) noexcept {
        const auto format = exact::clockForPreparation(frames.value, 1);
        if (format.valid && phase_.installPreparedFormat(format)) advancePrepared();
    }
    void advanceDeviceFrame() noexcept { advancePrepared(); }
    bool isPlaying() const noexcept { return playback_ == transport::PlaybackState::playing; }
    transport::PlaybackState playback() const noexcept { return playback_; }
    timeline::PreciseProjectFramePosition position() const noexcept { return {renderPosition().approximate()}; }
    timeline::ProjectFramePosition publicPosition() const noexcept { return {phase_.position().frame}; }
    DspFramePosition renderPosition() const noexcept {
        const auto value = phase_.position();
        return {{value.frame}, value.phase};
    }
    timeline::ProjectFrameCount duration() const noexcept { return duration_; }
    std::optional<LoopBounds> loop() const noexcept { return loop_; }
    bool isRunUntilStop() const noexcept { return runUntilStop_; }
    exact::ClockFormat exactFormat() const noexcept { return phase_.format(); }
    std::size_t continuousFramesAvailable(std::size_t requested) const noexcept {
        return loop_ && isPlaying() ? phase_.framesBefore(loop_->exactEnd, requested) : requested;
    }
    bool consumeWrapped() noexcept { const bool result = wrapped_; wrapped_ = false; return result; }
    struct Checkpoint {
        timeline::ProjectFramePosition position;
        exact::Phase phase;
        transport::PlaybackState playback{transport::PlaybackState::stopped};
    };
    Checkpoint checkpoint() const noexcept { return {publicPosition(), phase_.position().phase, playback_}; }
    bool restoreQuiescentCheckpoint(Checkpoint value) noexcept {
        if (!phase_.restore({{value.position.value, value.phase}})) return false;
        playback_ = value.playback;
        wrapped_ = false;
        return true;
    }
private:
    static exact::Boundary integerBoundary(std::int64_t frame) noexcept {
        return {static_cast<std::uint64_t>(frame), {}, 0, true};
    }
    void advancePrepared() noexcept {
        if (!isPlaying()) return;
        std::optional<exact::ProjectPhase::Loop> loop;
        if (loop_) loop = {{loop_->exactStart, loop_->exactEnd}};
        wrapped_ = phase_.advance(loop) || wrapped_;
        if (loop_) return;
        if (!phase_.before(integerBoundary(timeline::maximumSupportedProjectFrame().value))) {
            static_cast<void>(transition({transport::TransportActionKind::finish, timeline::maximumSupportedProjectFrame()}));
        } else if (!runUntilStop_ && !phase_.before(integerBoundary(duration_.value))) {
            static_cast<void>(transition({transport::TransportActionKind::finish, {duration_.value}}));
        }
    }
    exact::ProjectPhase phase_;
    timeline::ProjectFrameCount duration_;
    std::optional<LoopBounds> loop_;
    bool runUntilStop_{}, wrapped_{};
    transport::PlaybackState playback_{transport::PlaybackState::stopped};
};
} // namespace vitadaw::audio
