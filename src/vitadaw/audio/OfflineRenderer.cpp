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

struct InMemoryConsumer {
    std::vector<std::vector<float>>& channels;
    std::size_t offset{};
    const char* errorMessage{};

    static bool consume(void* context, ConstAudioBlockView block) noexcept {
        auto& collector = *static_cast<InMemoryConsumer*>(context);
        if (!block.isValid() || block.channelCount != collector.channels.size() ||
            block.frameCount > collector.channels.front().size() - collector.offset) {
            collector.errorMessage = "Offline render output block is invalid";
            return false;
        }
        for (std::size_t channel = 0; channel < block.channelCount; ++channel) {
            std::copy_n(block.channels[channel], block.frameCount,
                        collector.channels[channel].data() + collector.offset);
        }
        collector.offset += block.frameCount;
        return true;
    }
};

} // namespace

std::optional<std::size_t> offlineRenderOutputFrameCount(
    const OfflineRenderRequest& request,
    timeline::SampleRate projectRate) noexcept {
    if (!validRequest(request) || !projectRate.isValid()) return std::nullopt;
    const auto projectFrames = static_cast<long double>(
        request.endSample.value - request.startSample.value);
    const auto frames = std::ceil(
        projectFrames * static_cast<long double>(request.sampleRate.hertz()) /
        static_cast<long double>(projectRate.hertz()));
    if (!std::isfinite(frames) || frames <= 0.0L ||
        frames > static_cast<long double>(
                     std::numeric_limits<std::size_t>::max()) ||
        frames > static_cast<long double>(
                     std::numeric_limits<std::uint64_t>::max())) {
        return std::nullopt;
    }
    const auto result = static_cast<std::size_t>(frames);
    if (result == 0) return std::nullopt;
    return result;
}

OfflineRenderStreamResult renderOfflineBlocks(
    ProcessingPlanSpecification specification,
    std::span<const PreparedSourceView> sources,
    const OfflineRenderRequest& request, OfflineRenderBlockConsumer consumer,
    OfflineRenderCallbacks callbacks,
    const processors::IAudioProcessorFactory* processorFactory) noexcept {
    if (!consumer.isValid())
        return {OfflineRenderStatus::invalidRequest, {}, {},
                "Offline render block consumer is invalid"};

    const auto total = offlineRenderOutputFrameCount(
        request, specification.projectSampleRate);
    if (!total) return {OfflineRenderStatus::invalidRequest, {}, {},
                        "Invalid offline render range or configuration"};
    const auto totalFrames = *total;
    const timeline::DeviceFrameCount totalCount{
        static_cast<std::uint64_t>(totalFrames)};

    specification.processingSampleRate = request.sampleRate;
    specification.processingMode = processors::ProcessingMode::offline;
    auto preparation = prepareProcessingPlanFromSources(
        specification, sources, request.processingBlockSize,
        defaultProcessingMemoryBudgetBytes, processorFactory);
    if (!preparation.success()) {
        return {OfflineRenderStatus::preparationFailed, {}, totalCount,
                std::move(preparation.errorMessage)};
    }

    try {
        std::array<std::vector<float>, 2> renderBlocks;
        for (std::size_t channel = 0; channel < request.outputChannelCount;
             ++channel) {
            renderBlocks[channel].resize(request.processingBlockSize);
        }

        reportProgress(callbacks, 0, totalFrames);
        if (cancelled(callbacks)) {
            return {OfflineRenderStatus::cancelled, {}, totalCount,
                    "Offline render cancelled"};
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
                return {OfflineRenderStatus::renderFailed, {}, totalCount,
                        "Offline render transport could not start"};
            }
        }

        std::size_t completed{};
        while (completed < totalFrames) {
            if (cancelled(callbacks)) {
                return {OfflineRenderStatus::cancelled,
                        {static_cast<std::uint64_t>(completed)}, totalCount,
                        "Offline render cancelled"};
            }
            const auto count = std::min(request.processingBlockSize,
                                        totalFrames - completed);
            std::array<float*, 2> output{};
            std::array<const float*, 2> rendered{};
            for (std::size_t channel = 0; channel < request.outputChannelCount;
                 ++channel) {
                output[channel] = renderBlocks[channel].data();
                rendered[channel] = renderBlocks[channel].data();
                std::fill_n(output[channel], count, 0.0F);
            }
            if (hasRenderableTimeline) {
                engine.processBlock({output.data(), request.outputChannelCount, count},
                                    request.sampleRate);
            }
            if (!consumer.consume(consumer.context,
                                  {rendered.data(), request.outputChannelCount,
                                   count})) {
                return {OfflineRenderStatus::consumerFailed,
                        {static_cast<std::uint64_t>(completed)}, totalCount,
                        "Offline render consumer rejected an audio block"};
            }
            completed += count;
            reportProgress(callbacks, completed, totalFrames);
        }
        return {OfflineRenderStatus::success, totalCount, totalCount, {}};
    } catch (const std::bad_alloc&) {
        return {OfflineRenderStatus::preparationFailed, {}, totalCount,
                "Not enough memory for offline render block buffers"};
    } catch (...) {
        return {OfflineRenderStatus::preparationFailed, {}, totalCount,
                "Offline render block processing could not be prepared"};
    }
}

OfflineRenderResult renderOffline(
    ProcessingPlanSpecification specification,
    std::span<const PreparedSourceView> sources,
    const OfflineRenderRequest& request, OfflineRenderCallbacks callbacks,
    const processors::IAudioProcessorFactory* processorFactory) noexcept {
    const auto total = offlineRenderOutputFrameCount(
        request, specification.projectSampleRate);
    if (!total) return {OfflineRenderStatus::invalidRequest, {}, {},
                        "Invalid offline render range or configuration"};

    OfflineRenderResult result;
    try {
        result.channels.resize(request.outputChannelCount);
        for (auto& channel : result.channels) channel.resize(*total);
    } catch (const std::bad_alloc&) {
        return {OfflineRenderStatus::preparationFailed, {}, {},
                "Not enough memory for offline render output"};
    } catch (...) {
        return {OfflineRenderStatus::preparationFailed, {}, {},
                "Offline render output could not be prepared"};
    }

    InMemoryConsumer collector{result.channels, 0, nullptr};
    const auto streamed = renderOfflineBlocks(
        std::move(specification), sources, request,
        {&collector, &InMemoryConsumer::consume}, callbacks, processorFactory);
    result.status = streamed.status;
    result.renderedFrames = streamed.renderedFrames;
    result.errorMessage = collector.errorMessage == nullptr
                              ? streamed.errorMessage : collector.errorMessage;
    return result;
}

} // namespace vitadaw::audio
