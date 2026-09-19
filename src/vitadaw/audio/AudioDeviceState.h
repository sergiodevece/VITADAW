#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

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

// Ephemeral control/UI observation of the currently certified physical device.
// It owns its variable-size values and is never read or updated by RT.
struct DeviceLatencyReadModel {
    bool configurationAvailable{};
    std::uint32_t confirmedBufferSizeFrames{};
    std::vector<std::uint32_t> supportedBufferSizeFrames;
    std::vector<std::string> inputChannelNames;
    std::vector<std::string> outputChannelNames;
    double sampleRateHz{};
    std::optional<std::uint32_t> inputLatencyFrames;
    std::optional<std::uint32_t> outputLatencyFrames;
    std::optional<double> inputLatencyMilliseconds;
    std::optional<double> outputLatencyMilliseconds;
    std::optional<double> estimatedMonitoringLatencyMilliseconds;
    std::string lastBufferChangeError;
};

struct AudioDeviceBufferChangeResult {
    bool success{};
    std::string errorMessage;
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
