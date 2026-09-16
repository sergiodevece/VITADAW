#pragma once
#include "vitadaw/persistence/PersistenceResult.h"
#include "vitadaw/project/ProjectState.h"

namespace vitadaw::persistence {
inline constexpr std::size_t maximumDocumentBytes = 16 * 1024 * 1024;
inline constexpr std::size_t maximumDocumentMemory = 128 * 1024 * 1024;
struct ProjectDocument {
    static constexpr unsigned currentSchemaVersion = 3;
    unsigned schemaVersion{currentSchemaVersion};
    std::string writerAppVersion{"0.6.2"};
    project::ProjectState::DocumentData model;
};
struct DocumentResult {
    PersistenceResult result;
    std::unique_ptr<project::ProjectState> project;
};
struct SerializationResult {
    PersistenceResult result;
    std::string bytes;
};
[[nodiscard]] SerializationResult serializeProject(const project::ProjectState& project,
                                                   const std::filesystem::path& destination);
[[nodiscard]] DocumentResult deserializeProject(std::string_view bytes);
} // namespace vitadaw::persistence
