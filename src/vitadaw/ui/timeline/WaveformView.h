#pragma once

#include "vitadaw/clips/AudioClip.h"
#include "vitadaw/waveform/WaveformCache.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace vitadaw::ui::timeline {

struct WaveformColumn {
    std::uint32_t pixel{};
    std::array<waveform::PeakPair, 2> channels{};
};

struct WaveformVisitResult {
    std::size_t levelIndex{};
    std::size_t columns{};
    std::size_t bucketsVisited{};
};

[[nodiscard]] std::size_t selectWaveformLevel(
    const waveform::PreparedWaveformData&, double sourceFramesPerPixel) noexcept;

template <typename Visitor>
WaveformVisitResult visitWaveformColumns(
    const waveform::PreparedWaveformData& data,
    ::vitadaw::timeline::SourceFramePosition sourceOffset,
    ::vitadaw::timeline::ProjectFrameDuration clipDuration,
    ::vitadaw::timeline::SampleRate projectRate,
    std::uint32_t clipPixelWidth, std::uint32_t firstVisiblePixel,
    std::uint32_t visiblePixelCount, Visitor&& visitor) noexcept {
    WaveformVisitResult result;
    if (!projectRate.isValid() || !data.sourceSampleRate.isValid() ||
        data.levels.empty() || clipPixelWidth == 0 || visiblePixelCount == 0 ||
        clipDuration.value <= 0.0 || sourceOffset.value >= data.sourceFrameCount.value)
        return result;
    const auto sourcePerProject = data.sourceSampleRate.hertz() / projectRate.hertz();
    const auto sourceLength = clipDuration.value * sourcePerProject;
    const auto framesPerPixel = sourceLength / static_cast<double>(clipPixelWidth);
    result.levelIndex = selectWaveformLevel(data, framesPerPixel);
    const auto& level = data.levels[result.levelIndex];
    const auto lastPixel = std::min<std::uint64_t>(
        clipPixelWidth, static_cast<std::uint64_t>(firstVisiblePixel) + visiblePixelCount);
    for (std::uint64_t pixel = firstVisiblePixel; pixel < lastPixel; ++pixel) {
        const auto sourceBegin = static_cast<double>(sourceOffset.value) +
                                 pixel * framesPerPixel;
        const auto sourceEnd = static_cast<double>(sourceOffset.value) +
                               (pixel + 1U) * framesPerPixel;
        const auto firstBucket = static_cast<std::uint64_t>(
            std::floor(sourceBegin / level.framesPerBucket));
        auto endBucket = static_cast<std::uint64_t>(
            std::ceil(sourceEnd / level.framesPerBucket));
        endBucket = std::max(firstBucket + 1U, endBucket);
        WaveformColumn column;
        column.pixel = static_cast<std::uint32_t>(pixel);
        for (std::uint32_t channel = 0; channel < data.channelCount; ++channel) {
            column.channels[channel] = {std::numeric_limits<float>::infinity(),
                                        -std::numeric_limits<float>::infinity()};
            const auto& peaks = level.channels[channel];
            const auto boundedEnd = std::min<std::uint64_t>(endBucket, peaks.size());
            for (auto bucket = firstBucket; bucket < boundedEnd; ++bucket) {
                const auto& peak = peaks[static_cast<std::size_t>(bucket)];
                column.channels[channel].minimum =
                    std::min(column.channels[channel].minimum, peak.minimum);
                column.channels[channel].maximum =
                    std::max(column.channels[channel].maximum, peak.maximum);
                ++result.bucketsVisited;
            }
            if (!std::isfinite(column.channels[channel].minimum))
                column.channels[channel] = {};
        }
        visitor(column);
        ++result.columns;
    }
    return result;
}

} // namespace vitadaw::ui::timeline
