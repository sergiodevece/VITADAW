#include "vitadaw/audio/RecordingRecovery.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX 1
#endif
#include <windows.h>
#endif

namespace {
using namespace vitadaw;

void check(bool value, const char* message) {
    if (!value) { std::cerr << "FAILED: " << message << '\n'; std::exit(EXIT_FAILURE); }
}

void markerDiscovery() {
    const auto directory = std::filesystem::temp_directory_path() / "vitadaw-recording-recovery-tests";
    std::error_code error;
    std::filesystem::remove_all(directory, error);
    std::filesystem::create_directories(directory, error);
    check(!error, "recovery fixture directory created");
    { std::ofstream{directory / "Recording 000001.wav"} << "valid wav fixture"; }
    { std::ofstream{directory / ".Recording 000002.part.wav"} << "truncated"; }
    { std::ofstream{directory / ".Recording 000003.part.wav"} << "valid wav fixture"; }
    { std::ofstream{directory / "Recording 000004.wav"} << "valid wav fixture"; }
    { std::ofstream{directory / "Recording 999999.wav"} << "unrelated"; }

    audio::RecordingRecoveryMarker final;
    final.sessionId = "session-final";
    final.classification = audio::RecordingRecoveryClass::publishedFinal;
    final.publishedName = "Recording 000001.wav";
    final.deviceSampleRate = timeline::SampleRate{48000};
    final.acceptedFrames = {128};
    std::string message;
    check(audio::writeRecordingRecoveryMarker(directory, final, message),
          "published marker persisted");
    std::string duplicateMessage;
    check(!audio::writeRecordingRecoveryMarker(directory, final, duplicateMessage),
          "an existing marker is never silently replaced");
#if defined(_WIN32)
    constexpr auto duplicateCode = ERROR_FILE_EXISTS;
#else
    constexpr auto duplicateCode = EEXIST;
#endif
    check(duplicateMessage.find("create") != std::string::npos &&
              duplicateMessage.find("failed (" + std::to_string(duplicateCode) + ")") !=
                  std::string::npos,
          "existing marker reports an exclusive-create diagnostic");
    audio::RecordingRecoveryMarker temporary;
    temporary.sessionId = "session-temp";
    temporary.classification = audio::RecordingRecoveryClass::temporary;
    temporary.temporaryName = ".Recording 000002.part.wav";
    temporary.deviceSampleRate = timeline::SampleRate{48000};
    check(audio::writeRecordingRecoveryMarker(directory, temporary, message),
          "temporary marker persisted");
    audio::RecordingRecoveryMarker validTemporary;
    validTemporary.sessionId = "session-valid-temp";
    validTemporary.classification = audio::RecordingRecoveryClass::temporary;
    validTemporary.temporaryName = ".Recording 000003.part.wav";
    validTemporary.deviceSampleRate = timeline::SampleRate{48000};
    check(audio::writeRecordingRecoveryMarker(directory, validTemporary, message),
          "valid temporary marker persisted");
    audio::RecordingRecoveryMarker closed;
    closed.sessionId = "session-closed";
    closed.classification = audio::RecordingRecoveryClass::closedUncommitted;
    closed.temporaryName = ".Recording 000004.part.wav";
    closed.publishedName = "Recording 000004.wav";
    closed.deviceSampleRate = timeline::SampleRate{48000};
    check(audio::writeRecordingRecoveryMarker(directory, closed, message),
          "closed uncommitted marker persisted");
    { std::ofstream{directory / ".vitadaw-recording-torn-temporary.recovery"} << "torn"; }

    const auto candidates = audio::scanRecordingRecoveryMarkers(directory, [&](const auto& path) {
        std::ifstream input(path); std::string text; std::getline(input, text); return text == "valid wav fixture";
    });
    check(candidates.size() == 4, "only complete VitaDAW markers are discovered");
    check(candidates[0].recoverable || candidates[1].recoverable,
          "valid finalized candidate is offered for explicit recovery");
    const auto incomplete = std::find_if(candidates.begin(), candidates.end(), [](const auto& candidate) {
        return candidate.persistentSessionId == "session-temp";
    });
    check(incomplete != candidates.end() && !incomplete->recoverable &&
              incomplete->classification == audio::RecordingRecoveryClass::incomplete,
          "unreadable temporary is retained but never offered as valid media");
    const auto validTemp = std::find_if(candidates.begin(), candidates.end(), [](const auto& candidate) {
        return candidate.persistentSessionId == "session-valid-temp";
    });
    check(validTemp != candidates.end() && validTemp->recoverable &&
              validTemp->classification == audio::RecordingRecoveryClass::temporary,
          "valid temporary WAV is conservatively offered for explicit recovery");
    const auto closedCandidate = std::find_if(candidates.begin(), candidates.end(), [](const auto& candidate) {
        return candidate.persistentSessionId == "session-closed";
    });
    check(closedCandidate != candidates.end() && closedCandidate->recoverable &&
              closedCandidate->path.filename() == "Recording 000004.wav" &&
              closedCandidate->classification == audio::RecordingRecoveryClass::closedUncommitted,
          "closed marker prefers its uncommitted final candidate");
    std::filesystem::remove_all(directory, error);
}

void markerPersistenceFailuresPreserveSystemDetails() {
    const auto directory = std::filesystem::temp_directory_path() /
        "vitadaw-recording-recovery-marker-failure-tests";
    std::error_code error;
    std::filesystem::remove_all(directory, error);
    std::filesystem::create_directories(directory, error);
    check(!error, "marker-failure fixture directory created");

    const std::array cases{
        std::pair{audio::RecordingRecoveryMarkerFault::create, "create"},
        std::pair{audio::RecordingRecoveryMarkerFault::write, "write"},
        std::pair{audio::RecordingRecoveryMarkerFault::sync, "fsync"},
        std::pair{audio::RecordingRecoveryMarkerFault::close, "close"},
    };
    for (std::size_t index = 0; index < cases.size(); ++index) {
        audio::RecordingRecoveryMarker marker;
        marker.sessionId = "marker-failure-" + std::to_string(index);
        marker.classification = audio::RecordingRecoveryClass::temporary;
        marker.temporaryName = ".Recording 000001.part.wav";
        marker.deviceSampleRate = timeline::SampleRate{48000};
        std::string message;
        check(!audio::writeRecordingRecoveryMarker(
                  directory, marker, message,
                  audio::RecordingRecoveryMarkerWriteOptions{cases[index].first}),
              "deterministic marker persistence fault is reported");
        const auto markerName = ".vitadaw-recording-" + marker.sessionId + "-temporary.recovery";
#if defined(_WIN32)
        const auto expectedCode = cases[index].first == audio::RecordingRecoveryMarkerFault::create
                                      ? ERROR_ACCESS_DENIED
                                      : (cases[index].first == audio::RecordingRecoveryMarkerFault::write
                                             ? ERROR_DISK_FULL
                                             : (cases[index].first ==
                                                        audio::RecordingRecoveryMarkerFault::sync
                                                    ? ERROR_WRITE_FAULT
                                                    : ERROR_INVALID_HANDLE));
#else
        const auto expectedCode = cases[index].first == audio::RecordingRecoveryMarkerFault::create
                                      ? EACCES
                                      : (cases[index].first == audio::RecordingRecoveryMarkerFault::write
                                             ? ENOSPC : EIO);
#endif
        check(message.find(cases[index].second) != std::string::npos &&
                  message.find(markerName) != std::string::npos &&
                  message.find("failed (" + std::to_string(expectedCode) + ")") !=
                      std::string::npos,
              "marker diagnostic retains operation, path, and system error code");
    }
    std::filesystem::remove_all(directory, error);
}
}

int main() {
    markerDiscovery();
    markerPersistenceFailuresPreserveSystemDetails();
    std::cout << "Recording recovery tests passed\n";
}
