#include "vitadaw/platform/files/RecoveryInventory.h"

#include "vitadaw/audio/RecordingRecovery.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdint>
#include <limits>
#include <map>
#include <string_view>
#include <system_error>
#include <utility>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX 1
#endif
#include <fcntl.h>
#include <io.h>
#include <sys/stat.h>
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace vitadaw::platform::files {
namespace {

constexpr std::uint64_t maximumWavHeaderProbeBytes = 64U * 1024U;

struct FileSnapshot {
    std::optional<audio::RecoveryFileIdentity> identity;
    std::uintmax_t sizeBytes{};
    std::filesystem::file_time_type modifiedAt{};
};

struct OpenedReadOnlyFile {
    int descriptor{-1};
    FileSnapshot snapshot;

    OpenedReadOnlyFile(int descriptorValue, FileSnapshot fileSnapshot) noexcept
        : descriptor(descriptorValue), snapshot(std::move(fileSnapshot)) {}

    ~OpenedReadOnlyFile() noexcept {
#if defined(_WIN32)
        if (descriptor >= 0) static_cast<void>(::_close(descriptor));
#else
        if (descriptor >= 0) static_cast<void>(::close(descriptor));
#endif
    }
    OpenedReadOnlyFile(const OpenedReadOnlyFile&) = delete;
    OpenedReadOnlyFile& operator=(const OpenedReadOnlyFile&) = delete;
    OpenedReadOnlyFile(OpenedReadOnlyFile&& other) noexcept
        : descriptor(std::exchange(other.descriptor, -1)), snapshot(other.snapshot) {}
    OpenedReadOnlyFile& operator=(OpenedReadOnlyFile&&) = delete;
};

struct WavCheck {
    audio::RecoveryWavValidation state{audio::RecoveryWavValidation::unreadable};
    std::string diagnostic;
};

struct MarkerCheck {
    std::optional<audio::RecordingRecoveryMarker> marker;
    std::string diagnostic;
};

[[nodiscard]] bool isWithinRoot(const std::filesystem::path& path,
                                const std::filesystem::path& root) noexcept {
    const auto relative = path.lexically_relative(root);
    if (relative.empty()) return path == root;
    for (const auto& component : relative)
        if (component == "..") return false;
    return true;
}

[[nodiscard]] std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return value;
}

[[nodiscard]] bool isWavPath(const std::filesystem::path& path) {
    return lower(path.extension().string()) == ".wav";
}

[[nodiscard]] std::optional<OpenedReadOnlyFile> openReadOnlyFile(
    const std::filesystem::path& path, std::string& diagnostic) noexcept {
    std::error_code statusError;
    const auto status = std::filesystem::symlink_status(path, statusError);
    if (statusError) {
        diagnostic = "could not inspect file: " + statusError.message();
        return std::nullopt;
    }
    if (std::filesystem::is_symlink(status)) {
        diagnostic = "symlink was not followed";
        return std::nullopt;
    }
    if (!std::filesystem::is_regular_file(status)) {
        diagnostic = "path is not a regular file";
        return std::nullopt;
    }

    int descriptor{-1};
#if defined(_WIN32)
    const auto handle = ::CreateFileW(path.wstring().c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        diagnostic = "could not open file read-only";
        return std::nullopt;
    }
    BY_HANDLE_FILE_INFORMATION information {};
    if (!::GetFileInformationByHandle(handle, &information) ||
        (information.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U) {
        static_cast<void>(::CloseHandle(handle));
        diagnostic = "symlink or reparse point was not followed";
        return std::nullopt;
    }
    descriptor = ::_open_osfhandle(reinterpret_cast<intptr_t>(handle), _O_RDONLY | _O_BINARY);
    if (descriptor < 0) {
        static_cast<void>(::CloseHandle(handle));
        diagnostic = "could not retain read-only file handle";
        return std::nullopt;
    }
    struct _stat64 state {};
    if (::_fstat64(descriptor, &state) != 0) {
        static_cast<void>(::_close(descriptor));
        diagnostic = "could not obtain file identity";
        return std::nullopt;
    }
#else
    descriptor = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (descriptor < 0) {
        diagnostic = errno == ELOOP ? "symlink was not followed" : "could not open file read-only";
        return std::nullopt;
    }
    struct stat state {};
    if (::fstat(descriptor, &state) != 0 || !S_ISREG(state.st_mode)) {
        static_cast<void>(::close(descriptor));
        diagnostic = "could not obtain file identity";
        return std::nullopt;
    }
#endif
    FileSnapshot result;
    const audio::RecoveryFileIdentity identity{
        static_cast<std::uint64_t>(state.st_dev), static_cast<std::uint64_t>(state.st_ino)};
    if (identity.valid()) result.identity = identity;
    else diagnostic = "stable file identity is unavailable on this platform";
    if (state.st_size < 0) {
#if defined(_WIN32)
        static_cast<void>(::_close(descriptor));
#else
        static_cast<void>(::close(descriptor));
#endif
        diagnostic = "file size is invalid";
        return std::nullopt;
    }
    result.sizeBytes = static_cast<std::uintmax_t>(state.st_size);
    std::error_code metadataError;
    result.modifiedAt = std::filesystem::last_write_time(path, metadataError);
    if (metadataError) result.modifiedAt = {};
    return OpenedReadOnlyFile{descriptor, result};
}

[[nodiscard]] std::uint32_t littleEndian32(const std::array<std::byte, 4>& bytes) noexcept {
    return static_cast<std::uint32_t>(std::to_integer<unsigned char>(bytes[0])) |
           (static_cast<std::uint32_t>(std::to_integer<unsigned char>(bytes[1])) << 8U) |
           (static_cast<std::uint32_t>(std::to_integer<unsigned char>(bytes[2])) << 16U) |
           (static_cast<std::uint32_t>(std::to_integer<unsigned char>(bytes[3])) << 24U);
}

[[nodiscard]] std::uint16_t littleEndian16(const std::byte* bytes) noexcept {
    return static_cast<std::uint16_t>(std::to_integer<unsigned char>(bytes[0])) |
           (static_cast<std::uint16_t>(std::to_integer<unsigned char>(bytes[1])) << 8U);
}

[[nodiscard]] bool readExact(int descriptor, std::uint64_t offset,
                             void* destination, std::size_t count) noexcept {
    auto* output = static_cast<std::byte*>(destination);
    std::size_t completed{};
    while (completed < count) {
        if (offset > std::numeric_limits<std::uint64_t>::max() - completed) return false;
#if defined(_WIN32)
        const auto position = offset + completed;
        if (position > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) ||
            ::_lseeki64(descriptor, static_cast<std::int64_t>(position), SEEK_SET) < 0)
            return false;
        const auto requested = static_cast<unsigned int>(std::min(
            count - completed, static_cast<std::size_t>(std::numeric_limits<unsigned int>::max())));
        const auto read = ::_read(descriptor, output + completed, requested);
#else
        const auto position = offset + completed;
        if (position > static_cast<std::uint64_t>(std::numeric_limits<off_t>::max())) return false;
        const auto read = ::pread(descriptor, output + completed, count - completed,
                                  static_cast<off_t>(position));
#endif
        if (read <= 0) return false;
        completed += static_cast<std::size_t>(read);
    }
    return true;
}

[[nodiscard]] const char* markerParseDiagnostic(
    audio::RecordingRecoveryMarkerParseStatus status) noexcept {
    switch (status) {
    case audio::RecordingRecoveryMarkerParseStatus::valid:
        return {};
    case audio::RecordingRecoveryMarkerParseStatus::invalidFormat:
        return "recording recovery marker is malformed or incomplete";
    case audio::RecordingRecoveryMarkerParseStatus::exceedsTotalLimit:
        return "recording recovery marker exceeds the bounded metadata size";
    case audio::RecordingRecoveryMarkerParseStatus::exceedsLineLimit:
        return "recording recovery marker contains an overlong metadata line";
    case audio::RecordingRecoveryMarkerParseStatus::exceedsFieldLimit:
        return "recording recovery marker contains too many metadata fields";
    }
    return "recording recovery marker is invalid";
}

// The descriptor binds both the inspected identity and the bytes parsed below.
// The enumerated pathname is never reopened to read marker content.
[[nodiscard]] MarkerCheck readMarkerNoFollow(const std::filesystem::path& path) {
    std::string diagnostic;
    const auto opened = openReadOnlyFile(path, diagnostic);
    if (!opened) return {{}, std::move(diagnostic)};
    if (opened->snapshot.sizeBytes > audio::maximumRecordingRecoveryMarkerBytes)
        return {{}, "recording recovery marker exceeds the bounded metadata size"};

    std::array<char, audio::maximumRecordingRecoveryMarkerBytes> bytes{};
    const auto count = static_cast<std::size_t>(opened->snapshot.sizeBytes);
    if (!readExact(opened->descriptor, 0, bytes.data(), count))
        return {{}, "could not read recording recovery marker through its opened descriptor"};
    const auto parsed = audio::parseRecordingRecoveryMarker(
        std::string_view{bytes.data(), count});
    if (!parsed.marker) return {{}, markerParseDiagnostic(parsed.status)};
    return {std::move(parsed.marker), {}};
}

[[nodiscard]] WavCheck validateWavHeader(int descriptor,
                                         std::uintmax_t sizeBytes) {
    if (sizeBytes < 12U) return {audio::RecoveryWavValidation::truncated,
                                 "WAV header is truncated"};
    std::array<std::byte, 12> riff{};
    if (!readExact(descriptor, 0, riff.data(), riff.size()))
        return {audio::RecoveryWavValidation::truncated, "could not read WAV header"};
    constexpr std::array<char, 4> riffMagic{'R', 'I', 'F', 'F'};
    constexpr std::array<char, 4> waveMagic{'W', 'A', 'V', 'E'};
    if (!std::equal(riffMagic.begin(), riffMagic.end(), reinterpret_cast<const char*>(riff.data())) ||
        !std::equal(waveMagic.begin(), waveMagic.end(), reinterpret_cast<const char*>(riff.data() + 8)))
        return {audio::RecoveryWavValidation::invalidHeader, "not a RIFF/WAVE file"};

    const auto declaredSize = littleEndian32({riff[4], riff[5], riff[6], riff[7]});
    const auto declaredEnd = static_cast<std::uint64_t>(declaredSize) + 8U;
    if (declaredEnd < 12U) return {audio::RecoveryWavValidation::invalidHeader,
                                   "RIFF size is invalid"};
    if (declaredEnd > sizeBytes) return {audio::RecoveryWavValidation::truncated,
                                         "RIFF size exceeds file size"};

    const auto scanLimit = std::min<std::uint64_t>(declaredEnd, maximumWavHeaderProbeBytes);
    std::uint64_t offset{12U};
    bool formatSeen{};
    while (offset + 8U <= scanLimit) {
        std::array<std::byte, 8> chunk{};
        if (!readExact(descriptor, offset, chunk.data(), chunk.size()))
            return {audio::RecoveryWavValidation::truncated, "WAV chunk header is truncated"};
        const auto chunkSize = littleEndian32({chunk[4], chunk[5], chunk[6], chunk[7]});
        const auto dataOffset = offset + 8U;
        const auto padding = static_cast<std::uint64_t>(chunkSize & 1U);
        if (chunkSize > std::numeric_limits<std::uint64_t>::max() - dataOffset - padding)
            return {audio::RecoveryWavValidation::invalidHeader, "WAV chunk size overflows"};
        const auto next = dataOffset + chunkSize + padding;
        if (next > declaredEnd || next > sizeBytes)
            return {audio::RecoveryWavValidation::truncated,
                    "WAV chunk size exceeds the available file"};

        const auto* id = reinterpret_cast<const char*>(chunk.data());
        if (std::equal(id, id + 4, "fmt ")) {
            if (chunkSize < 16U) return {audio::RecoveryWavValidation::invalidHeader,
                                          "WAV fmt chunk is too small"};
            std::array<std::byte, 16> format{};
            if (!readExact(descriptor, dataOffset, format.data(), format.size()))
                return {audio::RecoveryWavValidation::truncated, "WAV fmt chunk is truncated"};
            const auto formatCode = littleEndian16(format.data());
            const auto channels = littleEndian16(format.data() + 2);
            const auto sampleRate = littleEndian32({format[4], format[5], format[6], format[7]});
            const auto byteRate = littleEndian32({format[8], format[9], format[10], format[11]});
            const auto blockAlign = littleEndian16(format.data() + 12);
            const auto bits = littleEndian16(format.data() + 14);
            const auto expectedByteRate = static_cast<std::uint64_t>(sampleRate) *
                                          static_cast<std::uint64_t>(blockAlign);
            if ((formatCode != 1U && formatCode != 3U) || (channels != 1U && channels != 2U) ||
                sampleRate == 0U || bits == 0U || bits % 8U != 0U ||
                blockAlign != channels * (bits / 8U) ||
                expectedByteRate > std::numeric_limits<std::uint32_t>::max() ||
                byteRate != static_cast<std::uint32_t>(expectedByteRate))
                return {audio::RecoveryWavValidation::invalidHeader,
                        "WAV basic format fields are inconsistent"};
            formatSeen = true;
        } else if (std::equal(id, id + 4, "data")) {
            if (!formatSeen) return {audio::RecoveryWavValidation::invalidHeader,
                                      "WAV data chunk precedes fmt chunk"};
            return {audio::RecoveryWavValidation::structurallyValid, {}};
        }
        if (next > scanLimit)
            return {audio::RecoveryWavValidation::invalidHeader,
                    "WAV header exceeds bounded inventory probe"};
        offset = next;
    }
    return {audio::RecoveryWavValidation::invalidHeader,
            "WAV has no complete fmt/data header within the bounded probe"};
}

[[nodiscard]] std::optional<std::filesystem::path> exportFinalForTemporary(
    const std::filesystem::path& temporary) {
    const auto filename = temporary.filename().string();
    constexpr std::string_view suffix{".part.wav"};
    if (!filename.starts_with('.') || !filename.ends_with(suffix)) return std::nullopt;
    const auto core = filename.substr(1, filename.size() - 1 - suffix.size());
    constexpr std::string_view marker{".export."};
    const auto markerAt = core.rfind(marker);
    if (markerAt == std::string::npos || markerAt == 0U) return std::nullopt;
    const auto finalName = core.substr(0, markerAt);
    const auto nonce = std::string_view{core}.substr(markerAt + marker.size());
    if (nonce.empty() || !std::all_of(nonce.begin(), nonce.end(), [](unsigned char c) {
            return std::isdigit(c) != 0;
        }) || !isWavPath(finalName)) return std::nullopt;
    return temporary.parent_path() / finalName;
}

[[nodiscard]] audio::RecoveryCandidate candidateFor(
    const std::filesystem::path& path, const std::filesystem::path& root,
    audio::RecoveryOrigin origin, audio::RecoveryCandidateClass classification,
    std::string reason, std::optional<std::string> session = {}) {
    audio::RecoveryCandidate candidate;
    candidate.path = path;
    candidate.authorizedRoot = root;
    candidate.origin = origin;
    candidate.classification = classification;
    candidate.reason = std::move(reason);
    candidate.recordingSessionId = std::move(session);
    std::string diagnostic;
    auto opened = openReadOnlyFile(path, diagnostic);
    if (!opened) {
        candidate.classification = origin == audio::RecoveryOrigin::unknown
            ? audio::RecoveryCandidateClass::provenanceUnknown
            : audio::RecoveryCandidateClass::recognizedIncomplete;
        candidate.wavValidation = audio::RecoveryWavValidation::unreadable;
        candidate.diagnostic = std::move(diagnostic);
        return candidate;
    }
    candidate.identity = opened->snapshot.identity;
    candidate.sizeBytes = opened->snapshot.sizeBytes;
    candidate.modifiedAt = opened->snapshot.modifiedAt;
    const auto wav = validateWavHeader(opened->descriptor, opened->snapshot.sizeBytes);
    candidate.wavValidation = wav.state;
    candidate.diagnostic = wav.diagnostic;

    std::string afterDiagnostic;
    const auto after = openReadOnlyFile(path, afterDiagnostic);
    if (!after || opened->snapshot.identity != after->snapshot.identity ||
        opened->snapshot.sizeBytes != after->snapshot.sizeBytes) {
        candidate.classification = origin == audio::RecoveryOrigin::unknown
            ? audio::RecoveryCandidateClass::provenanceUnknown
            : audio::RecoveryCandidateClass::recognizedIncomplete;
        candidate.wavValidation = audio::RecoveryWavValidation::changedDuringScan;
        candidate.diagnostic = after ? "file changed while it was being inventoried"
                                     : "file could not be revalidated after inventory read: " + afterDiagnostic;
        return candidate;
    }
    if (wav.state != audio::RecoveryWavValidation::structurallyValid &&
        origin != audio::RecoveryOrigin::unknown)
        candidate.classification = audio::RecoveryCandidateClass::recognizedIncomplete;
    return candidate;
}

[[nodiscard]] int priority(audio::RecoveryCandidateClass value) noexcept {
    switch (value) {
    case audio::RecoveryCandidateClass::publishedTemporaryAlias: return 4;
    case audio::RecoveryCandidateClass::recognizedPotentiallyRecoverable: return 3;
    case audio::RecoveryCandidateClass::recognizedIncomplete: return 2;
    case audio::RecoveryCandidateClass::provenanceUnknown: return 1;
    }
    return 0;
}

void addCandidate(std::map<std::filesystem::path, audio::RecoveryCandidate>& candidates,
                  audio::RecoveryCandidate candidate) {
    const auto found = candidates.find(candidate.path);
    if (found == candidates.end() || priority(candidate.classification) >= priority(found->second.classification))
        candidates[candidate.path] = std::move(candidate);
}

} // namespace

bool matchesRecoveryCandidateSnapshot(const audio::RecoveryCandidate& candidate) noexcept {
    if (!candidate.identity || !candidate.identity->valid()) return false;
    std::string diagnostic;
    const auto current = openReadOnlyFile(candidate.path, diagnostic);
    return current && current->snapshot.identity && *current->snapshot.identity == *candidate.identity;
}

audio::RecoveryInventory scanRecoveryInventory(
    std::span<const std::filesystem::path> authorizedRoots) {
    audio::RecoveryInventory result;
    std::map<std::filesystem::path, audio::RecoveryCandidate> candidates;

    for (const auto& suppliedRoot : authorizedRoots) {
        std::error_code error;
        auto root = std::filesystem::absolute(suppliedRoot, error);
        if (error) {
            result.rootErrors.push_back({suppliedRoot, {}, "could not make root absolute: " + error.message()});
            continue;
        }
        root = root.lexically_normal();
        const auto rootStatus = std::filesystem::symlink_status(root, error);
        if (error || std::filesystem::is_symlink(rootStatus) || !std::filesystem::is_directory(rootStatus)) {
            result.rootErrors.push_back({root, {}, error ? "could not inspect root: " + error.message()
                                                           : "authorized root must be a non-symlink directory"});
            continue;
        }
        std::filesystem::directory_iterator rootProbe{root, std::filesystem::directory_options::none,
                                                       error};
        if (error) {
            result.rootErrors.push_back({root, {}, "could not enumerate root: " + error.message()});
            continue;
        }

        std::vector<std::filesystem::path> wavFiles;
        std::vector<std::filesystem::path> markerFiles;
        std::filesystem::recursive_directory_iterator it{
            root, std::filesystem::directory_options::skip_permission_denied, error};
        if (error) {
            result.rootErrors.push_back({root, {}, "could not enumerate root: " + error.message()});
            continue;
        }
        const std::filesystem::recursive_directory_iterator end;
        while (it != end) {
            const auto path = it->path().lexically_normal();
            std::error_code statusError;
            const auto status = it->symlink_status(statusError);
            if (statusError) {
                result.diagnostics.push_back({root, path, "could not inspect directory entry: " + statusError.message()});
            } else if (!isWithinRoot(path, root)) {
                result.diagnostics.push_back({root, path, "entry outside authorized root was ignored"});
                it.disable_recursion_pending();
            } else if (std::filesystem::is_symlink(status)) {
                result.diagnostics.push_back({root, path, "symlink was not followed"});
                it.disable_recursion_pending();
            } else if (std::filesystem::is_directory(status)) {
                std::filesystem::directory_iterator directoryProbe{
                    path, std::filesystem::directory_options::none, statusError};
                if (statusError) {
                    result.diagnostics.push_back(
                        {root, path, "could not enumerate subdirectory: " + statusError.message()});
                    it.disable_recursion_pending();
                }
            } else if (std::filesystem::is_regular_file(status)) {
                if (audio::isRecordingRecoveryMarkerPath(path)) markerFiles.push_back(path);
                else if (isWavPath(path)) wavFiles.push_back(path);
            }
            it.increment(error);
            if (error) {
                result.diagnostics.push_back({root, path, "directory iteration error: " + error.message()});
                error.clear();
            }
        }

        for (const auto& path : wavFiles) {
            addCandidate(candidates, candidateFor(path, root, audio::RecoveryOrigin::unknown,
                audio::RecoveryCandidateClass::provenanceUnknown,
                "WAV-like file has no demonstrable VitaDAW recovery provenance"));
        }
        for (const auto& markerPath : markerFiles) {
            const auto markerCheck = readMarkerNoFollow(markerPath);
            if (!markerCheck.marker) {
                result.diagnostics.push_back({root, markerPath, markerCheck.diagnostic});
                continue;
            }
            const auto& marker = markerCheck.marker;
            const auto directory = markerPath.parent_path();
            const auto temporary = marker->temporaryName.empty()
                ? std::filesystem::path{} : directory / marker->temporaryName;
            const auto published = marker->publishedName.empty()
                ? std::filesystem::path{} : directory / marker->publishedName;
            const auto selected = (marker->classification == audio::RecordingRecoveryClass::publishedFinal ||
                                   marker->classification == audio::RecordingRecoveryClass::closedUncommitted) &&
                                      !published.empty() ? published : temporary;
            if (selected.empty()) {
                result.diagnostics.push_back({root, markerPath,
                    "recording recovery marker names no media candidate"});
                continue;
            }
            addCandidate(candidates, candidateFor(selected, root, audio::RecoveryOrigin::recording,
                audio::RecoveryCandidateClass::recognizedPotentiallyRecoverable,
                "validated VitaDAW recording recovery marker", marker->sessionId));

            if (!temporary.empty() && !published.empty()) {
                auto finalCandidate = candidateFor(published, root, audio::RecoveryOrigin::recording,
                    audio::RecoveryCandidateClass::recognizedPotentiallyRecoverable,
                    "validated VitaDAW recording recovery marker", marker->sessionId);
                auto temporaryCandidate = candidateFor(temporary, root, audio::RecoveryOrigin::recording,
                    audio::RecoveryCandidateClass::recognizedPotentiallyRecoverable,
                    "validated VitaDAW recording recovery marker", marker->sessionId);
                if (finalCandidate.identity && temporaryCandidate.identity &&
                    *finalCandidate.identity == *temporaryCandidate.identity) {
                    temporaryCandidate.classification = audio::RecoveryCandidateClass::publishedTemporaryAlias;
                    temporaryCandidate.associatedPath = published;
                    temporaryCandidate.reason = "retained recording temporary aliases a published final";
                }
                addCandidate(candidates, std::move(finalCandidate));
                addCandidate(candidates, std::move(temporaryCandidate));
            }
        }

        for (const auto& temporary : wavFiles) {
            const auto final = exportFinalForTemporary(temporary);
            if (!final) continue;
            auto exportCandidate = candidateFor(temporary, root, audio::RecoveryOrigin::exportWav,
                audio::RecoveryCandidateClass::recognizedPotentiallyRecoverable,
                "strict VitaDAW retained export-temporary naming convention");
            std::string finalDiagnostic;
            const auto finalSnapshot = openReadOnlyFile(*final, finalDiagnostic);
            if (finalSnapshot && exportCandidate.identity && finalSnapshot->snapshot.identity &&
                *exportCandidate.identity == *finalSnapshot->snapshot.identity) {
                exportCandidate.classification = audio::RecoveryCandidateClass::publishedTemporaryAlias;
                exportCandidate.associatedPath = *final;
                exportCandidate.reason = "retained export temporary aliases a published final";
                addCandidate(candidates, candidateFor(*final, root, audio::RecoveryOrigin::exportWav,
                    audio::RecoveryCandidateClass::recognizedPotentiallyRecoverable,
                    "published final paired with a retained VitaDAW export temporary"));
            } else if (finalSnapshot) {
                exportCandidate.classification = audio::RecoveryCandidateClass::recognizedIncomplete;
                exportCandidate.diagnostic = "expected export final exists but has a different identity";
            }
            addCandidate(candidates, std::move(exportCandidate));
        }
    }
    result.candidates.reserve(candidates.size());
    for (auto& [_, candidate] : candidates) result.candidates.push_back(std::move(candidate));
    return result;
}

} // namespace vitadaw::platform::files
