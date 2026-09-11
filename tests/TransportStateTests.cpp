#include "vitadaw/audio/RealtimeTransportExchange.h"
#include "vitadaw/transport/TransportState.h"

#include <cstdlib>
#include <iostream>
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
    using namespace vitadaw;
    audio::RealtimeTransportExchange exchange;
    exchange.publish({true, {24000}, {48000}, 7});
    const auto progress = exchange.snapshot();
    check(progress.playing && progress.position.value == 24000 &&
              progress.duration.value == 48000 &&
              progress.lastProcessedCommandSequence == 7,
          "RT exchange should publish a coherent snapshot");

    transport::TransportState state;
    state.setDuration({48000});
    state.markPlaying();
    state.synchronise(progress.playing, progress.position, progress.duration);
    check(state.playback == transport::PlaybackState::playing &&
              state.position.value == 24000,
          "transport should track playback position");

    exchange.publish({false, {48000}, {48000}, 7});
    const auto naturalEnd = exchange.snapshot();
    state.synchronise(naturalEnd.playing, naturalEnd.position, naturalEnd.duration);
    check(state.playback == transport::PlaybackState::stopped,
          "natural end should transition transport to stopped");
    check(state.position.value == state.duration.value,
          "natural end should remain at the end position");
    state.stopAndRewind();
    check(state.playback == transport::PlaybackState::stopped && state.position.value == 0,
          "explicit Stop should rewind to zero");
    std::cout << "All transport-state tests passed\n";
    return EXIT_SUCCESS;
}
