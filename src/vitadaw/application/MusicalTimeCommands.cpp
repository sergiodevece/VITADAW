#include "vitadaw/application/DawApplication.h"
#include <algorithm>
#include <limits>

namespace vitadaw::application {
commands::CommandResult DawApplication::commitMusicalProject(project::ProjectState candidate,
    history::UndoManager::PendingAppend* pending, int direction,
    std::unique_ptr<const musical::PreparedMusicalTimeMap> prepared) {
    using namespace commands;
    if (transport_.playback != transport::PlaybackState::stopped)
        return {CommandStatus::rejected, "Stop before editing musical time", CommandError::transportMustBeStopped};
    if (musicalRevision_ == UINT64_MAX) return {CommandStatus::rejected, "Musical revision exhausted", CommandError::capacityExceeded};
    if (!prepared) {
        auto compiled = musical::PreparedMusicalTimeMap::compile(candidate.musicalTime(), candidate.sampleRate(), musicalRevision_ + 1);
        if (!compiled) return {CommandStatus::rejected, musical::errorName(compiled.error), CommandError::validationFailed};
        prepared = std::move(compiled.value);
    }
    auto temporal = audioEngine_.prepareTemporalContext(
        candidate.musicalTime(), candidate.loopRange(), candidate.sampleRate(),
        musicalRevision_ + 1);
    if (!temporal.success())
        return {CommandStatus::rejected, std::move(temporal.errorMessage),
                CommandError::validationFailed};
    struct Context {
        DawApplication* application;
        project::ProjectState* candidate;
        std::unique_ptr<const musical::PreparedMusicalTimeMap>* prepared;
        history::UndoManager::PendingAppend* pending;
        int direction;
    } context{this, &candidate, &prepared, pending, direction};
    const audio::AudioFileCommitAction commit{&context, [](void* raw) noexcept {
        auto& value = *static_cast<Context*>(raw);
        value.application->session_.project.swap(*value.candidate);
        value.application->musicalTime_.swap(*value.prepared);
        ++value.application->musicalRevision_;
        if (value.pending) value.application->session_.history.commit(std::move(*value.pending));
        else if (value.direction > 0) value.application->session_.history.commitRedo();
        else value.application->session_.history.commitUndo();
    }};
    if (!audioEngine_.commitPreparedTemporalContext(
            std::move(temporal.prepared), commit))
        return {CommandStatus::rejected, "Temporal context commit failed",
                CommandError::preparationFailed};
    return {CommandStatus::accepted, "Musical time committed"};
}
commands::CommandResult DawApplication::musicalCommand(const commands::Command& command) {
    using namespace commands;
    using namespace musical;
    if (transport_.playback != transport::PlaybackState::stopped)
        return {CommandStatus::rejected, "Stop before editing musical time", CommandError::transportMustBeStopped};
    auto candidate = session_.project;
    auto map = candidate.musicalTime();
    history::UndoableOperation operation;
    const auto error = std::visit([&](const auto& c) -> Error {
        using T = std::decay_t<decltype(c)>;
        if constexpr (std::is_same_v<T, SetLoopRangeMusical>) {
            const musical::MusicalLoopRange next{c.start, c.end};
            operation.payload = history::LoopRangeEdit{candidate.loopRange(), next};
        } else if constexpr (std::is_same_v<T, AddTempoChange>) {
            if (map.tempo.events.size() >= maximumEvents || map.tempo.nextId.value == UINT64_MAX) return Error::capacityExceeded;
            TempoEvent e{map.tempo.nextId, c.tick, c.bpm};
            operation.payload = history::TempoEdit{{}, e};
        } else if constexpr (std::is_same_v<T, AddTimeSignatureChange>) {
            if (map.signatures.events.size() >= maximumEvents || map.signatures.nextId.value == UINT64_MAX) return Error::capacityExceeded;
            TimeSignatureEvent e{map.signatures.nextId, c.bar, c.signature};
            operation.payload = history::SignatureEdit{{}, e};
        } else if constexpr (std::is_same_v<T, SetTempo> || std::is_same_v<T, MoveTempoChange> || std::is_same_v<T, RemoveTempoChange>) {
            auto i = std::find_if(map.tempo.events.begin(), map.tempo.events.end(), [&](auto& e) { return e.id == c.id; });
            if (i == map.tempo.events.end()) return Error::eventNotFound;
            if constexpr (!std::is_same_v<T, SetTempo>) if (i->tick.value == 0) return Error::protectedInitialEvent;
            std::optional<TempoEvent> next = *i;
            if constexpr (std::is_same_v<T, SetTempo>) next->bpm = c.bpm;
            else if constexpr (std::is_same_v<T, MoveTempoChange>) next->tick = c.tick;
            else next.reset();
            operation.payload = history::TempoEdit{*i, next};
        } else if constexpr (std::is_same_v<T, SetTimeSignature> || std::is_same_v<T, MoveTimeSignatureChange> || std::is_same_v<T, RemoveTimeSignatureChange>) {
            auto i = std::find_if(map.signatures.events.begin(), map.signatures.events.end(), [&](auto& e) { return e.id == c.id; });
            if (i == map.signatures.events.end()) return Error::eventNotFound;
            if constexpr (!std::is_same_v<T, SetTimeSignature>) if (i->bar.value == 0) return Error::protectedInitialEvent;
            std::optional<TimeSignatureEvent> next = *i;
            if constexpr (std::is_same_v<T, SetTimeSignature>) next->signature = c.signature;
            else if constexpr (std::is_same_v<T, MoveTimeSignatureChange>) next->bar = c.bar;
            else next.reset();
            operation.payload = history::SignatureEdit{*i, next};
        } else return Error::outOfRange;
        return Error::none;
    }, command);
    if (error != Error::none) return {CommandStatus::rejected, errorName(error), CommandError::validationFailed};
    if (!operation.apply(candidate, true)) return {CommandStatus::rejected, "Invalid musical edit", CommandError::validationFailed};
    if (musicalRevision_ == UINT64_MAX) return {CommandStatus::rejected, "Musical revision exhausted", CommandError::capacityExceeded};
    auto valid = PreparedMusicalTimeMap::compile(candidate.musicalTime(), candidate.sampleRate(), musicalRevision_ + 1);
    if (!valid) return {CommandStatus::rejected, errorName(valid.error), CommandError::validationFailed};
    auto pending = session_.history.stage(std::move(operation));
    if (!pending) return {CommandStatus::rejected, "History capacity exceeded", CommandError::historyCapacityExceeded};
    return commitMusicalProject(std::move(candidate), &*pending, 0, std::move(valid.value));
}
} // namespace vitadaw::application
