#include "vitadaw/audio/RecordingRecovery.h"

#include <atomic>
#include <algorithm>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <limits>
#include <map>
#include <random>
#include <sstream>
#include <cerrno>
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX 1
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace vitadaw::audio {
namespace {
constexpr std::string_view prefix{".vitadaw-recording-"};
constexpr std::string_view suffix{".recovery"};

const char* name(RecordingRecoveryClass value) noexcept {
    switch (value) {
    case RecordingRecoveryClass::temporary: return "temporary";
    case RecordingRecoveryClass::closedUncommitted: return "closed";
    case RecordingRecoveryClass::publishedFinal: return "published";
    case RecordingRecoveryClass::incomplete: return "incomplete";
    case RecordingRecoveryClass::ambiguous: return "ambiguous";
    }
    return "ambiguous";
}

std::optional<RecordingRecoveryClass> parseClass(std::string_view value) noexcept {
    if (value == "temporary") return RecordingRecoveryClass::temporary;
    if (value == "closed") return RecordingRecoveryClass::closedUncommitted;
    if (value == "published") return RecordingRecoveryClass::publishedFinal;
    if (value == "incomplete") return RecordingRecoveryClass::incomplete;
    if (value == "ambiguous") return RecordingRecoveryClass::ambiguous;
    return std::nullopt;
}

std::optional<RecordingRecoveryMarker> parse(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    std::string line;
    RecordingRecoveryMarker marker;
    bool version{}, session{}, classification{};
    while (std::getline(input, line)) {
        const auto equal = line.find('=');
        if (equal == std::string::npos) return std::nullopt;
        const auto key = std::string_view{line}.substr(0, equal);
        const auto value = std::string_view{line}.substr(equal + 1);
        if (key == "version") version = value == "1";
        else if (key == "session") { marker.sessionId = value; session = !value.empty(); }
        else if (key == "class") {
            const auto parsed = parseClass(value);
            if (!parsed) return std::nullopt;
            marker.classification = *parsed;
            classification = true;
        } else if (key == "temporary") marker.temporaryName = value;
        else if (key == "published") marker.publishedName = value;
        else if (key == "channels") {
            if (value == "1") marker.layout = media::AudioChannelLayout::mono;
            else if (value == "2") marker.layout = media::AudioChannelLayout::stereo;
            else return std::nullopt;
        } else if (key == "sampleRate") {
            try { marker.deviceSampleRate = timeline::SampleRate{std::stod(std::string{value})}; }
            catch (...) { return std::nullopt; }
        } else if (key == "frames") {
            try { marker.acceptedFrames = {std::stoull(std::string{value})}; }
            catch (...) { return std::nullopt; }
        } else if (key == "sha256") {
            if (value.size() != 64) return std::nullopt;
            marker.fingerprint = media::MediaFingerprint{std::string{value}, 0};
        } else if (key == "bytes") {
            if (!marker.fingerprint) return std::nullopt;
            try { marker.fingerprint->fileSizeBytes = std::stoull(std::string{value}); }
            catch (...) { return std::nullopt; }
        } else return std::nullopt;
    }
    if (!input.eof() || !version || !session || !classification ||
        (!marker.temporaryName.empty() && marker.temporaryName.has_parent_path()) ||
        (!marker.publishedName.empty() && marker.publishedName.has_parent_path())) return std::nullopt;
    return marker;
}
}

std::string makeRecordingRecoverySessionId() {
    static std::atomic<std::uint64_t> sequence{};
    const auto now = static_cast<std::uint64_t>(std::chrono::high_resolution_clock::now()
                                                    .time_since_epoch().count());
    std::random_device random;
    const auto entropy = (static_cast<std::uint64_t>(random()) << 32U) ^ random();
    std::ostringstream result;
    result << std::hex << now << '-' << entropy << '-' << ++sequence;
    return result.str();
}

bool writeRecordingRecoveryMarker(const std::filesystem::path& directory,
                                  const RecordingRecoveryMarker& marker,
                                  std::string& errorMessage,
                                  RecordingRecoveryMarkerWriteOptions options) {
    if (marker.sessionId.empty() || !marker.deviceSampleRate.isValid()) {
        errorMessage = "Invalid recording recovery marker";
        return false;
    }
    const auto markerPath = directory / (std::string{prefix} + marker.sessionId + "-" +
                                         name(marker.classification) + std::string{suffix});
    const auto failure = [&](const char* operation, std::error_code system) {
        if (!system) system = {EIO, std::generic_category()};
        errorMessage = std::string{operation} + " recovery marker '" + markerPath.string() +
            "' failed (" + std::to_string(system.value()) + "): " + system.message();
        return false;
    };
    std::error_code existsError;
    if (std::filesystem::exists(markerPath, existsError)) {
#if defined(_WIN32)
        return failure("create", {ERROR_FILE_EXISTS, std::system_category()});
#else
        return failure("create", {EEXIST, std::generic_category()});
#endif
    }
    if (existsError) return failure("inspect", existsError);
    std::ostringstream output;
    output << "version=1\n"
           << "session=" << marker.sessionId << '\n'
           << "class=" << name(marker.classification) << '\n'
           << "temporary=" << marker.temporaryName.string() << '\n'
           << "published=" << marker.publishedName.string() << '\n'
           << "channels=" << media::channelCount(marker.layout) << '\n'
           << "sampleRate=" << std::setprecision(17) << marker.deviceSampleRate.hertz() << '\n'
           << "frames=" << marker.acceptedFrames.value << '\n';
    if (marker.fingerprint) {
        output << "sha256=" << marker.fingerprint->sha256 << '\n'
               << "bytes=" << marker.fingerprint->fileSizeBytes << '\n';
    }
#if !defined(_WIN32)
    const auto bytes = output.str();
    if (options.injectedFault == RecordingRecoveryMarkerFault::create)
        return failure("create", {EACCES, std::generic_category()});
    const auto descriptor = ::open(markerPath.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (descriptor < 0) return failure("create", {errno, std::generic_category()});
    std::size_t offset{};
    while (offset < bytes.size()) {
        if (options.injectedFault == RecordingRecoveryMarkerFault::write) {
            static_cast<void>(::close(descriptor));
            return failure("write", {ENOSPC, std::generic_category()});
        }
        const auto count = ::write(descriptor, bytes.data() + offset, bytes.size() - offset);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) {
            const auto code = errno == 0 ? EIO : errno;
            static_cast<void>(::close(descriptor));
            return failure("write", {code, std::generic_category()});
        }
        offset += static_cast<std::size_t>(count);
    }
    const auto syncError = options.injectedFault == RecordingRecoveryMarkerFault::sync
                               ? EIO : (::fsync(descriptor) == 0 ? 0 : errno);
    const auto closeError = ::close(descriptor) == 0 ? 0 : errno;
    if (syncError != 0) return failure("fsync", {syncError, std::generic_category()});
    if (options.injectedFault == RecordingRecoveryMarkerFault::close)
        return failure("close", {EIO, std::generic_category()});
    if (closeError != 0) return failure("close", {closeError, std::generic_category()});
    return true;
#else
    const auto bytes = output.str();
    if (options.injectedFault == RecordingRecoveryMarkerFault::create)
        return failure("create", {ERROR_ACCESS_DENIED, std::system_category()});
    const auto handle = ::CreateFileW(markerPath.wstring().c_str(), GENERIC_WRITE, 0, nullptr,
                                      CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE)
        return failure("create", {static_cast<int>(::GetLastError()), std::system_category()});
    const auto close = [&] {
        if (::CloseHandle(handle)) return DWORD{};
        return ::GetLastError();
    };
    std::size_t offset{};
    while (offset < bytes.size()) {
        if (options.injectedFault == RecordingRecoveryMarkerFault::write) {
            static_cast<void>(close());
            return failure("write", {ERROR_DISK_FULL, std::system_category()});
        }
        const auto requested = static_cast<DWORD>(std::min(
            bytes.size() - offset, static_cast<std::size_t>(std::numeric_limits<DWORD>::max())));
        DWORD written{};
        if (!::WriteFile(handle, bytes.data() + offset, requested, &written, nullptr)) {
            const auto code = ::GetLastError();
            static_cast<void>(close());
            return failure("write", {static_cast<int>(code), std::system_category()});
        }
        if (written == 0) {
            static_cast<void>(close());
            return failure("write", {ERROR_WRITE_FAULT, std::system_category()});
        }
        offset += written;
    }
    const auto syncError = options.injectedFault == RecordingRecoveryMarkerFault::sync
                               ? ERROR_WRITE_FAULT
                               : (::FlushFileBuffers(handle) ? ERROR_SUCCESS : ::GetLastError());
    const auto closeError = close();
    if (syncError != ERROR_SUCCESS)
        return failure("fsync", {static_cast<int>(syncError), std::system_category()});
    if (options.injectedFault == RecordingRecoveryMarkerFault::close)
        return failure("close", {ERROR_INVALID_HANDLE, std::system_category()});
    if (closeError != ERROR_SUCCESS)
        return failure("close", {static_cast<int>(closeError), std::system_category()});
    return true;
#endif
}

std::vector<RecordingRecoveryArtifact> scanRecordingRecoveryMarkers(
    const std::filesystem::path& directory,
    const std::function<bool(const std::filesystem::path&)>& readableWav) {
    std::map<std::string, RecordingRecoveryArtifact> unique;
    std::error_code error;
    for (std::filesystem::directory_iterator it{directory, error}, end; !error && it != end;
         it.increment(error)) {
        const auto filename = it->path().filename().string();
        if (!filename.starts_with(prefix) || !filename.ends_with(suffix)) continue;
        const auto marker = parse(it->path());
        if (!marker) continue;
        const auto selected = (marker->classification == RecordingRecoveryClass::publishedFinal ||
                               marker->classification == RecordingRecoveryClass::closedUncommitted) &&
                                      !marker->publishedName.empty()
                                  ? marker->publishedName : marker->temporaryName;
        if (selected.empty()) continue;
        const auto media = directory / selected;
        const bool valid = readableWav && readableWav(media);
        auto classification = marker->classification;
        if (!valid && classification == RecordingRecoveryClass::temporary)
            classification = RecordingRecoveryClass::incomplete;
        RecordingRecoveryArtifact candidate{marker->sessionId, media, classification,
            valid && classification != RecordingRecoveryClass::incomplete &&
                classification != RecordingRecoveryClass::ambiguous};
        const auto rank = [](RecordingRecoveryClass value) noexcept {
            switch (value) {
            case RecordingRecoveryClass::publishedFinal: return 4;
            case RecordingRecoveryClass::closedUncommitted: return 3;
            case RecordingRecoveryClass::incomplete: return 2;
            case RecordingRecoveryClass::temporary: return 1;
            case RecordingRecoveryClass::ambiguous: return 0;
            }
            return 0;
        };
        const auto found = unique.find(candidate.persistentSessionId);
        if (found == unique.end() || rank(candidate.classification) > rank(found->second.classification))
            unique[candidate.persistentSessionId] = std::move(candidate);
    }
    std::vector<RecordingRecoveryArtifact> result;
    result.reserve(unique.size());
    for (auto& [_, candidate] : unique) result.push_back(std::move(candidate));
    return result;
}

} // namespace vitadaw::audio
