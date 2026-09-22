#pragma once

#include "vitadaw/audio/RecoveryInventory.h"

#include <filesystem>
#include <span>

namespace vitadaw::platform::files {

// Synchronous, read-only control-side inventory of caller-authorized roots.
// Callers must supply explicit, trusted roots: the root itself cannot be a
// symlink, and discovered symlink/reparse entries are rejected. This 0.8.2A
// contract is intentionally not a proof of canonical physical containment
// against every symlink that may exist in an ancestor component of an
// authorized path. It performs only metadata reads and bounded WAV-header
// reads; it never writes, publishes, adopts, moves, or deletes media.
//
// A returned candidate is informative discovery data, not future mutation
// authority. Recover/Delete/Adopt/GC implementations must reopen and
// revalidate identity immediately before acting, and apply their own suitable
// containment policy for a mutating operation.
[[nodiscard]] audio::RecoveryInventory scanRecoveryInventory(
    std::span<const std::filesystem::path> authorizedRoots);

// Revalidates the pathname/identity pair from a discovery snapshot. It is a
// guard for a future mutating operation, not an authorization by itself.
[[nodiscard]] bool matchesRecoveryCandidateSnapshot(
    const audio::RecoveryCandidate&) noexcept;

} // namespace vitadaw::platform::files
