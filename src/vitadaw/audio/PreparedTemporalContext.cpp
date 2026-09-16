#include "vitadaw/audio/PreparedTemporalContext.h"

#include <algorithm>
#include <cmath>

namespace vitadaw::audio {
namespace {

std::optional<exact::UInt256> ticksAt(
    exact::Position position, exact::UInt128 denominator) noexcept {
    const auto floor = exact::floorPosition(position);
    if (floor.frame < 0 || exact::zero(exact::wide(floor.denominator))) return {};
    const auto scale = exact::divmod(exact::wide(denominator),
                                     exact::wide(floor.denominator));
    if (!scale.valid || !exact::zero(scale.remainder) ||
        !exact::fits128(scale.quotient)) return {};
    auto result = exact::multiply(
        {static_cast<std::uint64_t>(floor.frame), 0}, denominator);
    exact::UInt256 fraction;
    if (!exact::fits128((fraction = exact::multiply(
            floor.remainder, exact::low128(scale.quotient))))) return {};
    exact::add(result, fraction, result);
    return result;
}

std::optional<exact::Position> positionAt(
    exact::UInt256 ticks, exact::UInt128 denominator) noexcept {
    const auto split = exact::divmod(ticks, exact::wide(denominator));
    if (!split.valid || !exact::fits64(split.quotient) ||
        split.quotient.words[0] > exact::maximumFrame) return {};
    exact::UInt256 twice;
    if (exact::shiftLeft(split.remainder, 1, twice)) return {};
    const bool roundUp = exact::compare(twice, exact::wide(denominator)) >= 0;
    auto magnitude = split.remainder;
    if (roundUp) exact::subtract(exact::wide(denominator), magnitude, magnitude);
    if (!exact::fits128(magnitude) ||
        (roundUp && split.quotient.words[0] == exact::maximumFrame)) return {};
    return exact::Position{
        static_cast<std::int64_t>(split.quotient.words[0] + (roundUp ? 1 : 0)),
        {exact::low128(magnitude), denominator, roundUp}};
}

std::optional<DspFramePosition> positionInDenominator(
    exact::Position position, exact::UInt128 denominator) noexcept {
    const auto ticks = ticksAt(position, denominator);
    if (!ticks) return {};
    const auto converted = positionAt(*ticks, denominator);
    if (!converted) return {};
    return DspFramePosition{{converted->frame}, converted->phase};
}

} // namespace

bool PreparedLoopRange::isValid() const noexcept {
    return musical.start.value >= 0 && musical.end.value > musical.start.value &&
           std::isfinite(start.value) && std::isfinite(end.value) &&
           end.value > start.value && std::isfinite(duration.value) &&
           duration.value > 0.0 && clockBounds.valid;
}

bool PreparedLoopView::operator==(const PreparedLoopView& other) const noexcept {
    return musical_ == other.musical_ &&
        exact::comparePositions(exactStart_.exactPosition(),
                                other.exactStart_.exactPosition()) == 0 &&
        exact::comparePositions(exactEnd_.exactPosition(),
                                other.exactEnd_.exactPosition()) == 0 &&
        presentationStart_.value == other.presentationStart_.value &&
        presentationEnd_.value == other.presentationEnd_.value &&
        temporalRevision_ == other.temporalRevision_;
}

bool PreparedLoopView::contains(DspFramePosition position) const noexcept {
    return exact::comparePositions(position.exactPosition(),
                                   exactStart_.exactPosition()) >= 0 &&
           exact::comparePositions(position.exactPosition(),
                                   exactEnd_.exactPosition()) < 0;
}

std::optional<ExactProjectFrameDuration>
PreparedLoopView::distanceToEnd(DspFramePosition position) const noexcept {
    if (!contains(position)) return {};
    const auto at = ticksAt(position.exactPosition(), commonDenominator_);
    const auto end = ticksAt(exactEnd_.exactPosition(), commonDenominator_);
    if (!at || !end) return {};
    exact::UInt256 distance;
    if (exact::subtract(*end, *at, distance)) return {};
    const auto value = positionAt(distance, commonDenominator_);
    if (!value) return {};
    return ExactProjectFrameDuration{
        DspFramePosition{{value->frame}, value->phase}};
}

std::optional<DspFramePosition>
PreparedLoopView::positionAfterWrap(DspFramePosition position) const noexcept {
    if (exact::comparePositions(position.exactPosition(),
                                exactEnd_.exactPosition()) < 0) return {};
    const auto at = ticksAt(position.exactPosition(), commonDenominator_);
    const auto start = ticksAt(exactStart_.exactPosition(), commonDenominator_);
    const auto end = ticksAt(exactEnd_.exactPosition(), commonDenominator_);
    if (!at || !start || !end) return {};
    exact::UInt256 distance, length;
    if (exact::subtract(*at, *end, distance) ||
        exact::subtract(*end, *start, length) || exact::zero(length)) return {};
    const auto overshoot = exact::divmod(distance, length);
    if (!overshoot.valid) return {};
    exact::UInt256 wrapped;
    if (exact::add(*start, overshoot.remainder, wrapped)) return {};
    const auto value = positionAt(wrapped, commonDenominator_);
    if (!value) return {};
    return DspFramePosition{{value->frame}, value->phase};
}

TemporalContextPreparationResult prepareTemporalContext(
    const musical::MusicalTimeMap& map,
    std::optional<musical::MusicalLoopRange> loop,
    timeline::SampleRate projectSampleRate,
    timeline::SampleRate deviceSampleRate,
    std::uint64_t revision) {
    if (!projectSampleRate.isValid() || !deviceSampleRate.isValid())
        return {nullptr, "Invalid temporal sample rate"};
    const auto withinSchedulingDomain = [](double rate) {
        const auto value = exact::decodeForPreparation(rate);
        return value.valid && exact::compare(exact::wide(value.numerator), exact::wide(value.denominator)) >= 0 &&
            exact::compare(exact::wide(value.numerator), exact::multiply(value.denominator, {1048576, 0})) <= 0;
    };
    if (!withinSchedulingDomain(projectSampleRate.hertz()) || !withinSchedulingDomain(deviceSampleRate.hertz()))
        return {nullptr, "Temporal scheduling requires certified rates in [1, 1048576] Hz"};
    auto compiled = musical::PreparedMusicalTimeMap::compile(
        map, projectSampleRate, revision);
    if (!compiled) return {nullptr, musical::errorName(compiled.error)};

    auto result = std::make_unique<PreparedTemporalContext>();
    result->documentMap = map;
    result->revision = revision;
    result->projectSampleRate = projectSampleRate;
    result->deviceSampleRate = deviceSampleRate;
    result->musicalTime = std::move(compiled.value);
    result->exactClock = exact::clockForPreparation(
        projectSampleRate.hertz(), deviceSampleRate.hertz());
    if (!result->exactClock.valid)
        return {nullptr, "Uncertified project/device clock ratio"};
    std::vector<std::int64_t> signatureTicks;
    signatureTicks.reserve(map.signatures.events.size());
    for (const auto& signature : map.signatures.events) {
        const auto tick = result->musicalTime->tickAt({signature.bar, {0}, {0}});
        if (!tick) return {nullptr, "Unrepresentable signature anchor"};
        signatureTicks.push_back(tick.value.value);
    }
    // At most 8191 intersections for the two existing 4096-event limits.
    result->beats.reserve(map.tempo.events.size() + signatureTicks.size());
    std::size_t tempoIndex = 0, signatureIndex = 0;
    std::int64_t tick = 0;
    while (tick <= musical::maximumCoordinate) {
        const auto end = std::min({
            tempoIndex + 1 < map.tempo.events.size() ? map.tempo.events[tempoIndex + 1].tick.value : musical::maximumCoordinate + 1,
            signatureIndex + 1 < signatureTicks.size() ? signatureTicks[signatureIndex + 1] : musical::maximumCoordinate + 1,
            musical::maximumCoordinate + 1});
        const auto& tempo = map.tempo.events[tempoIndex];
        const auto& signature = map.signatures.events[signatureIndex];
        PreparedBeatSegment segment;
        segment.tempoTick = tempo.tick.value;
        segment.signatureTick = signatureTicks[signatureIndex];
        segment.ticksPerBeat = musical::ppq * 4 / signature.signature.denominator;
        segment.ticksPerBar = segment.ticksPerBeat * signature.signature.numerator;
        segment.firstTick = segment.signatureTick +
            ((tick - segment.signatureTick + segment.ticksPerBeat - 1) / segment.ticksPerBeat) * segment.ticksPerBeat;
        segment.endTick = end;
        const auto anchor = result->musicalTime->exactProjectFrameAtTick(tempo.tick);
        if (!anchor) return {nullptr, "Unrepresentable beat anchor"};
        const auto rate = exact::rateRatioForPreparation(projectSampleRate.hertz(), tempo.bpm.value);
        if (!rate.valid) return {nullptr, "Uncertified beat ratio"};
        // 60 / ppq = 1 / 256, evaluated with integers, not rounded BPM products.
        exact::UInt256 denominator;
        if (exact::shiftLeft(exact::wide(rate.denominator), 8, denominator)) return {nullptr, "Beat ratio overflow"};
        const auto framesPerTick = exact::reduceForPreparation(
            exact::wide(rate.numerator), denominator);
        segment.framesFromTempo = exact::linearMappingForPreparation(
            anchor.value, framesPerTick);
        if (!segment.framesFromTempo.valid)
            return {nullptr, "Beat segment exceeds exact 128/256-bit capacity"};
        if (segment.firstTick < end) {
            segment.firstFrame = exact::floorPosition(segment.positionAt(segment.firstTick)).frame;
            const auto lastTick = segment.firstTick + ((end - 1 - segment.firstTick) / segment.ticksPerBeat) * segment.ticksPerBeat;
            segment.lastFrame = exact::floorPosition(segment.positionAt(lastTick)).frame + 1;
            result->beats.push_back(segment);
        }
        tick = end;
        if (tempoIndex + 1 < map.tempo.events.size() && map.tempo.events[tempoIndex + 1].tick.value == tick) ++tempoIndex;
        if (signatureIndex + 1 < signatureTicks.size() && signatureTicks[signatureIndex + 1] == tick) ++signatureIndex;
    }
    if (loop) {
        if (!loop->isStructurallyValid())
            return {nullptr, "Loop must contain at least 1024 ticks"};
        const auto start = result->musicalTime->exactProjectFrameAtTick(loop->start);
        const auto end = result->musicalTime->exactProjectFrameAtTick(loop->end);
        if (!start || !end || exact::comparePositions(end.value, start.value) <= 0)
            return {nullptr, "Loop cannot be represented in project time"};
        const auto present = [](exact::Position position) {
            return static_cast<double>(position.frame) +
                   exact::phaseForPresentation(position.phase);
        };
        const auto startPresentation = present(start.value);
        const auto endPresentation = present(end.value);
        result->loop = PreparedLoopRange{*loop, {startPresentation},
                                         {endPresentation},
                                         {endPresentation-startPresentation}, revision, {}};
        result->loop->clockBounds = {
            exact::rationalBoundaryForPreparation(start.value),
            exact::rationalBoundaryForPreparation(end.value),
            startPresentation, endPresentation};
        if (!result->loop->clockBounds.valid)
            return {nullptr, "Loop is outside the certified exact temporal domain"};
        const auto tenMilliseconds = exact::rateRatioForPreparation(projectSampleRate.hertz(), 100);
        const auto oneDeviceFrame = exact::rateRatioForPreparation(projectSampleRate.hertz(), deviceSampleRate.hertz());
        const auto atLeast = [&](exact::Ratio128 threshold) {
            const auto mapping = exact::linearMappingForPreparation(start.value,
                                                                    threshold);
            return mapping.valid &&
                exact::comparePositions(end.value, mapping.at(1)) >= 0;
        };
        if (!atLeast({{1,0},{1,0},true}) || !atLeast(tenMilliseconds) || !atLeast(oneDeviceFrame))
            return {nullptr, "Loop must be at least 10 ms and one project/device frame"};
        result->exactClock = exact::extendClockForPreparation(
            result->exactClock, result->loop->clockBounds.exactStart,
            result->loop->clockBounds.exactEnd);
        if (!result->exactClock.valid)
            return {nullptr, "Loop cannot be integrated into the exact device clock"};
        const auto exactStart = positionInDenominator(
            exact::boundaryPosition(result->loop->clockBounds.exactStart),
            result->exactClock.denominator);
        const auto exactEnd = positionInDenominator(
            exact::boundaryPosition(result->loop->clockBounds.exactEnd),
            result->exactClock.denominator);
        if (!exactStart || !exactEnd)
            return {nullptr, "Loop read view cannot be represented exactly"};
        PreparedLoopView view;
        view.musical_ = *loop;
        view.exactStart_ = *exactStart;
        view.exactEnd_ = *exactEnd;
        view.presentationStart_ = {startPresentation};
        view.presentationEnd_ = {endPresentation};
        view.temporalRevision_ = revision;
        view.commonDenominator_ = result->exactClock.denominator;
        result->loopView = view;
    }

    auto& clicks = result->clicks;
    clicks.deviceSampleRate = deviceSampleRate;
    const auto clickDuration = exact::rateRatioForPreparation(deviceSampleRate.hertz(), 250);
    auto clickFrames = exact::divmod(exact::wide(clickDuration.numerator), exact::wide(clickDuration.denominator));
    if (!exact::zero(clickFrames.remainder)) exact::add(clickFrames.quotient, exact::wide(1), clickFrames.quotient);
    clicks.frameCount = std::clamp<std::size_t>(clickFrames.quotient.words[0], 1, maximumClickTableFrames);
    constexpr double pi = 3.1415926535897932384626433832795;
    for (std::size_t frame = 0; frame < clicks.frameCount; ++frame) {
        const auto seconds = static_cast<double>(frame) / deviceSampleRate.hertz();
        const auto envelope = 1.0 - static_cast<double>(frame) /
                                      static_cast<double>(clicks.frameCount);
        const auto shaped = envelope * envelope;
        clicks.normal[frame] = static_cast<float>(
            0.32 * shaped * std::sin(2.0 * pi * 1000.0 * seconds));
        clicks.accent[frame] = static_cast<float>(
            0.48 * shaped * std::sin(2.0 * pi * 1600.0 * seconds));
    }
    return {std::move(result), {}};
}

} // namespace vitadaw::audio
