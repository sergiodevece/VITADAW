#pragma once

#include <cstdint>
#include <string>

namespace vitadaw::audio {

enum class AudioDeviceStatus {
    closed,
    active,
    error,
};

struct AudioDeviceInfo {
    std::string outputDeviceName;
    double sampleRate{};
    std::uint32_t bufferSizeFrames{};
    std::uint32_t availableInputChannels{};
    std::uint32_t availableOutputChannels{};

    bool operator==(const AudioDeviceInfo&) const = default;
};

struct AudioDeviceState {
    AudioDeviceStatus status{AudioDeviceStatus::closed};
    AudioDeviceInfo info;
    std::string errorMessage;

    bool operator==(const AudioDeviceState&) const = default;
};

// Application-thread model. It owns strings and must never be accessed from the
// realtime callback.
class AudioDeviceStateModel {
public:
    [[nodiscard]] const AudioDeviceState& state() const noexcept;

    void markActive(AudioDeviceInfo info);
    void markError(std::string message);
    void markClosed();

private:
    AudioDeviceState state_;
};

} // namespace vitadaw::audio

