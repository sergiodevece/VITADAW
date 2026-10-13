// VitaDAW 0.8.3-C2-1 - device failure characterization.
//
// Every scenario drives the REAL JuceAudioDeviceAdapter, DawApplication and
// juce::AudioDeviceManager. Only the audio device is virtual: it has no thread,
// so every callback, stop, restart, error and device-list change below is
// invoked deterministically by the test and travels through JUCE's own
// AudioDeviceManager forwarding, exactly as a real device's would.
//
// Each case runs in its own process (argv[1]). Cases come in two kinds:
//   GREEN - a contract that holds today and must keep holding;
//   RED   - a contract from audit 0.8.3-C1 that does NOT hold today. A RED case
//           first proves its preconditions (exit 1 if they fail, like any
//           broken test), prints what it observed, and then exits 3 with
//           "CONTRACT VIOLATED [<finding>]". Exit 3 is the expected result
//           until the corresponding C2 change lands.
//
// What the virtual device does NOT prove: anything about CoreAudio's physical
// timing, its xrun semantics, or how real hardware reports a rate change. Where
// a case models JUCE 9 CoreAudio behaviour, the model and its source are named
// in that case's comment.

#include "vitadaw/platform/juce/JuceAudioDeviceAdapter.h"
#include "vitadaw/application/DawApplication.h"
#include "vitadaw/commands/CommandDispatcher.h"

#include <juce_audio_formats/juce_audio_formats.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {
using namespace vitadaw;

constexpr auto deviceTypeName = "VitaDAW device-failure test type";
constexpr auto interfaceA = "Interface A";
constexpr auto interfaceB = "Interface B";
constexpr int blockFrames = 256;
constexpr int exitContractViolated = 3;

void check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "SETUP OR GREEN ASSERTION FAILED: " << message << '\n';
        std::exit(1);
    }
}

// A RED contract: the precondition checks above it must already have passed.
void contract(bool condition, const char* finding, const char* message) {
    if (!condition) {
        std::cerr << "CONTRACT VIOLATED [" << finding << "]: " << message << '\n';
        std::exit(exitContractViolated);
    }
}

void observed(const std::string& text) { std::cerr << "OBSERVED: " << text << '\n'; }

// Exact in a float WAV; restarts at zero for every take.
float pattern(std::uint64_t frame) {
    return static_cast<float>(frame % 8192) / 8192.0F - 0.5F;
}

// ---------------------------------------------------------------------------
// Virtual device
// ---------------------------------------------------------------------------
class VirtualAudioDevice final : public juce::AudioIODevice {
public:
    VirtualAudioDevice(const juce::String& name, VirtualAudioDevice** slot)
        : AudioIODevice(name, deviceTypeName), slot_(slot) {}
    ~VirtualAudioDevice() override {
        if (slot_ != nullptr && *slot_ == this) *slot_ = nullptr;
    }

    juce::StringArray getOutputChannelNames() override { return {"Left", "Right"}; }
    juce::StringArray getInputChannelNames() override { return {"Input 1", "Input 2"}; }
    juce::Array<double> getAvailableSampleRates() override { return {44100.0, 48000.0}; }
    juce::Array<int> getAvailableBufferSizes() override { return {128, 256, 512}; }
    int getDefaultBufferSize() override { return blockFrames; }
    juce::String open(const juce::BigInteger& input, const juce::BigInteger& output,
                      double rate, int buffer) override {
        ++openCount;
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
    int getXRunCount() const noexcept override { return xruns; }

    // One device cycle through JUCE's AudioDeviceManager callback handler.
    void render(const float* mono, int frames, const std::uint64_t* hostTimeNs) {
        if (callback_ == nullptr || !playing_) return;
        std::array<const float*, 2> input{mono, mono};
        std::array<float, 4096> left{}, right{};
        std::array<float*, 2> output{left.data(), right.data()};
        juce::AudioIODeviceCallbackContext context;
        context.hostTimeNs = hostTimeNs;
        callback_->audioDeviceIOCallbackWithContext(
            input.data(), inputChannels_.countNumberOfSetBits(), output.data(),
            outputChannels_.countNumberOfSetBits(), frames, context);
    }
    // JUCE CoreAudio's asynchronous restart: stop, then start again.
    void restart(double newRate) {
        stop();
        sampleRate_ = newRate;
        start(callback_);
    }
    void reportError() {
        if (callback_ != nullptr) callback_->audioDeviceError("virtual device error");
    }
    void setReportedSampleRate(double rate) noexcept { sampleRate_ = rate; }

    int openCount{};
    int xruns{-1};

private:
    VirtualAudioDevice** slot_{};
    juce::AudioIODeviceCallback* callback_{};
    juce::BigInteger inputChannels_;
    juce::BigInteger outputChannels_;
    double sampleRate_{48000.0};
    int bufferSize_{blockFrames};
    bool open_{};
    bool playing_{};
};

// Two interfaces, each both an input and an output device, like a USB
// interface. Removing one and notifying runs JUCE's own fallback logic
// (AudioDeviceManager::audioDeviceListChanged).
class VirtualAudioDeviceType final : public juce::AudioIODeviceType {
public:
    VirtualAudioDeviceType() : AudioIODeviceType(deviceTypeName) {}

    void scanForDevices() override {}
    juce::StringArray getDeviceNames(bool) const override { return names_; }
    int getDefaultDeviceIndex(bool) const override {
        return std::max(0, names_.indexOf(interfaceB));
    }
    int getIndexOfDevice(juce::AudioIODevice* device, bool) const override {
        return device == nullptr ? -1 : names_.indexOf(device->getName());
    }
    bool hasSeparateInputsAndOutputs() const override { return true; }
    juce::AudioIODevice* createDevice(const juce::String& output,
                                      const juce::String& input) override {
        const auto name = output.isNotEmpty() ? output : input;
        if (!names_.contains(name) || (input.isNotEmpty() && !names_.contains(input)))
            return nullptr;
        return current = new VirtualAudioDevice(name, &current);
    }

    void remove(const char* name) {
        names_.removeString(name);
        callDeviceChangeListeners();
    }
    void add(const char* name) {
        names_.addIfNotAlreadyThere(name);
        callDeviceChangeListeners();
    }

    VirtualAudioDevice* current{};

private:
    juce::StringArray names_{interfaceA, interfaceB};
};

std::vector<float> readMonoWav(const std::filesystem::path& file) {
    juce::WavAudioFormat format;
    std::unique_ptr<juce::AudioFormatReader> reader{format.createReaderFor(
        new juce::FileInputStream(juce::File{juce::String(file.string())}), true)};
    if (!reader || reader->numChannels != 1) return {};
    juce::AudioBuffer<float> buffer(1, static_cast<int>(reader->lengthInSamples));
    if (!reader->read(&buffer, 0, buffer.getNumSamples(), 0, true, false)) return {};
    return {buffer.getReadPointer(0), buffer.getReadPointer(0) + buffer.getNumSamples()};
}

bool isTakePrefix(const std::vector<float>& samples, std::uint64_t frames) {
    if (samples.size() != frames) return false;
    for (std::size_t index = 0; index < samples.size(); ++index)
        if (samples[index] != pattern(index)) return false;
    return true;
}
} // namespace

namespace vitadaw::platform::juce_adapter {

// Reuses the adapter's existing hardware-free test friend; no production
// declaration is added for this suite.
class PersistenceIntegrationAccess {
public:
    static bool open(JuceAudioDeviceAdapter& adapter, VirtualAudioDeviceType*& type) {
        auto owned = std::make_unique<VirtualAudioDeviceType>();
        type = owned.get();
        adapter.deviceManager_.addAudioDeviceType(std::move(owned));
        adapter.deviceManager_.setCurrentAudioDeviceType(deviceTypeName, false);
        juce::AudioDeviceManager::AudioDeviceSetup setup;
        setup.outputDeviceName = interfaceA;
        setup.inputDeviceName = interfaceA;
        setup.sampleRate = 48000.0;
        setup.bufferSize = blockFrames;
        setup.inputChannels.clear();
        setup.useDefaultInputChannels = false;
        setup.outputChannels.setRange(0, 2, true);
        setup.useDefaultOutputChannels = false;
        if (adapter.deviceManager_.setAudioDeviceSetup(setup, false).isNotEmpty()) return false;
        auto* device = adapter.deviceManager_.getCurrentAudioDevice();
        if (device == nullptr) return false;
        adapter.deviceSampleRate_ = timeline::SampleRate{device->getCurrentSampleRate()};
        std::string error;
        if (!adapter.reprepareForCurrentDevice(error)) return false;
        adapter.attachAudioCallback(true);
        adapter.refreshState();
        return adapter.callbackRegistered_ &&
               adapter.realtimeEngine_.deviceState() == audio::DeviceProcessingState::operational;
    }
    static audio::RecordingSnapshot capture(const JuceAudioDeviceAdapter& adapter) {
        return adapter.realtimeEngine_.recordingSnapshot();
    }
    static audio::RecordingCaptureTelemetry telemetry(const JuceAudioDeviceAdapter& adapter) {
        return adapter.realtimeEngine_.recordingTelemetry();
    }
    static audio::DeviceProcessingState engineState(const JuceAudioDeviceAdapter& adapter) {
        return adapter.realtimeEngine_.deviceState();
    }
    static bool writerOpen(const JuceAudioDeviceAdapter& adapter) {
        return adapter.recordingWriter_ != nullptr;
    }
    static bool callbackRegistered(const JuceAudioDeviceAdapter& adapter) {
        return adapter.callbackRegistered_;
    }
    static int pendingEvent(const JuceAudioDeviceAdapter& adapter) {
        return static_cast<int>(adapter.pendingLifecycleEvent_.load());
    }
    // The seam existing suites use: bypasses JUCE and calls the adapter.
    static void reportErrorDirectly(JuceAudioDeviceAdapter& adapter) {
        adapter.audioDeviceError("direct test error");
    }
    static juce::String currentInputDevice(const JuceAudioDeviceAdapter& adapter) {
        return adapter.deviceManager_.getAudioDeviceSetup().inputDeviceName;
    }
};

} // namespace vitadaw::platform::juce_adapter

namespace {
using Access = platform::juce_adapter::PersistenceIntegrationAccess;
using Clock = std::chrono::steady_clock;

const char* phaseName(audio::RecordingPhase phase) {
    switch (phase) {
    case audio::RecordingPhase::idle: return "idle";
    case audio::RecordingPhase::prepared: return "prepared";
    case audio::RecordingPhase::capturing: return "capturing";
    case audio::RecordingPhase::finalizing: return "finalizing";
    case audio::RecordingPhase::complete: return "complete";
    case audio::RecordingPhase::failed: return "failed";
    }
    return "?";
}

// One saved project with one armed mono track on Interface A, at 48 kHz / 256.
struct Rig {
    explicit Rig(const char* name)
        : dir(juce::File::getSpecialLocation(juce::File::tempDirectory)
                  .getNonexistentChildFile(juce::String("vitadaw-c2-1-") + name, "", false)) {
        check(dir.createDirectory().wasOk(), "temporary directory");
        check(Access::open(adapter, type), "Interface A is open and operational");
        check(send(commands::AddAudioTrack{"Take"}).status == commands::CommandStatus::accepted &&
                  send(commands::SaveProjectAs{path("project.vitadaw")}).status ==
                      commands::CommandStatus::accepted &&
                  send(commands::SetTrackRecordArmed{{1}, true}).status ==
                      commands::CommandStatus::accepted,
              "saved project with one armed track");
    }
    ~Rig() {
        adapter.shutdown();
        dir.deleteRecursively();
    }

    std::filesystem::path path(const std::string& name) const {
        return std::filesystem::path{dir.getChildFile(name).getFullPathName().toStdString()};
    }
    std::filesystem::path audio(const std::string& name) const {
        return path("project Audio") / name;
    }
    commands::CommandResult send(const commands::Command& command) {
        return dispatcher.dispatch(command);
    }
    VirtualAudioDevice& device() {
        check(type->current != nullptr, "a virtual device is open");
        return *type->current;
    }
    // One device cycle carrying the next frames of the current take.
    void render(const std::uint64_t* hostTimeNs = nullptr) {
        std::array<float, blockFrames> block{};
        for (int index = 0; index < blockFrames; ++index)
            block[static_cast<std::size_t>(index)] =
                pattern(offered + static_cast<std::uint64_t>(index));
        device().render(block.data(), blockFrames, hostTimeNs);
        offered += blockFrames;
    }
    void renderBlocks(int count) {
        for (int index = 0; index < count; ++index) render();
    }
    // JuceApplication::timerCallback order: lifecycle first, then the app.
    void tick() {
        adapter.pollDeviceLifecycle();
        app.synchroniseTransport();
    }
    void beginTake() {
        offered = 0;
        check(send(commands::Record{}).status == commands::CommandStatus::accepted,
              "Record accepted");
    }
    // Ends a take normally: Stop, the callback that consumes it, the timer.
    void stopTake() {
        check(send(commands::Stop{}).status == commands::CommandStatus::accepted,
              "Stop accepted");
        render(); // consumes Stop; this block is excluded from the take
        tick();
    }
    std::uint64_t accepted() const { return Access::capture(adapter).acceptedDeviceFrames.value; }
    // One device cycle of digital silence.
    void renderSilence() {
        std::array<float, blockFrames> block{};
        device().render(block.data(), blockFrames, nullptr);
        offered += blockFrames;
    }
    // Runs the 30 Hz timer (no callbacks) until the application reports a
    // failed take; returns the elapsed time, or a negative value on timeout.
    std::chrono::milliseconds tickUntilFailed(std::chrono::milliseconds limit) {
        const auto started = Clock::now();
        while (Clock::now() - started < limit) {
            tick();
            if (app.recordingPhase() == audio::RecordingPhase::failed)
                return std::chrono::duration_cast<std::chrono::milliseconds>(
                    Clock::now() - started);
            std::this_thread::sleep_for(std::chrono::milliseconds(15));
        }
        return std::chrono::milliseconds{-1};
    }

    juce::File dir;
    platform::juce_adapter::JuceAudioDeviceAdapter adapter;
    application::DawApplication app{adapter, timeline::SampleRate{48000}};
    commands::CommandDispatcher dispatcher{app};
    VirtualAudioDeviceType* type{};
    std::uint64_t offered{};
};

// Shared assertions for a take that must have failed without being published.
void checkFailedAndRetained(Rig& rig, std::uint64_t acceptedFrames, const char* reasonText,
                            const std::string& temporary, const std::string& published) {
    check(rig.app.recordingPhase() == audio::RecordingPhase::failed,
          "the take is failed");
    check(rig.app.recordingError().find(reasonText) == 0,
          "the failure reason is reported to the user");
    check(rig.app.recordingError().find("retained for recovery") != std::string::npos,
          "the user is told the media is retained");
    check(rig.app.project().sources().empty() && rig.app.project().tracks()[0].clips.empty() &&
              !rig.app.session().dirty(),
          "nothing of the failed take is committed");
    check(!std::filesystem::exists(rig.audio(published)), "the failed take is not published");
    check(!Access::writerOpen(rig.adapter), "the writer is closed by cleanup");
    check(isTakePrefix(readMonoWav(rig.audio(temporary)), acceptedFrames),
          "the retained temporary holds exactly the accepted frames, sample for sample");
}

constexpr auto stalledMessage =
    "Recording stopped because the audio device stopped delivering audio";
// Watchdog acceptance window. The threshold is 1 s for these devices (256
// frames at 48 kHz: ten periods is 53 ms, below the 1 s floor). Detection may
// not happen earlier, and must happen within the C2-1 acceptance bound.
constexpr std::chrono::milliseconds watchdogThreshold{1000};
constexpr std::chrono::milliseconds watchdogAcceptance{2500};

// Shared assertions for a new, normal take after a failure.
void checkNewTakePublishes(Rig& rig, const std::string& published, int blocks) {
    rig.beginTake();
    rig.renderBlocks(blocks);
    rig.tick();
    rig.stopTake();
    check(rig.app.recordingPhase() == audio::RecordingPhase::complete,
          "the new take completes");
    check(rig.app.project().sources().size() == 1 &&
              rig.app.project().tracks()[0].clips.size() == 1,
          "the new take is committed");
    check(isTakePrefix(readMonoWav(rig.audio(published)),
                       static_cast<std::uint64_t>(blocks * blockFrames)),
          "the new take is published with exactly its own PCM");
}

// ===========================================================================
// A - device loss (T-1, T-6)
// ===========================================================================
// Initial state: capturing on Interface A, 20 blocks accepted.
// Events: uncontrolled device stop (JUCE forwards audioDeviceStopped), timer.
// Expected: failed/deviceLost; transport stopped; no commit; writer closed;
// retained temporary == accepted frames. Then the device starts again (JUCE
// aboutToStart), the timer re-certifies it, and a new take records normally.
void t1DeviceStopFailsTakeAndRecovers() {
    Rig rig{"t1"};
    rig.beginTake();
    rig.renderBlocks(20);
    rig.tick();
    const auto accepted = rig.accepted();
    check(accepted == 20 * blockFrames, "20 blocks captured before the loss");

    rig.device().stop();
    check(Access::capture(rig.adapter).phase == audio::RecordingPhase::capturing,
          "an uncontrolled stop is only a pending event until the timer runs");
    rig.tick();
    check(Access::capture(rig.adapter).phase == audio::RecordingPhase::idle,
          "cleanup has reset the ring after the failure");
    check(rig.app.transport().playback == transport::PlaybackState::stopped,
          "transport stopped");
    checkFailedAndRetained(rig, accepted, "Recording stopped because the audio device was lost",
                           ".Recording 000001.1.part.wav", "Recording 000001.wav");

    // Recovery: the device runs again; one timer pass re-certifies it.
    rig.device().restart(48000.0);
    rig.tick();
    check(Access::engineState(rig.adapter) == audio::DeviceProcessingState::operational,
          "the restarted device is operational again");
    checkNewTakePublishes(rig, "Recording 000002.wav", 12);
}

// T-2a PROBE of JUCE 9.0.2: every device is given AudioDeviceManager's
// CallbackMaxSizeEnforcer as its callback (juce_AudioDeviceManager.cpp:157,196).
// It forwards aboutToStart, IO and stopped, but NOT audioDeviceError, whose base
// implementation is empty (juce_AudioIODevice.cpp:53). A device error therefore
// never reaches VitaDAW. This case pins that fact so a JUCE upgrade that
// changes it is noticed, and checks that a device which keeps delivering audio
// after an error notification still yields an intact take (D1: a notification
// without evidence of discontinuity is not a failure).
void t2aJuceDropsDeviceErrors() {
    Rig rig{"t2a"};
    rig.beginTake();
    rig.renderBlocks(15);
    rig.tick();
    rig.device().reportError();
    check(Access::pendingEvent(rig.adapter) == 0,
          "JUCE 9.0.2 does not forward a device error to the adapter");
    rig.tick();
    check(Access::capture(rig.adapter).phase == audio::RecordingPhase::capturing &&
              rig.adapter.state().status == audio::AudioDeviceStatus::active,
          "the error notification changes nothing in VitaDAW");
    rig.renderBlocks(5);
    rig.tick();
    rig.stopTake();
    check(rig.app.recordingPhase() == audio::RecordingPhase::complete &&
              isTakePrefix(readMonoWav(rig.audio("Recording 000001.wav")), 20 * blockFrames),
          "a stream that continues after the notification yields an intact take");
}

// T-2b: the adapter's own error path, reached only through the direct seam the
// existing suites use (no JUCE delivery exists, see T-2a). It is fail-safe for
// the take. Its recovery behaviour is printed, not asserted: the error branch
// detaches the callback and nothing re-attaches it, a latent defect that only
// matters if errors are ever delivered.
void t2bDirectErrorPathIsFailSafe() {
    Rig rig{"t2b"};
    rig.beginTake();
    rig.renderBlocks(15);
    rig.tick();
    const auto accepted = rig.accepted();
    Access::reportErrorDirectly(rig.adapter);
    rig.tick();
    check(rig.adapter.state().status == audio::AudioDeviceStatus::error,
          "the device is reported in error");
    checkFailedAndRetained(rig, accepted, "Recording stopped because the audio device was lost",
                           ".Recording 000001.1.part.wav", "Recording 000001.wav");
    rig.device().restart(48000.0);
    for (int index = 0; index < 3; ++index) { rig.render(); rig.tick(); }
    const auto record = rig.send(commands::Record{});
    observed(std::string{"after the direct error and a device restart: callback registered "} +
             (Access::callbackRegistered(rig.adapter) ? "yes" : "no") + ", engine " +
             (Access::engineState(rig.adapter) == audio::DeviceProcessingState::operational
                  ? "operational" : "not operational") +
             ", Record -> " + (record.status == commands::CommandStatus::accepted
                                   ? std::string{"accepted"} : "rejected: " + record.message));
}

// ===========================================================================
// B - device restart (T-3)
// ===========================================================================
// JUCE CoreAudio's restart is stop() followed by start() (juce_CoreAudio_mac.cpp,
// handleAsyncUpdate). The take must not survive it, and blocks delivered after
// the restart must not be appended to the take.
void t3RestartFailsTakeWithoutSilentContinuation() {
    Rig rig{"t3"};
    rig.beginTake();
    rig.renderBlocks(18);
    rig.tick();
    const auto beforeRestart = rig.accepted();
    rig.device().restart(48000.0);
    check(Access::capture(rig.adapter).phase == audio::RecordingPhase::failed &&
              Access::capture(rig.adapter).failure == audio::RecordingFailure::deviceLost,
          "aboutToStart fails the take immediately as deviceLost");
    rig.renderBlocks(6); // the device runs again
    check(rig.accepted() == beforeRestart,
          "no frame delivered after the restart is appended to the failed take");
    rig.tick();
    checkFailedAndRetained(rig, beforeRestart,
                           "Recording stopped because the audio device was lost",
                           ".Recording 000001.1.part.wav", "Recording 000001.wav");
}

// ===========================================================================
// T-4 - configuration divergence observed by the timer
// ===========================================================================
// Model: the device reports a new rate before any restart (a backend that,
// unlike JUCE 9 CoreAudio, reports the change at once). The timer sees the
// divergence and must fail the take BEFORE re-preparing, without resetting the
// ring before the writer drains it.
void t4DivergenceFailsBeforeReprepare() {
    Rig rig{"t4"};
    rig.beginTake();
    rig.renderBlocks(16);
    rig.tick();
    rig.device().setReportedSampleRate(44100.0);
    rig.renderBlocks(4); // delivered before the timer notices
    const auto accepted = rig.accepted();
    check(accepted == 20 * blockFrames,
          "characterization: blocks before the timer pass are accepted");
    rig.tick();
    checkFailedAndRetained(rig, accepted, "Recording stopped because the audio device was lost",
                           ".Recording 000001.1.part.wav", "Recording 000001.wav");
    check(Access::engineState(rig.adapter) == audio::DeviceProcessingState::operational,
          "the engine is re-prepared for the new rate after the take has failed");
}

// ===========================================================================
// T-5 - buffer change while capturing
// ===========================================================================
void t5BufferChangeRejectedWhileCapturing() {
    Rig rig{"t5"};
    rig.beginTake();
    rig.renderBlocks(8);
    rig.tick();
    const auto opens = rig.device().openCount;
    const auto result = rig.send(commands::SetAudioBufferSize{512});
    check(result.status == commands::CommandStatus::rejected &&
              rig.device().openCount == opens &&
              rig.device().getCurrentBufferSizeSamples() == blockFrames,
          "a buffer change is rejected while capturing, before any hardware mutation");
    rig.renderBlocks(8);
    rig.tick();
    rig.stopTake();
    check(rig.app.recordingPhase() == audio::RecordingPhase::complete &&
              isTakePrefix(readMonoWav(rig.audio("Recording 000001.wav")), 16 * blockFrames),
          "the take continues and completes normally");
}

// ===========================================================================
// C - external sample-rate change (H-2)
// ===========================================================================
// MODEL of JUCE 9 CoreAudio, from its source: a nominal-rate change only
// updates requestedSetup and schedules an asynchronous restart
// (juce_CoreAudio_mac.cpp:1858-1871); getCurrentSampleRate() keeps returning
// currentSetup (:2101-2103) until that restart runs. The virtual device
// therefore keeps reporting 48 kHz while its host timestamps are spaced at
// 44.1 kHz, the only trace of the change available in the callback. Whether
// real hardware behaves like this model must be confirmed physically (S-2/S-3).
constexpr std::uint64_t blockNs48 = 256ULL * 1000000000ULL / 48000ULL;
constexpr std::uint64_t blockNs441 = 256ULL * 1000000000ULL / 44100ULL;

// GREEN characterization: the late restart fails the take; nothing is
// published. The retained media includes the frames captured inside the window
// (documented contamination, not a fix).
void cRateWindowThenRestartFailsTake() {
    Rig rig{"c-restart"};
    rig.beginTake();
    std::uint64_t host = 1000000000ULL;
    for (int index = 0; index < 100; ++index) { rig.render(&host); host += blockNs48; }
    rig.tick();
    const auto beforeChange = rig.accepted();
    for (int index = 0; index < 100; ++index) { rig.render(&host); host += blockNs441; }
    rig.tick(); // the timer runs inside the window and sees no divergence
    check(Access::capture(rig.adapter).phase == audio::RecordingPhase::capturing,
          "characterization: inside the window the take keeps capturing");
    const auto inWindow = rig.accepted() - beforeChange;
    check(inWindow == 100 * blockFrames,
          "characterization: every block delivered inside the window is accepted");
    rig.device().restart(44100.0); // JUCE's late restart
    rig.tick();
    checkFailedAndRetained(rig, beforeChange + inWindow,
                           "Recording stopped because the audio device was lost",
                           ".Recording 000001.1.part.wav", "Recording 000001.wav");
    observed(std::to_string(inWindow) +
             " frames captured at the new hardware rate are in the retained temporary, "
             "whose header declares 48000 Hz");
}

// RED [H-2]: Stop consumed inside the window publishes a take that crossed an
// effective rate change.
void redStopInsideRateWindowPublishes() {
    Rig rig{"red-h2"};
    rig.beginTake();
    std::uint64_t host = 1000000000ULL;
    for (int index = 0; index < 200; ++index) { rig.render(&host); host += blockNs48; }
    rig.tick();
    for (int index = 0; index < 200; ++index) { rig.render(&host); host += blockNs441; }
    check(rig.send(commands::Stop{}).status == commands::CommandStatus::accepted,
          "precondition: Stop accepted inside the window");
    rig.render(&host); // consumes Stop
    rig.tick();        // finalization runs before JUCE's restart
    const auto published = std::filesystem::exists(rig.audio("Recording 000001.wav"));
    observed(std::string{"phase "} + phaseName(rig.app.recordingPhase()) +
             ", published " + (published ? "yes" : "no") +
             "; 200 of 400 blocks were delivered at 44.1 kHz host spacing under a 48 kHz header");
    contract(rig.app.recordingPhase() != audio::RecordingPhase::complete && !published,
             "H-2", "a take captured across an effective rate change must not be published");
}

// ===========================================================================
// D - callbacks stop without any notification (M-1, fixed in 0.8.3-C2-2)
// ===========================================================================
// MODEL of JUCE 9 CoreAudio's dead-device path: running = false, nobody is
// notified (juce_CoreAudio_mac.cpp:1416-1420). The virtual device simply stops
// calling back. The watchdog (JuceAudioDeviceAdapter::superviseRecordingCallbacks)
// fails a capturing take whose accepted-frame count has not advanced for
// max(1 s, 10 device periods); here that is 1 s.

// Formerly red_m1_stalled_callbacks (C2-1, finding M-1). Its original contract
// is kept verbatim at the end; the rest strengthens it.
void m1StopPendingStallFailsTake() {
    Rig rig{"m1-stop"};
    rig.beginTake();
    rig.renderBlocks(10);
    rig.tick();
    const auto accepted = rig.accepted();
    check(Access::capture(rig.adapter).phase == audio::RecordingPhase::capturing,
          "precondition: capturing before the stall");

    const auto stalled = Clock::now();
    const auto stop = rig.send(commands::Stop{});
    check(stop.status == commands::CommandStatus::accepted,
          "Stop is accepted but cannot be consumed without callbacks");
    const auto failedAfter = rig.tickUntilFailed(watchdogAcceptance);
    const auto sinceStall =
        std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - stalled);
    observed("Stop pending; watchdog failed the take " + std::to_string(sinceStall.count()) +
             " ms after the last callback");
    check(failedAfter.count() >= 0, "the watchdog terminalizes the take within 2.5 s");
    check(sinceStall >= watchdogThreshold,
          "the watchdog does not fire before its threshold");
    check(rig.app.recordingError().find(stalledMessage) == 0,
          "the user is told the device stopped delivering audio");
    checkFailedAndRetained(rig, accepted, stalledMessage, ".Recording 000001.1.part.wav",
                           "Recording 000001.wav");
    check(rig.app.transport().playback == transport::PlaybackState::stopped,
          "the transport is stopped");
    const auto cancel = rig.send(commands::CancelRecording{});
    check(cancel.status == commands::CommandStatus::rejected,
          "there is no take left to cancel");
    const auto phase = Access::capture(rig.adapter).phase;
    contract(phase == audio::RecordingPhase::failed || phase == audio::RecordingPhase::idle,
             "M-1", "a take whose device stopped calling back must reach a terminal state "
                    "without quitting VitaDAW");
}

// Sustained stall with no command at all.
void m1SustainedStallFailsTake() {
    Rig rig{"m1-stall"};
    rig.beginTake();
    rig.renderBlocks(12);
    rig.tick();
    const auto accepted = rig.accepted();
    const auto stalled = Clock::now();
    check(rig.tickUntilFailed(watchdogAcceptance).count() >= 0,
          "a stall with no command is terminalized");
    check(Clock::now() - stalled >= watchdogThreshold, "not before the threshold");
    checkFailedAndRetained(rig, accepted, stalledMessage, ".Recording 000001.1.part.wav",
                           "Recording 000001.wav");
    check(rig.app.transport().playback == transport::PlaybackState::stopped,
          "the failure path stops the projected transport");
}

// Cancel pending: a capturing take's Cancel is an RT command and cannot be
// consumed either; the watchdog ends the take, and the queued Cancel is inert
// once the device resumes.
void m1CancelPendingStallFailsTake() {
    Rig rig{"m1-cancel"};
    rig.beginTake();
    rig.renderBlocks(12);
    rig.tick();
    const auto accepted = rig.accepted();
    check(rig.send(commands::CancelRecording{}).status == commands::CommandStatus::accepted,
          "Cancel is accepted while capturing");
    check(rig.tickUntilFailed(watchdogAcceptance).count() >= 0,
          "the watchdog ends a take whose Cancel cannot be consumed");
    checkFailedAndRetained(rig, accepted, stalledMessage, ".Recording 000001.1.part.wav",
                           "Recording 000001.wav");
    rig.renderBlocks(4); // the device resumes: the stale Cancel and Stop are consumed
    rig.tick();
    check(Access::capture(rig.adapter).phase == audio::RecordingPhase::idle &&
              rig.accepted() == 0 && rig.app.recordingPhase() == audio::RecordingPhase::failed,
          "the queued commands are inert after cleanup");
}

// Continuous callbacks, longer than the threshold: never a false failure.
void m1ContinuousCallbacksNeverTrip() {
    Rig rig{"m1-continuous"};
    rig.beginTake();
    const auto started = Clock::now();
    auto lastTick = started;
    while (Clock::now() - started < std::chrono::milliseconds(1600)) {
        rig.render();
        if (Clock::now() - lastTick >= std::chrono::milliseconds(30)) {
            rig.tick();
            lastTick = Clock::now();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(4));
    }
    rig.tick();
    check(rig.app.recordingPhase() == audio::RecordingPhase::capturing,
          "1.6 s of continuous callbacks never trips the watchdog");
    const auto frames = rig.offered;
    rig.stopTake();
    check(rig.app.recordingPhase() == audio::RecordingPhase::complete &&
              isTakePrefix(readMonoWav(rig.audio("Recording 000001.wav")), frames),
          "the take completes intact");
}

// Silence is not the absence of callbacks.
void m1SilentInputIsNotAStall() {
    Rig rig{"m1-silence"};
    rig.beginTake();
    const auto started = Clock::now();
    auto lastTick = started;
    while (Clock::now() - started < std::chrono::milliseconds(1600)) {
        rig.renderSilence();
        if (Clock::now() - lastTick >= std::chrono::milliseconds(30)) {
            rig.tick();
            lastTick = Clock::now();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(4));
    }
    rig.tick();
    check(rig.app.recordingPhase() == audio::RecordingPhase::capturing,
          "1.6 s of digital silence never trips the watchdog");
    const auto frames = rig.offered;
    rig.stopTake();
    const auto pcm = readMonoWav(rig.audio("Recording 000001.wav"));
    bool silent = pcm.size() == frames;
    for (const auto sample : pcm) silent = silent && sample == 0.0F;
    check(rig.app.recordingPhase() == audio::RecordingPhase::complete && silent,
          "a silent take is recorded and published like any other");
}

// The message thread is blocked for longer than the threshold while callbacks
// keep arriving (B2 measured up to 643 ms in the background; this uses 1.5 s).
// The watchdog compares progress between observations, so it must not fire.
void m1MessageThreadDelayWithLiveCallbacks() {
    Rig rig{"m1-ui-delay"};
    rig.beginTake();
    rig.renderBlocks(8);
    rig.tick();
    const auto blocked = Clock::now();
    while (Clock::now() - blocked < std::chrono::milliseconds(1500)) {
        rig.render(); // callbacks continue; no timer pass at all
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    for (int index = 0; index < 10; ++index) {
        rig.render();
        rig.tick();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    check(rig.app.recordingPhase() == audio::RecordingPhase::capturing,
          "a 1.5 s message-thread block with live callbacks is not a stall");
    const auto frames = rig.offered;
    rig.stopTake();
    check(rig.app.recordingPhase() == audio::RecordingPhase::complete &&
              isTakePrefix(readMonoWav(rig.audio("Recording 000001.wav")), frames),
          "the take completes intact after the delay");
}

// A late callback just before the threshold re-arms the watchdog; detection
// is then measured from that callback, not from the first silence.
void m1LateCallbackRearmsWatchdog() {
    Rig rig{"m1-rearm"};
    rig.beginTake();
    rig.renderBlocks(8);
    rig.tick();
    const auto quiet = Clock::now();
    while (Clock::now() - quiet < std::chrono::milliseconds(700)) {
        rig.tick();
        std::this_thread::sleep_for(std::chrono::milliseconds(15));
    }
    check(rig.app.recordingPhase() == audio::RecordingPhase::capturing,
          "700 ms without callbacks is below the threshold");
    rig.render(); // one late callback
    const auto late = Clock::now();
    const auto accepted = rig.accepted();
    check(rig.tickUntilFailed(watchdogAcceptance).count() >= 0, "the stall is detected");
    check(Clock::now() - late >= watchdogThreshold,
          "the threshold is measured from the last callback");
    checkFailedAndRetained(rig, accepted, stalledMessage, ".Recording 000001.1.part.wav",
                           "Recording 000001.wav");
}

// Callbacks on their own thread while the timer runs on the main thread, as in
// the application; the producer then stops. Run under TSan as well. The first
// callback runs before the producer thread exists, so the take is already
// capturing: this case isolates the watchdog. The prepared-to-capturing
// transition under concurrency is c1_prepared_transition_under_concurrency.
void m1ConcurrentCallbacksThenStall() {
    Rig rig{"m1-concurrent"};
    rig.beginTake();
    rig.render();
    check(Access::capture(rig.adapter).phase == audio::RecordingPhase::capturing,
          "the take is capturing before callbacks move to their own thread");
    std::atomic<bool> producing{true};
    std::atomic<std::int64_t> lastCallbackNs{};
    std::thread producer([&] {
        const auto started = Clock::now();
        while (Clock::now() - started < std::chrono::milliseconds(600)) {
            rig.render();
            lastCallbackNs.store(Clock::now().time_since_epoch().count());
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        producing.store(false);
    });
    const auto failedAfter = rig.tickUntilFailed(std::chrono::milliseconds(4000));
    producer.join();
    check(!producing.load(), "the producer finished before the failure");
    check(failedAfter.count() >= 0, "the stall after concurrent callbacks is detected");
    const auto lastCallback = Clock::time_point{Clock::duration{lastCallbackNs.load()}};
    check(Clock::now() - lastCallback >= watchdogThreshold,
          "no false failure while callbacks were concurrent");
    checkFailedAndRetained(rig, rig.offered, stalledMessage, ".Recording 000001.1.part.wav",
                           "Recording 000001.wav");
}

// C-1 (found by C2-2, fixed in 0.8.3-C2-2b): the callback consumes beginRecord
// on its own thread while the timer thread takes recording snapshots. Before
// C2-2b, ThreadSanitizer reported a data race here: RealtimeCapture::begin()
// wrote session/track/layout/start/rate as plain fields while the take was
// still "prepared", and RealtimeCapture::snapshot() read them in that phase.
// Request metadata is now written only by the control thread while the ring is
// idle, and begin() publishes its values with a release CAS; this case must be
// clean under ThreadSanitizer, with no suppression. Its functional assertion is
// unchanged. See docs/audit-recording-state-hardening-0.8.3C2-2b.md.
void c1PreparedTransitionUnderConcurrency() {
    Rig rig{"c1-race"};
    rig.beginTake(); // prepared: beginRecord is still queued
    std::thread producer([&] {
        for (int index = 0; index < 60; ++index) {
            rig.render();
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    });
    for (int index = 0; index < 20; ++index) {
        rig.tick();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    producer.join();
    rig.tick();
    const auto frames = rig.offered;
    rig.stopTake();
    check(rig.app.recordingPhase() == audio::RecordingPhase::complete &&
              isTakePrefix(readMonoWav(rig.audio("Recording 000001.wav")), frames),
          "functionally, the take begun under concurrency completes intact");
}

// Overflow needs live callbacks; the watchdog needs their absence. The first
// terminal transition wins and keeps its reason.
void m1OverflowWinsOverWatchdog() {
    Rig rig{"m1-overflow"};
    rig.beginTake();
    for (int guard = 0; guard < 1000 && !Access::capture(rig.adapter).terminal(); ++guard)
        rig.render(); // no timer pass: the ring overflows
    const auto accepted = rig.accepted();
    check(Access::capture(rig.adapter).failure == audio::RecordingFailure::overflow,
          "the ring overflowed");
    const auto quiet = Clock::now();
    while (Clock::now() - quiet < std::chrono::milliseconds(1300)) {
        rig.tick();
        std::this_thread::sleep_for(std::chrono::milliseconds(15));
    }
    checkFailedAndRetained(rig, accepted,
                           "Recording stopped because the capture buffer overflowed",
                           ".Recording 000001.1.part.wav", "Recording 000001.wav");
}

// The device comes back after the failure: the old take is not resumed, the
// stale Stop is inert, and only an explicit Record starts a new take.
void m1DeviceResumesAfterStallFailure() {
    Rig rig{"m1-resume"};
    rig.beginTake();
    rig.renderBlocks(10);
    rig.tick();
    const auto accepted = rig.accepted();
    check(rig.tickUntilFailed(watchdogAcceptance).count() >= 0, "the stall is detected");
    checkFailedAndRetained(rig, accepted, stalledMessage, ".Recording 000001.1.part.wav",
                           "Recording 000001.wav");
    rig.renderBlocks(20);
    rig.tick();
    check(Access::capture(rig.adapter).phase == audio::RecordingPhase::idle &&
              rig.accepted() == 0 && rig.app.recordingPhase() == audio::RecordingPhase::failed &&
              !rig.adapter.transportSnapshot().playing,
          "resumed callbacks neither revive the take nor restart the transport");
    check(isTakePrefix(readMonoWav(rig.audio(".Recording 000001.1.part.wav")), accepted),
          "the retained temporary is untouched by the resumed callbacks");
    checkNewTakePublishes(rig, "Recording 000002.wav", 10);
}

// Watchdog versus an audioDeviceStopped notification, in both orders. The
// timer runs pollDeviceLifecycle() before the service pass, so a notification
// that is already pending wins; one that arrives after the watchdog fired finds
// the take terminal and changes nothing. Either way: one terminal transition.
void m1WatchdogAndStopNotification() {
    {
        Rig rig{"m1-notified-first"};
        rig.beginTake();
        rig.renderBlocks(10);
        rig.tick();
        const auto accepted = rig.accepted();
        rig.device().stop(); // notification pending, callbacks gone
        std::this_thread::sleep_for(watchdogThreshold + std::chrono::milliseconds(100));
        rig.tick();
        checkFailedAndRetained(rig, accepted,
                               "Recording stopped because the audio device was lost",
                               ".Recording 000001.1.part.wav", "Recording 000001.wav");
    }
    Rig rig{"m1-watchdog-first"};
    rig.beginTake();
    rig.renderBlocks(10);
    rig.tick();
    const auto accepted = rig.accepted();
    check(rig.tickUntilFailed(watchdogAcceptance).count() >= 0, "the watchdog fires first");
    rig.device().stop(); // the notification arrives afterwards
    rig.tick();
    checkFailedAndRetained(rig, accepted, stalledMessage, ".Recording 000001.1.part.wav",
                           "Recording 000001.wav");
}

// Application shutdown during a stall shorter than the threshold: the shutdown
// path terminalizes and retains the take; the watchdog plays no part.
void m1ShutdownDuringStall() {
    Rig rig{"m1-shutdown"};
    rig.beginTake();
    rig.renderBlocks(10);
    rig.tick();
    const auto accepted = rig.accepted();
    const auto quiet = Clock::now();
    while (Clock::now() - quiet < std::chrono::milliseconds(400)) {
        rig.tick();
        std::this_thread::sleep_for(std::chrono::milliseconds(15));
    }
    rig.app.shutdownRecording();
    checkFailedAndRetained(rig, accepted, "Recording cancelled during application shutdown",
                           ".Recording 000001.1.part.wav", "Recording 000001.wav");
}

// The C2-1 window: a pause shorter than the threshold followed by a resume is
// not the watchdog's to judge (it is a continuity question for C2-4, H-1). This
// case pins only the boundary - the watchdog must not claim it - and prints
// what the take became, without endorsing it.
void m1ShortPauseIsLeftToContinuityChecks() {
    Rig rig{"m1-short-pause"};
    rig.beginTake();
    std::uint64_t host = 1000000000ULL;
    for (int index = 0; index < 20; ++index) { rig.render(&host); host += blockNs48; }
    rig.tick();
    const auto paused = Clock::now();
    while (Clock::now() - paused < std::chrono::milliseconds(400)) {
        rig.tick();
        std::this_thread::sleep_for(std::chrono::milliseconds(15));
    }
    host += 400000000ULL;
    for (int index = 0; index < 20; ++index) { rig.render(&host); host += blockNs48; }
    rig.tick();
    rig.stopTake();
    observed(std::string{"400 ms pause between two watchdog thresholds: phase "} +
             phaseName(rig.app.recordingPhase()) + ", published " +
             (std::filesystem::exists(rig.audio("Recording 000001.wav")) ? "yes" : "no") +
             " (undetected splice: the H-1 window, for C2-4)");
    check(rig.app.recordingError().find(stalledMessage) == std::string::npos,
          "the watchdog does not claim a sub-threshold pause");
}

// ===========================================================================
// D2 - no callback at all after Record (M-1b and P-1, fixed in 0.8.3-C2-2b)
// ===========================================================================
// A take is "prepared" until the first callback consumes beginRecord. The
// watchdog supervises that phase too. Its deadline starts when the service
// first observes the prepared take; the first callback, which turns the take
// to capturing, is the progress that re-arms it (accepted frames cannot
// measure progress before that callback: none exist). The failure is
// session-scoped. A beginRecord that a resumed device consumes afterwards is
// refused by RealtimeCapture::begin(), which begins only the session that is
// prepared right now, so nothing is revived.

// Formerly red_m1b_prepared_stall (C2-1, finding M-1b). Its preconditions and
// contract are kept verbatim; the observation now also reports when the take
// failed, and the checks after the contract strengthen it.
void m1bPreparedStallTerminalizes() {
    Rig rig{"m1b-stop"};
    rig.offered = 0;
    const auto recorded = Clock::now();
    check(rig.send(commands::Record{}).status == commands::CommandStatus::accepted,
          "precondition: Record accepted");
    check(Access::capture(rig.adapter).phase == audio::RecordingPhase::prepared,
          "precondition: no callback has consumed beginRecord");
    check(rig.send(commands::Stop{}).status == commands::CommandStatus::accepted,
          "precondition: Stop accepted");
    const auto failedAfter = rig.tickUntilFailed(watchdogAcceptance);
    const auto sinceRecord =
        std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - recorded);
    observed("Stop pending, no callback since Record: failed " +
             std::to_string(sinceRecord.count()) + " ms after Record; capture " +
             phaseName(Access::capture(rig.adapter).phase) + ", application " +
             phaseName(rig.app.recordingPhase()) + ", writer open " +
             (Access::writerOpen(rig.adapter) ? "yes" : "no"));
    contract(failedAfter.count() >= 0, "M-1b",
             "a take whose device never calls back after Record must reach a terminal state "
             "without quitting VitaDAW");
    check(sinceRecord >= watchdogThreshold, "the watchdog does not fire before its threshold");
    checkFailedAndRetained(rig, 0, stalledMessage, ".Recording 000001.1.part.wav",
                           "Recording 000001.wav");
    check(rig.app.transport().playback == transport::PlaybackState::stopped,
          "the transport is stopped");
}

// Formerly red_p1_stale_begin_revives_cleaned_session (C2-1, finding P-1). Its
// original body and contract are kept verbatim; the checks after the contract
// strengthen it. Before C2-2b, begin() prepared an idle ring itself, so the
// queued beginRecord revived the cleaned-up session.
void p1StaleBeginCannotReviveCleanedSession() {
    Rig rig{"p1"};
    rig.offered = 0;
    check(rig.send(commands::Record{}).status == commands::CommandStatus::accepted &&
              Access::capture(rig.adapter).phase == audio::RecordingPhase::prepared,
          "precondition: prepared, beginRecord still queued");
    check(rig.send(commands::CancelRecording{}).status == commands::CommandStatus::accepted,
          "precondition: the prepared take is cancelled directly");
    rig.tick();
    check(rig.app.recordingPhase() == audio::RecordingPhase::failed &&
              Access::capture(rig.adapter).phase == audio::RecordingPhase::idle,
          "precondition: the take is cleaned up and the ring is idle");
    rig.render(); // the device resumes
    const auto after = Access::capture(rig.adapter);
    observed(std::string{"after one resumed callback: capture "} + phaseName(after.phase) +
             ", session " + std::to_string(after.session) + ", accepted frames " +
             std::to_string(after.acceptedDeviceFrames.value) + ", RT transport " +
             (rig.adapter.transportSnapshot().playing ? "playing" : "not playing"));
    contract(after.phase == audio::RecordingPhase::idle && after.session == 0,
             "P-1", "a cleaned-up session must never be prepared or begun again");
    check(after.acceptedDeviceFrames.value == 0 && !rig.adapter.transportSnapshot().playing,
          "the resumed callback captured nothing and started no transport");
    rig.renderBlocks(10);
    rig.tick();
    check(Access::capture(rig.adapter).phase == audio::RecordingPhase::idle &&
              rig.app.recordingPhase() == audio::RecordingPhase::failed &&
              rig.app.recordingError().find("Recording cancelled") == 0,
          "further callbacks change nothing, and the take stays cancelled");
    checkNewTakePublishes(rig, "Recording 000002.wav", 10);
}

// Record and nothing else: no callback and no command.
void m1bPreparedStallWithoutCommands() {
    Rig rig{"m1b-no-command"};
    const auto recorded = Clock::now();
    rig.beginTake();
    check(Access::capture(rig.adapter).phase == audio::RecordingPhase::prepared,
          "the take is prepared");
    check(rig.tickUntilFailed(watchdogAcceptance).count() >= 0,
          "a prepared take with neither callbacks nor commands is terminalized");
    check(Clock::now() - recorded >= watchdogThreshold, "not before the threshold");
    checkFailedAndRetained(rig, 0, stalledMessage, ".Recording 000001.1.part.wav",
                           "Recording 000001.wav");
    check(rig.app.transport().playback == transport::PlaybackState::stopped,
          "the failure path stops the projected transport");
}

// A slow device start, below the threshold, is not a stall: the first callback
// comes 700 ms after Record. That callback re-arms the deadline, so when the
// device then stalls, detection is measured from the first callback - not from
// Record, which would have failed the take about 300 ms after it.
void m1bFirstCallbackRearmsWatchdog() {
    Rig rig{"m1b-slow-start"};
    rig.beginTake();
    const auto recorded = Clock::now();
    while (Clock::now() - recorded < std::chrono::milliseconds(700)) {
        rig.tick();
        std::this_thread::sleep_for(std::chrono::milliseconds(15));
    }
    check(rig.app.recordingPhase() == audio::RecordingPhase::prepared &&
              Access::capture(rig.adapter).phase == audio::RecordingPhase::prepared,
          "700 ms without a first callback is below the threshold");
    rig.render(); // the first callback begins the take
    const auto first = Clock::now();
    const auto accepted = rig.accepted();
    check(Access::capture(rig.adapter).phase == audio::RecordingPhase::capturing &&
              accepted == blockFrames,
          "the first callback begins the take");
    check(rig.tickUntilFailed(watchdogAcceptance).count() >= 0,
          "the stall after the first callback is detected");
    const auto sinceFirst =
        std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - first);
    observed("first callback 700 ms after Record; failed " + std::to_string(sinceFirst.count()) +
             " ms after that callback");
    check(sinceFirst >= watchdogThreshold,
          "the threshold is measured from the first callback, not from Record");
    checkFailedAndRetained(rig, accepted, stalledMessage, ".Recording 000001.1.part.wav",
                           "Recording 000001.wav");
}

// A slow start followed by a healthy take: published intact.
void m1bSlowFirstCallbackIsNotAStall() {
    Rig rig{"m1b-slow-healthy"};
    rig.beginTake();
    const auto recorded = Clock::now();
    while (Clock::now() - recorded < std::chrono::milliseconds(700)) {
        rig.tick();
        std::this_thread::sleep_for(std::chrono::milliseconds(15));
    }
    const auto started = Clock::now();
    auto lastTick = started;
    while (Clock::now() - started < std::chrono::milliseconds(1200)) {
        rig.render();
        if (Clock::now() - lastTick >= std::chrono::milliseconds(30)) {
            rig.tick();
            lastTick = Clock::now();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(4));
    }
    rig.tick();
    check(rig.app.recordingPhase() == audio::RecordingPhase::capturing,
          "a 700 ms start-up followed by live callbacks never trips the watchdog");
    const auto frames = rig.offered;
    rig.stopTake();
    check(rig.app.recordingPhase() == audio::RecordingPhase::complete &&
              isTakePrefix(readMonoWav(rig.audio("Recording 000001.wav")), frames),
          "the take completes intact");
}

// The device resumes after a failure in "prepared": its queued beginRecord is
// refused, nothing is captured, the transport does not start, and the next
// take is normal.
void m1bDeviceResumesAfterPreparedFailure() {
    Rig rig{"m1b-resume"};
    rig.beginTake();
    check(rig.tickUntilFailed(watchdogAcceptance).count() >= 0, "the prepared stall is detected");
    checkFailedAndRetained(rig, 0, stalledMessage, ".Recording 000001.1.part.wav",
                           "Recording 000001.wav");
    rig.renderBlocks(20);
    rig.tick();
    const auto capture = Access::capture(rig.adapter);
    check(capture.phase == audio::RecordingPhase::idle && capture.session == 0 &&
              capture.acceptedDeviceFrames.value == 0 &&
              rig.app.recordingPhase() == audio::RecordingPhase::failed &&
              !rig.adapter.transportSnapshot().playing,
          "resumed callbacks neither begin the failed take nor start the transport");
    checkNewTakePublishes(rig, "Recording 000002.wav", 10);
}

// Stop before the first callback, device alive: one callback consumes
// beginRecord and Stop together, so the take completes with zero frames. An
// empty take is never committed or published (existing finalization rule).
void stopBeforeFirstCallbackLeavesNothing() {
    Rig rig{"stop-first"};
    rig.beginTake();
    check(rig.send(commands::Stop{}).status == commands::CommandStatus::accepted,
          "Stop accepted before any callback");
    rig.render();
    const auto capture = Access::capture(rig.adapter);
    check(capture.phase == audio::RecordingPhase::complete &&
              capture.acceptedDeviceFrames.value == 0,
          "the first callback completes the take with zero frames");
    rig.tick();
    observed(std::string{"Stop before the first callback: application "} +
             phaseName(rig.app.recordingPhase()) + ", message \"" + rig.app.recordingError() +
             "\"");
    check(rig.app.recordingPhase() == audio::RecordingPhase::failed &&
              rig.app.project().sources().empty() &&
              rig.app.project().tracks()[0].clips.empty() &&
              !std::filesystem::exists(rig.audio("Recording 000001.wav")) &&
              !Access::writerOpen(rig.adapter) &&
              rig.app.transport().playback == transport::PlaybackState::stopped,
          "the empty take ends terminal, uncommitted, unpublished, writer closed, transport "
          "stopped");
    checkNewTakePublishes(rig, "Recording 000002.wav", 10);
}

// Record, failure and recovery, several times in one run of the application:
// a stall while capturing, a stall while prepared, a Cancel before the first
// callback, a device loss with restart, then a normal take. Each failed take
// retains exactly its own media, and nothing leaks into the next take.
void cyclesOfFailureAndRecovery() {
    Rig rig{"cycles"};
    rig.beginTake();
    rig.renderBlocks(6);
    rig.tick();
    auto accepted = rig.accepted();
    check(rig.tickUntilFailed(watchdogAcceptance).count() >= 0, "cycle 1: capturing stall");
    checkFailedAndRetained(rig, accepted, stalledMessage, ".Recording 000001.1.part.wav",
                           "Recording 000001.wav");
    rig.renderBlocks(2); // the device resumes: queued commands are inert
    rig.tick();

    rig.beginTake();
    check(rig.tickUntilFailed(watchdogAcceptance).count() >= 0, "cycle 2: prepared stall");
    checkFailedAndRetained(rig, 0, stalledMessage, ".Recording 000002.2.part.wav",
                           "Recording 000002.wav");
    rig.renderBlocks(2);
    rig.tick();

    rig.beginTake();
    check(rig.send(commands::CancelRecording{}).status == commands::CommandStatus::accepted,
          "cycle 3: Cancel before the first callback");
    rig.tick();
    checkFailedAndRetained(rig, 0, "Recording cancelled", ".Recording 000003.3.part.wav",
                           "Recording 000003.wav");
    rig.renderBlocks(2);
    rig.tick();

    rig.beginTake();
    rig.renderBlocks(8);
    rig.tick();
    accepted = rig.accepted();
    rig.device().stop();
    rig.tick();
    checkFailedAndRetained(rig, accepted, "Recording stopped because the audio device was lost",
                           ".Recording 000004.4.part.wav", "Recording 000004.wav");
    rig.device().restart(48000.0);
    rig.tick();
    check(Access::engineState(rig.adapter) == audio::DeviceProcessingState::operational,
          "cycle 4: the restarted device is operational again");

    checkNewTakePublishes(rig, "Recording 000005.wav", 12);
    for (const auto* temporary : {".Recording 000001.1.part.wav", ".Recording 000004.4.part.wav"})
        check(std::filesystem::exists(rig.audio(temporary)),
              "earlier failed takes keep their retained media after later takes");
}

// ===========================================================================
// E - xruns and host time (H-1)
// ===========================================================================
// GREEN [D1]: a suspected xrun is diagnostic only; it must not abort a take.
// Covers "not available" (-1), zero, increments and a reset to a lower value.
void eSuspectedXrunsDoNotAbort() {
    Rig rig{"e-xruns"};
    rig.beginTake();
    const int sequence[] = {-1, -1, 0, 0, 1, 1, 3, 7, 0, 2};
    for (const auto xruns : sequence) {
        rig.device().xruns = xruns;
        rig.renderBlocks(3);
        rig.tick();
    }
    rig.stopTake();
    check(rig.app.recordingPhase() == audio::RecordingPhase::complete &&
              isTakePrefix(readMonoWav(rig.audio("Recording 000001.wav")), 30 * blockFrames),
          "xrun counter changes alone do not abort or alter a take");
}

// GREEN: a backend that provides no host time must not cause a failure.
void eMissingHostTimeIsNotAFailure() {
    Rig rig{"e-no-host-time"};
    rig.beginTake();
    rig.renderBlocks(40); // context.hostTimeNs == nullptr
    rig.tick();
    rig.stopTake();
    check(rig.app.recordingPhase() == audio::RecordingPhase::complete &&
              isTakePrefix(readMonoWav(rig.audio("Recording 000001.wav")), 40 * blockFrames),
          "absent host time is not treated as a discontinuity");
}

// RED [H-1]: a host-time gap of one full second between consecutive blocks
// (about 187 device cycles that never reached the callback) is a confirmed
// input discontinuity under D1. The gap is deliberately extreme so that no
// calibration choice in C2-4 could reasonably accept it.
void redHostTimeGapIsPublished() {
    Rig rig{"red-h1"};
    rig.beginTake();
    std::uint64_t host = 1000000000ULL;
    for (int index = 0; index < 100; ++index) { rig.render(&host); host += blockNs48; }
    host += 1000000000ULL; // one second of cycles never delivered
    for (int index = 0; index < 100; ++index) { rig.render(&host); host += blockNs48; }
    rig.tick();
    rig.stopTake();
    const auto published = std::filesystem::exists(rig.audio("Recording 000001.wav"));
    observed(std::string{"phase "} + phaseName(rig.app.recordingPhase()) + ", published " +
             (published ? "yes" : "no") + " with a 1 s host-time gap spliced out");
    contract(rig.app.recordingPhase() == audio::RecordingPhase::failed && !published,
             "H-1", "a confirmed host-time discontinuity must fail the take, not publish it");
}

// ===========================================================================
// F - the interface in use disappears (DF-1, T-6, M-2)
// ===========================================================================
// JUCE's own AudioDeviceManager::audioDeviceListChanged runs: it closes the
// vanished device and calls initialiseDefault(preferredName, &currentSetup).
// That copies the current setup, which names the vanished interface, and
// insertDefaultDeviceNames only fills EMPTY names - so JUCE retries the same,
// missing device and ends with none (juce_AudioDeviceManager.cpp:271-311).
// The fake only supplies device enumeration; the fallback logic is JUCE's.
void fInterfaceLossFailsTakeAndLeavesNoDevice() {
    Rig rig{"f-loss"};
    rig.beginTake();
    rig.renderBlocks(10);
    rig.tick();
    const auto accepted = rig.accepted();
    check(Access::currentInputDevice(rig.adapter) == interfaceA, "recording from Interface A");
    rig.type->remove(interfaceA);
    rig.tick();
    checkFailedAndRetained(rig, accepted, "Recording stopped because the audio device was lost",
                           ".Recording 000001.1.part.wav", "Recording 000001.wav");
    check(rig.type->current == nullptr &&
              rig.adapter.state().status == audio::AudioDeviceStatus::error,
          "JUCE opens no fallback and VitaDAW reports the device error (no silent source change)");
    check(rig.send(commands::Record{}).status == commands::CommandStatus::rejected,
          "without a device a new Record is refused rather than recorded elsewhere");
}

// RED [R-2]: the interface comes back, but nothing reopens it. JUCE only
// reacts to list changes while it still has a current device (:273), and
// VitaDAW has no device-selection path, so recording needs an application
// restart.
void redReconnectedInterfaceIsNotUsable() {
    Rig rig{"red-r2"};
    rig.beginTake();
    rig.renderBlocks(10);
    rig.tick();
    rig.type->remove(interfaceA);
    rig.tick();
    check(rig.app.recordingPhase() == audio::RecordingPhase::failed &&
              rig.type->current == nullptr,
          "precondition: the take failed and no device is open");
    rig.type->add(interfaceA);
    for (int index = 0; index < 5; ++index) rig.tick();
    rig.offered = 0;
    const auto record = rig.send(commands::Record{});
    observed(std::string{"after Interface A reappears: device open "} +
             (rig.type->current != nullptr ? "yes" : "no") + ", device status " +
             (rig.adapter.state().status == audio::AudioDeviceStatus::active ? "active" : "not active") +
             ", Record -> " + (record.status == commands::CommandStatus::accepted
                                   ? std::string{"accepted"} : "rejected: " + record.message));
    contract(record.status == commands::CommandStatus::accepted,
             "R-2", "a reconnected interface must allow a new take without restarting VitaDAW");
}

// ===========================================================================
// G - terminal concurrency (T-7, T-8)
// ===========================================================================
// Stop consumed first, then the device is lost before the timer: every frame
// up to Stop was captured, so publishing is correct.
void g1StopThenDeviceLossPublishes() {
    Rig rig{"g1"};
    rig.beginTake();
    rig.renderBlocks(14);
    check(rig.send(commands::Stop{}).status == commands::CommandStatus::accepted, "Stop");
    rig.render(); // Stop consumed: the take is complete
    check(Access::capture(rig.adapter).phase == audio::RecordingPhase::complete,
          "the take completed before the loss");
    rig.device().stop();
    rig.tick();
    check(rig.app.recordingPhase() == audio::RecordingPhase::complete &&
              rig.app.project().tracks()[0].clips.size() == 1 &&
              isTakePrefix(readMonoWav(rig.audio("Recording 000001.wav")), 14 * blockFrames),
          "a take completed before the device loss is published intact");
}

// The loss arrives while Stop is still queued: the take cannot be proven
// complete, so it fails.
void g2DeviceLossBeforeStopIsConsumed() {
    Rig rig{"g2"};
    rig.beginTake();
    rig.renderBlocks(14);
    const auto accepted = rig.accepted();
    check(rig.send(commands::Stop{}).status == commands::CommandStatus::accepted, "Stop");
    rig.device().stop(); // no callback consumed the Stop
    rig.tick();
    checkFailedAndRetained(rig, accepted, "Recording stopped because the audio device was lost",
                           ".Recording 000001.1.part.wav", "Recording 000001.wav");
}

// Overflow first, then device loss: the first terminal transition wins.
void g3OverflowThenDeviceLoss() {
    Rig rig{"g3"};
    rig.beginTake();
    // No timer pass: the ring overflows. The take is still "prepared" until
    // the first callback consumes beginRecord, so loop until it is terminal.
    for (int guard = 0; guard < 1000 && !Access::capture(rig.adapter).terminal(); ++guard)
        rig.render();
    const auto accepted = rig.accepted();
    check(Access::capture(rig.adapter).failure == audio::RecordingFailure::overflow,
          "the ring overflowed first");
    rig.device().stop();
    rig.tick();
    checkFailedAndRetained(rig, accepted,
                           "Recording stopped because the capture buffer overflowed",
                           ".Recording 000001.1.part.wav", "Recording 000001.wav");
    check(Access::telemetry(rig.adapter).overflowEvents == 0,
          "cleanup reset the ring telemetry for the next take");
}

// Stop queued, then a device restart: the restart cancels the queued command
// and fails the take.
void g4StopPendingThenRestart() {
    Rig rig{"g4"};
    rig.beginTake();
    rig.renderBlocks(14);
    const auto accepted = rig.accepted();
    check(rig.send(commands::Stop{}).status == commands::CommandStatus::accepted, "Stop");
    rig.device().restart(48000.0);
    rig.renderBlocks(2);
    rig.tick();
    checkFailedAndRetained(rig, accepted, "Recording stopped because the audio device was lost",
                           ".Recording 000001.1.part.wav", "Recording 000001.wav");
}

// T-8: the take completed, then the device is lost before finalization runs.
// The invariant is consistency: either the take is committed AND published,
// or it is neither committed nor presented as a valid take.
void t8DeviceLossDuringFinalization() {
    Rig rig{"t8"};
    rig.beginTake();
    rig.renderBlocks(14);
    check(rig.send(commands::Stop{}).status == commands::CommandStatus::accepted, "Stop");
    rig.render();
    rig.device().stop();
    rig.tick(); // poll marks the device lost, then finalization runs
    const auto committed = rig.app.project().tracks()[0].clips.size() == 1;
    const auto published = std::filesystem::exists(rig.audio("Recording 000001.wav"));
    observed(std::string{"after device loss before finalization: phase "} +
             phaseName(rig.app.recordingPhase()) + ", committed " + (committed ? "yes" : "no") +
             ", published " + (published ? "yes" : "no") + ", message \"" +
             rig.app.recordingError() + "\"");
    if (committed) {
        check(rig.app.recordingPhase() == audio::RecordingPhase::complete && published &&
                  isTakePrefix(readMonoWav(rig.audio("Recording 000001.wav")), 14 * blockFrames),
              "a committed take is complete, published and intact");
    } else {
        check(rig.app.recordingPhase() == audio::RecordingPhase::failed &&
                  rig.app.project().sources().empty() && !rig.app.recordingError().empty(),
              "an uncommitted take is reported as failed with a reason");
    }
}


} // namespace

int main(int argc, char** argv) {
    const std::map<std::string, std::function<void()>> cases{
        {"t1_device_stop_fails_take_and_recovers", t1DeviceStopFailsTakeAndRecovers},
        {"t2a_juce_drops_device_errors", t2aJuceDropsDeviceErrors},
        {"t2b_direct_error_path_is_fail_safe", t2bDirectErrorPathIsFailSafe},
        {"t3_restart_fails_take_without_silent_continuation",
         t3RestartFailsTakeWithoutSilentContinuation},
        {"t4_divergence_fails_before_reprepare", t4DivergenceFailsBeforeReprepare},
        {"t5_buffer_change_rejected_while_capturing", t5BufferChangeRejectedWhileCapturing},
        {"t8_device_loss_during_finalization", t8DeviceLossDuringFinalization},
        {"c_rate_window_then_restart_fails_take", cRateWindowThenRestartFailsTake},
        {"e_suspected_xruns_do_not_abort", eSuspectedXrunsDoNotAbort},
        {"e_missing_host_time_is_not_a_failure", eMissingHostTimeIsNotAFailure},
        {"f_interface_loss_fails_take_and_leaves_no_device",
         fInterfaceLossFailsTakeAndLeavesNoDevice},
        {"g1_stop_then_device_loss_publishes", g1StopThenDeviceLossPublishes},
        {"g2_device_loss_before_stop_is_consumed", g2DeviceLossBeforeStopIsConsumed},
        {"g3_overflow_then_device_loss", g3OverflowThenDeviceLoss},
        {"g4_stop_pending_then_restart", g4StopPendingThenRestart},
        {"red_h2_stop_inside_rate_window", redStopInsideRateWindowPublishes},
        {"red_h1_host_time_gap", redHostTimeGapIsPublished},
        {"m1_stop_pending_stall_fails_take", m1StopPendingStallFailsTake},
        {"m1_sustained_stall_fails_take", m1SustainedStallFailsTake},
        {"m1_cancel_pending_stall_fails_take", m1CancelPendingStallFailsTake},
        {"m1_continuous_callbacks_never_trip", m1ContinuousCallbacksNeverTrip},
        {"m1_silent_input_is_not_a_stall", m1SilentInputIsNotAStall},
        {"m1_message_thread_delay_with_live_callbacks", m1MessageThreadDelayWithLiveCallbacks},
        {"m1_late_callback_rearms_watchdog", m1LateCallbackRearmsWatchdog},
        {"m1_concurrent_callbacks_then_stall", m1ConcurrentCallbacksThenStall},
        {"m1_overflow_wins_over_watchdog", m1OverflowWinsOverWatchdog},
        {"m1_watchdog_and_stop_notification", m1WatchdogAndStopNotification},
        {"m1_shutdown_during_stall", m1ShutdownDuringStall},
        {"c1_prepared_transition_under_concurrency", c1PreparedTransitionUnderConcurrency},
        {"m1_device_resumes_after_stall_failure", m1DeviceResumesAfterStallFailure},
        {"m1_short_pause_is_left_to_continuity_checks", m1ShortPauseIsLeftToContinuityChecks},
        {"m1b_prepared_stall_terminalizes", m1bPreparedStallTerminalizes},
        {"p1_stale_begin_cannot_revive_cleaned_session", p1StaleBeginCannotReviveCleanedSession},
        {"m1b_prepared_stall_without_commands", m1bPreparedStallWithoutCommands},
        {"m1b_first_callback_rearms_watchdog", m1bFirstCallbackRearmsWatchdog},
        {"m1b_slow_first_callback_is_not_a_stall", m1bSlowFirstCallbackIsNotAStall},
        {"m1b_device_resumes_after_prepared_failure", m1bDeviceResumesAfterPreparedFailure},
        {"stop_before_first_callback_leaves_nothing", stopBeforeFirstCallbackLeavesNothing},
        {"cycles_of_failure_and_recovery", cyclesOfFailureAndRecovery},
        {"red_r2_reconnected_interface_unusable", redReconnectedInterfaceIsNotUsable},
    };
    if (argc != 2 || cases.find(argv[1]) == cases.end()) {
        std::cerr << "usage: vitadaw_recording_device_failure_tests <case>\n";
        for (const auto& [name, unused] : cases) std::cerr << "  " << name << '\n';
        return 2;
    }
    cases.at(argv[1])();
    std::cout << "PASS " << argv[1] << '\n';
    return 0;
}
