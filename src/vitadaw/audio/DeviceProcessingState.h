#pragma once

#include <cstdint>

namespace vitadaw::audio {

// Portable lifecycle observed by the command producer and the RT consumer.
// "operational" means that a registered callback has an active device
// consumer, not merely that JUCE has announced a device object.
enum class DeviceProcessingState : std::uint8_t {
    unavailable,
    initializing,
    operational,
    stopped,
    error,
};

} // namespace vitadaw::audio
