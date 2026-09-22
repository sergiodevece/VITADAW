#include "vitadaw/audio/OfflineRenderer.h"

#include "vitadaw/audio/RealtimeAudioEngine.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <new>
#include <utility>

namespace vitadaw::audio {
namespace {

[[nodiscard]] bool validRequest(const OfflineRenderRequest& request) noexcept {
    return timeline::isSupportedProjectFramePosition(request.startSample) &&
           timeline::isSupportedProjectFramePosition(request.endSample) &&
           request.endSample.value > request.startSample.value &&
           request.sampleRate.isValid() &&
           (request.outputChannelCount == 1 ||
            request.outputChannelCount == 2) &&
           request.processingBlockSize > 0;
}

[[nodiscard]] bool outputFrameCount(
    const OfflineRenderRequest& request, timeline::SampleRate projectRate,
    std::size_t& result) noexcept {
    if (!projectRate.isValid()) return false;
    const auto projectFrames = static_cast<long double>(
        request.endSample.value - request.startSample.value);
    const auto frames = std::ceil(
        projectFrames * static_cast<long double>(request.sampleRate.hertz()) /
        static_cast<long double>(projectRate.hertz()));
    if (!std::isfinite(frames) || frames <= 0.0L ||
        frames > static_cast<long double>(
                     std::numeric_limits<std::size_t>::max())) {
        return false;
    }
    result = static_cast<std::size_t>(frames);
    return result != 0;
}

void reportProgress(const OfflineRenderCallbacks& callbacks,
                    std::size_t completed, std::size_t total) noexcept {
    if (callbacks.progress != nullptr) {
        callbacks.progress(callbacks.context, {completed}, {total});
    }
}

[[nodiscard]] bool cancelled(
    const OfflineRenderCallbacks& callbacks) noexcept {
    return callbacks.cancellationRequested != nullptr &&
           callbacks.cancellationRequested(callbacks.context);
}

} // namespace

OfflineRenderResult renderOffline(
    ProcessingPlanSpecification specification,
    std::span<const PreparedSourceView> sources,
    const OfflineRenderRequest& request, OfflineRenderCallbacks callbacks,
    const processors::IAudioProcessorFactory* processorFactory) noexcept {
    if (!validRequest(request)) {
        return {OfflineRenderStatus::invalidRequest, {}, {},
                "Invalid offline render range or configuration"};
    }

    std::size_t totalFrames{};
    if (!outputFrameCount(request, specification.projectSampleRate,
                          totalFrames)) {
        return {OfflineRenderStatus::invalidRequest, {}, {},
                "Offline render length is invalid or exceeds platform capacity"};
    }

    specification.processingSampleRate = request.sampleRate;
    specification.processingMode = processors::ProcessingMode::offline;
    auto preparation = prepareProcessingPlanFromSources(
        specification, sources, request.processingBlockSize,
        defaultProcessingMemoryBudgetBytes, processorFactory);
    if (!preparation.success()) {
        return {OfflineRenderStatus::preparationFailed, {}, {},
                std::move(preparation.errorMessage)};
    }

    OfflineRenderResult result;
    try {
        result.channels.resize(request.outputChannelCount);
        for (auto& channel : result.channels) channel.resize(totalFrames);
    } catch (const std::bad_alloc&) {
        return {OfflineRenderStatus::preparationFailed, {}, {},
                "Not enough memory for offline render output"};
    } catch (...) {
        return {OfflineRenderStatus::preparationFailed, {}, {},
                "Offline render output could not be prepared"};
    }

    reportProgress(callbacks, 0, totalFrames);
    if (cancelled(callbacks)) {
        result.status = OfflineRenderStatus::cancelled;
        result.errorMessage = "Offline render cancelled";
        return result;
    }

    // A range outside an empty or already-ended project is deterministically
    // silent. It still advances progress/cancellation block by block.
    const bool hasRenderableTimeline =
        preparation.prepared->plan.duration.value > request.startSample.value;

    RealtimeAudioEngine engine;
    if (hasRenderableTimeline) {
        engine.configure(preparation.prepared->plan,
                         preparation.prepared->runtime);
        engine.deviceInitialising();
        engine.deviceConsumerStarted();
        const auto seek = engine.tryRequestSeek(request.startSample);
        const auto play = engine.tryRequestPlay();
        if (!seek.accepted || !play.accepted) {
            result.status = OfflineRenderStatus::renderFailed;
            result.errorMessage = "Offline render transport could not start";
            return result;
        }
    }

    std::size_t completed{};
    while (completed < totalFrames) {
        if (cancelled(callbacks)) {
            result.status = OfflineRenderStatus::cancelled;
            result.renderedFrames = {completed};
            result.errorMessage = "Offline render cancelled";
            return result;
        }
        const auto count = std::min(request.processingBlockSize,
                                    totalFrames - completed);
        if (hasRenderableTimeline) {
            std::array<float*, 2> output{};
            for (std::size_t channel = 0; channel < result.channels.size();
                 ++channel) {
                output[channel] = result.channels[channel].data() + completed;
            }
            engine.processBlock(
                {output.data(), result.channels.size(), count},
                request.sampleRate);
        }
        completed += count;
        result.renderedFrames = {completed};
        reportProgress(callbacks, completed, totalFrames);
    }

    result.status = OfflineRenderStatus::success;
    return result;
}

} // namespace vitadaw::audio
