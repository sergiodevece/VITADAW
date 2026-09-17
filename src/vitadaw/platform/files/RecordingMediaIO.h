#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>

namespace vitadaw::platform::files {

// Kept local to recording-media tests and injected only on the non-RT path.
// Production uses the all-clear default and never consults these as a callback
// control mechanism.
struct RecordingIoFaultInjection {
    std::size_t maximumWriteChunk{};
    bool failWrite{};
    bool failHeader{};
    bool failSync{};
    bool failClose{};
    bool failPublication{};
    bool failDirectorySync{};
};

class RecordingFileHandle {
public:
    explicit RecordingFileHandle(int descriptor,
                                 RecordingIoFaultInjection* faults = nullptr) noexcept;
    ~RecordingFileHandle() noexcept;
    RecordingFileHandle(const RecordingFileHandle&) = delete;
    RecordingFileHandle& operator=(const RecordingFileHandle&) = delete;
    RecordingFileHandle(RecordingFileHandle&&) = delete;
    RecordingFileHandle& operator=(RecordingFileHandle&&) = delete;

    [[nodiscard]] bool write(const void*, std::size_t, bool finalizing) noexcept;
    [[nodiscard]] bool setPosition(std::int64_t, bool finalizing) noexcept;
    [[nodiscard]] bool sync() noexcept;
    [[nodiscard]] bool close() noexcept;
    void setFinalizing() noexcept { finalizing_ = true; }
    [[nodiscard]] bool finalizing() const noexcept { return finalizing_; }
    [[nodiscard]] int descriptor() const noexcept { return descriptor_; }
    [[nodiscard]] const char* error() const noexcept { return error_; }

private:
    void fail(const char*) noexcept;

    int descriptor_{-1};
    RecordingIoFaultInjection* faults_{};
    const char* error_{};
    bool closeFailed_{};
    bool finalizing_{};
};

[[nodiscard]] bool publishRecordingNoReplace(const std::filesystem::path& temporary,
                                             const std::filesystem::path& published,
                                             RecordingIoFaultInjection*,
                                             const char*& error) noexcept;
[[nodiscard]] bool syncRecordingDirectory(const std::filesystem::path& directory,
                                          RecordingIoFaultInjection*,
                                          const char*& error) noexcept;

} // namespace vitadaw::platform::files
