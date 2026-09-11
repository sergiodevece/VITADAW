#pragma once

#include "vitadaw/timeline/Timeline.h"

#include <cstdint>
#include <filesystem>

namespace vitadaw::clips {

using ClipId = std::uint64_t;

// Editable project data. This object must never be read directly by the RT thread.
struct AudioClip {
    ClipId id{};
    std::filesystem::path sourceFile;
    timeline::FrameRange timelineRange;
    timeline::SourceFrameCount sourceOffset;
    timeline::SourceFrameCount sourceFrameCount;
    timeline::SampleRate sourceSampleRate;
};

} // namespace vitadaw::clips
