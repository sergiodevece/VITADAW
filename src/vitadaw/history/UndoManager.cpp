#include "vitadaw/history/UndoManager.h"

#include <algorithm>
#include <cassert>
#include <limits>
#include <type_traits>

namespace vitadaw::history {

std::string_view UndoableOperation::label() const noexcept {
    constexpr std::string_view labels[]{"history.moveClip", "history.duplicateClip",
        "history.splitClip", "history.trimClipLeft", "history.trimClipRight",
        "history.deleteClip"};
    return labels[payload.index()];
}

bool UndoableOperation::apply(project::ProjectState& candidate, bool forward) const {
    return std::visit([&](const auto& edit) {
        using T = std::decay_t<decltype(edit)>;
        const auto matches = [&](const clips::AudioClip& expected) {
            const auto* track = candidate.findTrack(edit.track);
            return track != nullptr && std::any_of(track->clips.begin(), track->clips.end(),
                [&](const auto& clip) { return clip == expected; });
        };
        const auto restore = [&](const clips::AudioClip& clip) {
            return candidate.restoreHistoryClip(edit.track, clip);
        };
        const auto replace = [&](const clips::AudioClip& clip) {
            return candidate.replaceHistoryClip(edit.track, clip);
        };
        if constexpr (std::is_same_v<T, DuplicateClip>) {
            if (forward) return restore(edit.created);
            return matches(edit.created) && bool(candidate.deleteClip(edit.created.id));
        } else if constexpr (std::is_same_v<T, DeleteClip>) {
            if (!forward) return restore(edit.removed);
            return matches(edit.removed) && bool(candidate.deleteClip(edit.removed.id));
        } else if constexpr (std::is_same_v<T, SplitClip>) {
            if (forward) {
                if (!matches(edit.original) || candidate.findClip(edit.right.id)) return false;
                return replace(edit.left) && restore(edit.right);
            }
            if (!matches(edit.left) || !matches(edit.right)) return false;
            return bool(candidate.deleteClip(edit.right.id)) && replace(edit.original);
        } else {
            return matches(forward ? edit.before : edit.after) &&
                   replace(forward ? edit.after : edit.before);
        }
    }, payload);
}

const HistoryEntry* UndoManager::undoEntry() const noexcept {
    return canUndo() ? &entries_[cursor_ - 1] : nullptr;
}
const HistoryEntry* UndoManager::redoEntry() const noexcept {
    return canRedo() ? &entries_[cursor_] : nullptr;
}
std::string_view UndoManager::undoLabel() const noexcept {
    return canUndo() ? undoEntry()->operation.label() : std::string_view{};
}
std::string_view UndoManager::redoLabel() const noexcept {
    return canRedo() ? redoEntry()->operation.label() : std::string_view{};
}
bool UndoManager::canCreateState() const noexcept {
    return nextToken_ != std::numeric_limits<std::uint64_t>::max() &&
           revision_ != std::numeric_limits<std::uint64_t>::max();
}
std::optional<UndoManager::PendingAppend> UndoManager::stage(UndoableOperation operation) const {
    const auto capacity = std::min({limits_.entries, maximumEntries,
        limits_.bytes / sizeof(HistoryEntry), memoryBudget / sizeof(HistoryEntry)});
    if (capacity == 0 || !canCreateState()) return std::nullopt;
    const auto retained = std::min(cursor_, capacity - 1);
    PendingAppend result{{}, {nextToken_}};
    result.entries.reserve(retained + 1);
    const auto first = entries_.begin() + static_cast<std::ptrdiff_t>(cursor_ - retained);
    result.entries.insert(result.entries.end(), first,
                          entries_.begin() + static_cast<std::ptrdiff_t>(cursor_));
    result.entries.push_back({std::move(operation), current_, result.token});
    return result;
}
void UndoManager::commit(PendingAppend&& pending) noexcept {
    entries_.swap(pending.entries);
    cursor_ = entries_.size();
    current_ = pending.token;
    ++nextToken_;
    ++revision_;
}
void UndoManager::commitUndo() noexcept {
    assert(canUndo());
    current_ = entries_[--cursor_].beforeStateToken;
    ++revision_;
}
void UndoManager::commitRedo() noexcept {
    assert(canRedo());
    current_ = entries_[cursor_++].afterStateToken;
    ++revision_;
}
void UndoManager::clearHistory() noexcept {
    entries_.clear();
    cursor_ = 0;
}
void UndoManager::commitBarrier() noexcept {
    assert(canCreateState());
    clearHistory();
    current_ = {nextToken_++};
    ++revision_;
}
} // namespace vitadaw::history
