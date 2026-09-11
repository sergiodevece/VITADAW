#include "vitadaw/audio/RealtimePlaybackCursor.h"

#include <cmath>
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
    audio::RealtimePlaybackCursor cursor;
    check(!cursor.play(), "play without prepared frames should fail");
    cursor.prepare({100});
    check(cursor.play(), "play should succeed after preparation");
    cursor.advance({25.5});
    check(cursor.position().value == 25.5, "playback should advance by source frames");
    cursor.stopAndRewind();
    check(!cursor.isPlaying(), "stop should leave playback stopped");
    check(cursor.position().value == 0.0, "stop should rewind to the first frame");
    check(cursor.play(), "play should restart after stop");
    cursor.advance({100.0});
    check(!cursor.isPlaying(), "end of resource should stop playback");
    check(cursor.position().value == 100.0, "natural end should retain end position");
    check(cursor.play(), "play at end should restart from the beginning");
    check(cursor.position().value == 0.0, "replay at end should rewind first");

    cursor.prepare({44100});
    check(cursor.play(), "sample-rate conversion test should start");
    const auto increment = timeline::sourceFramesForDeviceFrames(
        {1}, timeline::SampleRate{44100.0}, timeline::SampleRate{48000.0});
    for (int deviceFrame = 0; deviceFrame < 48000; ++deviceFrame) {
        cursor.advance(increment);
    }
    check(!cursor.isPlaying(),
          "44100 source frames should span 48000 device frames after conversion");
    check(std::abs(cursor.position().value - 44100.0) < 1.0e-7,
          "sample-rate conversion should finish without evident drift");
    std::cout << "All playback-cursor tests passed\n";
    return EXIT_SUCCESS;
}
