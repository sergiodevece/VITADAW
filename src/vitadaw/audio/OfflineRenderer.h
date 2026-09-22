#pragma once

#include "vitadaw/audio/AudioBlockView.h"
#include "vitadaw/audio/PreparedProcessingPlan.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace vitadaw::audio {

struct OfflineRenderRequest {
    timeline::ProjectFramePosition startSample;
    timeline::ProjectFramePosition endSample;
    timeline::SampleRate sampleRate;
    std::uint32_t outputChannelCount{2};
    std::size_t processingBlockSize{defaultProcessingBlockCapacity};
};

struct OfflineRenderCallbacks {
    void* context{};
    bool (*cancellationRequested)(void*) noexcept {};
    void (*progress)(void*, timeline::DeviceFrameCount completed,
                     timeline::DeviceFrameCount total) noexcept {};
};

enum class OfflineRenderStatus : std::uint8_t {
    success,
    cancelled,
    invalidRequest,
    preparationFailed,
    renderFailed,
    consumerFailed,
};

struct OfflineRenderResult {
    OfflineRenderStatus status{OfflineRenderStatus::invalidRequest};
    std::vector<std::vector<float>> channels;
    timeline::DeviceFrameCount renderedFrames;
    std::string errorMessage;

    [[nodiscard]] bool success() const noexcept {
        return status == OfflineRenderStatus::success;
    }
};

// A synchronous control-side recipient for blocks produced by the local
// offline engine.  Every invocation carries valid channel pointers and the
// exact frame count for that block; the final block can be shorter than the
// requested processing capacity.  The recipient must consume or copy samples
// before returning.
struct OfflineRenderBlockConsumer {
    void* context{};
    bool (*consume)(void*, ConstAudioBlockView) noexcept {};

    [[nodiscard]] bool isValid() const noexcept {
        return context != nullptr && consume != nullptr;
    }
};

struct OfflineRenderStreamResult {
    OfflineRenderStatus status{OfflineRenderStatus::invalidRequest};
    timeline::DeviceFrameCount renderedFrames;
    timeline::DeviceFrameCount totalFrames;
    std::string errorMessage;

    [[nodiscard]] bool success() const noexcept {
        return status == OfflineRenderStatus::success;
    }
};

// Returns the exact output frame count used by both in-memory and streaming
// offline consumers. A missing value means the request cannot be represented
// safely on this platform.
[[nodiscard]] std::optional<std::size_t> offlineRenderOutputFrameCount(
    const OfflineRenderRequest&, timeline::SampleRate projectSampleRate) noexcept;

// Synchronous, device-independent block rendering of one prepared project
// view. The function constructs and owns a separate local RealtimeAudioEngine,
// so reusing processBlock() here never shares or mutates the live realtime
// engine. It intentionally has no filesystem, device or UI dependency.
//
// The specification is copied by value; prepared source PCM is borrowed and
// must remain immutable and alive until this call returns. A platform entry
// point may retain captured resources with shared_ptr for that lifetime.
// Lifetime safety is not a transactional semantic snapshot: project mutation,
// source/media replacement, prepared-plan replacement and adapter
// reconfiguration must not overlap this call until an explicit concurrency
// model exists.
[[nodiscard]] OfflineRenderStreamResult renderOfflineBlocks(
    ProcessingPlanSpecification specification,
    std::span<const PreparedSourceView> sources,
    const OfflineRenderRequest& request,
    OfflineRenderBlockConsumer consumer,
    OfflineRenderCallbacks callbacks = {},
    const processors::IAudioProcessorFactory* processorFactory = nullptr)
    noexcept;

// Synchronous, device-independent rendering of one prepared project view.
// It is the in-memory consumer of renderOfflineBlocks(). Its output allocation
// is therefore intentionally proportional to the requested duration; streaming
// export users should call the block API through their platform export entry.
[[nodiscard]] OfflineRenderResult renderOffline(
    ProcessingPlanSpecification specification,
    std::span<const PreparedSourceView> sources,
    const OfflineRenderRequest& request,
    OfflineRenderCallbacks callbacks = {},
    const processors::IAudioProcessorFactory* processorFactory = nullptr)
    noexcept;

} // namespace vitadaw::audio
