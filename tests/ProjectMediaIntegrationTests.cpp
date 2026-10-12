#include "vitadaw/platform/juce/JuceAudioDeviceAdapter.h"
#include "vitadaw/application/DawApplication.h"
#include "vitadaw/persistence/ProjectPersistence.h"
#include <juce_audio_formats/juce_audio_formats.h>
#include <array>
#include <atomic>
#include <cstdlib>
#include <fcntl.h>
#include <iostream>
#include <new>
#include <optional>
#include <vector>
#include <memory>
#include <unistd.h>

namespace {
std::atomic<bool> countRealtimeAllocations{};
std::atomic<std::size_t> realtimeAllocations{};
}

void* operator new(std::size_t size) {
    if (countRealtimeAllocations.load(std::memory_order_relaxed))
        realtimeAllocations.fetch_add(1, std::memory_order_relaxed);
    if (auto* memory = std::malloc(size)) return memory;
    throw std::bad_alloc{};
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete[](void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }
void operator delete[](void* memory, std::size_t) noexcept { std::free(memory); }

namespace vitadaw::platform::juce_adapter {
namespace {
constexpr auto virtualDeviceTypeName = "VitaDAW test device";
constexpr auto virtualInputName = "VitaDAW test input";
constexpr auto virtualOutputName = "VitaDAW test output";

class VirtualAudioDevice final : public juce::AudioIODevice {
public:
    VirtualAudioDevice()
        : AudioIODevice("VitaDAW test device", virtualDeviceTypeName) {}

    juce::StringArray getOutputChannelNames() override { return {"Left", "Right"}; }
    juce::StringArray getInputChannelNames() override { return {"Input 1", "Input 2"}; }
    juce::Array<double> getAvailableSampleRates() override { return {48000.0}; }
    juce::Array<int> getAvailableBufferSizes() override { return {256}; }
    int getDefaultBufferSize() override { return 256; }
    juce::String open(const juce::BigInteger& input, const juce::BigInteger& output,
                      double rate, int buffer) override {
        inputChannels_ = input;
        outputChannels_ = output;
        sampleRate_ = rate;
        bufferSize_ = buffer;
        open_ = true;
        return {};
    }
    void close() override { open_ = false; }
    bool isOpen() override { return open_; }
    void start(juce::AudioIODeviceCallback* callback) override {
        callback_ = callback;
        playing_ = true;
        if (callback_ != nullptr) callback_->audioDeviceAboutToStart(this);
    }
    void stop() override {
        if (!playing_) return;
        playing_ = false;
        if (callback_ != nullptr) callback_->audioDeviceStopped();
    }
    bool isPlaying() override { return playing_; }
    juce::String getLastError() override { return {}; }
    int getCurrentBufferSizeSamples() override { return bufferSize_; }
    double getCurrentSampleRate() override { return sampleRate_; }
    int getCurrentBitDepth() override { return 32; }
    juce::BigInteger getActiveOutputChannels() const override { return outputChannels_; }
    juce::BigInteger getActiveInputChannels() const override { return inputChannels_; }
    int getOutputLatencyInSamples() override { return 0; }
    int getInputLatencyInSamples() override { return 0; }

private:
    juce::AudioIODeviceCallback* callback_{};
    juce::BigInteger inputChannels_;
    juce::BigInteger outputChannels_;
    double sampleRate_{};
    int bufferSize_{};
    bool open_{};
    bool playing_{};
};

class VirtualAudioDeviceType final : public juce::AudioIODeviceType {
public:
    VirtualAudioDeviceType() : AudioIODeviceType(virtualDeviceTypeName) {}

    void scanForDevices() override {}
    juce::StringArray getDeviceNames(bool wantInput) const override {
        return wantInput ? juce::StringArray{virtualInputName}
                         : juce::StringArray{virtualOutputName};
    }
    int getDefaultDeviceIndex(bool) const override { return 0; }
    int getIndexOfDevice(juce::AudioIODevice*, bool) const override { return 0; }
    bool hasSeparateInputsAndOutputs() const override { return true; }
    juce::AudioIODevice* createDevice(const juce::String& output,
                                      const juce::String& input) override {
        if (output != virtualOutputName || input != virtualInputName) return nullptr;
        return new VirtualAudioDevice;
    }
};
} // namespace

class PersistenceIntegrationAccess {
public:
    static bool configureHardwareFreeRecordingDevice(JuceAudioDeviceAdapter& adapter) {
        adapter.deviceManager_.addAudioDeviceType(std::make_unique<VirtualAudioDeviceType>());
        adapter.deviceManager_.setCurrentAudioDeviceType(virtualDeviceTypeName, false);
        juce::AudioDeviceManager::AudioDeviceSetup setup;
        setup.inputDeviceName = virtualInputName;
        setup.outputDeviceName = virtualOutputName;
        setup.sampleRate = 48000.0;
        setup.bufferSize = 256;
        setup.useDefaultInputChannels = true;
        setup.useDefaultOutputChannels = true;
        if (adapter.deviceManager_.setAudioDeviceSetup(setup, false).isNotEmpty()) return false;
        auto* device = adapter.deviceManager_.getCurrentAudioDevice();
        if (device == nullptr) return false;
        adapter.deviceSampleRate_ = timeline::SampleRate{device->getCurrentSampleRate()};
        std::string error;
        if (!adapter.reprepareForCurrentDevice(error)) return false;
        adapter.attachAudioCallback(true);
        return adapter.callbackRegistered_ &&
               adapter.realtimeEngine_.deviceState() == audio::DeviceProcessingState::operational;
    }
    static void setRecoveryMarkerFault(JuceAudioDeviceAdapter& adapter,
                                       audio::RecordingRecoveryMarkerFault fault) {
        adapter.recordingRecoveryMarkerWriteOptions_ = {fault};
    }
    static bool writerAndCapturePrepared(const JuceAudioDeviceAdapter& adapter) {
        return adapter.recordingWriter_ != nullptr && adapter.recordingFile_ != nullptr &&
               adapter.realtimeEngine_.recordingSnapshot().phase == audio::RecordingPhase::idle;
    }
    static bool recoveryMarkerFailureIsNonBlocking(const std::filesystem::path& directory,
                                                   audio::RecordingRecoveryMarkerFault fault) {
        audio::RecordingRecoveryMarker marker;
        marker.sessionId = "adapter-marker-" + std::to_string(static_cast<int>(fault));
        marker.classification = audio::RecordingRecoveryClass::temporary;
        marker.temporaryName = ".Recording 000001.part.wav";
        marker.publishedName = "Recording 000001.wav";
        marker.deviceSampleRate = timeline::SampleRate{48000};
        const auto result = JuceAudioDeviceAdapter::persistInitialRecoveryMetadata(
            directory, marker, audio::RecordingRecoveryMarkerWriteOptions{fault});
        return !result.available &&
               result.warning.find("Recording started, but crash-recovery metadata") !=
                   std::string::npos;
    }
    static std::optional<JuceAudioDeviceAdapter::FileIdentity> createExclusiveTemporary(
        const std::filesystem::path& path, std::error_code& error) {
        auto temporary = JuceAudioDeviceAdapter::createExclusiveTemporaryFile(path, error);
        if (!temporary) return std::nullopt;
        static_cast<void>(::close(temporary->release()));
        return temporary->identity;
    }
    static bool closesExclusiveOnAbandon(const std::filesystem::path& path,
                                         std::error_code& error) {
        auto temporary = JuceAudioDeviceAdapter::createExclusiveTemporaryFile(path, error);
        if (!temporary) return false;
        const auto descriptor = temporary->descriptor_;
        temporary.reset(); // Models allocation failure before stream construction.
        errno = 0;
        return ::fcntl(descriptor, F_GETFD) == -1 && errno == EBADF;
    }
    static bool discard(JuceAudioDeviceAdapter& adapter,
                        const std::filesystem::path& temporary,
                        std::optional<JuceAudioDeviceAdapter::FileIdentity> temporaryIdentity,
                        const std::filesystem::path& published,
                        std::optional<JuceAudioDeviceAdapter::FileIdentity> publishedIdentity,
                        bool removePublished) {
        adapter.recordingTemporaryPath_ = temporary;
        adapter.recordingTemporaryIdentity_ = temporaryIdentity;
        adapter.recordingPublishedPath_ = published;
        adapter.recordingPublishedIdentity_ = publishedIdentity;
        return adapter.discardRecording(removePublished);
    }
    // Drives the production audio callback exactly as the device would. The
    // virtual device has no thread, so this is the only callback source.
    static void callback(JuceAudioDeviceAdapter& adapter, const float* input, int frames) {
        std::array<float, 256> left{}, right{};
        std::array<float*, 2> output{left.data(), right.data()};
        const std::array<const float*, 1> inputs{input};
        adapter.audioDeviceIOCallbackWithContext(inputs.data(), 1, output.data(), 2, frames,
                                                 juce::AudioIODeviceCallbackContext{});
    }
    static audio::RecordingCaptureTelemetry recordingTelemetry(
        const JuceAudioDeviceAdapter& adapter) {
        return adapter.realtimeEngine_.recordingTelemetry();
    }
    static float render(JuceAudioDeviceAdapter& adapter) {
        // Never initialise AudioDeviceManager: exclusive, synchronous test consumer.
        adapter.realtimeEngine_.deviceInitialising();
        adapter.realtimeEngine_.deviceConsumerStarted();
        if (!adapter.tryRequestPlay().accepted) std::abort();
        std::array<float,1024> left{}, right{};
        std::array<float*,2> channels{left.data(),right.data()};
        realtimeAllocations.store(0, std::memory_order_relaxed);
        countRealtimeAllocations.store(true, std::memory_order_relaxed);
        adapter.realtimeEngine_.processBlock({channels.data(),2,left.size()},timeline::SampleRate{48000});
        countRealtimeAllocations.store(false, std::memory_order_relaxed);
        if (realtimeAllocations.load(std::memory_order_relaxed) != 0) std::abort();
        return left[10];
    }
};
}
namespace {
using namespace vitadaw;
void check(bool condition,const char* text){if(!condition){std::cerr<<text<<'\n';std::exit(1);}}
void wav(const juce::File& file,float sample,double rate){
    juce::AudioBuffer<float> buffer(1,480);for(int i=0;i<480;++i)buffer.setSample(0,i,sample);
    juce::WavAudioFormat format;
    std::unique_ptr<juce::OutputStream> stream=file.createOutputStream();
    auto writer=format.createWriterFor(stream,juce::AudioFormatWriterOptions{}.withSampleRate(rate).withNumChannels(1).withBitsPerSample(16));
    check(writer!=nullptr&&writer->writeFromAudioSampleBuffer(buffer,0,480),"write real WAV");
}
}
// 0.8.3-B2 regression: overflow -> cleanup -> new Record -> real PCM -> Stop ->
// published clip, on the real JUCE adapter and DawApplication. Mirrors the P-8c
// physical smoke without audio hardware: the virtual device has no thread, so
// every callback below is the production callback, invoked deterministically.
float overflowPattern(std::uint64_t frame, bool secondTake) {
    // Dyadic values are exact in a float WAV; the sign separates the takes.
    const auto phase = static_cast<float>(frame % 8192) / 8192.0F - 0.5F;
    return secondTake ? -phase : phase;
}

std::vector<float> readMonoWav(const std::filesystem::path& file) {
    juce::WavAudioFormat format;
    std::unique_ptr<juce::AudioFormatReader> reader{format.createReaderFor(
        new juce::FileInputStream(juce::File{juce::String(file.string())}), true)};
    if (!reader || reader->numChannels != 1) return {};
    juce::AudioBuffer<float> buffer(1, static_cast<int>(reader->lengthInSamples));
    if (!reader->read(&buffer, 0, buffer.getNumSamples(), 0, true, false)) return {};
    return {buffer.getReadPointer(0), buffer.getReadPointer(0) + buffer.getNumSamples()};
}

bool matchesPattern(const std::vector<float>& samples, bool secondTake) {
    for (std::size_t frame = 0; frame < samples.size(); ++frame)
        if (samples[frame] != overflowPattern(frame, secondTake)) return false;
    return true;
}

void recordingRecoversAfterOverflow() {
    using Access = platform::juce_adapter::PersistenceIntegrationAccess;
    constexpr int blockFrames = 256; // the virtual device buffer
    const auto dir = juce::File::getSpecialLocation(juce::File::tempDirectory)
                         .getNonexistentChildFile("vitadaw-overflow-recovery", "", false);
    check(dir.createDirectory().wasOk(), "overflow recovery: temporary directory");
    const auto path = [&](const char* name) {
        return std::filesystem::path{dir.getChildFile(name).getFullPathName().toStdString()};
    };
    const auto audioDir = path("recovery Audio");

    platform::juce_adapter::JuceAudioDeviceAdapter adapter;
    check(Access::configureHardwareFreeRecordingDevice(adapter),
          "overflow recovery: hardware-free device is operational");
    application::DawApplication app{adapter, timeline::SampleRate{48000}};
    const auto accepted = [&](commands::Command command) {
        return app.handle(command).status == commands::CommandStatus::accepted;
    };
    check(accepted(commands::AddAudioTrack{"Take"}) &&
              accepted(commands::SaveProjectAs{path("recovery.vitadaw")}) &&
              accepted(commands::SetTrackRecordArmed{{1}, true}),
          "overflow recovery: saved project with one armed mono track");
    const auto historyCursor = app.history().cursor();
    const auto historyToken = app.history().currentStateToken();

    std::array<float, blockFrames> block{};
    std::uint64_t offered{};
    const auto feed = [&](bool secondTake) {
        for (int index = 0; index < blockFrames; ++index)
            block[static_cast<std::size_t>(index)] = overflowPattern(
                offered + static_cast<std::uint64_t>(index), secondTake);
        Access::callback(adapter, block.data(), blockFrames);
        offered += blockFrames;
    };

    // --- Take A: the service never runs, so the ring must overflow. --------
    check(accepted(commands::Record{}), "overflow recovery: first Record accepted");
    for (int guard = 0; guard < 1000 &&
                        adapter.recordingSnapshot().phase != audio::RecordingPhase::failed;
         ++guard)
        feed(false);
    const auto overflowed = adapter.recordingSnapshot();
    const auto overflowTelemetry = Access::recordingTelemetry(adapter);
    check(overflowed.phase == audio::RecordingPhase::failed &&
              overflowed.failure == audio::RecordingFailure::overflow &&
              overflowTelemetry.overflowEvents == 1 &&
              overflowTelemetry.droppedFrames == blockFrames &&
              overflowed.acceptedDeviceFrames.value == overflowTelemetry.ringCapacityFrames,
          "overflow recovery: the real adapter ring fails explicitly with one rejected callback");

    // The application timer drains the published prefix, observes the
    // failure, cleans up and stops the transport.
    app.synchroniseTransport();
    feed(false); // consumes the Stop the failure path requested
    app.synchroniseTransport();
    check(app.recordingPhase() == audio::RecordingPhase::failed &&
              app.recordingError().find(
                  "Recording stopped because the capture buffer overflowed") == 0 &&
              app.recordingError().find("retained for recovery") != std::string::npos,
          "overflow recovery: the take fails and its media is retained");
    check(app.project().sources().empty() && app.project().tracks()[0].clips.empty() &&
              !app.session().dirty() && app.history().cursor() == historyCursor &&
              app.history().currentStateToken() == historyToken,
          "overflow recovery: nothing of the failed take is committed");
    check(app.transport().playback == transport::PlaybackState::stopped,
          "overflow recovery: transport is stopped after the failure");
    const auto failedTemporary = audioDir / ".Recording 000001.1.part.wav";
    const auto retained = readMonoWav(failedTemporary);
    check(retained.size() == overflowed.acceptedDeviceFrames.value &&
              matchesPattern(retained, false),
          "overflow recovery: the retained temporary is a readable WAV holding exactly "
          "the accepted prefix");

    // --- Take B: same process, same project, no restart. --------------------
    check(accepted(commands::Record{}), "overflow recovery: a new Record is accepted");
    offered = 0;
    constexpr int takeBlocks = 40;
    for (int index = 0; index < takeBlocks; ++index) {
        feed(true);
        if (index % 4 == 3) app.synchroniseTransport(); // the writer service
    }
    const auto capturing = adapter.recordingSnapshot();
    const auto fresh = Access::recordingTelemetry(adapter);
    check(capturing.phase == audio::RecordingPhase::capturing && capturing.session == 2 &&
              capturing.acceptedDeviceFrames.value == takeBlocks * blockFrames,
          "overflow recovery: the new take really captures PCM");
    check(fresh.overflowEvents == 0 && fresh.droppedFrames == 0 &&
              fresh.producedFrames == takeBlocks * blockFrames &&
              fresh.peakOccupancyFrames < fresh.ringCapacityFrames,
          "overflow recovery: no overflow counter is inherited from the failed take");

    check(accepted(commands::Stop{}), "overflow recovery: Stop accepted");
    feed(true); // the callback consumes Stop; this block is excluded from the take
    app.synchroniseTransport();
    // A successful publication always reports its retained hidden temporary;
    // that warning is the success path of the retention policy, not a failure.
    check(app.recordingPhase() == audio::RecordingPhase::complete &&
              app.recordingError() ==
                  "Recorded WAV published; temporary retained as an ownership-safe orphan",
          "overflow recovery: the new take completes and finalizes");
    check(app.project().sources().size() == 1 && app.project().tracks()[0].clips.size() == 1 &&
              app.project().sources()[0].frameCount ==
                  timeline::SourceFrameCount{takeBlocks * blockFrames},
          "overflow recovery: exactly one source and clip are committed");
    const auto published = audioDir / "Recording 000002.wav";
    check(std::filesystem::exists(published) && !std::filesystem::exists(
                                                    audioDir / "Recording 000001.wav"),
          "overflow recovery: only the new take is published");
    const auto publishedPcm = readMonoWav(published);
    check(publishedPcm.size() == takeBlocks * blockFrames && matchesPattern(publishedPcm, true),
          "overflow recovery: the published clip holds the new take's PCM exactly");
    check(readMonoWav(failedTemporary) == retained,
          "overflow recovery: the failed take's retained media is untouched by the new take");

    adapter.shutdown();
    check(dir.deleteRecursively(), "overflow recovery: remove own temporary files");
}

int main(){
    using namespace vitadaw;
    recordingRecoversAfterOverflow();
    const auto dir=juce::File::getSpecialLocation(juce::File::tempDirectory).getNonexistentChildFile("vitadaw-media", "",false);
    check(dir.createDirectory().wasOk(),"temporary directory");
    const auto a=dir.getChildFile("a.wav"),b=dir.getChildFile("b.wav");wav(a,0.25F,44100);wav(b,-0.5F,48000);
    const auto text=dir.getChildFile("not-audio.txt");
    const auto corrupt=dir.getChildFile("corrupt.wav");
    check(text.replaceWithText("not audio")&&corrupt.replaceWithText("not a wav"),
          "invalid media fixtures");
    platform::juce_adapter::JuceAudioDeviceAdapter engine;
    application::DawApplication app{engine,timeline::SampleRate{48000}};
    auto ok=[&](commands::Command c){auto r=app.handle(c);if(r.status!=commands::CommandStatus::accepted)std::cerr<<r.message<<" "<<persistence::codeName(r.persistence.code)<<'\n';check(r.status==commands::CommandStatus::accepted,"command accepted");};
    const auto path=[&](const char* name){return std::filesystem::path{dir.getChildFile(name).getFullPathName().toStdString()};};
    check(app.project().tracks().empty(),"native New Project starts with zero tracks");
    const auto cleanToken=app.history().currentStateToken();
    const auto missing=app.handle(commands::ImportAudioFile{path("missing.wav"),{0}});
    const auto unsupported=app.handle(commands::ImportAudioFile{path("not-audio.txt"),{0}});
    const auto decodeFailure=app.handle(commands::ImportAudioFile{path("corrupt.wav"),{0}});
    check(missing.error==commands::CommandError::fileNotFound&&
          unsupported.error==commands::CommandError::unsupportedFormat&&
          decodeFailure.error==commands::CommandError::decodeFailed&&
          app.history().currentStateToken()==cleanToken&&
          app.project().tracks().empty()&&app.project().sources().empty(),
          "real file access, format and decode failures are typed and transactional");
    ok(commands::ImportAudioFile{path("a.wav"),{0}});
    const auto waveformA = app.waveformCache().find({1});
    check(app.project().tracks().size()==1&&app.project().sources().size()==1&&
          app.project().tracks()[0].clips.size()==1&&
          app.timelineSnapshot().tracks[0].clips.size()==1&&
          app.project().duration().value==523&&
          waveformA&&waveformA->levels[0].channels[0][0].maximum>0.2F,
          "actual WAV first import creates coherent source waveform and timeline duration");
    for(int i=1;i<10;++i)ok(commands::AddClip{{1},{1},{i*10},{200},{0}});
    check(app.waveformCache().size()==1&&app.waveformCache().find({1})==waveformA,
          "ten clips retain one source-level waveform identity");
    ok(commands::SaveProjectAs{path("a.vitadaw")});
    check(platform::juce_adapter::PersistenceIntegrationAccess::render(engine)>0,"A actual JUCE decoded render");app.synchroniseTransport();
    auto decoded=engine.prepareWav(path("b.wav"));check(decoded.success(),"B WAV decoded");
    project::ProjectState model{timeline::SampleRate{48000}};auto track=model.addAudioTrack("B");
    check(model.importAudioToTrack(track,decoded.prepared->media,{480},timeline::SampleRate{48000},media::AudioChannelLayout::mono).source.value==1,"B SourceId one");
    auto serialized=persistence::serializeProject(model,path("b.vitadaw"));check(serialized.result.success(),"B serialize");
    check(platform::files::nativeProjectFileIO().replace(path("b.vitadaw"),serialized.bytes).success(),"B save");
    ok(commands::LoadProject{path("b.vitadaw")});
    const auto waveformB=app.waveformCache().find({1});
    check(waveformB&&waveformB!=waveformA&&
          waveformB->levels[0].channels[0][0].minimum<-0.4F,
          "successful load replaces the session cache despite SourceId collision");
    check(platform::juce_adapter::PersistenceIntegrationAccess::render(engine)<0,"B collision renders B via actual adapter and processBlock");app.synchroniseTransport();
    const auto fingerprint=*app.project().sources()[0].media.fingerprint;
    check(a.copyFileTo(b),"replace media bytes");
    check(engine.prepareVerifiedWav(path("b.wav"),fingerprint,0).result.code==persistence::PersistenceCode::mediaChanged,"changed bytes rejected before decode");
    const auto token=app.history().currentStateToken();
    check(app.handle(commands::LoadProject{path("b.vitadaw")}).persistence.code==persistence::PersistenceCode::mediaChanged,"load rollback changed WAV");
    check(app.history().currentStateToken()==token&&app.waveformCache().find({1})==waveformB&&
          platform::juce_adapter::PersistenceIntegrationAccess::render(engine)<0,
          "old PCM and waveform survive failed load");
    app.synchroniseTransport();ok(commands::LoadProject{path("a.vitadaw")});
    check(app.project().tracks()[0].clips.size()==10&&app.project().sources().size()==1,"shared Source survives actual JUCE load");
    check(app.waveformCache().size()==1&&app.waveformCache().find({1})&&
          app.waveformCache().find({1})->levels[0].channels[0][0].maximum>0.2F,
          "waveform rebuilds from verified media on project reopen");
    check(engine.preparedAudioBytes()==480*sizeof(float),"one decoded PCM for ten clips");
    std::error_code ownershipError;
    const auto externalTemporary = path(".Recording 000001.1.part.wav");
    check(platform::juce_adapter::PersistenceIntegrationAccess::recoveryMarkerFailureIsNonBlocking(
              path("marker-failure"), audio::RecordingRecoveryMarkerFault::create),
          "actual adapter recovery-metadata seam classifies marker create failure as non-blocking");
    std::filesystem::create_directories(path("marker-failure"), ownershipError);
    check(!ownershipError &&
              platform::juce_adapter::PersistenceIntegrationAccess::recoveryMarkerFailureIsNonBlocking(
                  path("marker-failure"), audio::RecordingRecoveryMarkerFault::write) &&
              platform::juce_adapter::PersistenceIntegrationAccess::recoveryMarkerFailureIsNonBlocking(
                  path("marker-failure"), audio::RecordingRecoveryMarkerFault::sync) &&
              platform::juce_adapter::PersistenceIntegrationAccess::recoveryMarkerFailureIsNonBlocking(
                  path("marker-failure"), audio::RecordingRecoveryMarkerFault::close),
          "actual adapter recovery-metadata seam preserves best-effort policy for write/fsync/close");
    check(platform::juce_adapter::PersistenceIntegrationAccess::configureHardwareFreeRecordingDevice(engine),
          "hardware-free JUCE recording preflight device is operational");
    platform::juce_adapter::PersistenceIntegrationAccess::setRecoveryMarkerFault(
        engine, audio::RecordingRecoveryMarkerFault::create);
    const auto markerFailurePreflight = engine.prepareRecording(
        {{1}, media::AudioChannelLayout::mono, path("marker-preflight.vitadaw")});
    check(markerFailurePreflight.success() &&
              markerFailurePreflight.warningMessage.find("crash-recovery metadata") !=
                  std::string::npos &&
              platform::juce_adapter::PersistenceIntegrationAccess::writerAndCapturePrepared(engine),
          "real JUCE preflight continues through writer and capture after marker failure");
    check(engine.tryRequestRecord(markerFailurePreflight.request).accepted &&
              engine.recordingSnapshot().phase == audio::RecordingPhase::prepared,
          "real JUCE preflight reaches and accepts the production Record request");
    const auto markerFailureCleanup = engine.discardRecordingWithDiagnostics(
        false, "test markerless cleanup");
    check(!markerFailureCleanup.clean() && !markerFailureCleanup.retainedArtifacts.empty(),
          "markerless preflight cleanup safely retains its temporary media");
    platform::juce_adapter::PersistenceIntegrationAccess::setRecoveryMarkerFault(
        engine, audio::RecordingRecoveryMarkerFault::none);
    const auto secondPreflight = engine.prepareRecording(
        {{1}, media::AudioChannelLayout::mono, path("marker-preflight.vitadaw")});
    check(secondPreflight.success(),
          "markerless cleanup allows the real adapter to preflight a subsequent Record");
    check(secondPreflight.warningMessage.empty(),
          "subsequent preflight clears the previous marker diagnostic");
    check(platform::juce_adapter::PersistenceIntegrationAccess::writerAndCapturePrepared(engine),
          "subsequent preflight recreates writer and capture resources");
    check(engine.tryRequestRecord(secondPreflight.request).accepted,
          "markerless cleanup resets the real adapter for a subsequent Record");
    check(!engine.discardRecordingWithDiagnostics(false, "test subsequent cleanup").clean(),
          "subsequent preflight cleanup remains ownership-safe");
    const auto ownedTemporary = path(".Recording 000001.2.part.wav");
    const auto abandonedTemporary = path(".Recording 000001.0.part.wav");
    const auto competingFinal = path("Recording 000001.wav");
    check(juce::File{juce::String(externalTemporary.wstring().c_str())}
              .replaceWithText("external temporary"),
          "create externally owned temporary-like fixture");
    check(platform::juce_adapter::PersistenceIntegrationAccess::closesExclusiveOnAbandon(
              abandonedTemporary, ownershipError),
          "exclusive temporary RAII closes its descriptor before stream ownership transfer");
    check(!platform::juce_adapter::PersistenceIntegrationAccess::createExclusiveTemporary(
              externalTemporary, ownershipError).has_value() &&
              juce::File{juce::String(externalTemporary.wstring().c_str())}
                  .loadFileAsString() == "external temporary",
          "exclusive temporary creation never deletes a pre-existing temporary-like file");
    ownershipError.clear();
    const auto ownedTemporaryIdentity =
        platform::juce_adapter::PersistenceIntegrationAccess::createExclusiveTemporary(
            ownedTemporary, ownershipError);
    check(ownedTemporaryIdentity &&
              !platform::juce_adapter::PersistenceIntegrationAccess::discard(
                  engine, ownedTemporary, ownedTemporaryIdentity, {}, std::nullopt, false) &&
              juce::File{juce::String(ownedTemporary.wstring().c_str())}.existsAsFile() &&
              juce::File{juce::String(externalTemporary.wstring().c_str())}.existsAsFile(),
          "temporary cleanup retains an owned orphan rather than deleting by pathname");
    const auto publishedTemporary = path(".Recording 000001.3.part.wav");
    const auto replacedTemporary = path(".Recording 000001.4.part.wav");
    const auto ownedPublishedTemporary =
        platform::juce_adapter::PersistenceIntegrationAccess::createExclusiveTemporary(
            publishedTemporary, ownershipError);
    check(ownedPublishedTemporary &&
              juce::File{juce::String(competingFinal.wstring().c_str())}
                  .replaceWithText("external final"),
          "create publication-collision fixtures");
    std::filesystem::create_hard_link(publishedTemporary, competingFinal, ownershipError);
    check(ownershipError &&
              juce::File{juce::String(competingFinal.wstring().c_str())}
                  .loadFileAsString() == "external final",
          "no-replace publication preserves a competing final file");
    check(!platform::juce_adapter::PersistenceIntegrationAccess::discard(
              engine, publishedTemporary, ownedPublishedTemporary, competingFinal, std::nullopt, true) &&
              juce::File{juce::String(competingFinal.wstring().c_str())}
                  .loadFileAsString() == "external final",
          "failed publication cleanup never deletes an unowned competing final");

    const auto ownedFinalTemporary = path(".Recording 000002.3.part.wav");
    const auto ownedFinalIdentity =
        platform::juce_adapter::PersistenceIntegrationAccess::createExclusiveTemporary(
            ownedFinalTemporary, ownershipError);
    const auto ownedFinal = path("Recording 000002.wav");
    ownershipError.clear();
    std::filesystem::create_hard_link(ownedFinalTemporary, ownedFinal, ownershipError);
    check(ownedFinalIdentity && !ownershipError &&
              !platform::juce_adapter::PersistenceIntegrationAccess::discard(
                  engine, ownedFinalTemporary, ownedFinalIdentity, ownedFinal,
                  ownedFinalIdentity, false) &&
              juce::File{juce::String(ownedFinal.wstring().c_str())}.existsAsFile(),
          "owned final survives retained temporary cleanup");
    check(!platform::juce_adapter::PersistenceIntegrationAccess::discard(
              engine, {}, std::nullopt, ownedFinal, ownedFinalIdentity, true) &&
              juce::File{juce::String(ownedFinal.wstring().c_str())}.existsAsFile(),
          "rollback retains a published final as an explicit safe orphan");

    const auto replacementTemporary = path(".Recording 000003.3.part.wav");
    const auto replacementIdentity =
        platform::juce_adapter::PersistenceIntegrationAccess::createExclusiveTemporary(
            replacementTemporary, ownershipError);
    const auto replaceableFinal = path("Recording 000003.wav");
    ownershipError.clear();
    std::filesystem::create_hard_link(replacementTemporary, replaceableFinal, ownershipError);
    check(replacementIdentity && !ownershipError && std::filesystem::remove(replaceableFinal, ownershipError) &&
              juce::File{juce::String(replaceableFinal.wstring().c_str())}
                  .replaceWithText("replacement final") &&
              !platform::juce_adapter::PersistenceIntegrationAccess::discard(
                  engine, replacementTemporary, replacementIdentity, replaceableFinal,
                  replacementIdentity, true) &&
              juce::File{juce::String(replaceableFinal.wstring().c_str())}
                  .loadFileAsString() == "replacement final",
          "cleanup rejects a final pathname replaced after publication");

    const auto replacedTemporaryIdentity =
        platform::juce_adapter::PersistenceIntegrationAccess::createExclusiveTemporary(
            replacedTemporary, ownershipError);
    check(replacedTemporaryIdentity && std::filesystem::remove(replacedTemporary, ownershipError) &&
              juce::File{juce::String(replacedTemporary.wstring().c_str())}
                  .replaceWithText("replacement temporary") &&
              !platform::juce_adapter::PersistenceIntegrationAccess::discard(
                  engine, replacedTemporary, replacedTemporaryIdentity, {}, std::nullopt, false) &&
              juce::File{juce::String(replacedTemporary.wstring().c_str())}
                  .loadFileAsString() == "replacement temporary",
          "cleanup never deletes a temporary pathname replaced after exclusive creation");
    engine.shutdown();check(dir.deleteRecursively(),"remove own temporary files");
    std::cout<<"JUCE media persistence integration passed without audio hardware\n";
}
