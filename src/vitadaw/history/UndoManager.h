#pragma once

#include "vitadaw/project/ProjectState.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string_view>
#include <variant>
#include <vector>

namespace vitadaw::history {

struct StateToken {
    std::uint64_t value{};
    bool operator==(const StateToken&) const = default;
};
struct MoveClip {
    tracks::TrackId beforeTrack, afterTrack;
    clips::AudioClip before, after;
};
struct TrimClipLeft { tracks::TrackId track; clips::AudioClip before, after; };
struct TrimClipRight { tracks::TrackId track; clips::AudioClip before, after; };
struct DuplicateClip { tracks::TrackId track; clips::AudioClip created; };
struct DeleteClip { tracks::TrackId track; clips::AudioClip removed; };
struct TempoEdit { std::optional<musical::TempoEvent> before, after; };
struct SignatureEdit { std::optional<musical::TimeSignatureEvent> before, after; };
struct LoopRangeEdit { std::optional<musical::MusicalLoopRange> before, after; };
struct SplitClip { tracks::TrackId track; clips::AudioClip original, left, right; };
using TrackHistoryStatePtr =
    std::shared_ptr<const project::ProjectState::TrackHistoryState>;
struct AddAudioTrack { TrackHistoryStatePtr created; };
struct DeleteAudioTrack { TrackHistoryStatePtr removed; };
struct ReorderAudioTrack {
    tracks::TrackId track;
    std::size_t beforeIndex{}, afterIndex{};
};
struct MoveClips {
    std::vector<project::ProjectState::ClipHistoryState> before, after;
};
struct DuplicateClips {
    std::vector<project::ProjectState::ClipHistoryState> created;
};
struct DeleteClips {
    std::vector<project::ProjectState::ClipHistoryState> removed;
};
struct RecordAudio {
    tracks::TrackId track;
    media::AudioSource source;
    clips::AudioClip clip;
};

// Model values only. No borrowed views or prepared/runtime ownership.
class UndoableOperation {
public:
    using Payload = std::variant<MoveClip, DuplicateClip, SplitClip,
                                 TrimClipLeft, TrimClipRight, DeleteClip,
                                 TempoEdit, SignatureEdit, LoopRangeEdit,
                                 AddAudioTrack, DeleteAudioTrack,
                                 ReorderAudioTrack, MoveClips,
                                 DuplicateClips, DeleteClips, RecordAudio>;
    Payload payload;
    [[nodiscard]] bool isMusical() const noexcept {
        return std::holds_alternative<TempoEdit>(payload) ||
               std::holds_alternative<SignatureEdit>(payload) ||
               std::holds_alternative<LoopRangeEdit>(payload);
    }
    [[nodiscard]] std::string_view label() const noexcept;
    [[nodiscard]] std::size_t approximateMemoryBytes() const noexcept;
    // Operates on a disposable application-thread candidate, never active state.
    [[nodiscard]] bool apply(project::ProjectState& candidate, bool forward) const;
};

struct HistoryEntry {
    UndoableOperation operation;
    StateToken beforeStateToken, afterStateToken;
    [[nodiscard]] std::size_t approximateMemoryBytes() const noexcept {
        return sizeof(HistoryEntry) + operation.approximateMemoryBytes();
    }
};

// Application-thread only. A staged append owns all storage before model commit.
class UndoManager {
public:
    static constexpr std::size_t maximumEntries = 512;
    static constexpr std::size_t memoryBudget = 8 * 1024 * 1024;
    struct Limits { std::size_t entries{maximumEntries}, bytes{memoryBudget}; };
    struct PendingAppend {
        std::vector<HistoryEntry> entries;
        StateToken token;
    };
    UndoManager() = default;
    explicit UndoManager(Limits limits) : limits_(limits) {}
    [[nodiscard]] bool canUndo() const noexcept { return cursor_ != 0; }
    [[nodiscard]] bool canRedo() const noexcept { return cursor_ < entries_.size(); }
    [[nodiscard]] const HistoryEntry* undoEntry() const noexcept;
    [[nodiscard]] const HistoryEntry* redoEntry() const noexcept;
    [[nodiscard]] std::string_view undoLabel() const noexcept;
    [[nodiscard]] std::string_view redoLabel() const noexcept;
    [[nodiscard]] std::optional<PendingAppend> stage(UndoableOperation operation) const;
    void commit(PendingAppend&& pending) noexcept;
    void commitUndo() noexcept;
    void commitRedo() noexcept;
    void clearHistory() noexcept;
    void commitBarrier() noexcept;
    [[nodiscard]] bool canCreateState() const noexcept;
    [[nodiscard]] StateToken currentStateToken() const noexcept { return current_; }
    [[nodiscard]] std::uint64_t revision() const noexcept { return revision_; }
    [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }
    [[nodiscard]] std::size_t cursor() const noexcept { return cursor_; }
    [[nodiscard]] std::size_t memoryBytes() const noexcept {
        std::size_t result{};
        for (const auto& entry : entries_) result += entry.approximateMemoryBytes();
        return result;
    }
private:
    Limits limits_;
    std::vector<HistoryEntry> entries_;
    std::size_t cursor_{};
    StateToken current_{1};
    std::uint64_t nextToken_{2}, revision_{};
};

} // namespace vitadaw::history
