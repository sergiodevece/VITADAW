#include "vitadaw/audio/AudioDeviceState.h"

#include <cstdlib>
#include <iostream>
#include <limits>
#include <string_view>

namespace {

void check(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

} // namespace

int main() {
    using namespace vitadaw::audio;

    AudioDeviceStateModel model;
    check(model.state().status == AudioDeviceStatus::closed,
          "device should initially be closed");

    const AudioDeviceInfo initialInfo{"Test Output", 48000.0, 256, 2, 2};
    model.markActive(initialInfo);
    check(model.state().status == AudioDeviceStatus::active,
          "valid configuration should become active");
    check(model.state().info == initialInfo, "active configuration should be retained");
    check(model.state().errorMessage.empty(), "active state should clear old errors");

    model.markError("Device disconnected");
    check(model.state().status == AudioDeviceStatus::error,
          "device failure should enter error state");
    check(model.state().info == AudioDeviceInfo{},
          "device failure should clear stale configuration");
    check(model.state().errorMessage == "Device disconnected",
          "device failure should retain its diagnostic");

    const AudioDeviceInfo recoveredInfo{"Recovered Output", 44100.0, 512, 0, 2};
    model.markActive(recoveredInfo);
    check(model.state().status == AudioDeviceStatus::active,
          "valid reinitialisation should recover from an error");
    check(model.state().info == recoveredInfo,
          "reinitialisation should publish the new configuration");
    check(model.state().errorMessage.empty(), "recovery should clear the diagnostic");

    model.markClosed();
    check(model.state() == AudioDeviceState{}, "close should restore a clean state");

    model.markActive({"", 0.0, 0, 0, 0});
    check(model.state().status == AudioDeviceStatus::error,
          "invalid active configuration should be rejected coherently");
    model.markActive({"Infinite rate", std::numeric_limits<double>::infinity(),
                      256, 0, 2});
    check(model.state().status == AudioDeviceStatus::error,
          "non-finite device sample rates should be rejected");

    std::cout << "All audio-device state tests passed\n";
    return EXIT_SUCCESS;
}
