#pragma once

#include "vitadaw/timeline/Time.h"

#include <compare>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

namespace vitadaw::media {

struct SourceId {
    std::uint64_t value{};

    [[nodiscard]] constexpr bool isValid() const noexcept { return value != 0; }
    auto operator<=>(const SourceId&) const = default;
};

enum class AudioChannelLayout : std::uint8_t { mono = 1, stereo = 2 };

[[nodiscard]] constexpr std::uint32_t channelCount(
    AudioChannelLayout layout) noexcept {
    return static_cast<std::uint32_t>(layout);
}

struct MediaFingerprint {
    std::string sha256;
    std::uint64_t fileSizeBytes{};
    bool operator==(const MediaFingerprint&) const = default;
};

struct MediaReference {
    std::filesystem::path originalPath;
    std::optional<std::filesystem::path> projectRelativePath;
    std::optional<MediaFingerprint> fingerprint;

    [[nodiscard]] bool isValid() const noexcept {
        return !originalPath.empty() ||
               (projectRelativePath.has_value() &&
                !projectRelativePath->empty());
    }

    bool operator==(const MediaReference&) const = default;
};

// Portable project metadata. Decoded PCM is owned by the platform/preparation
// layer and is deliberately absent from this object.
struct AudioSource {
    SourceId id;
    MediaReference media;
    timeline::SourceFrameCount frameCount;
    timeline::SampleRate sampleRate;
    AudioChannelLayout layout{AudioChannelLayout::mono};

    [[nodiscard]] bool isValid() const noexcept {
        return id.isValid() && media.isValid() && frameCount.value > 0 &&
               sampleRate.isValid();
    }
    bool operator==(const AudioSource&) const = default;
};

} // namespace vitadaw::media
