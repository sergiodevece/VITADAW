#pragma once

#include <cstddef>

namespace vitadaw::audio {

struct AudioBlockView {
    float* const* channels{};
    std::size_t channelCount{};
    std::size_t frameCount{};
};

// This boundary is called only by the device callback. Implementations must be
// allocation-free, lock-free, bounded, and noexcept.
class IRealtimeAudioProcessor {
public:
    virtual ~IRealtimeAudioProcessor() = default;
    virtual void process(AudioBlockView output) noexcept = 0;
};

} // namespace vitadaw::audio

