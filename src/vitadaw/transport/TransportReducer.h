#pragma once

#include "vitadaw/transport/TransportState.h"
#include "vitadaw/audio/ExactTemporal.h"

#include <cmath>
#include <optional>

namespace vitadaw::transport {

enum class TransportActionKind : std::uint8_t { play, pause, stop, seek, finish, rewind };

struct TransportAction {
    TransportActionKind kind{TransportActionKind::stop};
    timeline::ProjectFramePosition target;
};

struct TransportBoundaryFacts {
    bool beforeContentEnd{};
    bool beforeLoopEnd{};
};

enum class ClockEffect : std::uint8_t { preserve, locate, loopStart };

struct TransportReduction {
    bool accepted{};
    TransportState state;
    bool seekDiscontinuity{};
    ClockEffect clockEffect{ClockEffect::preserve};
};

struct TransportReductionPolicy {
    struct Loop {
        timeline::PreciseProjectFramePosition start;
        timeline::PreciseProjectFramePosition end;
        audio::exact::RationalBoundary exactStart, exactEnd;
        Loop() = default;
        // Non-RT compatibility preparation. RT uses the prepared-value overload.
        Loop(timeline::PreciseProjectFramePosition a, timeline::PreciseProjectFramePosition b) noexcept
            : start(a), end(b), exactStart(audio::exact::rationalBoundaryForPreparation(a.value)),
              exactEnd(audio::exact::rationalBoundaryForPreparation(b.value)) {}
        explicit Loop(const audio::exact::LoopBounds& prepared) noexcept
            : start{prepared.start}, end{prepared.end}, exactStart(prepared.exactStart), exactEnd(prepared.exactEnd) {}
    };
    std::optional<Loop> loop;
    std::optional<TransportBoundaryFacts> boundaries;
};

// Pure command-order semantics. `contentDuration` remains descriptive; only
// the supported navigation domain limits Seek. Callers decide separately
// whether Play has an audible source/loop/metronome available.
[[nodiscard]] inline TransportReduction reduceTransport(
    const TransportState& current, TransportAction action,
    TransportReductionPolicy policy = {}) noexcept {
    auto next = current;
    switch (action.kind) {
    case TransportActionKind::play:
        if (policy.loop &&
            !(policy.boundaries ? policy.boundaries->beforeLoopEnd :
                audio::exact::compareBoundary({next.position.value, {}}, policy.loop->exactEnd) < 0)) {
            const auto& start = policy.loop->exactStart;
            audio::exact::UInt256 twice;
            audio::exact::shiftLeft(audio::exact::wide(start.numerator), 1, twice);
            const bool carry = audio::exact::compare(twice, audio::exact::wide(start.denominator)) >= 0;
            next.position = {static_cast<std::int64_t>(start.whole + (carry ? 1 : 0))};
            next.playback = PlaybackState::playing;
            return {true, next, false, ClockEffect::loopStart};
        } else if (!policy.loop && next.duration.value > 0 &&
                   !(policy.boundaries ? policy.boundaries->beforeContentEnd :
                       next.position.value < next.duration.value)) {
            next.position = {0};
            next.playback = PlaybackState::playing;
            return {true, next, false, ClockEffect::locate};
        }
        next.playback = PlaybackState::playing;
        return {true, next, false};
    case TransportActionKind::pause:
        if (next.playback == PlaybackState::playing)
            next.playback = PlaybackState::paused;
        return {true, next, false};
    case TransportActionKind::stop:
        if (next.playback == PlaybackState::stopped) {
            next.position = {0};
            return {true, next, false, ClockEffect::locate};
        }
        next.playback = PlaybackState::stopped;
        return {true, next, false};
    case TransportActionKind::seek:
        if (next.playback == PlaybackState::playing ||
            !timeline::isSupportedProjectFramePosition(action.target)) {
            return {false, current, false};
        }
        next.position = action.target;
        return {true, next, true, ClockEffect::locate};
    case TransportActionKind::finish:
        next.position = action.target;
        next.playback = PlaybackState::stopped;
        return {true, next, false, ClockEffect::locate};
    case TransportActionKind::rewind:
        next.position = {};
        next.playback = PlaybackState::stopped;
        return {true, next, false, ClockEffect::locate};
    }
    return {false, current, false};
}

// Pure companion to the reduction. Replaying descriptions never applies DSP
// effects, touches the real clock or consumes a discontinuity.
[[nodiscard]] inline TransportBoundaryFacts boundariesAfterReduction(
    const TransportReduction& result, TransportBoundaryFacts previous,
    const TransportReductionPolicy& policy) noexcept {
    if (result.clockEffect == ClockEffect::preserve) return previous;
    const auto atLoopStart = result.clockEffect == ClockEffect::loopStart && policy.loop;
    audio::exact::Position position{result.state.position.value, {}};
    if (atLoopStart) {
        const auto start = policy.loop->exactStart;
        position = {static_cast<std::int64_t>(start.whole),
                    {start.numerator, start.denominator, false}};
    }
    return {audio::exact::compareBoundary(position,
                audio::exact::Boundary{static_cast<std::uint64_t>(result.state.duration.value), {}, 0, true}) < 0,
            policy.loop && audio::exact::compareBoundary(position, policy.loop->exactEnd) < 0};
}

} // namespace vitadaw::transport
