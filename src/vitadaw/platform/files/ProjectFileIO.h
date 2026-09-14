#pragma once
#include "vitadaw/persistence/PersistenceResult.h"
#include "vitadaw/media/AudioSource.h"
#include <vector>
#include <span>

namespace vitadaw::platform::files {
struct FileReadIdentity {
    std::uint64_t device{}, inode{};
    std::int64_t modifiedSeconds{}, modifiedNanos{}, changedSeconds{}, changedNanos{};
    bool operator==(const FileReadIdentity&) const = default;
};
struct ReadResult {
    persistence::PersistenceResult result;
    std::vector<std::byte> bytes;
    FileReadIdentity identity{};
};
class IProjectFileIO {
public:
    virtual ~IProjectFileIO() = default;
    virtual ReadResult read(const std::filesystem::path&, std::size_t maximumBytes) = 0;
    virtual persistence::PersistenceResult replace(const std::filesystem::path&, std::string_view bytes) = 0;
};
// Test faults exercise the production file protocol at its actual phase boundaries.
class NativeProjectFileIO final : public IProjectFileIO {
public:
    explicit NativeProjectFileIO(persistence::PersistencePhase fault = persistence::PersistencePhase::none)
        : fault_(fault) {}
    ReadResult read(const std::filesystem::path&, std::size_t maximumBytes) override;
    persistence::PersistenceResult replace(const std::filesystem::path&, std::string_view bytes) override;
private:
    persistence::PersistencePhase fault_;
};
[[nodiscard]] IProjectFileIO& nativeProjectFileIO();
// Hash the same immutable encoded bytes supplied to the WAV decoder.
[[nodiscard]] media::MediaFingerprint fingerprint(std::span<const std::byte> bytes);
// Bounded streaming verification after decode; never retains a second encoded file.
[[nodiscard]] persistence::PersistenceResult verifyFingerprint(
    const std::filesystem::path&, const media::MediaFingerprint&,
    const FileReadIdentity* originalRead = nullptr);
} // namespace vitadaw::platform::files
