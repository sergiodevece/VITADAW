#pragma once

#include "vitadaw/mixer/Metering.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <span>

namespace vitadaw::audio {

// Single RT writer, non-blocking readers. Every field is atomic so a failed
// optimistic read is still data-race-free under C++20. seq_cst plus equal even
// revisions prevents accepting fields from different publications.
class RealtimeMeterExchange {
public:
    void publish(std::span<const tracks::TrackId> trackIds,
                 std::span<const mixer::StereoPeak> trackPeaks,
                 mixer::StereoPeak master) noexcept {
        const auto count = std::min(
            {trackIds.size(), trackPeaks.size(),
             mixer::maximumMeteredTracks});
        const auto startingRevision = revision_.load();
        revision_.store(startingRevision + 1);
        trackCount_.store(count);
        for (std::size_t index = 0; index < count; ++index) {
            tracks_[index].id.store(trackIds[index].value);
            tracks_[index].left.store(encode(trackPeaks[index].left));
            tracks_[index].right.store(encode(trackPeaks[index].right));
        }
        masterLeft_.store(encode(master.left));
        masterRight_.store(encode(master.right));
        revision_.store(startingRevision + 2);
    }

    [[nodiscard]] mixer::MeterSnapshot snapshot() const noexcept {
        for (int attempt = 0; attempt < 3; ++attempt) {
            const auto before = revision_.load();
            if ((before & 1U) != 0U) {
                continue;
            }
            mixer::MeterSnapshot candidate;
            candidate.trackCount = std::min(
                trackCount_.load(), mixer::maximumMeteredTracks);
            for (std::size_t index = 0; index < candidate.trackCount; ++index) {
                candidate.tracks[index].track = {tracks_[index].id.load()};
                candidate.tracks[index].peak.left =
                    decode(tracks_[index].left.load());
                candidate.tracks[index].peak.right =
                    decode(tracks_[index].right.load());
            }
            candidate.master.left = decode(masterLeft_.load());
            candidate.master.right = decode(masterRight_.load());
            const auto after = revision_.load();
            if (before == after && (after & 1U) == 0U) {
                return candidate;
            }
        }
        return {};
    }

private:
    struct AtomicTrackPeak {
        std::atomic<std::uint64_t> id{};
        std::atomic<std::uint32_t> left{};
        std::atomic<std::uint32_t> right{};
    };

    static_assert(std::atomic<std::uint32_t>::is_always_lock_free);
    static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
    static_assert(std::atomic<std::size_t>::is_always_lock_free);

    [[nodiscard]] static std::uint32_t encode(float value) noexcept {
        return std::bit_cast<std::uint32_t>(value);
    }
    [[nodiscard]] static float decode(std::uint32_t value) noexcept {
        return std::bit_cast<float>(value);
    }

    std::atomic<std::uint64_t> revision_{};
    std::atomic<std::size_t> trackCount_{};
    std::array<AtomicTrackPeak, mixer::maximumMeteredTracks> tracks_{};
    std::atomic<std::uint32_t> masterLeft_{};
    std::atomic<std::uint32_t> masterRight_{};
};

} // namespace vitadaw::audio
