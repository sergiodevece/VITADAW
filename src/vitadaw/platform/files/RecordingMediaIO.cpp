#include "vitadaw/platform/files/RecordingMediaIO.h"

#include <algorithm>
#include <climits>
#include <cstdint>
#include <system_error>

#if defined(_WIN32)
#include <io.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace vitadaw::platform::files {

RecordingFileHandle::RecordingFileHandle(int descriptor,
                                         RecordingIoFaultInjection* faults) noexcept
    : descriptor_(descriptor), faults_(faults) {}

RecordingFileHandle::~RecordingFileHandle() noexcept { static_cast<void>(close()); }

void RecordingFileHandle::fail(const char* message) noexcept {
    if (error_ == nullptr) error_ = message;
}

bool RecordingFileHandle::write(const void* bytes, std::size_t count, bool finalizing) noexcept {
    if (descriptor_ < 0) { fail("Recording file descriptor is closed"); return false; }
    if ((finalizing && faults_ && faults_->failHeader) ||
        (!finalizing && faults_ && faults_->failWrite)) {
        fail(finalizing ? "Injected WAV header finalization failure"
                        : "Injected recording write failure");
        return false;
    }
    const auto* cursor = static_cast<const std::byte*>(bytes);
    while (count != 0) {
        auto requested = count;
        if (faults_ && faults_->maximumWriteChunk != 0)
            requested = std::min(requested, faults_->maximumWriteChunk);
#if defined(_WIN32)
        requested = std::min(requested, static_cast<std::size_t>(UINT_MAX));
        const auto written = ::_write(descriptor_, cursor, static_cast<unsigned int>(requested));
#else
        const auto written = ::write(descriptor_, cursor, requested);
#endif
        if (written <= 0) { fail(finalizing ? "WAV header finalization failed" : "WAV write failed"); return false; }
        cursor += written;
        count -= static_cast<std::size_t>(written);
    }
    return true;
}

bool RecordingFileHandle::setPosition(std::int64_t position, bool finalizing) noexcept {
    if (position < 0 || descriptor_ < 0) { fail("Invalid recording file seek"); return false; }
    if (finalizing && faults_ && faults_->failHeader) {
        fail("Injected WAV header finalization failure");
        return false;
    }
#if defined(_WIN32)
    if (::_lseeki64(descriptor_, position, SEEK_SET) < 0) {
#else
    if (::lseek(descriptor_, static_cast<off_t>(position), SEEK_SET) < 0) {
#endif
        fail(finalizing ? "WAV header seek failed" : "Recording file seek failed");
        return false;
    }
    return true;
}

bool RecordingFileHandle::sync() noexcept {
    if (descriptor_ < 0 || (faults_ && faults_->failSync)) {
        fail(faults_ && faults_->failSync ? "Injected recording file fsync failure"
                                          : "Recording file fsync failed");
        return false;
    }
#if !defined(_WIN32)
    if (::fsync(descriptor_) != 0) { fail("Recording file fsync failed"); return false; }
#endif
    return true;
}

bool RecordingFileHandle::close() noexcept {
    if (descriptor_ < 0) return !closeFailed_;
#if defined(_WIN32)
    const bool closed = ::_close(descriptor_) == 0;
#else
    const bool closed = ::close(descriptor_) == 0;
#endif
    descriptor_ = -1;
    if (!closed || (faults_ && faults_->failClose)) {
        closeFailed_ = true;
        fail(faults_ && faults_->failClose ? "Injected recording file close failure"
                                           : "Recording file close failed");
        return false;
    }
    return true;
}

bool publishRecordingNoReplace(const std::filesystem::path& temporary,
                               const std::filesystem::path& published,
                               RecordingIoFaultInjection* faults,
                               const char*& error) noexcept {
    if (faults && faults->failPublication) {
        error = "Injected recording publication failure";
        return false;
    }
    std::error_code ec;
    std::filesystem::create_hard_link(temporary, published, ec);
    if (ec) { error = "The recorded WAV could not be published"; return false; }
    return true;
}

bool syncRecordingDirectory(const std::filesystem::path& directory,
                            RecordingIoFaultInjection* faults,
                            const char*& error) noexcept {
    if (faults && faults->failDirectorySync) {
        error = "Injected recording directory fsync failure";
        return false;
    }
#if defined(_WIN32)
    static_cast<void>(directory);
    error = "Directory fsync is unavailable on this platform";
    return false;
#else
    const auto descriptor = ::open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (descriptor < 0) { error = "Could not open recording directory for fsync"; return false; }
    const bool synced = ::fsync(descriptor) == 0;
    const bool closed = ::close(descriptor) == 0;
    if (!synced) error = "Recording directory fsync failed";
    else if (!closed) error = "Recording directory close failed";
    return synced && closed;
#endif
}

} // namespace vitadaw::platform::files
