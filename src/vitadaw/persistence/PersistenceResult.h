#pragma once
#include <filesystem>
#include <string>
#include <system_error>

namespace vitadaw::persistence {
enum class PersistenceCode {
    success, savePathRequired, fileNotFound, permissionDenied, parseError,
    unsupportedSchema, schemaValidationFailed, semanticValidationFailed,
    missingMedia, mediaChanged, processorUnavailable, preparationFailed,
    atomicReplaceFailed, durabilityUncertain, fileTooLarge, capacityExceeded,
    unsavedChanges, transportMustBeStopped, ioError, unsupportedBackend
};
enum class PersistencePhase { none, read, parse, migrate, validate, media, prepare,
    tempCreate, write, flush, replace, durability, commit };
inline const char* codeName(PersistenceCode code) noexcept {
    constexpr const char* names[]{"success", "savePathRequired", "fileNotFound", "permissionDenied", "parseError",
        "unsupportedSchema", "schemaValidationFailed", "semanticValidationFailed", "missingMedia", "mediaChanged",
        "processorUnavailable", "preparationFailed", "atomicReplaceFailed", "durabilityUncertain", "fileTooLarge",
        "capacityExceeded", "unsavedChanges", "transportMustBeStopped", "ioError", "unsupportedBackend"};
    return names[static_cast<unsigned>(code)];
}
struct PersistenceResult {
    PersistenceResult(PersistenceCode c = PersistenceCode::success,
        PersistencePhase p = PersistencePhase::none, std::filesystem::path file = {},
        std::string pointer = {}, std::string kind = {}, std::string id = {},
        std::error_code system = {}) noexcept
        : code(c), phase(p), path(std::move(file)), jsonPointer(std::move(pointer)),
          entityKind(std::move(kind)), entityId(std::move(id)), systemError(system) {}
    PersistenceCode code{PersistenceCode::success};
    PersistencePhase phase{PersistencePhase::none};
    std::filesystem::path path;
    std::string jsonPointer;
    std::string entityKind;
    std::string entityId;
    std::error_code systemError;
    [[nodiscard]] bool success() const noexcept { return code == PersistenceCode::success; }
};
} // namespace vitadaw::persistence
