#include "vitadaw/audio/PreparedTemporalContext.h"

#include <algorithm>
#include <cmath>

namespace vitadaw::audio {

bool PreparedLoopRange::isValid() const noexcept {
    return musical.start.value >= 0 && musical.end.value > musical.start.value &&
           std::isfinite(start.value) && std::isfinite(end.value) &&
           end.value > start.value && std::isfinite(duration.value) &&
           duration.value > 0.0 && clockBounds.valid;
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
        if (loop->start.value < 0 || loop->end.value <= loop->start.value ||
            loop->end.value - loop->start.value < 1024)
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
