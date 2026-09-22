#pragma once

#include "vitadaw/audio/PreparedProcessingPlan.h"

#include <cstddef>
#include <cstdint>
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

// Synchronous, device-independent rendering of one prepared project view.
// The renderer constructs and owns a separate local RealtimeAudioEngine, so
// reusing processBlock() here never shares or mutates the live realtime engine.
// The specification is copied by value; prepared source PCM is borrowed and
// must remain immutable and alive until this call returns. The JUCE entry point
// retains its captured resources with shared_ptr for that lifetime.
//
// This is a lifetime guarantee, not a transactional semantic snapshot. Until
// an explicit concurrency model exists, project mutation, source/media
// replacement, prepared-plan replacement and adapter reconfiguration must not
// overlap this synchronous call.
[[nodiscard]] OfflineRenderResult renderOffline(
    ProcessingPlanSpecification specification,
    std::span<const PreparedSourceView> sources,
    const OfflineRenderRequest& request,
    OfflineRenderCallbacks callbacks = {},
    const processors::IAudioProcessorFactory* processorFactory = nullptr)
    noexcept;

} // namespace vitadaw::audio
