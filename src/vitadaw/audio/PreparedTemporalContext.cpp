#include "vitadaw/audio/PreparedTemporalContext.h"

#include <algorithm>
#include <cmath>

namespace vitadaw::audio {

bool PreparedLoopRange::isValid() const noexcept {
    return musical.start.value >= 0 && musical.end.value > musical.start.value &&
           std::isfinite(start.value) && std::isfinite(end.value) &&
           end.value > start.value && std::isfinite(duration.value) &&
           duration.value > 0.0;
}

TemporalContextPreparationResult prepareTemporalContext(
    const musical::MusicalTimeMap& map,
    std::optional<musical::MusicalLoopRange> loop,
    timeline::SampleRate projectSampleRate,
    timeline::SampleRate deviceSampleRate,
    std::uint64_t revision) {
    if (!projectSampleRate.isValid() || !deviceSampleRate.isValid())
        return {nullptr, "Invalid temporal sample rate"};
    auto compiled = musical::PreparedMusicalTimeMap::compile(
        map, projectSampleRate, revision);
    if (!compiled) return {nullptr, musical::errorName(compiled.error)};

    auto result = std::make_unique<PreparedTemporalContext>();
    result->documentMap = map;
    result->revision = revision;
    result->musicalTime = std::move(compiled.value);
    if (loop) {
        if (loop->start.value < 0 || loop->end.value <= loop->start.value ||
            loop->end.value - loop->start.value < 1024)
            return {nullptr, "Loop must contain at least 1024 ticks"};
        const auto start = result->musicalTime->preciseProjectFrameAtTick(loop->start);
        const auto end = result->musicalTime->preciseProjectFrameAtTick(loop->end);
        if (!start || !end || !std::isfinite(start.value.value) ||
            !std::isfinite(end.value.value) || end.value.value <= start.value.value)
            return {nullptr, "Loop cannot be represented in project time"};
        const auto duration = end.value.value - start.value.value;
        if (duration / projectSampleRate.hertz() < 0.010 || duration < 1.0 ||
            duration * deviceSampleRate.hertz() / projectSampleRate.hertz() < 1.0)
            return {nullptr, "Loop must be at least 10 ms and one project/device frame"};
        result->loop = PreparedLoopRange{*loop, start.value, end.value,
                                         {duration}, revision};
    }

    auto& clicks = result->clicks;
    clicks.deviceSampleRate = deviceSampleRate;
    clicks.frameCount = std::clamp<std::size_t>(
        static_cast<std::size_t>(std::ceil(deviceSampleRate.hertz() * 0.004)),
        1, maximumClickTableFrames);
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
