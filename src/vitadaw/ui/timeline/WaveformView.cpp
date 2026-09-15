#include "vitadaw/ui/timeline/WaveformView.h"

namespace vitadaw::ui::timeline {

std::size_t selectWaveformLevel(
    const waveform::PreparedWaveformData& data,
    double sourceFramesPerPixel) noexcept {
    if (data.levels.empty() || !std::isfinite(sourceFramesPerPixel) ||
        sourceFramesPerPixel <= 0.0) return 0;
    std::size_t selected{};
    // Choose the coarsest level no larger than the pixel footprint. This keeps
    // normal/low zoom near one or two buckets per pixel. At high zoom the base
    // 128-frame bucket is the deliberately documented resolution limit.
    for (std::size_t i = 1; i < data.levels.size(); ++i) {
        if (static_cast<double>(data.levels[i].framesPerBucket) >
            sourceFramesPerPixel) break;
        selected = i;
    }
    return selected;
}

} // namespace vitadaw::ui::timeline
