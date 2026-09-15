#include "vitadaw/waveform/WaveformCache.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <new>

namespace vitadaw::waveform {
namespace {
bool addWouldOverflow(std::size_t a, std::size_t b) noexcept {
    return b > std::numeric_limits<std::size_t>::max() - a;
}
}

PreparationResult prepareWaveform(PcmView pcm, std::size_t budgetBytes) {
    try {
        if (!pcm.sampleRate.isValid() || pcm.frameCount.value == 0 ||
            pcm.channelCount == 0 || pcm.channelCount > 2) {
            return {{}, "Invalid waveform source shape"};
        }
        for (std::uint32_t channel = 0; channel < pcm.channelCount; ++channel)
            if (pcm.channels[channel] == nullptr)
                return {{}, "Missing waveform PCM channel"};

        const auto baseCount = 1U + (pcm.frameCount.value - 1U) /
                                     baseBucketFrames;
        // All levels together contain fewer than twice the base level.
        if (baseCount > std::numeric_limits<std::size_t>::max() /
                            (2U * pcm.channelCount * sizeof(PeakPair)))
            return {{}, "Waveform size overflows the platform size type"};
        const auto upperEstimate = static_cast<std::size_t>(baseCount) * 2U *
                                   pcm.channelCount * sizeof(PeakPair);
        if (upperEstimate > budgetBytes)
            return {{}, "Waveform exceeds the configured cache budget"};

        auto result = std::make_shared<PreparedWaveformData>();
        result->sourceSampleRate = pcm.sampleRate;
        result->sourceFrameCount = pcm.frameCount;
        result->channelCount = pcm.channelCount;
        result->levels.emplace_back();
        auto& base = result->levels.back();
        base.framesPerBucket = baseBucketFrames;
        for (std::uint32_t channel = 0; channel < pcm.channelCount; ++channel) {
            auto& peaks = base.channels[channel];
            peaks.reserve(static_cast<std::size_t>(baseCount));
            for (std::uint64_t first = 0; first < pcm.frameCount.value;
                 first += baseBucketFrames) {
                const auto end = std::min(pcm.frameCount.value,
                                          first + baseBucketFrames);
                auto minimum = std::numeric_limits<float>::infinity();
                auto maximum = -std::numeric_limits<float>::infinity();
                for (auto frame = first; frame < end; ++frame) {
                    const auto sample = pcm.channels[channel][frame];
                    if (!std::isfinite(sample))
                        return {{}, "Waveform source contains a non-finite sample"};
                    minimum = std::min(minimum, sample);
                    maximum = std::max(maximum, sample);
                }
                peaks.push_back({minimum, maximum});
            }
        }

        while (result->levels.back().channels[0].size() > 1) {
            const auto& lower = result->levels.back();
            WaveformLevel upper;
            if (lower.framesPerBucket >
                std::numeric_limits<std::uint64_t>::max() / 2U)
                return {{}, "Waveform level size overflow"};
            upper.framesPerBucket = lower.framesPerBucket * 2U;
            for (std::uint32_t channel = 0; channel < pcm.channelCount; ++channel) {
                const auto& input = lower.channels[channel];
                auto& output = upper.channels[channel];
                output.reserve((input.size() + 1U) / 2U);
                for (std::size_t i = 0; i < input.size(); i += 2U) {
                    auto peak = input[i];
                    if (i + 1U < input.size()) {
                        peak.minimum = std::min(peak.minimum, input[i + 1U].minimum);
                        peak.maximum = std::max(peak.maximum, input[i + 1U].maximum);
                    }
                    output.push_back(peak);
                }
            }
            result->levels.push_back(std::move(upper));
        }

        std::size_t bytes = sizeof(PreparedWaveformData);
        for (const auto& level : result->levels)
            for (std::uint32_t channel = 0; channel < pcm.channelCount; ++channel) {
                const auto payload = level.channels[channel].size() * sizeof(PeakPair);
                if (addWouldOverflow(bytes, payload))
                    return {{}, "Waveform byte count overflow"};
                bytes += payload;
            }
        result->approximateBytes = bytes;
        if (bytes > budgetBytes)
            return {{}, "Waveform exceeds the configured cache budget"};
        return {std::move(result), {}};
    } catch (const std::bad_alloc&) {
        return {{}, "Not enough memory to prepare waveform"};
    } catch (...) {
        return {{}, "Unexpected waveform preparation failure"};
    }
}

WaveformCache::StoreResult WaveformCache::store(
    media::SourceId source,
    std::shared_ptr<const PreparedWaveformData> waveform) {
    if (!source.isValid() || !waveform || waveform->approximateBytes == 0)
        return StoreResult::invalid;
    const auto existing = entries_.find(source.value);
    const auto oldBytes = existing == entries_.end() || !existing->second.prepared
                              ? 0U : existing->second.prepared->approximateBytes;
    const auto base = usedBytes_ - oldBytes;
    if (waveform->approximateBytes > budgetBytes_ - std::min(base, budgetBytes_))
        return StoreResult::budgetExceeded;
    const auto bytes = waveform->approximateBytes;
    entries_[source.value] = {std::move(waveform), {}};
    usedBytes_ = base + bytes;
    ++revision_;
    return StoreResult::stored;
}

std::shared_ptr<const PreparedWaveformData> WaveformCache::find(
    media::SourceId source) const noexcept {
    const auto found = entries_.find(source.value);
    return found == entries_.end() ? nullptr : found->second.prepared;
}

void WaveformCache::markError(media::SourceId source,
                              std::string diagnostic) {
    if (!source.isValid()) return;
    const auto found = entries_.find(source.value);
    if (found != entries_.end() && found->second.prepared)
        usedBytes_ -= found->second.prepared->approximateBytes;
    if (diagnostic.empty()) diagnostic = "Waveform unavailable";
    entries_[source.value] = {{}, std::move(diagnostic)};
    ++revision_;
}

WaveformCache::State WaveformCache::state(media::SourceId source) const noexcept {
    const auto found = entries_.find(source.value);
    if (found == entries_.end()) return State::missing;
    return found->second.prepared ? State::ready : State::error;
}

std::string_view WaveformCache::diagnostic(media::SourceId source) const noexcept {
    const auto found = entries_.find(source.value);
    return found == entries_.end() ? std::string_view{} :
                                     std::string_view{found->second.diagnostic};
}

void WaveformCache::swap(WaveformCache& other) noexcept {
    entries_.swap(other.entries_);
    std::swap(budgetBytes_, other.budgetBytes_);
    std::swap(usedBytes_, other.usedBytes_);
    std::swap(revision_, other.revision_);
}

} // namespace vitadaw::waveform
