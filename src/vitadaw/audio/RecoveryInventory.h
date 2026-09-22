#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace vitadaw::audio {

// This is an identity/path discovery snapshot, never an ownership or mutation
// capability. It does not prove that the same filesystem object has unchanged
// contents or metadata. A later adopt/delete operation must reopen and
// revalidate identity, containment, the evidence/type, and the content or
// metadata required by that specific operation immediately before acting.
// Discovery alone is not authorization to mutate.
struct RecoveryFileIdentity {
    std::uint64_t device{};
    std::uint64_t inode{};

    [[nodiscard]] bool valid() const noexcept { return device != 0 || inode != 0; }
    bool operator==(const RecoveryFileIdentity&) const = default;
};

enum class RecoveryOrigin : std::uint8_t {
    recording,
    exportWav,
    unknown,
};

enum class RecoveryCandidateClass : std::uint8_t {
    // Proven VitaDAW marker or strict temporary naming, with a structurally
    // coherent WAV. It is still only a discovery result.
    recognizedPotentiallyRecoverable,
    // Proven VitaDAW artifact, but missing, malformed, truncated, or changed
    // while it was being inspected.
    recognizedIncomplete,
    // A published WAV and a retained VitaDAW temporary name share one stable
    // identity. The temporary is an alias, not a second media copy.
    publishedTemporaryAlias,
    // A WAV-like file without sufficient VitaDAW provenance. This category
    // must never grant a future automatic deletion policy.
    provenanceUnknown,
};

enum class RecoveryWavValidation : std::uint8_t {
    notApplicable,
    structurallyValid,
    truncated,
    invalidHeader,
    unreadable,
    changedDuringScan,
};

struct RecoveryCandidate {
    RecoveryCandidateClass classification{RecoveryCandidateClass::provenanceUnknown};
    RecoveryOrigin origin{RecoveryOrigin::unknown};
    std::filesystem::path path;
    std::filesystem::path authorizedRoot;
    std::optional<RecoveryFileIdentity> identity;
    std::uintmax_t sizeBytes{};
    std::filesystem::file_time_type modifiedAt{};
    std::optional<std::string> recordingSessionId;
    std::optional<std::filesystem::path> associatedPath;
    RecoveryWavValidation wavValidation{RecoveryWavValidation::notApplicable};
    std::string reason;
    std::string diagnostic;
};

struct RecoveryInventoryDiagnostic {
    std::filesystem::path root;
    std::filesystem::path path;
    std::string message;
};

struct RecoveryInventory {
    std::vector<RecoveryCandidate> candidates;
    std::vector<RecoveryInventoryDiagnostic> diagnostics;
    std::vector<RecoveryInventoryDiagnostic> rootErrors;
};

} // namespace vitadaw::audio
