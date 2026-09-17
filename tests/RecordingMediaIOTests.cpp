#include "vitadaw/platform/files/RecordingMediaIO.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace {
using namespace vitadaw::platform::files;

void check(bool value, const char* message) {
    if (!value) { std::cerr << "FAILED: " << message << '\n'; std::exit(EXIT_FAILURE); }
}

int create(const std::filesystem::path& path) {
#if defined(_WIN32)
    return ::_open(path.string().c_str(), _O_CREAT | _O_EXCL | _O_WRONLY | _O_BINARY,
                  _S_IREAD | _S_IWRITE);
#else
    return ::open(path.c_str(), O_CREAT | O_EXCL | O_WRONLY, 0600);
#endif
}

void fileFailures(const std::filesystem::path& directory) {
    {
        RecordingIoFaultInjection faults;
        faults.maximumWriteChunk = 2;
        const auto path = directory / "short-write.wav";
        RecordingFileHandle file{create(path), &faults};
        check(file.write("abcdef", 6, false) && file.sync() && file.close(),
              "short writes are completed before successful finalization");
        std::ifstream input(path); std::string bytes; input >> bytes;
        check(bytes == "abcdef", "short-write loop preserves all bytes");
    }
    {
        RecordingIoFaultInjection faults; faults.failWrite = true;
        RecordingFileHandle file{create(directory / "disk-full.wav"), &faults};
        check(!file.write("x", 1, false) && file.error() != nullptr && file.close(),
              "write failure is observable and descriptor closes");
    }
    {
        RecordingIoFaultInjection faults; faults.failHeader = true;
        RecordingFileHandle file{create(directory / "header.wav"), &faults};
        check(!file.write("x", 1, true) && file.error() != nullptr && file.close(),
              "header finalization failure is observable");
    }
    {
        RecordingIoFaultInjection faults; faults.failSync = true;
        RecordingFileHandle file{create(directory / "fsync.wav"), &faults};
        check(file.write("x", 1, false) && !file.sync() && file.error() != nullptr && file.close(),
              "file fsync failure prevents success and still closes");
    }
    {
        RecordingIoFaultInjection faults; faults.failClose = true;
        RecordingFileHandle file{create(directory / "close.wav"), &faults};
        check(file.write("x", 1, false) && !file.close() && file.error() != nullptr,
              "close failure is observable without a double-close");
    }
}

void publicationFailures(const std::filesystem::path& directory) {
    const auto temporary = directory / ".owned.part.wav";
    const auto published = directory / "published.wav";
    { std::ofstream{temporary} << "owned"; }
    const char* error{};
    RecordingIoFaultInjection fault; fault.failPublication = true;
    check(!publishRecordingNoReplace(temporary, published, &fault, error) &&
              !std::filesystem::exists(published),
          "injected publication failure does not create or replace destination");
    error = nullptr;
    check(publishRecordingNoReplace(temporary, published, nullptr, error) &&
              std::filesystem::exists(published),
          "hard-link publication creates a sibling final without replacing");
    RecordingIoFaultInjection directoryFault; directoryFault.failDirectorySync = true;
    error = nullptr;
    check(!syncRecordingDirectory(directory, &directoryFault, error) && error != nullptr &&
              std::filesystem::exists(published),
          "directory-sync failure retains the valid published candidate");
    error = nullptr;
    check(!syncRecordingDirectory(directory / "missing", nullptr, error) && error != nullptr,
          "inaccessible recording directory fails explicitly");
}
}

int main() {
    const auto directory = std::filesystem::temp_directory_path() / "vitadaw-recording-media-io-tests";
    std::error_code ignored;
    std::filesystem::remove_all(directory, ignored);
    std::filesystem::create_directories(directory, ignored);
    check(!ignored, "temporary test directory created");
    fileFailures(directory);
    publicationFailures(directory);
    std::filesystem::remove_all(directory, ignored);
    std::cout << "Recording media I/O tests passed\n";
}
