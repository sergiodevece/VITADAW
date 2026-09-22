#include "vitadaw/application/DawApplication.h"
#include "vitadaw/platform/juce/JuceAudioDeviceAdapter.h"

#include <juce_audio_formats/juce_audio_formats.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string_view>
#include <vector>

namespace vitadaw::platform::juce_adapter {

class AudioExportIntegrationAccess {
public:
    static void failNextInitialisation(JuceAudioDeviceAdapter& adapter) noexcept {
        adapter.wavExportFaults_.failInitialisation = true;
    }

    static void failNextWrite(JuceAudioDeviceAdapter& adapter) noexcept {
        adapter.wavExportFaults_.failWrite = true;
    }

    static void failNextFinalization(JuceAudioDeviceAdapter& adapter) noexcept {
        adapter.wavExportFaults_.failFinalization = true;
    }

    static void failNextFileSync(JuceAudioDeviceAdapter& adapter) noexcept {
        adapter.wavExportFaults_.failFileSync = true;
    }

    static void failNextFileClose(JuceAudioDeviceAdapter& adapter) noexcept {
        adapter.wavExportFaults_.failFileClose = true;
    }

    static void failNextPublication(JuceAudioDeviceAdapter& adapter) noexcept {
        adapter.wavExportFaults_.failPublication = true;
    }

    static void failNextDirectorySync(JuceAudioDeviceAdapter& adapter) noexcept {
        adapter.wavExportFaults_.failDirectorySync = true;
    }

    static void clearFaults(JuceAudioDeviceAdapter& adapter) noexcept {
        adapter.wavExportFaults_ = {};
    }
};

} // namespace vitadaw::platform::juce_adapter

namespace {

using namespace vitadaw;
using Adapter = platform::juce_adapter::JuceAudioDeviceAdapter;
using ExportAccess = platform::juce_adapter::AudioExportIntegrationAccess;

void check(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

void checkNear(float actual, float expected, std::string_view message) {
    check(std::abs(actual - expected) < 1.0e-6F, message);
}

void checkAccepted(const commands::CommandResult& result, std::string_view message) {
    if (result.status != commands::CommandStatus::accepted) {
        std::cerr << result.message << '\n';
        check(false, message);
    }
}

void writeSource(const juce::File& file,
                 const std::vector<std::vector<float>>& channels,
                 double sampleRate) {
    check(!channels.empty() && channels.size() <= 2 && !channels.front().empty(),
          "source fixture must be mono or stereo with frames");
    for (const auto& channel : channels)
        check(channel.size() == channels.front().size(), "source channels must align");
    juce::AudioBuffer<float> samples(static_cast<int>(channels.size()),
                                     static_cast<int>(channels.front().size()));
    for (std::size_t channel = 0; channel < channels.size(); ++channel) {
        samples.copyFrom(static_cast<int>(channel), 0, channels[channel].data(),
                         static_cast<int>(channels[channel].size()));
    }
    std::unique_ptr<juce::OutputStream> stream = file.createOutputStream();
    juce::WavAudioFormat format;
    auto writer = format.createWriterFor(
        stream, juce::AudioFormatWriterOptions{}
                    .withSampleRate(sampleRate)
                    .withNumChannels(static_cast<int>(channels.size()))
                    .withBitsPerSample(32)
                    .withSampleFormat(
                        juce::AudioFormatWriterOptions::SampleFormat::floatingPoint));
    check(writer != nullptr && writer->writeFromAudioSampleBuffer(
                                  samples, 0, samples.getNumSamples()),
          "source WAV fixture must write");
}

std::vector<std::vector<float>> readWav(const std::filesystem::path& path,
                                        std::uint32_t expectedChannels,
                                        std::size_t expectedFrames) {
    juce::WavAudioFormat format;
    const juce::File file{juce::String(path.string())};
    auto stream = file.createInputStream();
    std::unique_ptr<juce::AudioFormatReader> reader{
        format.createReaderFor(stream.release(), true)};
    check(reader != nullptr && reader->numChannels == expectedChannels &&
              reader->lengthInSamples == static_cast<juce::int64>(expectedFrames),
          "exported WAV header must match the requested float stream");
    juce::AudioBuffer<float> decoded(static_cast<int>(expectedChannels),
                                     static_cast<int>(expectedFrames));
    check(reader->read(decoded.getArrayOfWritePointers(),
                       static_cast<int>(expectedChannels), 0,
                       static_cast<int>(expectedFrames)),
          "exported WAV must decode");
    std::vector<std::vector<float>> result(expectedChannels,
                                            std::vector<float>(expectedFrames));
    for (std::size_t channel = 0; channel < result.size(); ++channel) {
        std::copy_n(decoded.getReadPointer(static_cast<int>(channel)), expectedFrames,
                    result[channel].data());
    }
    return result;
}

tracks::TrackId importTrack(application::DawApplication& app,
                            const std::filesystem::path& file,
                            media::AudioChannelLayout layout) {
    checkAccepted(app.handle(commands::AddAudioTrack{"Export test", layout}),
                  "track creation must prepare");
    const auto track = app.project().tracks().back().id;
    checkAccepted(app.handle(commands::ImportAudioToTrack{file, track, {0}}),
                  "source import must prepare a project for export");
    return track;
}

audio::OfflineRenderRequest request(std::int64_t start, std::int64_t end,
                                    double rate = 8.0, std::uint32_t channels = 2,
                                    std::size_t block = 3) {
    return {{start}, {end}, timeline::SampleRate{rate}, channels, block};
}

struct ProgressProbe {
    std::uint64_t last{};
    std::uint64_t total{};
    std::size_t calls{};
    bool monotonic{true};
    bool reachedTerminal{};
    bool cancel{};
};

struct FinalBlockCancellationProbe {
    std::uint64_t last{};
    std::uint64_t total{};
    bool cancel{};
    bool reachedTerminal{};
};

bool cancellationRequested(void* context) noexcept {
    const auto& probe = *static_cast<ProgressProbe*>(context);
    return probe.cancel && probe.last >= 3;
}

void progress(void* context, timeline::DeviceFrameCount completed,
              timeline::DeviceFrameCount total) noexcept {
    auto& probe = *static_cast<ProgressProbe*>(context);
    probe.monotonic = probe.monotonic && completed.value >= probe.last;
    probe.last = completed.value;
    probe.total = total.value;
    probe.reachedTerminal = probe.reachedTerminal || completed.value == total.value;
    ++probe.calls;
}

bool cancellationAtFinalRenderProgress(void* context) noexcept {
    return static_cast<const FinalBlockCancellationProbe*>(context)->cancel;
}

void cancelAtFinalRenderProgress(void* context, timeline::DeviceFrameCount completed,
                                 timeline::DeviceFrameCount total) noexcept {
    auto& probe = *static_cast<FinalBlockCancellationProbe*>(context);
    probe.last = completed.value;
    probe.total = total.value;
    probe.reachedTerminal = probe.reachedTerminal || completed.value == total.value;
    if (completed.value + 1U == total.value) probe.cancel = true;
}

void compareExport(application::DawApplication& app,
                   const std::filesystem::path& destination,
                   audio::OfflineRenderRequest render,
                   audio::OfflineRenderCallbacks callbacks = {}) {
    const auto expected = app.renderOffline(render);
    check(expected.success(), "equivalent in-memory offline render must succeed");
    const auto exported = app.exportWav({destination, render}, callbacks);
    check(exported.success() && exported.publishedFile == destination &&
              exported.renderedFrames == expected.renderedFrames &&
              std::filesystem::exists(destination) &&
              !exported.retainedTemporaryFile.empty() &&
              std::filesystem::exists(exported.retainedTemporaryFile),
          "streamed WAV export must publish only after completion and retain its temporary");
    const auto decoded = readWav(destination, render.outputChannelCount,
                                 static_cast<std::size_t>(expected.renderedFrames.value));
    check(decoded.size() == expected.channels.size(),
          "decoded export must preserve output channel count");
    for (std::size_t channel = 0; channel < decoded.size(); ++channel) {
        for (std::size_t frame = 0; frame < decoded[channel].size(); ++frame) {
            checkNear(decoded[channel][frame], expected.channels[channel][frame],
                      "streamed float WAV must match the equivalent offline render");
        }
    }
}

} // namespace

int main() {
    const auto directory = juce::File::getSpecialLocation(juce::File::tempDirectory)
        .getNonexistentChildFile("vitadaw-audio-export", "", false);
    check(directory.createDirectory().wasOk(), "temporary export directory must exist");
    const auto path = [&directory](const char* name) {
        return std::filesystem::path{directory.getChildFile(name).getFullPathName().toStdString()};
    };

    const std::vector<float> mono{1.0F, 2.0F, 3.0F, 4.0F, 5.0F, 6.0F, 7.0F, 8.0F};
    const std::vector<float> other(8, 0.5F);
    const std::vector<float> left{1.0F, 2.0F, 3.0F, 4.0F};
    const std::vector<float> right{4.0F, 3.0F, 2.0F, 1.0F};
    writeSource(juce::File{juce::String(path("mono-source.wav").string())}, {mono}, 8.0);
    writeSource(juce::File{juce::String(path("other-source.wav").string())}, {other}, 8.0);
    writeSource(juce::File{juce::String(path("stereo-source.wav").string())},
                {left, right}, 8.0);

    // A/P/F/G/H/N/O: exported blocks decode to the exact same samples as the
    // in-memory consumer, including partial ranges, a tail block and a rate
    // conversion. The operation leaves the installed transport untouched.
    Adapter adapter;
    application::DawApplication app{adapter, timeline::SampleRate{8.0}};
    importTrack(app, path("mono-source.wav"), media::AudioChannelLayout::mono);
    const auto transportBefore = adapter.transportSnapshot();
    const auto recordingBefore = adapter.recordingSnapshot();
    ProgressProbe probe;
    compareExport(app, path("mono-full.wav"), request(0, 8),
                  {&probe, cancellationRequested, progress});
    check(probe.calls >= 2 && probe.monotonic && probe.last == probe.total &&
              probe.total == 8 && probe.reachedTerminal,
          "successful export progress must be monotonic and finish at terminal confirmation");
    compareExport(app, path("mono-partial.wav"), request(2, 7, 8.0, 2, 4));
    compareExport(app, path("mono-tail.wav"), request(0, 8, 8.0, 2, 3));
    compareExport(app, path("mono-half-rate.wav"), request(0, 8, 4.0, 2, 3));
    compareExport(app, path("mono-output.wav"), request(0, 8, 8.0, 1, 3));
    const auto transportAfter = adapter.transportSnapshot();
    const auto recordingAfter = adapter.recordingSnapshot();
    check(transportBefore.playback == transportAfter.playback &&
              transportBefore.position == transportAfter.position &&
              transportBefore.loopEnabled == transportAfter.loopEnabled &&
              transportBefore.monitoringEnabled == transportAfter.monitoringEnabled &&
              recordingBefore.phase == recordingAfter.phase,
          "export must not mutate realtime transport, monitoring or recording state");

    // C/D/E: stereo, existing mono semantics and multiple-track summing all
    // use the same renderer/plan as offline playback.
    Adapter stereoAdapter;
    application::DawApplication stereoApp{stereoAdapter, timeline::SampleRate{8.0}};
    importTrack(stereoApp, path("stereo-source.wav"), media::AudioChannelLayout::stereo);
    compareExport(stereoApp, path("stereo.wav"), request(0, 4, 8.0, 2, 3));
    compareExport(stereoApp, path("stereo-mono.wav"), request(0, 4, 8.0, 1, 3));

    Adapter sumAdapter;
    application::DawApplication sumApp{sumAdapter, timeline::SampleRate{8.0}};
    importTrack(sumApp, path("mono-source.wav"), media::AudioChannelLayout::mono);
    importTrack(sumApp, path("other-source.wav"), media::AudioChannelLayout::mono);
    compareExport(sumApp, path("summed.wav"), request(0, 8));

    // I: no result-sized buffer is required to stream a long silent range.
    Adapter emptyAdapter;
    application::DawApplication emptyApp{emptyAdapter, timeline::SampleRate{8.0}};
    const auto longResult = emptyApp.exportWav(
        {path("long-silence.wav"), request(0, 200003, 8.0, 2, 257)});
    check(longResult.success() && longResult.renderedFrames.value == 200003,
          "long silent export must stream without an in-memory mix result");
    const auto emptyResult = emptyApp.exportWav(
        {path("empty.wav"), request(0, 7, 8.0, 2, 4)});
    check(emptyResult.success(), "empty project export must produce a valid silent WAV");
    const auto emptyDecoded = readWav(path("empty.wav"), 2, 7);
    check(std::all_of(emptyDecoded.front().begin(), emptyDecoded.front().end(),
                      [](float sample) { return sample == 0.0F; }),
          "empty project WAV must contain silence");

    // J: cancellation occurs between blocks and never publishes a final file.
    ProgressProbe cancelledProbe;
    cancelledProbe.cancel = true;
    const auto cancelled = app.exportWav(
        {path("cancelled.wav"), request(0, 8, 8.0, 2, 3)},
        {&cancelledProbe, cancellationRequested, progress});
    check(cancelled.status == audio::WavExportStatus::cancelled &&
              !std::filesystem::exists(path("cancelled.wav")) &&
              !cancelled.retainedTemporaryFile.empty() &&
              std::filesystem::exists(cancelled.retainedTemporaryFile) &&
              cancelledProbe.last < cancelledProbe.total && !cancelledProbe.reachedTerminal,
          "cancelled export must retain only its unpublished temporary");

    // The final rendered block reserves total/total for the successful
    // filesystem commit, so it remains a final cancellable boundary.
    FinalBlockCancellationProbe finalBlockCancellation;
    const auto cancelledAtFinalRender = app.exportWav(
        {path("cancelled-at-final-render.wav"), request(0, 8, 8.0, 2, 3)},
        {&finalBlockCancellation, cancellationAtFinalRenderProgress,
         cancelAtFinalRenderProgress});
    check(cancelledAtFinalRender.status == audio::WavExportStatus::cancelled &&
              !std::filesystem::exists(path("cancelled-at-final-render.wav")) &&
              !cancelledAtFinalRender.retainedTemporaryFile.empty() &&
              std::filesystem::exists(cancelledAtFinalRender.retainedTemporaryFile) &&
              !finalBlockCancellation.reachedTerminal,
          "cancellation after the final render block must prevent finalization and publication");

    // K: construction transfers FD ownership before any fallible writer setup.
    ExportAccess::failNextInitialisation(adapter);
    const auto initialisationFailure = app.exportWav(
        {path("initialisation-failure.wav"), request(0, 8, 8.0, 2, 3)});
    ExportAccess::clearFaults(adapter);
    check(initialisationFailure.status == audio::WavExportStatus::wavInitialisationFailed &&
              !std::filesystem::exists(path("initialisation-failure.wav")) &&
              !initialisationFailure.retainedTemporaryFile.empty() &&
              std::filesystem::exists(initialisationFailure.retainedTemporaryFile),
          "writer initialisation failure must retain the temporary without publishing");

    // L: an audio write failure is distinct from success and cannot publish.
    ExportAccess::failNextWrite(adapter);
    ProgressProbe writeProbe;
    const auto writeFailure = app.exportWav(
        {path("write-failure.wav"), request(0, 8, 8.0, 2, 3)},
        {&writeProbe, cancellationRequested, progress});
    ExportAccess::clearFaults(adapter);
    check(writeFailure.status == audio::WavExportStatus::wavWriteFailed &&
              !std::filesystem::exists(path("write-failure.wav")) &&
              std::filesystem::exists(writeFailure.retainedTemporaryFile) &&
              !writeProbe.reachedTerminal,
          "writer failure must not publish an incomplete WAV");

    ExportAccess::failNextFinalization(adapter);
    ProgressProbe finalizationProbe;
    const auto finalizationFailure = app.exportWav(
        {path("finalization-failure.wav"), request(0, 8, 8.0, 2, 3)},
        {&finalizationProbe, cancellationRequested, progress});
    ExportAccess::clearFaults(adapter);
    check(finalizationFailure.status == audio::WavExportStatus::wavFinalizationFailed &&
              !std::filesystem::exists(path("finalization-failure.wav")) &&
              !finalizationProbe.reachedTerminal,
          "WAV finalization failure must not publish a final file");

    ExportAccess::failNextFileSync(adapter);
    ProgressProbe syncProbe;
    const auto syncFailure = app.exportWav(
        {path("sync-failure.wav"), request(0, 8, 8.0, 2, 3)},
        {&syncProbe, cancellationRequested, progress});
    ExportAccess::clearFaults(adapter);
    check(syncFailure.status == audio::WavExportStatus::wavFinalizationFailed &&
              !std::filesystem::exists(path("sync-failure.wav")) && !syncProbe.reachedTerminal,
          "WAV file fsync failure must not publish terminal progress");

    ExportAccess::failNextFileClose(adapter);
    ProgressProbe closeProbe;
    const auto closeFailure = app.exportWav(
        {path("close-failure.wav"), request(0, 8, 8.0, 2, 3)},
        {&closeProbe, cancellationRequested, progress});
    ExportAccess::clearFaults(adapter);
    check(closeFailure.status == audio::WavExportStatus::wavFinalizationFailed &&
              !std::filesystem::exists(path("close-failure.wav")) && !closeProbe.reachedTerminal,
          "WAV file close failure must not publish terminal progress");

    ExportAccess::failNextPublication(adapter);
    ProgressProbe publicationProbe;
    const auto publicationFailure = app.exportWav(
        {path("publication-failure.wav"), request(0, 8, 8.0, 2, 3)},
        {&publicationProbe, cancellationRequested, progress});
    ExportAccess::clearFaults(adapter);
    check(publicationFailure.status == audio::WavExportStatus::publicationFailed &&
              publicationFailure.publishedFile.empty() &&
              !std::filesystem::exists(path("publication-failure.wav")) &&
              !publicationProbe.reachedTerminal,
          "publication failure must not create a final file");

    ExportAccess::failNextDirectorySync(adapter);
    ProgressProbe directorySyncProbe;
    const auto directorySyncFailure = app.exportWav(
        {path("directory-sync-failure.wav"), request(0, 8, 8.0, 2, 3)},
        {&directorySyncProbe, cancellationRequested, progress});
    ExportAccess::clearFaults(adapter);
    check(directorySyncFailure.status == audio::WavExportStatus::publicationFailed &&
              directorySyncFailure.publishedFile == path("directory-sync-failure.wav") &&
              std::filesystem::exists(directorySyncFailure.publishedFile) &&
              !directorySyncFailure.success() && !directorySyncProbe.reachedTerminal,
          "post-publication directory sync failure must remain an explicit non-success");

    // M/N: existing destinations are never overwritten and malformed requests
    // fail before creating a final file.
    const auto existing = juce::File{juce::String(path("existing.wav").string())};
    check(existing.replaceWithText("preserve me"), "existing destination fixture must write");
    const auto existingResult = app.exportWav(
        {path("existing.wav"), request(0, 8, 8.0, 2, 3)});
    check(existingResult.status == audio::WavExportStatus::destinationExists &&
              existing.loadFileAsString() == "preserve me",
          "existing destination must remain untouched");
    const auto invalid = app.exportWav(
        {path("invalid.wav"), request(4, 4, 8.0, 2, 3)});
    check(invalid.status == audio::WavExportStatus::invalidRequest &&
              !std::filesystem::exists(path("invalid.wav")),
          "invalid export request must not publish a WAV");
    const auto tooLarge = emptyApp.exportWav(
        {path("too-large.wav"), request(0, 600000000, 8.0, 2, 257)});
    check(tooLarge.status == audio::WavExportStatus::invalidRequest &&
              !std::filesystem::exists(path("too-large.wav")),
          "classic WAV data overflow must be rejected before file creation");

    check(directory.deleteRecursively(), "temporary export directory must clean up");
    std::cout << "All audio export tests passed\n";
    return EXIT_SUCCESS;
}
