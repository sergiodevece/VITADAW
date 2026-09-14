#pragma once

#include <cstddef>

namespace vitadaw::audio {

struct ConstAudioBlockView {
    const float* const* channels{};
    std::size_t channelCount{};
    std::size_t frameCount{};

    [[nodiscard]] bool isValid() const noexcept {
        return channels != nullptr && channelCount > 0 && frameCount > 0;
    }
};

struct AudioBlockView {
    float* const* channels{};
    std::size_t channelCount{};
    std::size_t frameCount{};

    [[nodiscard]] bool isValid() const noexcept {
        return channels != nullptr && channelCount > 0 && frameCount > 0;
    }

};

} // namespace vitadaw::audio
