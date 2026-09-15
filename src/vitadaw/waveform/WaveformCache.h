#pragma once

#include "vitadaw/media/AudioSource.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace vitadaw::waveform {

inline constexpr std::uint64_t baseBucketFrames = 128;
inline constexpr std::size_t defaultCacheBudgetBytes = 64U * 1024U * 1024U;

struct PeakPair {
    float minimum{};
    float maximum{};
    bool operator==(const PeakPair&) const = default;
};

struct WaveformLevel {
    std::uint64_t framesPerBucket{};
    std::array<std::vector<PeakPair>, 2> channels;
};

struct PreparedWaveformData {
    timeline::SampleRate sourceSampleRate;
    timeline::SourceFrameCount sourceFrameCount;
    std::uint32_t channelCount{};
    std::vector<WaveformLevel> levels;
    std::size_t approximateBytes{};
};

struct PcmView {
    std::array<const float*, 2> channels{};
    std::uint32_t channelCount{};
    timeline::SourceFrameCount frameCount;
    timeline::SampleRate sampleRate;
};

struct PreparationResult {
    std::shared_ptr<const PreparedWaveformData> prepared;
    std::string diagnostic;
    [[nodiscard]] bool success() const noexcept { return prepared != nullptr; }
};

[[nodiscard]] PreparationResult prepareWaveform(
    PcmView, std::size_t budgetBytes = defaultCacheBudgetBytes);

class WaveformCache {
public:
    enum class StoreResult { stored, invalid, budgetExceeded };
    enum class State { missing, ready, error };

    explicit WaveformCache(std::size_t budgetBytes = defaultCacheBudgetBytes) noexcept
        : budgetBytes_(budgetBytes) {}

    [[nodiscard]] StoreResult store(
        media::SourceId, std::shared_ptr<const PreparedWaveformData>);
    [[nodiscard]] std::shared_ptr<const PreparedWaveformData> find(
        media::SourceId) const noexcept;
    void markError(media::SourceId, std::string diagnostic);
    [[nodiscard]] State state(media::SourceId) const noexcept;
    [[nodiscard]] std::string_view diagnostic(media::SourceId) const noexcept;
    [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }
    [[nodiscard]] std::size_t approximateBytes() const noexcept { return usedBytes_; }
    [[nodiscard]] std::size_t budgetBytes() const noexcept { return budgetBytes_; }
    [[nodiscard]] std::uint64_t revision() const noexcept { return revision_; }
    void swap(WaveformCache&) noexcept;

private:
    struct Entry {
        std::shared_ptr<const PreparedWaveformData> prepared;
        std::string diagnostic;
    };
    std::map<std::uint64_t, Entry> entries_;
    std::size_t budgetBytes_{};
    std::size_t usedBytes_{};
    std::uint64_t revision_{};
};

} // namespace vitadaw::waveform
