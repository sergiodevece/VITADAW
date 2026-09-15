#pragma once

#include <cstdint>

namespace vitadaw::transport {

enum class PlaybackState : std::uint8_t { stopped, playing, paused };

} // namespace vitadaw::transport
