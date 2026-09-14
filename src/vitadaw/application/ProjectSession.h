#pragma once
#include "vitadaw/history/UndoManager.h"

namespace vitadaw::application {
struct ProjectSession {
    explicit ProjectSession(timeline::SampleRate rate) : project(rate) {}
    project::ProjectState project;
    history::UndoManager history;
    std::filesystem::path projectFilePath;
    history::StateToken savedStateToken{history.currentStateToken()};
    [[nodiscard]] bool dirty() const noexcept { return history.currentStateToken() != savedStateToken; }
    void adopt(project::ProjectState& candidate, std::filesystem::path& path) noexcept {
        project.swap(candidate);
        projectFilePath.swap(path);
        history.commitBarrier(); // New session identity, monotonic revision, no old Undo.
        savedStateToken = history.currentStateToken();
    }
};
} // namespace vitadaw::application
