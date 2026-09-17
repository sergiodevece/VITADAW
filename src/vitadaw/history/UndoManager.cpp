#include "vitadaw/history/UndoManager.h"

#include <algorithm>
#include <cassert>
#include <limits>
#include <type_traits>

namespace vitadaw::history {

std::string_view UndoableOperation::label() const noexcept {
    constexpr std::string_view labels[]{"history.moveClip", "history.duplicateClip",
        "history.splitClip", "history.trimClipLeft", "history.trimClipRight",
        "history.deleteClip", "history.tempo", "history.timeSignature",
        "history.loopRange", "history.addAudioTrack", "history.deleteAudioTrack",
        "history.reorderAudioTrack", "history.moveClips",
        "history.duplicateClips", "history.deleteClips", "history.recordAudio"};
    return labels[payload.index()];
}

std::size_t UndoableOperation::approximateMemoryBytes() const noexcept {
    const auto trackBytes = [](const project::ProjectState::TrackHistoryState& state) {
        std::size_t bytes = sizeof(state) + state.track.name.capacity() +
            state.track.clips.capacity() * sizeof(clips::AudioClip) +
            state.track.inserts.processors.capacity() * sizeof(processors::ProcessorState) +
            state.sends.capacity() * sizeof(project::ProjectState::IndexedSendRoute);
        for (const auto& processor : state.track.inserts.processors) {
            bytes += processor.type.identifier.capacity() +
                processor.parameters.capacity() * sizeof(processors::ProcessorParameterState) +
                processor.serializedState.capacity();
        }
        return bytes;
    };
    return std::visit([&](const auto& edit) -> std::size_t {
        using T = std::decay_t<decltype(edit)>;
        if constexpr (std::is_same_v<T, AddAudioTrack>)
            return edit.created ? trackBytes(*edit.created) : 0;
        else if constexpr (std::is_same_v<T, DeleteAudioTrack>)
            return edit.removed ? trackBytes(*edit.removed) : 0;
        else if constexpr (std::is_same_v<T, MoveClips>)
            return (edit.before.capacity() + edit.after.capacity()) *
                   sizeof(project::ProjectState::ClipHistoryState);
        else if constexpr (std::is_same_v<T, DuplicateClips>)
            return edit.created.capacity() *
                   sizeof(project::ProjectState::ClipHistoryState);
        else if constexpr (std::is_same_v<T, DeleteClips>)
            return edit.removed.capacity() *
                   sizeof(project::ProjectState::ClipHistoryState);
        else if constexpr (std::is_same_v<T, RecordAudio>)
            return edit.source.media.originalPath.native().size() +
                   (edit.source.media.projectRelativePath
                        ? edit.source.media.projectRelativePath->native().size() : 0) +
                   (edit.source.media.fingerprint
                        ? edit.source.media.fingerprint->sha256.capacity() : 0);
        return 0;
    }, payload);
}

bool UndoableOperation::apply(project::ProjectState& candidate, bool forward) const {
    return std::visit([&](const auto& edit) {
        using T = std::decay_t<decltype(edit)>;
        if constexpr (std::is_same_v<T, TempoEdit> || std::is_same_v<T, SignatureEdit>) {
            auto map = candidate.musicalTime();
            if (!map.replace(forward ? edit.before : edit.after, forward ? edit.after : edit.before)) return false;
            candidate.setMusicalTime(std::move(map));
            return true;
        } else if constexpr (std::is_same_v<T, LoopRangeEdit>) {
            const auto& expected = forward ? edit.before : edit.after;
            if (candidate.loopRange() != expected) return false;
            candidate.setLoopRange(forward ? edit.after : edit.before);
            return true;
        } else if constexpr (std::is_same_v<T, AddAudioTrack>) {
            if (!edit.created) return false;
            if (forward) return candidate.restoreHistoryTrack(*edit.created);
            const auto removed = candidate.removeAudioTrack(edit.created->track.id);
            return removed && *removed == *edit.created;
        } else if constexpr (std::is_same_v<T, DeleteAudioTrack>) {
            if (!edit.removed) return false;
            if (!forward) return candidate.restoreHistoryTrack(*edit.removed);
            const auto removed = candidate.removeAudioTrack(edit.removed->track.id);
            return removed && *removed == *edit.removed;
        } else if constexpr (std::is_same_v<T, ReorderAudioTrack>) {
            return candidate.reorderHistoryTrack(
                edit.track, forward ? edit.beforeIndex : edit.afterIndex,
                forward ? edit.afterIndex : edit.beforeIndex);
        } else if constexpr (std::is_same_v<T, MoveClips>) {
            return candidate.replaceHistoryClips(
                forward ? std::span<const project::ProjectState::ClipHistoryState>{edit.before}
                        : std::span<const project::ProjectState::ClipHistoryState>{edit.after},
                forward ? std::span<const project::ProjectState::ClipHistoryState>{edit.after}
                        : std::span<const project::ProjectState::ClipHistoryState>{edit.before});
        } else if constexpr (std::is_same_v<T, DuplicateClips>) {
            return forward
                ? candidate.restoreHistoryClips(edit.created)
                : candidate.deleteHistoryClips(edit.created);
        } else if constexpr (std::is_same_v<T, DeleteClips>) {
            return forward
                ? candidate.deleteHistoryClips(edit.removed)
                : candidate.restoreHistoryClips(edit.removed);
        } else if constexpr (std::is_same_v<T, RecordAudio>) {
            return forward
                ? candidate.restoreHistoryRecording(edit.track, edit.source,
                                                    edit.clip)
                : candidate.deleteHistoryRecording(edit.track, edit.source,
                                                   edit.clip);
        } else if constexpr (std::is_same_v<T, MoveClip>) {
            return candidate.transferHistoryClip(
                forward ? edit.beforeTrack : edit.afterTrack,
                forward ? edit.before : edit.after,
                forward ? edit.afterTrack : edit.beforeTrack,
                forward ? edit.after : edit.before);
        } else {
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
    const auto capacity = std::min(limits_.entries, maximumEntries);
    const auto byteCapacity = std::min(limits_.bytes, memoryBudget);
    if (capacity == 0 || byteCapacity < sizeof(HistoryEntry) ||
        !canCreateState()) return std::nullopt;
    HistoryEntry next{std::move(operation), current_, {nextToken_}};
    const auto nextBytes = next.approximateMemoryBytes();
    if (nextBytes > byteCapacity) return std::nullopt;
    auto retained = std::min(cursor_, capacity - 1);
    std::size_t retainedBytes{};
    for (std::size_t index = cursor_ - retained; index < cursor_; ++index)
        retainedBytes += entries_[index].approximateMemoryBytes();
    while (retained != 0 && retainedBytes > byteCapacity - nextBytes) {
        retainedBytes -= entries_[cursor_ - retained].approximateMemoryBytes();
        --retained;
    }
    PendingAppend result{{}, {nextToken_}};
    result.entries.reserve(retained + 1);
    const auto first = entries_.begin() + static_cast<std::ptrdiff_t>(cursor_ - retained);
    result.entries.insert(result.entries.end(), first,
                          entries_.begin() + static_cast<std::ptrdiff_t>(cursor_));
    result.entries.push_back(std::move(next));
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
