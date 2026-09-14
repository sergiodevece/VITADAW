#include "vitadaw/platform/files/ProjectFileIO.h"
#include <array>
#include <cerrno>
#include <limits>
#include <stdexcept>
#if defined(__APPLE__) || defined(__linux__)
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#endif
#ifdef __APPLE__
#include <CommonCrypto/CommonDigest.h>
#endif

namespace vitadaw::platform::files {
using namespace persistence;
namespace {
PersistenceResult error(PersistenceCode code, PersistencePhase phase, int number = 0) {
    return {code, phase, {}, {}, {}, {}, std::error_code{number, std::generic_category()}};
}
#if defined(__APPLE__) || defined(__linux__)
struct Descriptor {
    int value{-1};
    ~Descriptor() { if (value >= 0) ::close(value); }
};
FileReadIdentity identity(const struct stat& s) noexcept {
#ifdef __APPLE__
    return {static_cast<std::uint64_t>(s.st_dev), s.st_ino, s.st_mtimespec.tv_sec, s.st_mtimespec.tv_nsec,
            s.st_ctimespec.tv_sec, s.st_ctimespec.tv_nsec};
#else
    return {static_cast<std::uint64_t>(s.st_dev), s.st_ino, s.st_mtim.tv_sec, s.st_mtim.tv_nsec,
            s.st_ctim.tv_sec, s.st_ctim.tv_nsec};
#endif
}
#endif
}
ReadResult NativeProjectFileIO::read(const std::filesystem::path& path, std::size_t limit) {
#if defined(__APPLE__) || defined(__linux__)
    Descriptor fd{::open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC)};
    if (fd.value < 0) return {error(errno == ENOENT ? PersistenceCode::fileNotFound :
        errno == EACCES ? PersistenceCode::permissionDenied : PersistenceCode::ioError, PersistencePhase::read, errno), {}};
    struct stat before{}, after{};
    if (::fstat(fd.value, &before) || !S_ISREG(before.st_mode))
        return {error(PersistenceCode::ioError, PersistencePhase::read, errno), {}};
    if (before.st_size < 0 || static_cast<std::uintmax_t>(before.st_size) > limit)
        return {error(PersistenceCode::fileTooLarge, PersistencePhase::read), {}};
    ReadResult result;
    result.identity = identity(before);
    result.bytes.reserve(static_cast<std::size_t>(before.st_size));
    std::array<std::byte, 65536> chunk;
    for (;;) {
        const auto count = ::read(fd.value, chunk.data(), chunk.size());
        if (count < 0 && errno == EINTR) continue;
        if (count < 0) return {error(PersistenceCode::ioError, PersistencePhase::read, errno), {}};
        if (!count) break;
        if (static_cast<std::size_t>(count) > limit - result.bytes.size())
            return {error(PersistenceCode::fileTooLarge, PersistencePhase::read), {}};
        // Do not grow capacity on a concurrently-growing file. A read is an
        // immutable-size snapshot, bounded by both the observed size and limit.
        if (static_cast<std::size_t>(count) > static_cast<std::size_t>(before.st_size) - result.bytes.size())
            return {error(PersistenceCode::mediaChanged, PersistencePhase::read), {}};
        result.bytes.insert(result.bytes.end(), chunk.begin(), chunk.begin() + count);
    }
    if (::fstat(fd.value, &after)) return {error(PersistenceCode::ioError, PersistencePhase::read, errno), {}};
#ifdef __APPLE__
    const bool changed = before.st_mtimespec.tv_sec != after.st_mtimespec.tv_sec ||
        before.st_mtimespec.tv_nsec != after.st_mtimespec.tv_nsec ||
        before.st_ctimespec.tv_sec != after.st_ctimespec.tv_sec || before.st_ctimespec.tv_nsec != after.st_ctimespec.tv_nsec;
#else
    const bool changed = before.st_mtim.tv_sec != after.st_mtim.tv_sec || before.st_mtim.tv_nsec != after.st_mtim.tv_nsec ||
        before.st_ctim.tv_sec != after.st_ctim.tv_sec || before.st_ctim.tv_nsec != after.st_ctim.tv_nsec;
#endif
    if (changed || before.st_size != after.st_size || static_cast<std::uintmax_t>(after.st_size) != result.bytes.size())
        return {error(PersistenceCode::mediaChanged, PersistencePhase::read), {}};
    return result;
#else
    return {error(PersistenceCode::unsupportedBackend, PersistencePhase::read), {}};
#endif
}
PersistenceResult NativeProjectFileIO::replace(const std::filesystem::path& path, std::string_view bytes) {
#ifdef __APPLE__
    // Prepare every string before rename. Afterwards only syscalls and trivial results.
    auto pattern = (path.parent_path() / ".vitadaw-save-XXXXXX").string();
    std::vector<char> name(pattern.begin(), pattern.end()); name.push_back('\0');
    if (fault_ == PersistencePhase::tempCreate) return error(PersistenceCode::ioError, fault_);
    Descriptor directory{::open(path.parent_path().c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC)};
    if (directory.value < 0) return error(PersistenceCode::permissionDenied, PersistencePhase::tempCreate, errno);
    Descriptor temp{::mkstemp(name.data())};
    if (temp.value < 0) return error(errno == EACCES ? PersistenceCode::permissionDenied : PersistenceCode::ioError,
                                    PersistencePhase::tempCreate, errno);
    struct Cleanup { const char* name; bool active{true}; ~Cleanup() { if (active) ::unlink(name); } } cleanup{name.data()};
    std::size_t offset{};
    while (offset < bytes.size()) {
        if (fault_ == PersistencePhase::write) return error(PersistenceCode::ioError, fault_);
        const auto count = ::write(temp.value, bytes.data() + offset, bytes.size() - offset);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) return error(PersistenceCode::ioError, PersistencePhase::write, errno);
        offset += static_cast<std::size_t>(count);
    }
    if (fault_ == PersistencePhase::flush || ::fsync(temp.value) || ::fcntl(temp.value, F_FULLFSYNC))
        return error(PersistenceCode::ioError, PersistencePhase::flush, errno);
    if (fault_ == PersistencePhase::replace || ::rename(name.data(), path.c_str()))
        return error(PersistenceCode::atomicReplaceFailed, PersistencePhase::replace, errno);
    cleanup.active = false;
    if (fault_ == PersistencePhase::durability || ::fsync(directory.value))
        return error(PersistenceCode::durabilityUncertain, PersistencePhase::durability, errno);
    return {};
#else
    (void)path; (void)bytes;
    return error(PersistenceCode::unsupportedBackend, PersistencePhase::replace);
#endif
}
IProjectFileIO& nativeProjectFileIO() { static NativeProjectFileIO value; return value; }
PersistenceResult verifyFingerprint(const std::filesystem::path& path, const media::MediaFingerprint& expected,
    const FileReadIdentity* originalRead) {
#ifdef __APPLE__
    Descriptor fd{::open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC)};
    if (fd.value < 0) return error(PersistenceCode::missingMedia, PersistencePhase::media, errno);
    struct stat before{}, after{}, named{};
    if (::fstat(fd.value, &before) || !S_ISREG(before.st_mode) || before.st_size < 0 ||
        static_cast<std::uint64_t>(before.st_size) != expected.fileSizeBytes ||
        (originalRead && identity(before) != *originalRead))
        return error(PersistenceCode::mediaChanged, PersistencePhase::media);
    CC_SHA256_CTX hash;
    CC_SHA256_Init(&hash);
    std::array<std::byte, 65536> bytes;
    std::uint64_t count{};
    for (;;) {
        const auto n = ::read(fd.value, bytes.data(), bytes.size());
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) return error(PersistenceCode::ioError, PersistencePhase::media, errno);
        if (!n) break;
        if (static_cast<std::uint64_t>(n) > expected.fileSizeBytes - count)
            return error(PersistenceCode::mediaChanged, PersistencePhase::media);
        count += static_cast<std::uint64_t>(n);
        CC_SHA256_Update(&hash, bytes.data(), static_cast<CC_LONG>(n));
    }
    if (::fstat(fd.value, &after) || ::stat(path.c_str(), &named) || count != expected.fileSizeBytes ||
        before.st_mtimespec.tv_sec != after.st_mtimespec.tv_sec || before.st_mtimespec.tv_nsec != after.st_mtimespec.tv_nsec ||
        before.st_ctimespec.tv_sec != after.st_ctimespec.tv_sec || before.st_ctimespec.tv_nsec != after.st_ctimespec.tv_nsec ||
        after.st_ino != named.st_ino || after.st_dev != named.st_dev)
        return error(PersistenceCode::mediaChanged, PersistencePhase::media);
    std::array<unsigned char, CC_SHA256_DIGEST_LENGTH> digest;
    CC_SHA256_Final(digest.data(), &hash);
    constexpr char hex[] = "0123456789abcdef";
    if (expected.sha256.size() != 64) return error(PersistenceCode::mediaChanged, PersistencePhase::media);
    for (std::size_t i = 0; i < digest.size(); ++i)
        if (expected.sha256[i * 2] != hex[digest[i] >> 4] || expected.sha256[i * 2 + 1] != hex[digest[i] & 15])
            return error(PersistenceCode::mediaChanged, PersistencePhase::media);
    return {};
#else
    (void)path; (void)expected; (void)originalRead;
    return error(PersistenceCode::unsupportedBackend, PersistencePhase::media);
#endif
}
media::MediaFingerprint fingerprint(std::span<const std::byte> bytes) {
#ifdef __APPLE__
    if (bytes.size() > std::numeric_limits<CC_LONG>::max()) throw std::length_error("hash input");
    std::array<unsigned char, CC_SHA256_DIGEST_LENGTH> digest{};
    CC_SHA256(bytes.data(), static_cast<CC_LONG>(bytes.size()), digest.data());
    constexpr char hex[] = "0123456789abcdef";
    std::string value; value.reserve(64);
    for (auto byte : digest) { value.push_back(hex[byte >> 4]); value.push_back(hex[byte & 15]); }
    return {std::move(value), bytes.size()};
#else
    (void)bytes;
    throw std::runtime_error("SHA-256 backend unavailable");
#endif
}
} // namespace vitadaw::platform::files
