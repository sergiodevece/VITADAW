#include "vitadaw/audio/RealtimePlaybackCursor.h"

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
    vitadaw::audio::RealtimePlaybackCursor cursor;

    check(!cursor.play(), "play without prepared frames should fail");

    cursor.prepare(100);
    check(cursor.play(), "play should succeed after preparation");
    cursor.advance(25.5);
    check(cursor.position() == 25.5, "playback should advance by source frames");

    cursor.stopAndRewind();
    check(!cursor.isPlaying(), "stop should leave playback stopped");
    check(cursor.position() == 0.0, "stop should rewind to the first frame");

    check(cursor.play(), "play should restart after stop");
    cursor.advance(100.0);
    check(!cursor.isPlaying(), "end of resource should stop playback");
    check(cursor.play(), "play at end should restart from the beginning");
    check(cursor.position() == 0.0, "replay at end should rewind first");

    cursor.prepare(44100);
    check(cursor.play(), "sample-rate conversion test should start");
    constexpr auto sourceFramesPerDeviceFrame = 44100.0 / 48000.0;
    for (int deviceFrame = 0; deviceFrame < 48000; ++deviceFrame) {
        cursor.advance(sourceFramesPerDeviceFrame);
    }
    check(!cursor.isPlaying(),
          "44100 source frames should span 48000 device frames after conversion");
    check(cursor.position() == 44100.0,
          "sample-rate conversion should finish exactly at the source end");

    std::cout << "All playback-cursor tests passed\n";
    return EXIT_SUCCESS;
}
