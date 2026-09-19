#include "vitadaw/application/DawApplication.h"
#include "vitadaw/commands/CommandDispatcher.h"
#include "vitadaw/platform/juce/JuceAudioDeviceAdapter.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <thread>
#include <vector>

namespace {

constexpr auto deviceTypeName = "VitaDAW monitoring test device";
constexpr auto inputName = "VitaDAW monitoring test input";
constexpr auto outputName = "VitaDAW monitoring test output";

void check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

class VirtualAudioDevice final : public juce::AudioIODevice {
public:
    VirtualAudioDevice(int inputChannels, bool rejectInputs,
                       bool inputDeviceAttached,
                       VirtualAudioDevice** productiveSlot)
        : AudioIODevice("VitaDAW monitoring test device", deviceTypeName),
          inputChannelsAvailable_(inputChannels),
          inputDeviceAttached_(inputDeviceAttached),
          productiveSlot_(productiveSlot),
          rejectInputOpen(rejectInputs) {}

    juce::StringArray getOutputChannelNames() override { return {"Left", "Right"}; }
    juce::StringArray getInputChannelNames() override {
        if (!inputDeviceAttached_) return {};
        return inputChannelsAvailable_ == 2 ? juce::StringArray{"Input 1", "Input 2"}
                                            : juce::StringArray{"Input 1"};
    }
    juce::Array<double> getAvailableSampleRates() override { return {44100.0, 48000.0}; }
    juce::Array<int> getAvailableBufferSizes() override {
        juce::Array<int> sizes;
        for (const auto size : availableBufferSizes_) sizes.add(size);
        return sizes;
    }
    int getDefaultBufferSize() override { return 256; }
    juce::String open(const juce::BigInteger& input, const juce::BigInteger& output,
                      double rate, int buffer) override {
        ++openCount;
        if (rejectInputOpen && input.countNumberOfSetBits() != 0)
            return "input open rejected by test device";
        if (input.countNumberOfSetBits() > inputChannelsAvailable_)
            return "requested input channels are unavailable";
        inputChannels_ = input;
        outputChannels_ = output;
        sampleRate_ = effectiveSampleRateOverride > 0.0 ? effectiveSampleRateOverride : rate;
        if (sharedFailOpenAttempts != nullptr && *sharedFailOpenAttempts > 0) {
            --*sharedFailOpenAttempts;
            return "buffer open rejected by test device";
        }
        bufferSize_ = effectiveBufferOverride > 0 ? effectiveBufferOverride : buffer;
        if (effectiveInputChannelCountOverride >= 0) {
            inputChannels_.clear();
            inputChannels_.setRange(0, effectiveInputChannelCountOverride, true);
        }
        if (effectiveOutputChannelCountOverride >= 0) {
            outputChannels_.clear();
            outputChannels_.setRange(0, effectiveOutputChannelCountOverride, true);
        }
        open_ = true;
        if (productiveSlot_ != nullptr) *productiveSlot_ = this;
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
    int getCurrentBufferSizeSamples() override {
        ++currentBufferSizeQueries;
        return bufferSize_;
    }
    double getCurrentSampleRate() override { return sampleRate_; }
    int getCurrentBitDepth() override { return 32; }
    juce::BigInteger getActiveOutputChannels() const override { return outputChannels_; }
    juce::BigInteger getActiveInputChannels() const override { return inputChannels_; }
    int getOutputLatencyInSamples() override {
        ++outputLatencyQueries;
        return outputLatency;
    }
    int getInputLatencyInSamples() override {
        ++inputLatencyQueries;
        return inputLatency;
    }

    void render(float sample, std::size_t frames = 256) {
        check(frames <= 4096, "virtual callback buffer capacity");
        std::array<float, 4096> input0{}, input1{}, output0{}, output1{};
        input0.fill(sample);
        input1.fill(sample * 0.5F);
        std::array<const float*, 2> input{input0.data(), input1.data()};
        std::array<float*, 2> output{output0.data(), output1.data()};
        if (callback_ != nullptr) {
            callback_->audioDeviceIOCallbackWithContext(
                input.data(), inputChannels_.countNumberOfSetBits(), output.data(),
                outputChannels_.countNumberOfSetBits(),
                static_cast<int>(frames), {});
        }
        lastOutput = output0[frames - 1];
    }

    void renderPhysicalLoopback(std::size_t delayFrames,
                                std::size_t frames = 256,
                                bool returnOutput = true) {
        check(frames <= 4096 && delayFrames + frames < loopbackLine_.size(),
              "virtual physical loopback capacity");
        std::array<float, 4096> input0{}, output0{};
        for (std::size_t frame = 0; frame < frames; ++frame) {
            const auto position = static_cast<std::size_t>(
                (loopbackFrame_ + frame) % loopbackLine_.size());
            input0[frame] = loopbackLine_[position];
            loopbackLine_[position] = 0.0F;
        }
        std::array<const float*, 1> input{input0.data()};
        std::array<float*, 1> output{output0.data()};
        if (callback_ != nullptr) {
            callback_->audioDeviceIOCallbackWithContext(
                input.data(), inputChannels_.countNumberOfSetBits(), output.data(),
                outputChannels_.countNumberOfSetBits(), static_cast<int>(frames), {});
        }
        if (returnOutput) {
            for (std::size_t frame = 0; frame < frames; ++frame) {
                const auto position = static_cast<std::size_t>(
                    (loopbackFrame_ + frame + delayFrames) % loopbackLine_.size());
                loopbackLine_[position] += output0[frame];
            }
        }
        loopbackFrame_ += frames;
        lastOutput = output0[frames - 1];
    }

    void setReportedBufferSize(int buffer) noexcept { bufferSize_ = buffer; }
    void setReportedSampleRate(double rate) noexcept { sampleRate_ = rate; }
    void setSupportedBufferSizes(std::vector<int> sizes) {
        availableBufferSizes_ = std::move(sizes);
    }
    void setAvailableInputChannels(int channels) noexcept {
        inputChannelsAvailable_ = channels;
    }
    void restart() { start(callback_); }
    void reportError() {
        if (callback_ != nullptr) callback_->audioDeviceError("virtual test error");
    }

    bool rejectInputOpen{};
    int openCount{};
    float lastOutput{};
    int effectiveBufferOverride{};
    double effectiveSampleRateOverride{};
    int effectiveInputChannelCountOverride{-1};
    int effectiveOutputChannelCountOverride{-1};
    int failOpenAttempts{};
    int* sharedFailOpenAttempts{};
    int inputLatency{48};
    int outputLatency{96};
    int currentBufferSizeQueries{};
    int inputLatencyQueries{};
    int outputLatencyQueries{};

private:
    int inputChannelsAvailable_{};
    bool inputDeviceAttached_{};
    VirtualAudioDevice** productiveSlot_{};
    std::vector<int> availableBufferSizes_{16, 32, 64, 128, 256, 512, 1024, 2048, 4096};
    juce::AudioIODeviceCallback* callback_{};
    juce::BigInteger inputChannels_;
    juce::BigInteger outputChannels_;
    double sampleRate_{48000.0};
    int bufferSize_{256};
    bool open_{};
    bool playing_{};
    std::array<float, 16384> loopbackLine_{};
    std::uint64_t loopbackFrame_{};
};

class VirtualAudioDeviceType final : public juce::AudioIODeviceType {
public:
    explicit VirtualAudioDeviceType(int inputs)
        : AudioIODeviceType(deviceTypeName), inputs_(inputs) {}

    void scanForDevices() override {}
    juce::StringArray getDeviceNames(bool wantInput) const override {
        return wantInput ? juce::StringArray{inputName} : juce::StringArray{outputName};
    }
    int getDefaultDeviceIndex(bool) const override { return 0; }
    int getIndexOfDevice(juce::AudioIODevice*, bool) const override { return 0; }
    bool hasSeparateInputsAndOutputs() const override { return true; }
    juce::AudioIODevice* createDevice(const juce::String& output,
                                      const juce::String& input) override {
        if (output != outputName || (input != inputName && input.isNotEmpty())) return nullptr;
        auto* device = new VirtualAudioDevice(
            inputs_, rejectInputOpen, input.isNotEmpty(), &lastDevice);
        device->setSupportedBufferSizes(supportedBufferSizes);
        device->effectiveBufferOverride = effectiveBufferOverride;
        device->effectiveSampleRateOverride = effectiveSampleRateOverride;
        device->effectiveInputChannelCountOverride = effectiveInputChannelCountOverride;
        device->effectiveOutputChannelCountOverride = effectiveOutputChannelCountOverride;
        device->sharedFailOpenAttempts = &failOpenAttempts;
        device->inputLatency = inputLatency;
        device->outputLatency = outputLatency;
        return device;
    }

    VirtualAudioDevice* lastDevice{};
    bool rejectInputOpen{};
    std::vector<int> supportedBufferSizes{16, 32, 64, 128, 256, 512, 1024, 2048, 4096};
    int effectiveBufferOverride{};
    double effectiveSampleRateOverride{};
    int effectiveInputChannelCountOverride{-1};
    int effectiveOutputChannelCountOverride{-1};
    int failOpenAttempts{};
    int inputLatency{48};
    int outputLatency{96};

private:
    int inputs_{};
};

} // namespace

namespace vitadaw::platform::juce_adapter {

class MonitoringIntegrationAccess {
public:
    static bool openOutputOnly(JuceAudioDeviceAdapter& adapter, int inputs,
                               VirtualAudioDeviceType*& type) {
        auto owned = std::make_unique<VirtualAudioDeviceType>(inputs);
        type = owned.get();
        adapter.deviceManager_.addAudioDeviceType(std::move(owned));
        adapter.deviceManager_.setCurrentAudioDeviceType(deviceTypeName, false);
        juce::AudioDeviceManager::AudioDeviceSetup setup;
        setup.outputDeviceName = outputName;
        setup.sampleRate = 48000.0;
        setup.bufferSize = 256;
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

    static bool openDefaultOutput(JuceAudioDeviceAdapter& adapter, int inputs,
                                  VirtualAudioDeviceType*& type) {
        auto owned = std::make_unique<VirtualAudioDeviceType>(inputs);
        type = owned.get();
        adapter.deviceManager_.addAudioDeviceType(std::move(owned));
        adapter.deviceManager_.setCurrentAudioDeviceType(deviceTypeName, false);
        if (adapter.deviceManager_.initialiseWithDefaultDevices(0, 2).isNotEmpty())
            return false;
        auto* device = adapter.deviceManager_.getCurrentAudioDevice();
        if (device == nullptr) return false;
        adapter.deviceSampleRate_ = timeline::SampleRate{device->getCurrentSampleRate()};
        std::string error;
        if (!adapter.reprepareForCurrentDevice(error)) return false;
        adapter.attachAudioCallback(true);
        adapter.refreshState();
        const auto setup = adapter.deviceManager_.getAudioDeviceSetup();
        return setup.useDefaultOutputChannels &&
               device->getActiveInputChannels().isZero() &&
               device->getActiveOutputChannels().countNumberOfSetBits() == 2 &&
               adapter.callbackRegistered_ &&
               adapter.realtimeEngine_.deviceState() ==
                   audio::DeviceProcessingState::operational;
    }

    static VirtualAudioDevice& device(VirtualAudioDeviceType& type) {
        return *type.lastDevice;
    }
    static bool monitoringDemand(const JuceAudioDeviceAdapter& adapter) {
        return adapter.monitoringInputDemand_;
    }
    static int monitoringChannels(const JuceAudioDeviceAdapter& adapter) {
        return adapter.monitoringInputChannels_;
    }
    static audio::RecordingSnapshot recording(const JuceAudioDeviceAdapter& adapter) {
        return adapter.realtimeEngine_.recordingSnapshot();
    }
    static bool fillRtCommandQueue(JuceAudioDeviceAdapter& adapter) {
        while (adapter.realtimeEngine_.trySetInputMonitoringEnabled(false).accepted) {}
        return true;
    }
    static audio::RealtimeTransportSnapshot snapshot(const JuceAudioDeviceAdapter& adapter) {
        return adapter.realtimeEngine_.transportSnapshot();
    }
    static audio::DeviceProcessingState deviceState(const JuceAudioDeviceAdapter& adapter) {
        return adapter.realtimeEngine_.deviceState();
    }
    static bool callbackRegistered(const JuceAudioDeviceAdapter& adapter) {
        return adapter.callbackRegistered_;
    }
    static void refreshState(JuceAudioDeviceAdapter& adapter) {
        adapter.refreshState();
    }
    static juce::AudioDeviceManager::AudioDeviceSetup setup(
        const JuceAudioDeviceAdapter& adapter) {
        return adapter.deviceManager_.getAudioDeviceSetup();
    }
    static void reportDeviceError(JuceAudioDeviceAdapter& adapter) {
        adapter.audioDeviceError({});
    }
    static void controlledRateChange(JuceAudioDeviceAdapter& adapter, double rate) {
        const auto callbackWasRegistered = adapter.callbackRegistered_;
        adapter.detachAudioCallback(true);
        const auto checkpoint = adapter.realtimeEngine_.temporalCheckpoint();
        auto setup = adapter.deviceManager_.getAudioDeviceSetup();
        setup.sampleRate = rate;
        check(adapter.deviceManager_.setAudioDeviceSetup(setup, false).isEmpty(),
              "test device accepts controlled rate change");
        auto* device = adapter.deviceManager_.getCurrentAudioDevice();
        adapter.deviceSampleRate_ = timeline::SampleRate{device->getCurrentSampleRate()};
        std::string error;
        check(adapter.reprepareForCurrentDevice(error) &&
                  adapter.realtimeEngine_.restoreTemporalCheckpoint(checkpoint),
              "controlled rate change reprepares the productive engine");
        if (callbackWasRegistered) adapter.attachAudioCallback(true);
        adapter.refreshState();
    }
    static std::size_t blockCapacity(const JuceAudioDeviceAdapter& adapter) {
        return adapter.realtimeEngine_.preparedBlockCapacity();
    }
    static void failNextMonitoringStagingPreparation(JuceAudioDeviceAdapter& adapter) {
        // Reprepare first retires old views through the legacy configuration,
        // then installs the candidate plan. Fail both preparation calls so the
        // tested failure is the candidate plan's staging allocation.
        adapter.realtimeEngine_.forcedMonitoringStagingPreparationFailuresForTesting_ = 2;
    }
    static audio::RealtimeLoopbackProbeStatus loopbackProbeStatus(
        const JuceAudioDeviceAdapter& adapter) {
        return adapter.loopbackProbe_.status();
    }
    static std::uint64_t thirdLoopbackEmission(
        const JuceAudioDeviceAdapter& adapter) {
        return adapter.loopbackProbe_.emittedFrames()[2];
    }
};

} // namespace vitadaw::platform::juce_adapter

namespace {
using namespace vitadaw;
using Adapter = platform::juce_adapter::JuceAudioDeviceAdapter;
using Access = platform::juce_adapter::MonitoringIntegrationAccess;

void outputOnlyEnableAndIdempotence() {
    Adapter adapter;
    application::DawApplication app{adapter, timeline::SampleRate{48000.0}};
    VirtualAudioDeviceType* type{};
    check(Access::openOutputOnly(adapter, 2, type), "open productive adapter with zero inputs");
    commands::CommandDispatcher dispatcher{app};
    check(Access::device(*type).getActiveInputChannels().countNumberOfSetBits() == 0,
          "fixture starts with no active input");
    const auto offCapabilities = app.deviceLatencyReadModel().inputChannelNames;
    check(offCapabilities == std::vector<std::string>({"Input 1", "Input 2"}),
          "output-only device publishes physical input capabilities while Monitoring is OFF");
    check(dispatcher.dispatch(commands::EnableInputMonitoring{}).status ==
              commands::CommandStatus::accepted,
          "enable monitoring opens input through command/application/adapter path");
    auto& device = Access::device(*type); // setAudioDeviceSetup may replace the device object.
    const auto opensAfterEnable = device.openCount;
    device.render(1.0F);
    app.synchroniseTransport();
    check(app.inputMonitoringEnabled() && Access::monitoringDemand(adapter) &&
              Access::monitoringChannels(adapter) == 2 && device.lastOutput > 0.1F,
          "stereo input is active and productive monitoring is confirmed after RT callback");
    check(app.deviceLatencyReadModel().inputChannelNames == offCapabilities,
          "Monitoring ON preserves the same physical input capability list");
    check(dispatcher.dispatch(commands::EnableInputMonitoring{}).status ==
              commands::CommandStatus::accepted && device.openCount == opensAfterEnable,
          "idempotent enable does not reconfigure hardware");
    check(dispatcher.dispatch(commands::DisableInputMonitoring{}).status ==
              commands::CommandStatus::accepted,
          "disable publishes only the monitoring route change");
    device.render(1.0F);
    app.synchroniseTransport();
    check(!app.inputMonitoringEnabled() && !Access::monitoringDemand(adapter) &&
              device.getActiveInputChannels().countNumberOfSetBits() == 2,
          "disable releases monitoring demand but leaves physical input stable");
    check(app.deviceLatencyReadModel().inputChannelNames == offCapabilities,
          "Monitoring ON to OFF does not erase or change input capabilities");

    device.setAvailableInputChannels(1);
    Access::refreshState(adapter);
    check(app.deviceLatencyReadModel().inputChannelNames ==
              std::vector<std::string>({"Input 1"}),
          "physical input capability change refreshes the read model");
    device.setAvailableInputChannels(2);
    Access::refreshState(adapter);
    check(app.deviceLatencyReadModel().inputChannelNames == offCapabilities,
          "subsequent physical capability refresh replaces stale channel names");

    Access::reportDeviceError(adapter);
    adapter.pollDeviceLifecycle();
    check(!app.deviceLatencyReadModel().configurationAvailable &&
              app.deviceLatencyReadModel().inputChannelNames.empty(),
          "device loss invalidates physical input capabilities instead of retaining stale names");
}

void monoFallbackAndRollback() {
    Adapter monoAdapter;
    application::DawApplication monoApp{monoAdapter, timeline::SampleRate{48000.0}};
    VirtualAudioDeviceType* monoType{};
    check(Access::openOutputOnly(monoAdapter, 1, monoType), "open mono test device output-only");
    commands::CommandDispatcher monoDispatcher{monoApp};
    check(monoDispatcher.dispatch(commands::EnableInputMonitoring{}).status ==
              commands::CommandStatus::accepted,
          "monitoring falls back from stereo request to mono route");
    Access::device(*monoType).render(1.0F);
    monoApp.synchroniseTransport();
    check(monoApp.inputMonitoringEnabled() && Access::monitoringChannels(monoAdapter) == 1,
          "mono fallback is the certified foundation route");

    Adapter rejectedAdapter;
    application::DawApplication rejectedApp{rejectedAdapter, timeline::SampleRate{48000.0}};
    VirtualAudioDeviceType* rejectedType{};
    check(Access::openOutputOnly(rejectedAdapter, 2, rejectedType), "open rollback fixture");
    rejectedType->rejectInputOpen = true;
    commands::CommandDispatcher rejectedDispatcher{rejectedApp};
    const auto result = rejectedDispatcher.dispatch(commands::EnableInputMonitoring{});
    check(result.status == commands::CommandStatus::rejected &&
              !rejectedApp.inputMonitoringEnabled() && !Access::monitoringDemand(rejectedAdapter) &&
              Access::device(*rejectedType).getActiveInputChannels().countNumberOfSetBits() == 0,
          "failed enable rolls back the output-only configuration and leaves monitoring off");
}

void rejectedRtPublicationRollsBackPreflight() {
    Adapter adapter;
    application::DawApplication app{adapter, timeline::SampleRate{48000.0}};
    VirtualAudioDeviceType* type{};
    check(Access::openOutputOnly(adapter, 2, type), "open queue-rejection fixture");
    check(Access::fillRtCommandQueue(adapter), "fill bounded RT command queue");
    commands::CommandDispatcher dispatcher{app};
    const auto result = dispatcher.dispatch(commands::EnableInputMonitoring{});
    check(result.status == commands::CommandStatus::rejected &&
              !Access::monitoringDemand(adapter) &&
              Access::device(*type).getActiveInputChannels().countNumberOfSetBits() == 0 &&
              !Access::snapshot(adapter).monitoringEnabled,
          "rejected RT Enable rolls back monitoring preflight and preserves output-only setup");
}

void bufferLifecycleAndForcedLossIntent() {
    Adapter adapter;
    application::DawApplication app{adapter, timeline::SampleRate{48000.0}};
    VirtualAudioDeviceType* type{};
    check(Access::openOutputOnly(adapter, 2, type), "open buffer lifecycle fixture");
    commands::CommandDispatcher dispatcher{app};
    check(dispatcher.dispatch(commands::EnableInputMonitoring{}).status ==
              commands::CommandStatus::accepted,
          "enable before buffer-only lifecycle change");
    auto& device = Access::device(*type);
    device.render(0.5F);
    app.synchroniseTransport();
    device.setReportedBufferSize(128);
    adapter.pollDeviceLifecycle();
    device.render(0.5F, 128);
    app.synchroniseTransport();
    const auto afterBuffer = Access::snapshot(adapter);
    check(app.inputMonitoringEnabled() && Access::monitoringDemand(adapter) &&
              afterBuffer.monitoringRouteSupported && device.lastOutput > 0.01F &&
              app.deviceLatencyReadModel().confirmedBufferSizeFrames == 128,
          "buffer-only divergence reprepares staging, refreshes read model and preserves monitoring");

    // Invalid current hardware is a real controlled-reprepare failure. The
    // productive polling path must force OFF and clear desired application intent.
    device.setReportedBufferSize(0);
    adapter.pollDeviceLifecycle();
    app.synchroniseTransport();
    check(!app.inputMonitoringEnabled() && !Access::monitoringDemand(adapter) &&
              Access::snapshot(adapter).monitoringLifecycleForcedOff &&
              adapter.state().status == audio::AudioDeviceStatus::error &&
              !app.deviceLatencyReadModel().configurationAvailable,
          "failed buffer lifecycle reprepare invalidates stale device data and forces Monitoring off");
}

void deviceLossClearsToggleIntentAndErrorIsDeferred() {
    Adapter adapter;
    application::DawApplication app{adapter, timeline::SampleRate{48000.0}};
    VirtualAudioDeviceType* type{};
    check(Access::openOutputOnly(adapter, 2, type), "open device-loss fixture");
    commands::CommandDispatcher dispatcher{app};
    check(dispatcher.dispatch(commands::EnableInputMonitoring{}).status ==
              commands::CommandStatus::accepted,
          "enable before device loss");
    auto& device = Access::device(*type);
    device.render(0.5F);
    app.synchroniseTransport();
    device.stop();
    adapter.pollDeviceLifecycle();
    app.synchroniseTransport();
    check(!app.inputMonitoringEnabled() && !Access::monitoringDemand(adapter) &&
              Access::snapshot(adapter).monitoringLifecycleForcedOff,
          "device loss confirms lifecycle-forced monitoring off");
    device.restart();
    device.render(0.0F);
    check(dispatcher.dispatch(commands::ToggleInputMonitoring{}).status ==
              commands::CommandStatus::accepted,
          "Toggle after forced loss issues one fresh Enable rather than a stale Disable");
    device.render(0.5F);
    app.synchroniseTransport();
    check(app.inputMonitoringEnabled() && Access::monitoringDemand(adapter),
          "fresh Toggle enable is confirmed after device restart");

    std::thread errorThread([&adapter] { Access::reportDeviceError(adapter); });
    errorThread.join();
    check(Access::monitoringDemand(adapter),
          "audioDeviceError publishes only an event and does not mutate control demand");
    adapter.pollDeviceLifecycle();
    app.synchroniseTransport();
    check(!Access::monitoringDemand(adapter) && !app.inputMonitoringEnabled(),
          "serialised lifecycle polling performs error cleanup and forced off");
    adapter.shutdown();
    adapter.shutdown();
    check(!Access::monitoringDemand(adapter) &&
              Access::deviceState(adapter) != audio::DeviceProcessingState::operational,
          "shutdown after a device error is idempotent and releases monitoring demand");
}

void controlledReconfigureAndLoss() {
    Adapter adapter;
    application::DawApplication app{adapter, timeline::SampleRate{48000.0}};
    VirtualAudioDeviceType* type{};
    check(Access::openOutputOnly(adapter, 2, type), "open controlled lifecycle fixture");
    commands::CommandDispatcher dispatcher{app};
    check(dispatcher.dispatch(commands::EnableInputMonitoring{}).status ==
              commands::CommandStatus::accepted,
          "enable before controlled reconfigure");
    Access::device(*type).render(0.5F);
    app.synchroniseTransport();
    Access::controlledRateChange(adapter, 44100.0);
    Access::device(*type).render(0.5F);
    app.synchroniseTransport();
    check(app.inputMonitoringEnabled() && Access::monitoringDemand(adapter),
          "successful controlled sample-rate reconfigure preserves monitoring intent");
    Access::device(*type).stop();
    adapter.pollDeviceLifecycle();
    app.synchroniseTransport();
    check(!app.inputMonitoringEnabled() && !Access::monitoringDemand(adapter) &&
              adapter.state().status == audio::AudioDeviceStatus::error,
          "real device stop forces monitoring off with a device diagnostic");
}

void recordingAndMonitoringShareInput() {
    Adapter adapter;
    application::DawApplication app{adapter, timeline::SampleRate{48000.0}};
    VirtualAudioDeviceType* type{};
    check(Access::openOutputOnly(adapter, 2, type), "open shared-input fixture");
    commands::CommandDispatcher dispatcher{app};
    check(dispatcher.dispatch(commands::EnableInputMonitoring{}).status ==
              commands::CommandStatus::accepted,
          "enable monitoring before recording");
    Access::device(*type).render(0.25F);
    app.synchroniseTransport();
    const auto project = std::filesystem::temp_directory_path() /
        "vitadaw-input-monitoring-lifecycle-test.vitadaw";
    const auto preflight = adapter.prepareRecording(
        {{1}, media::AudioChannelLayout::mono, project});
    check(preflight.success() && Access::monitoringDemand(adapter) &&
              Access::monitoringChannels(adapter) == 2,
          "Recording preserves the already-demanded stereo monitoring input");
    check(adapter.tryRequestRecord(preflight.request).accepted,
          "record request uses the normal capture lifecycle");
    Access::device(*type).render(0.25F);
    check(Access::recording(adapter).phase == audio::RecordingPhase::capturing,
          "recording capture starts while monitoring remains active");
    check(dispatcher.dispatch(commands::DisableInputMonitoring{}).status ==
              commands::CommandStatus::accepted,
          "disable monitoring is admitted during active recording");
    Access::device(*type).render(0.25F);
    app.synchroniseTransport();
    check(Access::recording(adapter).phase == audio::RecordingPhase::capturing &&
              !app.inputMonitoringEnabled(),
          "disable monitoring does not alter capture/writer lifecycle");
    check(dispatcher.dispatch(commands::EnableInputMonitoring{}).status ==
              commands::CommandStatus::accepted,
          "enable reuses recording's active input without a second device setup");
    Access::device(*type).render(0.25F);
    app.synchroniseTransport();
    check(app.inputMonitoringEnabled() && Access::recording(adapter).phase ==
              audio::RecordingPhase::capturing,
          "Recording and Monitoring coexist after re-enable");
    check(adapter.tryRequestStop().accepted, "stop recording transport request accepted");
    Access::device(*type).render(0.25F);
    check(Access::recording(adapter).phase == audio::RecordingPhase::complete &&
              app.inputMonitoringEnabled(),
          "stopping capture leaves monitoring enabled on its input route");
    adapter.shutdown();
    check(!Access::monitoringDemand(adapter) &&
              Access::recording(adapter).phase != audio::RecordingPhase::capturing &&
              Access::deviceState(adapter) != audio::DeviceProcessingState::operational,
          "shutdown after Recording plus Monitoring releases callback routes safely");
}

void recordingFinalizationPreservesMonitoring() {
    Adapter adapter;
    application::DawApplication app{adapter, timeline::SampleRate{48000.0}};
    VirtualAudioDeviceType* type{};
    check(Access::openOutputOnly(adapter, 2, type),
          "open recording-finalization monitoring fixture");
    commands::CommandDispatcher dispatcher{app};
    const auto project = std::filesystem::temp_directory_path() /
        "vitadaw-input-monitoring-finalization.vitadaw";
    check(dispatcher.dispatch(commands::AddAudioTrack{"Record target"}).status ==
              commands::CommandStatus::accepted &&
              dispatcher.dispatch(commands::SaveProjectAs{project}).status ==
                  commands::CommandStatus::accepted &&
              dispatcher.dispatch(commands::SetTrackRecordArmed{{1}, true}).status ==
                  commands::CommandStatus::accepted,
          "recording-finalization application fixture is established");
    check(dispatcher.dispatch(commands::EnableInputMonitoring{}).status ==
              commands::CommandStatus::accepted,
          "monitoring enables before recording finalization");
    const auto device = [&]() -> VirtualAudioDevice& { return Access::device(*type); };
    device().render(0.5F);
    app.synchroniseTransport();
    check(device().lastOutput > 0.1F && app.inputMonitoringEnabled(),
          "monitoring produces physical output before recording");

    const auto recordAndStop = [&] {
        check(dispatcher.dispatch(commands::Record{}).status ==
                  commands::CommandStatus::accepted,
              "application Record is accepted with monitoring enabled");
        device().render(0.5F);
        check(Access::recording(adapter).phase == audio::RecordingPhase::capturing &&
                  device().lastOutput > 0.1F,
              "recording and monitoring share the physical input route");
        check(dispatcher.dispatch(commands::Stop{}).status ==
                  commands::CommandStatus::accepted,
              "application Stop is accepted during recording");
        device().render(0.5F);
        app.synchroniseTransport();
    };

    recordAndStop();
    check(app.recordingPhase() == audio::RecordingPhase::complete &&
              app.project().sources().size() == 1 && app.inputMonitoringEnabled() &&
              Access::monitoringDemand(adapter) &&
              device().getActiveInputChannels().countNumberOfSetBits() == 2 &&
              !Access::snapshot(adapter).monitoringLifecycleForcedOff,
          "Record finalization keeps Monitoring's demand, input route, and lifecycle active");
    device().render(0.5F);
    app.synchroniseTransport();
    check(device().lastOutput > 0.1F && app.inputMonitoringEnabled(),
          "monitoring still produces physical output after the first recording commits");

    recordAndStop();
    device().render(0.5F);
    app.synchroniseTransport();
    check(app.recordingPhase() == audio::RecordingPhase::complete &&
              app.project().sources().size() == 2 && device().lastOutput > 0.1F &&
              app.inputMonitoringEnabled() && Access::monitoringDemand(adapter),
          "a second recording preserves Monitoring during and after finalization");
    adapter.shutdown();
}

void recordingStopsWithoutMonitoringDemand() {
    Adapter adapter;
    application::DawApplication app{adapter, timeline::SampleRate{48000.0}};
    VirtualAudioDeviceType* type{};
    check(Access::openOutputOnly(adapter, 2, type),
          "open recording-without-monitoring fixture");
    commands::CommandDispatcher dispatcher{app};
    const auto project = std::filesystem::temp_directory_path() /
        "vitadaw-input-monitoring-disabled-finalization.vitadaw";
    check(dispatcher.dispatch(commands::AddAudioTrack{"Record target"}).status ==
              commands::CommandStatus::accepted &&
              dispatcher.dispatch(commands::SaveProjectAs{project}).status ==
                  commands::CommandStatus::accepted &&
              dispatcher.dispatch(commands::SetTrackRecordArmed{{1}, true}).status ==
                  commands::CommandStatus::accepted,
          "monitoring-off recording fixture is established");
    check(dispatcher.dispatch(commands::Record{}).status == commands::CommandStatus::accepted,
          "recording starts with monitoring disabled");
    const auto device = [&]() -> VirtualAudioDevice& { return Access::device(*type); };
    device().render(0.5F);
    check(dispatcher.dispatch(commands::Stop{}).status == commands::CommandStatus::accepted,
          "monitoring-off recording stops");
    device().render(0.5F);
    app.synchroniseTransport();
    device().render(0.5F);
    app.synchroniseTransport();
    check(app.recordingPhase() == audio::RecordingPhase::complete &&
              !app.inputMonitoringEnabled() && !Access::monitoringDemand(adapter) &&
              device().lastOutput == 0.0F,
          "recording finalization leaves no monitoring demand or output zombie when Monitoring is off");
    adapter.shutdown();
}

void disablingMonitoringDuringRecordingAndAbortAreIndependent() {
    Adapter adapter;
    application::DawApplication app{adapter, timeline::SampleRate{48000.0}};
    VirtualAudioDeviceType* type{};
    check(Access::openOutputOnly(adapter, 2, type),
          "open recording monitoring-demand independence fixture");
    commands::CommandDispatcher dispatcher{app};
    const auto project = std::filesystem::temp_directory_path() /
        "vitadaw-input-monitoring-demand-independence.vitadaw";
    check(dispatcher.dispatch(commands::AddAudioTrack{"Record target"}).status ==
              commands::CommandStatus::accepted &&
              dispatcher.dispatch(commands::SaveProjectAs{project}).status ==
                  commands::CommandStatus::accepted &&
              dispatcher.dispatch(commands::SetTrackRecordArmed{{1}, true}).status ==
                  commands::CommandStatus::accepted,
          "monitoring-demand independence fixture is established");
    const auto device = [&]() -> VirtualAudioDevice& { return Access::device(*type); };

    check(dispatcher.dispatch(commands::EnableInputMonitoring{}).status ==
              commands::CommandStatus::accepted &&
              dispatcher.dispatch(commands::Record{}).status == commands::CommandStatus::accepted,
          "monitoring and recording start together for disable case");
    device().render(0.5F);
    check(dispatcher.dispatch(commands::DisableInputMonitoring{}).status ==
              commands::CommandStatus::accepted,
          "monitoring can be disabled without stopping recording");
    device().render(0.5F);
    check(Access::recording(adapter).phase == audio::RecordingPhase::capturing &&
              !Access::monitoringDemand(adapter),
          "disabling monitoring does not release Recording's input demand");
    check(dispatcher.dispatch(commands::Stop{}).status == commands::CommandStatus::accepted,
          "recording stops after Monitoring was disabled");
    device().render(0.5F);
    app.synchroniseTransport();
    device().render(0.5F);
    app.synchroniseTransport();
    check(app.recordingPhase() == audio::RecordingPhase::complete &&
              !app.inputMonitoringEnabled() && !Access::monitoringDemand(adapter) &&
              device().lastOutput == 0.0F,
          "after Stop no Monitoring demand remains when it was explicitly disabled");

    check(dispatcher.dispatch(commands::EnableInputMonitoring{}).status ==
              commands::CommandStatus::accepted &&
              dispatcher.dispatch(commands::Record{}).status == commands::CommandStatus::accepted,
          "monitoring and recording start together for abort case");
    device().render(0.5F);
    check(dispatcher.dispatch(commands::CancelRecording{}).status ==
              commands::CommandStatus::accepted,
          "recording cancellation is scheduled without a device loss");
    device().render(0.5F);
    app.synchroniseTransport();
    device().render(0.5F);
    app.synchroniseTransport();
    check(app.recordingPhase() == audio::RecordingPhase::failed &&
              app.inputMonitoringEnabled() && Access::monitoringDemand(adapter) &&
              device().lastOutput > 0.1F &&
              !Access::snapshot(adapter).monitoringLifecycleForcedOff,
          "recording abort preserves Monitoring while the hardware remains valid");
    adapter.shutdown();
}

void recordingAndMonitoringShutdown() {
    Adapter adapter;
    application::DawApplication app{adapter, timeline::SampleRate{48000.0}};
    VirtualAudioDeviceType* type{};
    check(Access::openOutputOnly(adapter, 2, type), "open recording shutdown fixture");
    commands::CommandDispatcher dispatcher{app};
    check(dispatcher.dispatch(commands::EnableInputMonitoring{}).status ==
              commands::CommandStatus::accepted,
          "enable monitoring before active-recording shutdown");
    Access::device(*type).render(0.25F);
    const auto project = std::filesystem::temp_directory_path() /
        "vitadaw-input-monitoring-shutdown-test.vitadaw";
    const auto preflight = adapter.prepareRecording(
        {{1}, media::AudioChannelLayout::mono, project});
    check(preflight.success() && adapter.tryRequestRecord(preflight.request).accepted,
          "active-recording shutdown uses the normal capture setup");
    Access::device(*type).render(0.25F);
    check(Access::recording(adapter).phase == audio::RecordingPhase::capturing,
          "capture is active before shutdown");
    adapter.shutdown();
    check(!Access::monitoringDemand(adapter) &&
              Access::recording(adapter).phase != audio::RecordingPhase::capturing &&
              Access::deviceState(adapter) != audio::DeviceProcessingState::operational,
          "shutdown during Recording plus Monitoring releases capture and monitoring safely");
}

void recordingPlacementSnapshotIsPreparedOutsideRt() {
    Adapter adapter;
    application::DawApplication app{adapter, timeline::SampleRate{48000.0}};
    VirtualAudioDeviceType* type{};
    check(Access::openOutputOnly(adapter, 2, type), "open placement snapshot fixture");
    commands::CommandDispatcher dispatcher{app};
    check(dispatcher.dispatch(commands::EnableInputMonitoring{}).status ==
              commands::CommandStatus::accepted,
          "placement snapshot fixture opens input before preflight");
    auto& device = Access::device(*type);
    device.inputLatency = 256;
    const auto project = std::filesystem::temp_directory_path() /
        "vitadaw-placement-snapshot.vitadaw";
    const auto preflight = adapter.prepareRecording(
        {{1}, media::AudioChannelLayout::mono, project, timeline::SampleRate{48000.0}, {-16}});
    check(preflight.success() &&
              preflight.request.placement.latencyStatus == audio::RecordingLatencyStatus::reported &&
              preflight.request.placement.reportedInputLatencyDeviceFrames ==
                  std::optional<timeline::DeviceFrameCount>{{256}} &&
              preflight.request.placement.reportedInputLatencyProjectFrames.value == 256 &&
              preflight.request.placement.manualOffsetProjectFrames.value == -16 &&
              preflight.request.placement.effectiveCompensationProjectFrames.value == 272,
          "preflight freezes certified input latency and manual placement before beginRecord");
    device.inputLatency = 0;
    check(adapter.tryRequestRecord(preflight.request).accepted, "placement request is accepted");
    device.render(0.25F);
    const auto capture = Access::recording(adapter);
    check(capture.placement.reportedInputLatencyProjectFrames.value == 256 &&
              capture.placement.manualOffsetProjectFrames.value == -16,
          "callback receives the frozen snapshot without querying replacement latency");
    adapter.shutdown();
}

void recordingPlacementIsMonitoringAndWavIndependent() {
    Adapter adapter;
    application::DawApplication app{adapter, timeline::SampleRate{48000.0}};
    VirtualAudioDeviceType* type{};
    check(Access::openOutputOnly(adapter, 2, type),
          "open placement monitoring-independence fixture");
    commands::CommandDispatcher dispatcher{app};
    const auto project = std::filesystem::temp_directory_path() /
        "vitadaw-placement-monitoring-independence.vitadaw";
    check(dispatcher.dispatch(commands::AddAudioTrack{"Record target"}).status ==
              commands::CommandStatus::accepted &&
              dispatcher.dispatch(commands::SaveProjectAs{project}).status ==
                  commands::CommandStatus::accepted &&
              dispatcher.dispatch(commands::SetTrackRecordArmed{{1}, true}).status ==
                  commands::CommandStatus::accepted &&
              dispatcher.dispatch(commands::SetRecordingOffset{-16}).status ==
                  commands::CommandStatus::accepted,
          "placement monitoring-independence fixture is established");
    const auto device = [&]() -> VirtualAudioDevice& { return Access::device(*type); };
    struct Take final {
        std::filesystem::path wav;
        timeline::ProjectFramePosition start;
        timeline::ProjectFrameDuration duration;
    };
    const auto record = [&](bool monitoring) -> Take {
        check(dispatcher.dispatch(monitoring ? commands::Command{commands::EnableInputMonitoring{}}
                                            : commands::Command{commands::DisableInputMonitoring{}}).status ==
                  commands::CommandStatus::accepted,
              "requested monitoring state is accepted before its comparison take");
        if (monitoring) {
            check(dispatcher.dispatch(commands::SetMonitorGain{{-3.0F}}).status ==
                      commands::CommandStatus::accepted,
                  "monitor gain changes before the monitored comparison take");
        }
        check(dispatcher.dispatch(commands::SeekToProjectFrame{{1000}}).status ==
                  commands::CommandStatus::accepted,
              "comparison take seek is accepted");
        device().render(0.0F);
        app.synchroniseTransport();
        check(dispatcher.dispatch(commands::Record{}).status == commands::CommandStatus::accepted,
              "comparison take Record is accepted");
        device().render(0.25F);
        check(dispatcher.dispatch(commands::Stop{}).status == commands::CommandStatus::accepted,
              "comparison take Stop is accepted");
        device().render(0.25F);
        app.synchroniseTransport();
        const auto& source = app.project().sources().back();
        const auto clip = std::find_if(app.project().tracks()[0].clips.begin(),
                                       app.project().tracks()[0].clips.end(),
                                       [&source](const auto& candidate) {
                                           return candidate.source == source.id;
                                       });
        check(clip != app.project().tracks()[0].clips.end(),
              "committed comparison take has an AudioClip");
        return {source.media.originalPath, clip->projectStart, clip->duration};
    };
    const auto off = record(false);
    const auto on = record(true);
    const auto readBytes = [](const std::filesystem::path& path) {
        std::ifstream stream(path, std::ios::binary);
        return std::vector<char>{std::istreambuf_iterator<char>{stream}, {}};
    };
    const auto offBytes = readBytes(off.wav);
    const auto onBytes = readBytes(on.wav);
    check(!offBytes.empty() && offBytes == onBytes && off.start == on.start &&
              off.duration == on.duration,
          "Monitoring state and gain leave raw WAV bytes and documentary placement identical");
    adapter.shutdown();
}

void recordingPlacementSnapshotsProductiveBufferChanges() {
    Adapter adapter;
    application::DawApplication app{adapter, timeline::SampleRate{48000.0}};
    VirtualAudioDeviceType* type{};
    check(Access::openOutputOnly(adapter, 2, type),
          "open productive placement buffer fixture");
    commands::CommandDispatcher dispatcher{app};
    const auto project = std::filesystem::temp_directory_path() /
        "vitadaw-placement-buffer-snapshots.vitadaw";
    check(dispatcher.dispatch(commands::AddAudioTrack{"Record target"}).status ==
              commands::CommandStatus::accepted &&
              dispatcher.dispatch(commands::SaveProjectAs{project}).status ==
                  commands::CommandStatus::accepted &&
              dispatcher.dispatch(commands::SetTrackRecordArmed{{1}, true}).status ==
                  commands::CommandStatus::accepted,
          "productive placement buffer fixture is established");
    const auto device = [&]() -> VirtualAudioDevice& { return Access::device(*type); };
    struct Take final {
        clips::ClipId clip;
        timeline::ProjectFramePosition start;
        timeline::ProjectFrameDuration duration;
    };
    const auto recordAt = [&]() -> Take {
        check(dispatcher.dispatch(commands::SeekToProjectFrame{{1000}}).status ==
                  commands::CommandStatus::accepted,
              "productive buffer take seek is accepted");
        device().render(0.0F);
        app.synchroniseTransport();
        check(dispatcher.dispatch(commands::Record{}).status == commands::CommandStatus::accepted,
              "productive buffer take Record is accepted");
        device().render(0.25F);
        check(dispatcher.dispatch(commands::Stop{}).status == commands::CommandStatus::accepted,
              "productive buffer take Stop is accepted");
        device().render(0.25F);
        app.synchroniseTransport();
        const auto& source = app.project().sources().back();
        const auto clip = std::find_if(app.project().tracks()[0].clips.begin(),
                                       app.project().tracks()[0].clips.end(),
                                       [&source](const auto& candidate) {
                                           return candidate.source == source.id;
                                       });
        check(clip != app.project().tracks()[0].clips.end(),
              "productive buffer take commits its clip");
        return {clip->id, clip->projectStart, clip->duration};
    };

    type->inputLatency = 64;
    device().inputLatency = 64;
    const auto first = recordAt();
    type->inputLatency = 256;
    device().inputLatency = 256;
    check(dispatcher.dispatch(commands::SetAudioBufferSize{512}).status ==
              commands::CommandStatus::accepted &&
              app.deviceLatencyReadModel().confirmedBufferSizeFrames == 512 &&
              app.deviceLatencyReadModel().inputLatencyFrames == std::optional<std::uint32_t>{256},
          "productive buffer change certifies the second input-latency configuration");
    const auto second = recordAt();
    check(first.start.value == 936 && second.start.value == 744 &&
              first.duration == second.duration,
          "take A and take B use independently frozen productive latency snapshots");
    check(dispatcher.dispatch(commands::SaveProject{}).status == commands::CommandStatus::accepted &&
              dispatcher.dispatch(commands::Undo{}).status == commands::CommandStatus::accepted &&
              dispatcher.dispatch(commands::Undo{}).status == commands::CommandStatus::accepted &&
              dispatcher.dispatch(commands::Redo{}).status == commands::CommandStatus::accepted &&
              app.project().findClip(first.clip)->projectStart == first.start &&
              dispatcher.dispatch(commands::Redo{}).status == commands::CommandStatus::accepted &&
              app.project().findClip(second.clip)->projectStart == second.start &&
              dispatcher.dispatch(commands::SaveProject{}).status == commands::CommandStatus::accepted &&
              dispatcher.dispatch(commands::LoadProject{project, true}).status ==
                  commands::CommandStatus::accepted &&
              app.project().findClip(first.clip)->projectStart == first.start &&
              app.project().findClip(second.clip)->projectStart == second.start,
          "undo/redo and save/load retain placement without consulting the later device latency");
    adapter.shutdown();
}

void unavailableInputLatencyAppliesManualPlacementProductively() {
    Adapter adapter;
    application::DawApplication app{adapter, timeline::SampleRate{48000.0}};
    VirtualAudioDeviceType* type{};
    check(Access::openOutputOnly(adapter, 2, type),
          "open productive unavailable-latency placement fixture");
    commands::CommandDispatcher dispatcher{app};
    const auto project = std::filesystem::temp_directory_path() /
        "vitadaw-placement-unavailable-latency.vitadaw";
    check(dispatcher.dispatch(commands::AddAudioTrack{"Record target"}).status ==
              commands::CommandStatus::accepted &&
              dispatcher.dispatch(commands::SaveProjectAs{project}).status ==
                  commands::CommandStatus::accepted &&
              dispatcher.dispatch(commands::SetTrackRecordArmed{{1}, true}).status ==
                  commands::CommandStatus::accepted &&
              dispatcher.dispatch(commands::SetRecordingOffset{-16}).status ==
                  commands::CommandStatus::accepted,
          "productive unavailable-latency fixture is established");
    const auto device = [&]() -> VirtualAudioDevice& { return Access::device(*type); };
    type->inputLatency = -1;
    device().inputLatency = -1;
    check(app.recordingPlacementReadModel().latencyStatus ==
              audio::RecordingLatencyStatus::unavailable &&
              app.recordingPlacementReadModel().effectiveCompensationProjectFrames.value == 16,
          "unavailable read model retains the manual effective compensation before Record");
    const auto record = [&](bool monitoring) {
        check(dispatcher.dispatch(monitoring ? commands::Command{commands::EnableInputMonitoring{}}
                                            : commands::Command{commands::DisableInputMonitoring{}}).status ==
                  commands::CommandStatus::accepted &&
                  dispatcher.dispatch(commands::SeekToProjectFrame{{1000}}).status ==
                      commands::CommandStatus::accepted,
              "unavailable-latency take state is accepted");
        device().render(0.0F);
        app.synchroniseTransport();
        check(dispatcher.dispatch(commands::Record{}).status == commands::CommandStatus::accepted,
              "unavailable-latency Record is accepted");
        device().render(0.25F);
        check(dispatcher.dispatch(commands::Stop{}).status == commands::CommandStatus::accepted,
              "unavailable-latency Stop is accepted");
        device().render(0.25F);
        app.synchroniseTransport();
        const auto& source = app.project().sources().back();
        const auto clip = std::find_if(app.project().tracks()[0].clips.begin(),
                                       app.project().tracks()[0].clips.end(),
                                       [&source](const auto& candidate) {
                                           return candidate.source == source.id;
                                       });
        check(clip != app.project().tracks()[0].clips.end() &&
                  std::filesystem::file_size(source.media.originalPath) > 0,
              "unavailable-latency take publishes valid WAV media");
        return clip->projectStart;
    };
    const auto off = record(false);
    const auto on = record(true);
    check(off.value == 984 && on == off &&
              app.recordingPlacementReadModel().latencyStatus ==
                  audio::RecordingLatencyStatus::unavailable &&
              app.recordingPlacementReadModel().effectiveCompensationProjectFrames.value == 16,
          "unavailable automatic latency applies only manual placement independently of Monitoring");
    adapter.shutdown();
}

void bufferControlPublishesEffectiveConfigurationAndLatency() {
    Adapter adapter;
    application::DawApplication app{adapter, timeline::SampleRate{48000.0}};
    VirtualAudioDeviceType* type{};
    check(Access::openOutputOnly(adapter, 2, type), "open buffer-control fixture");
    commands::CommandDispatcher dispatcher{app};
    const auto outputOnly = app.deviceLatencyReadModel();
    check(!outputOnly.inputLatencyFrames && !outputOnly.estimatedMonitoringLatencyMilliseconds &&
              outputOnly.outputLatencyFrames == std::optional<std::uint32_t>{96},
          "inactive input publishes Unknown latency rather than an invented zero estimate");
    check(dispatcher.dispatch(commands::EnableInputMonitoring{}).status ==
              commands::CommandStatus::accepted,
          "enable Monitoring before buffer control");
    Access::device(*type).render(0.5F);
    app.synchroniseTransport();

    const auto initial = app.deviceLatencyReadModel();
    check(initial.configurationAvailable && initial.confirmedBufferSizeFrames == 256 &&
              initial.supportedBufferSizeFrames ==
                  std::vector<std::uint32_t>{16, 32, 64, 128, 256, 512, 1024, 2048, 4096},
          "read model exposes normalized device-provided buffer sizes without a fixed range");
    check(initial.inputLatencyFrames == std::optional<std::uint32_t>{48} &&
              initial.outputLatencyFrames == std::optional<std::uint32_t>{96} &&
              initial.inputLatencyMilliseconds && initial.outputLatencyMilliseconds &&
              initial.estimatedMonitoringLatencyMilliseconds &&
              std::abs(*initial.estimatedMonitoringLatencyMilliseconds - 3.0) < 0.0001,
          "reported input/output latency and estimated monitoring latency use frames at effective rate");

    for (const auto requested : {16U, 32U, 64U, 128U, 256U, 512U, 1024U, 2048U, 4096U}) {
        check(dispatcher.dispatch(commands::SetAudioBufferSize{requested}).status ==
              commands::CommandStatus::accepted,
              "supported buffer change is accepted through Command System");
        const auto read = app.deviceLatencyReadModel();
        check(read.confirmedBufferSizeFrames == requested &&
                  Access::blockCapacity(adapter) == requested &&
                  Access::monitoringDemand(adapter),
              "effective device buffer certifies Core capacity and preserves Monitoring demand");
        Access::device(*type).render(0.5F, requested);
        app.synchroniseTransport();
        check(app.inputMonitoringEnabled() && Access::device(*type).lastOutput > 0.0F,
              "Monitoring remains productive after controlled buffer reconfigure");
    }

    Access::controlledRateChange(adapter, 44100.0);
    const auto at44100 = app.deviceLatencyReadModel();
    check(at44100.sampleRateHz == 44100.0 && at44100.inputLatencyMilliseconds &&
              std::abs(*at44100.inputLatencyMilliseconds - (48.0 / 44100.0 * 1000.0)) < 0.0001,
          "sample-rate lifecycle refresh recalculates latency milliseconds from reported frames");
    Access::controlledRateChange(adapter, 48000.0);

    check(dispatcher.dispatch(commands::SetMetronomeEnabled{true}).status ==
              commands::CommandStatus::accepted,
          "metronome establishes a playback-capable transport fixture");
    Access::device(*type).render(0.0F);
    app.synchroniseTransport();
    check(dispatcher.dispatch(commands::Play{}).status == commands::CommandStatus::accepted,
          "Playback starts before buffer reconfigure");
    Access::device(*type).render(0.0F);
    app.synchroniseTransport();
    const auto beforePosition = app.transport().position;
    const auto playbackBufferChange = dispatcher.dispatch(commands::SetAudioBufferSize{512});
    check(playbackBufferChange.status == commands::CommandStatus::accepted,
          "Playback accepts controlled buffer reconfigure");
    Access::device(*type).render(0.0F);
    app.synchroniseTransport();
    check(app.transport().playback == transport::PlaybackState::playing &&
              app.transport().position.value >= beforePosition.value,
          "controlled buffer reconfigure preserves logical Playback position");

    type->supportedBufferSizes = {64, 128, 256, 512};
    Access::device(*type).setSupportedBufferSizes(type->supportedBufferSizes);
    type->effectiveBufferOverride = 192;
    Access::device(*type).effectiveBufferOverride = 192;
    check(dispatcher.dispatch(commands::SetAudioBufferSize{128}).status ==
              commands::CommandStatus::accepted,
              "driver-effective buffer may differ without publishing the request as confirmed");
    const auto divergentEffective = app.deviceLatencyReadModel();
    check(divergentEffective.confirmedBufferSizeFrames == 192 &&
              Access::blockCapacity(adapter) == 192 &&
              divergentEffective.supportedBufferSizeFrames ==
                  std::vector<std::uint32_t>{64, 128, 256, 512},
          "effective buffer remains confirmed but is not forged into the supported list");
    type->effectiveBufferOverride = 0;
    Access::device(*type).effectiveBufferOverride = 0;
    check(dispatcher.dispatch(commands::SetAudioBufferSize{256}).status ==
              commands::CommandStatus::accepted &&
              app.deviceLatencyReadModel().confirmedBufferSizeFrames == 256 &&
              app.deviceLatencyReadModel().supportedBufferSizeFrames ==
                  std::vector<std::uint32_t>{64, 128, 256, 512},
          "a subsequent supported request replaces the divergent effective value cleanly");
    const auto beforeUnsupported = app.deviceLatencyReadModel().confirmedBufferSizeFrames;
    check(dispatcher.dispatch(commands::SetAudioBufferSize{999}).status ==
              commands::CommandStatus::rejected &&
              app.deviceLatencyReadModel().confirmedBufferSizeFrames == beforeUnsupported,
          "unsupported buffer is rejected without changing confirmed configuration");

    type->failOpenAttempts = 1;
    check(dispatcher.dispatch(commands::SetAudioBufferSize{512}).status ==
              commands::CommandStatus::rejected &&
              app.deviceLatencyReadModel().configurationAvailable &&
              app.deviceLatencyReadModel().confirmedBufferSizeFrames == beforeUnsupported,
          "failed device setup rolls back to the prior certified configuration");

    auto& device = Access::device(*type);
    device.currentBufferSizeQueries = 0;
    device.inputLatencyQueries = 0;
    device.outputLatencyQueries = 0;
    device.render(0.25F);
    check(device.currentBufferSizeQueries == 0 && device.inputLatencyQueries == 0 &&
              device.outputLatencyQueries == 0,
          "RT callback performs no device or latency query");
    adapter.shutdown();
}

void bufferControlRollbackFailureAndRecordingRejection() {
    {
        Adapter adapter;
        application::DawApplication app{adapter, timeline::SampleRate{48000.0}};
        VirtualAudioDeviceType* type{};
        check(Access::openOutputOnly(adapter, 2, type), "open rollback-failure fixture");
        commands::CommandDispatcher dispatcher{app};
        type->failOpenAttempts = 2;
        check(dispatcher.dispatch(commands::SetAudioBufferSize{512}).status ==
                  commands::CommandStatus::rejected &&
                  adapter.state().status == audio::AudioDeviceStatus::error &&
                  !app.deviceLatencyReadModel().configurationAvailable &&
                  !Access::monitoringDemand(adapter),
              "rollback failure follows the safe device-error policy without stale configuration");
        adapter.shutdown();
    }

    Adapter adapter;
    application::DawApplication app{adapter, timeline::SampleRate{48000.0}};
    VirtualAudioDeviceType* type{};
    check(Access::openOutputOnly(adapter, 2, type), "open recording-rejection fixture");
    commands::CommandDispatcher dispatcher{app};
    const auto project = std::filesystem::temp_directory_path() /
        "vitadaw-buffer-control-recording.vitadaw";
    check(dispatcher.dispatch(commands::AddAudioTrack{"Record target"}).status ==
              commands::CommandStatus::accepted &&
              dispatcher.dispatch(commands::SaveProjectAs{project}).status ==
                  commands::CommandStatus::accepted &&
              dispatcher.dispatch(commands::SetTrackRecordArmed{{1}, true}).status ==
                  commands::CommandStatus::accepted &&
              dispatcher.dispatch(commands::Record{}).status ==
                  commands::CommandStatus::accepted,
          "recording-prepared buffer rejection fixture is established");
    const auto openCount = Access::device(*type).openCount;
    const auto before = app.deviceLatencyReadModel().confirmedBufferSizeFrames;
    check(dispatcher.dispatch(commands::SetAudioBufferSize{512}).status ==
              commands::CommandStatus::rejected &&
              Access::device(*type).openCount == openCount &&
              app.deviceLatencyReadModel().confirmedBufferSizeFrames == before,
          "prepared Recording rejects buffer change before any hardware mutation");
    adapter.shutdown();
}

void bufferControlVerifiesRollbackCheckpointAndStagingPreparation() {
    {
        Adapter adapter;
        application::DawApplication app{adapter, timeline::SampleRate{48000.0}};
        VirtualAudioDeviceType* type{};
        check(Access::openOutputOnly(adapter, 2, type), "open complete rollback fixture");
        commands::CommandDispatcher dispatcher{app};
        check(dispatcher.dispatch(commands::EnableInputMonitoring{}).status ==
                  commands::CommandStatus::accepted,
              "complete rollback fixture enables its input route");
        const auto expectedInput = Access::device(*type).getActiveInputChannels();
        const auto expectedOutput = Access::device(*type).getActiveOutputChannels();
        type->failOpenAttempts = 1;
        check(dispatcher.dispatch(commands::SetAudioBufferSize{512}).status ==
                  commands::CommandStatus::rejected &&
                  adapter.state().status == audio::AudioDeviceStatus::active &&
                  app.deviceLatencyReadModel().confirmedBufferSizeFrames == 256 &&
                  Access::device(*type).getCurrentSampleRate() == 48000.0 &&
                  Access::device(*type).getActiveInputChannels() == expectedInput &&
                  Access::device(*type).getActiveOutputChannels() == expectedOutput,
              "rollback is accepted only after the full effective checkpoint is restored");
        adapter.shutdown();
    }

    {
        Adapter adapter;
        application::DawApplication app{adapter, timeline::SampleRate{48000.0}};
        VirtualAudioDeviceType* type{};
        check(Access::openOutputOnly(adapter, 2, type), "open rate-divergent rollback fixture");
        commands::CommandDispatcher dispatcher{app};
        type->effectiveSampleRateOverride = 44100.0;
        Access::device(*type).effectiveSampleRateOverride = 44100.0;
        type->failOpenAttempts = 1;
        check(dispatcher.dispatch(commands::SetAudioBufferSize{512}).status ==
                  commands::CommandStatus::rejected &&
                  adapter.state().status == audio::AudioDeviceStatus::error &&
                  !app.deviceLatencyReadModel().configurationAvailable &&
                  !Access::monitoringDemand(adapter),
              "rollback with a different effective sample rate enters the safe device-error state");
        adapter.shutdown();
    }

    {
        Adapter adapter;
        application::DawApplication app{adapter, timeline::SampleRate{48000.0}};
        VirtualAudioDeviceType* type{};
        check(Access::openOutputOnly(adapter, 2, type), "open layout-divergent rollback fixture");
        commands::CommandDispatcher dispatcher{app};
        type->effectiveOutputChannelCountOverride = 1;
        Access::device(*type).effectiveOutputChannelCountOverride = 1;
        type->failOpenAttempts = 1;
        check(dispatcher.dispatch(commands::SetAudioBufferSize{512}).status ==
                  commands::CommandStatus::rejected &&
                  adapter.state().status == audio::AudioDeviceStatus::error &&
                  !app.deviceLatencyReadModel().configurationAvailable &&
                  !Access::monitoringDemand(adapter),
              "rollback with a different effective output layout enters the safe device-error state");
        adapter.shutdown();
    }

    {
        Adapter adapter;
        application::DawApplication app{adapter, timeline::SampleRate{48000.0}};
        VirtualAudioDeviceType* type{};
        check(Access::openOutputOnly(adapter, 2, type), "open staging rollback fixture");
        commands::CommandDispatcher dispatcher{app};
        check(dispatcher.dispatch(commands::AddAudioTrack{"Staging target"}).status ==
                  commands::CommandStatus::accepted &&
                  dispatcher.dispatch(commands::EnableInputMonitoring{}).status ==
                  commands::CommandStatus::accepted,
              "staging rollback fixture has a prepared project and Monitoring route");
        Access::failNextMonitoringStagingPreparation(adapter);
        check(dispatcher.dispatch(commands::SetAudioBufferSize{512}).status ==
                  commands::CommandStatus::rejected &&
                  adapter.state().status == audio::AudioDeviceStatus::active &&
                  app.deviceLatencyReadModel().configurationAvailable &&
                  app.deviceLatencyReadModel().confirmedBufferSizeFrames == 256 &&
                  Access::blockCapacity(adapter) == 256 && Access::monitoringDemand(adapter),
              "staging preparation failure rolls back rather than certifying a limited route");
        Access::device(*type).render(0.5F, 256);
        app.synchroniseTransport();
        check(app.inputMonitoringEnabled() && Access::device(*type).lastOutput > 0.01F,
              "rollback after staging failure restores productive Monitoring");
        adapter.shutdown();
    }

    {
        Adapter adapter;
        application::DawApplication app{adapter, timeline::SampleRate{48000.0}};
        VirtualAudioDeviceType* type{};
        check(Access::openOutputOnly(adapter, 2, type), "open staging rollback-failure fixture");
        commands::CommandDispatcher dispatcher{app};
        check(dispatcher.dispatch(commands::AddAudioTrack{"Staging target"}).status ==
                  commands::CommandStatus::accepted &&
                  dispatcher.dispatch(commands::EnableInputMonitoring{}).status ==
                  commands::CommandStatus::accepted,
              "staging rollback-failure fixture has a prepared Monitoring route");
        // The candidate is allowed to open, then staging preparation fails.
        // Its rollback is deliberately negotiated to a different effective
        // rate, so the complete checkpoint cannot be re-established.
        type->effectiveSampleRateOverride = 44100.0;
        Access::device(*type).effectiveSampleRateOverride = 44100.0;
        Access::failNextMonitoringStagingPreparation(adapter);
        check(dispatcher.dispatch(commands::SetAudioBufferSize{512}).status ==
                  commands::CommandStatus::rejected &&
                  adapter.state().status == audio::AudioDeviceStatus::error &&
                  !app.deviceLatencyReadModel().configurationAvailable &&
                  !Access::monitoringDemand(adapter),
              "staging failure plus rollback failure enters safe device-error without stale state");
        adapter.shutdown();
    }
}

void loopbackPreconditionsCompletionCancelAndRecordingRecovery() {
    Adapter adapter;
    application::DawApplication app{adapter, timeline::SampleRate{48000.0}};
    VirtualAudioDeviceType* type{};
    check(Access::openOutputOnly(adapter, 2, type),
          "open physical-loopback lifecycle fixture");
    commands::CommandDispatcher dispatcher{app};

    check(dispatcher.dispatch(commands::EnableInputMonitoring{}).status ==
              commands::CommandStatus::accepted,
          "Monitoring enables before loopback rejection test");
    Access::device(*type).render(0.25F);
    app.synchroniseTransport();
    check(dispatcher.dispatch(commands::StartLoopbackLatencyTest{1, 1}).status ==
              commands::CommandStatus::rejected && app.inputMonitoringEnabled(),
          "loopback start is rejected while Monitoring is ON");
    check(dispatcher.dispatch(commands::DisableInputMonitoring{}).status ==
              commands::CommandStatus::accepted,
          "Monitoring can be disabled explicitly before loopback");
    Access::device(*type).render(0.0F);
    app.synchroniseTransport();

    check(dispatcher.dispatch(commands::SetMetronomeEnabled{true}).status ==
              commands::CommandStatus::accepted &&
              dispatcher.dispatch(commands::Play{}).status ==
                  commands::CommandStatus::accepted &&
              dispatcher.dispatch(commands::StartLoopbackLatencyTest{1, 1}).status ==
                  commands::CommandStatus::rejected &&
              app.transport().playback == transport::PlaybackState::playing,
          "loopback start is rejected while Playback is active");
    check(dispatcher.dispatch(commands::Stop{}).status == commands::CommandStatus::accepted,
          "Playback stops before loopback");
    Access::device(*type).render(0.0F);
    app.synchroniseTransport();
    check(dispatcher.dispatch(commands::SetMetronomeEnabled{false}).status ==
              commands::CommandStatus::accepted,
          "metronome is restored off before physical loopback");

    const auto temporaryProject = std::filesystem::temp_directory_path() /
        "vitadaw-loopback-recording-interlock.vitadaw";
    const auto preflight = adapter.prepareRecording(
        {{1}, media::AudioChannelLayout::mono, temporaryProject});
    check(preflight.success() && adapter.tryRequestRecord(preflight.request).accepted,
          "recording interlock fixture starts normal Capture");
    Access::device(*type).render(0.25F);
    check(dispatcher.dispatch(commands::StartLoopbackLatencyTest{1, 1}).status ==
              commands::CommandStatus::rejected &&
              Access::recording(adapter).phase == audio::RecordingPhase::capturing,
          "loopback start is rejected while Recording is active");
    check(adapter.tryRequestStop().accepted, "recording interlock fixture stops");
    Access::device(*type).render(0.0F);
    static_cast<void>(adapter.discardRecordingWithDiagnostics(true));

    check(dispatcher.dispatch(commands::SetRecordingOffset{-37}).status ==
              commands::CommandStatus::accepted,
          "manual Recording Offset is established independently of loopback");
    const auto historyToken = app.history().currentStateToken();
    const auto projectSources = app.project().sources().size();
    const auto projectTracks = app.project().tracks().size();
    const auto dirty = app.session().dirty();
    const auto expectedInput = Access::device(*type).getActiveInputChannels();
    const auto expectedOutput = Access::device(*type).getActiveOutputChannels();
    const auto opensBeforeInvalidSelection = Access::device(*type).openCount;
    check(dispatcher.dispatch(commands::StartLoopbackLatencyTest{99, 99}).status ==
              commands::CommandStatus::rejected &&
              Access::device(*type).openCount == opensBeforeInvalidSelection &&
              Access::device(*type).getActiveInputChannels() == expectedInput &&
              Access::device(*type).getActiveOutputChannels() == expectedOutput,
          "invalid physical pair is rejected before hardware mutation");

    check(dispatcher.dispatch(commands::StartLoopbackLatencyTest{1, 1}).status ==
              commands::CommandStatus::accepted,
          "valid idle state starts physical loopback through Command System");
    check(Access::device(*type).getActiveInputChannels()[1] &&
              Access::device(*type).getActiveInputChannels().countNumberOfSetBits() == 1 &&
              Access::device(*type).getActiveOutputChannels()[1] &&
              Access::device(*type).getActiveOutputChannels().countNumberOfSetBits() == 1,
          "loopback temporarily activates exactly the selected physical pair");
    check(dispatcher.dispatch(commands::SetAudioBufferSize{512}).status ==
              commands::CommandStatus::rejected &&
              app.loopbackLatencyReadModel().busy(),
          "buffer change is rejected while loopback is running");

    for (int block = 0; block < 1200 && app.loopbackLatencyReadModel().busy(); ++block) {
        Access::device(*type).renderPhysicalLoopback(512);
        app.synchroniseTransport();
    }
    const auto completed = app.loopbackLatencyReadModel();
    check(completed.status == audio::LoopbackLatencyStatus::completed &&
              completed.validTrials == 5 &&
              completed.measuredRoundTripFrames == std::optional<std::uint64_t>{512} &&
              completed.minimumFrames == std::optional<std::uint64_t>{512} &&
              completed.maximumFrames == std::optional<std::uint64_t>{512} &&
              completed.jitterFrames == std::optional<std::uint64_t>{0} &&
              completed.configuration.reportedInputFrames ==
                  std::optional<std::uint32_t>{48} &&
              completed.configuration.reportedOutputFrames ==
                  std::optional<std::uint32_t>{96} &&
              completed.configuration.activeInputChannels ==
                  std::vector<std::uint32_t>{1} &&
              completed.configuration.activeOutputChannels ==
                  std::vector<std::uint32_t>{1} &&
              completed.configuration.inputCallbackOrdinal == 0 &&
              completed.configuration.outputCallbackOrdinal == 0 &&
              completed.configuration.reportedRoundTripFrames ==
                  std::optional<std::uint64_t>{144} &&
              completed.residualFrames == std::optional<std::int64_t>{368},
          "productive loopback publishes reported, measured, residual and aggregate evidence");
    check(Access::device(*type).getCurrentBufferSizeSamples() == 256,
          "completed loopback restores the prior buffer");
    check(Access::device(*type).getCurrentSampleRate() == 48000.0,
          "completed loopback restores the prior sample rate");
    check(Access::device(*type).getActiveInputChannels() == expectedInput,
          "completed loopback restores the prior input mask");
    check(Access::device(*type).getActiveOutputChannels() == expectedOutput,
          "completed loopback restores the prior output mask");
    check(!app.inputMonitoringEnabled() && !Access::monitoringDemand(adapter) &&
              app.history().currentStateToken() == historyToken &&
              app.session().dirty() == dirty && !app.canUndo() && !app.canRedo() &&
              app.project().sources().size() == projectSources &&
              app.project().tracks().size() == projectTracks &&
              app.recordingPlacementReadModel().manualOffsetProjectFrames.value == -37,
          "loopback does not mutate ProjectState, history, dirty, Monitoring or Recording Placement");
    check(dispatcher.dispatch(commands::SetAudioBufferSize{512}).status ==
              commands::CommandStatus::accepted,
          "buffer control resumes normally after loopback completes");
    check(dispatcher.dispatch(commands::StartLoopbackLatencyTest{0, 0}).status ==
              commands::CommandStatus::accepted &&
              app.loopbackLatencyReadModel().configuration.bufferSizeFrames == 512,
          "a second physical loopback session snapshots the new effective buffer");
    for (int block = 0; block < 600 && app.loopbackLatencyReadModel().busy(); ++block) {
        Access::device(*type).renderPhysicalLoopback(1024, 512);
        app.synchroniseTransport();
    }
    check(app.loopbackLatencyReadModel().status ==
              audio::LoopbackLatencyStatus::completed &&
              app.loopbackLatencyReadModel().measuredRoundTripFrames ==
                  std::optional<std::uint64_t>{1024} &&
              Access::device(*type).getCurrentBufferSizeSamples() == 512,
          "loopback executes independently at a second virtual buffer");
    check(dispatcher.dispatch(commands::SetAudioBufferSize{256}).status ==
              commands::CommandStatus::accepted,
          "normal buffer control restores the fixture after the second measurement");

    const auto subsequent = adapter.prepareRecording(
        {{1}, media::AudioChannelLayout::mono, temporaryProject});
    check(subsequent.success() && adapter.tryRequestRecord(subsequent.request).accepted,
          "a subsequent normal Recording starts after loopback");
    Access::device(*type).render(0.25F);
    check(Access::recording(adapter).phase == audio::RecordingPhase::capturing &&
              adapter.tryRequestStop().accepted,
          "subsequent Recording captures and stops normally");
    Access::device(*type).render(0.0F);
    static_cast<void>(adapter.discardRecordingWithDiagnostics(true));

    check(dispatcher.dispatch(commands::StartLoopbackLatencyTest{0, 0}).status ==
              commands::CommandStatus::accepted,
          "a second loopback session starts after Recording cleanup");
    Access::device(*type).renderPhysicalLoopback(512);
    check(dispatcher.dispatch(commands::CancelLoopbackLatencyTest{}).status ==
              commands::CommandStatus::accepted,
          "loopback cancellation is accepted");
    app.synchroniseTransport();
    check(app.loopbackLatencyReadModel().status ==
              audio::LoopbackLatencyStatus::cancelled &&
              Access::device(*type).getActiveInputChannels() == expectedInput &&
              Access::device(*type).getActiveOutputChannels() == expectedOutput,
          "cancelled loopback restores the prior setup and leaves no active route");

    check(dispatcher.dispatch(commands::StartLoopbackLatencyTest{0, 0}).status ==
              commands::CommandStatus::accepted,
          "configuration-change loopback starts");
    Access::device(*type).setReportedBufferSize(128);
    adapter.pollDeviceLifecycle();
    app.synchroniseTransport();
    check(app.loopbackLatencyReadModel().status ==
              audio::LoopbackLatencyStatus::invalidated &&
              Access::device(*type).getCurrentBufferSizeSamples() == 256 &&
              Access::device(*type).getActiveInputChannels() == expectedInput &&
              Access::device(*type).getActiveOutputChannels() == expectedOutput,
          "critical configuration change invalidates the session and restores its checkpoint");
    adapter.shutdown();
}

void failedLoopbackRestoresDefaultOutputAndNormalWork() {
    {
        Adapter adapter;
        application::DawApplication app{adapter, timeline::SampleRate{48000.0}};
        VirtualAudioDeviceType* type{};
        check(Access::openDefaultOutput(adapter, 2, type),
              "open loopback fixture with JUCE default stereo output");
        commands::CommandDispatcher dispatcher{app};
        const auto before = Access::setup(adapter);
        check(before.useDefaultOutputChannels &&
                  Access::device(*type).getActiveInputChannels().isZero() &&
                  Access::device(*type).getActiveOutputChannels().countNumberOfSetBits() == 2,
              "fixture reproduces default-output checkpoint with two effective outputs");

        check(dispatcher.dispatch(commands::StartLoopbackLatencyTest{0, 0}).status ==
                  commands::CommandStatus::accepted &&
                  Access::device(*type).getActiveInputChannels().countNumberOfSetBits() == 1 &&
                  Access::device(*type).getActiveOutputChannels().countNumberOfSetBits() == 1,
              "loopback temporarily replaces defaults with one explicit physical pair");
        for (int block = 0; block < 1200 && app.loopbackLatencyReadModel().busy(); ++block) {
            Access::device(*type).render(0.0F);
            app.synchroniseTransport();
        }
        const auto failed = app.loopbackLatencyReadModel();
        check(failed.status == audio::LoopbackLatencyStatus::failed &&
                  failed.validTrials == 0 &&
                  std::all_of(failed.trials.begin(), failed.trials.end(), [](const auto& trial) {
                      return trial.quality == audio::LoopbackTrialQuality::signalTooLow;
                  }) &&
                  failed.diagnostic == "Fewer than three loopback trials were valid",
              "five invalid physical trials remain a measurement failure only");
        const auto restoredSetup = Access::setup(adapter);
        check(!restoredSetup.useDefaultInputChannels &&
                  !restoredSetup.useDefaultOutputChannels &&
                  restoredSetup.inputChannels.isZero() &&
                  restoredSetup.outputChannels.countNumberOfSetBits() == 2 &&
                  Access::device(*type).getActiveInputChannels().isZero() &&
                  Access::device(*type).getActiveOutputChannels().countNumberOfSetBits() == 2 &&
                  Access::callbackRegistered(adapter) &&
                  Access::deviceState(adapter) == audio::DeviceProcessingState::operational &&
                  adapter.state().status == audio::AudioDeviceStatus::active &&
                  app.deviceLatencyReadModel().configurationAvailable,
              "failed measurement restores effective masks, Core, callback and read model");
        check(!app.inputMonitoringEnabled() && !Access::monitoringDemand(adapter),
              "failed loopback leaves Monitoring OFF");

        check(dispatcher.dispatch(commands::SetMetronomeEnabled{true}).status ==
                  commands::CommandStatus::accepted &&
                  dispatcher.dispatch(commands::Play{}).status ==
                      commands::CommandStatus::accepted,
              "Playback request is available after failed loopback");
        Access::device(*type).render(0.0F);
        app.synchroniseTransport();
        check(app.transport().playback == transport::PlaybackState::playing &&
                  dispatcher.dispatch(commands::Stop{}).status ==
                      commands::CommandStatus::accepted,
              "normal Playback executes after failed loopback");
        Access::device(*type).render(0.0F);
        app.synchroniseTransport();
        check(dispatcher.dispatch(commands::SetMetronomeEnabled{false}).status ==
                  commands::CommandStatus::accepted,
              "Playback fixture returns to stopped silent state");

        const auto temporaryProject = std::filesystem::temp_directory_path() /
            "vitadaw-loopback-failed-measurement-recording.vitadaw";
        const auto preflight = adapter.prepareRecording(
            {{1}, media::AudioChannelLayout::mono, temporaryProject});
        check(preflight.success() && adapter.tryRequestRecord(preflight.request).accepted,
              "Recording prepares after failed loopback");
        Access::device(*type).render(0.25F);
        check(Access::recording(adapter).phase == audio::RecordingPhase::capturing &&
                  adapter.tryRequestStop().accepted,
              "Recording captures and accepts Stop after failed loopback");
        Access::device(*type).render(0.0F);
        static_cast<void>(adapter.discardRecordingWithDiagnostics(true));

        check(dispatcher.dispatch(commands::StartLoopbackLatencyTest{0, 0}).status ==
                  commands::CommandStatus::accepted &&
                  dispatcher.dispatch(commands::CancelLoopbackLatencyTest{}).status ==
                      commands::CommandStatus::accepted,
              "next loopback can start after a failed measurement");
        app.synchroniseTransport();
        check(app.loopbackLatencyReadModel().status ==
                  audio::LoopbackLatencyStatus::cancelled,
              "next loopback cancellation also restores normally");
        adapter.shutdown();
    }

    {
        Adapter adapter;
        application::DawApplication app{adapter, timeline::SampleRate{48000.0}};
        VirtualAudioDeviceType* type{};
        check(Access::openDefaultOutput(adapter, 2, type),
              "open two-valid-trial default-output fixture");
        commands::CommandDispatcher dispatcher{app};
        check(dispatcher.dispatch(commands::StartLoopbackLatencyTest{0, 0}).status ==
                  commands::CommandStatus::accepted,
              "two-valid-trial loopback starts");
        const auto stopReturningBefore = Access::thirdLoopbackEmission(adapter) - 512U;
        for (std::uint64_t frame = 0;
             frame < 1200U * 256U && app.loopbackLatencyReadModel().busy();
             frame += 256U) {
            Access::device(*type).renderPhysicalLoopback(
                512, 256, frame < stopReturningBefore);
            app.synchroniseTransport();
        }
        const auto failed = app.loopbackLatencyReadModel();
        check(failed.status == audio::LoopbackLatencyStatus::failed &&
                  failed.validTrials == 2 &&
                  Access::device(*type).getActiveOutputChannels().countNumberOfSetBits() == 2 &&
                  Access::callbackRegistered(adapter) &&
                  Access::deviceState(adapter) == audio::DeviceProcessingState::operational,
              "two valid trials still fail measurement but restore the default stereo output");
        adapter.shutdown();
    }

    {
        Adapter adapter;
        application::DawApplication app{adapter, timeline::SampleRate{48000.0}};
        VirtualAudioDeviceType* type{};
        check(Access::openDefaultOutput(adapter, 2, type),
              "open clipped loopback cleanup fixture");
        commands::CommandDispatcher dispatcher{app};
        check(dispatcher.dispatch(commands::StartLoopbackLatencyTest{0, 0}).status ==
                  commands::CommandStatus::accepted,
              "clipped loopback starts");
        Access::device(*type).render(1.0F);
        app.synchroniseTransport();
        const auto clipped = app.loopbackLatencyReadModel();
        check(clipped.status == audio::LoopbackLatencyStatus::failed &&
                  clipped.trials[0].clipped && clipped.trials[0].peak == 1.0F &&
                  Access::device(*type).getActiveOutputChannels().countNumberOfSetBits() == 2 &&
                  Access::callbackRegistered(adapter) &&
                  Access::deviceState(adapter) == audio::DeviceProcessingState::operational,
              "clipped measurement publishes peak and restores device/Core/callback");
        adapter.shutdown();
    }
}

void loopbackDeviceLossAndRollbackFailureAreSafe() {
    {
        Adapter adapter;
        application::DawApplication app{adapter, timeline::SampleRate{48000.0}};
        VirtualAudioDeviceType* type{};
        check(Access::openOutputOnly(adapter, 2, type),
              "open loopback device-loss fixture");
        commands::CommandDispatcher dispatcher{app};
        check(dispatcher.dispatch(commands::StartLoopbackLatencyTest{0, 0}).status ==
                  commands::CommandStatus::accepted,
              "device-loss loopback starts");
        Access::device(*type).renderPhysicalLoopback(512);
        Access::reportDeviceError(adapter);
        adapter.pollDeviceLifecycle();
        check(Access::loopbackProbeStatus(adapter) ==
                  audio::RealtimeLoopbackProbeStatus::deviceError,
              "device loss terminalizes the RT loopback probe as DeviceError");
        app.synchroniseTransport();
        check(app.loopbackLatencyReadModel().status ==
                  audio::LoopbackLatencyStatus::failed,
              "real device loss fails loopback explicitly");
        check(adapter.state().status == audio::AudioDeviceStatus::error,
              "real device loss retains the safe device-error state");
        check(!app.deviceLatencyReadModel().configurationAvailable,
              "real device loss invalidates the device read model");
        check(!Access::monitoringDemand(adapter),
              "real device loss cannot leave Monitoring demand active");
        adapter.shutdown();
    }

    {
        Adapter adapter;
        application::DawApplication app{adapter, timeline::SampleRate{48000.0}};
        VirtualAudioDeviceType* type{};
        check(Access::openOutputOnly(adapter, 2, type),
              "open loopback shutdown fixture");
        commands::CommandDispatcher dispatcher{app};
        check(dispatcher.dispatch(commands::StartLoopbackLatencyTest{0, 0}).status ==
                  commands::CommandStatus::accepted,
              "shutdown loopback starts");
        Access::device(*type).renderPhysicalLoopback(512);
        adapter.shutdown();
        check(app.loopbackLatencyReadModel().status ==
                  audio::LoopbackLatencyStatus::cancelled &&
                  Access::deviceState(adapter) != audio::DeviceProcessingState::operational &&
                  !Access::monitoringDemand(adapter),
              "shutdown cancels loopback, removes callback demand and closes safely");
    }

    {
        Adapter adapter;
        application::DawApplication app{adapter, timeline::SampleRate{48000.0}};
        VirtualAudioDeviceType* type{};
        check(Access::openOutputOnly(adapter, 2, type),
              "open unknown reported-latency fixture");
        type->inputLatency = -1;
        commands::CommandDispatcher dispatcher{app};
        check(dispatcher.dispatch(commands::StartLoopbackLatencyTest{0, 0}).status ==
                  commands::CommandStatus::accepted,
              "loopback can start when reported input latency is unknown");
        const auto running = app.loopbackLatencyReadModel();
        check(!running.configuration.reportedInputFrames &&
                  running.configuration.reportedOutputFrames ==
                      std::optional<std::uint32_t>{96} &&
                  !running.configuration.reportedRoundTripFrames,
              "unknown latency remains Unknown and is not replaced by zero");
        check(dispatcher.dispatch(commands::CancelLoopbackLatencyTest{}).status ==
                  commands::CommandStatus::accepted,
              "unknown reported-latency fixture cancels");
        app.synchroniseTransport();
        adapter.shutdown();
    }

    {
        Adapter adapter;
        application::DawApplication app{adapter, timeline::SampleRate{48000.0}};
        VirtualAudioDeviceType* type{};
        check(Access::openOutputOnly(adapter, 2, type),
              "open loopback rollback-failure fixture");
        commands::CommandDispatcher dispatcher{app};
        check(dispatcher.dispatch(commands::StartLoopbackLatencyTest{0, 0}).status ==
                  commands::CommandStatus::accepted,
              "rollback-failure loopback starts");
        type->effectiveSampleRateOverride = 44100.0;
        check(dispatcher.dispatch(commands::CancelLoopbackLatencyTest{}).status ==
                  commands::CommandStatus::accepted,
              "rollback-failure loopback is cancelled");
        app.synchroniseTransport();
        check(app.loopbackLatencyReadModel().status ==
                  audio::LoopbackLatencyStatus::failed &&
                  adapter.state().status == audio::AudioDeviceStatus::error &&
                  adapter.state().errorMessage.find("restored sample rate differs") !=
                      std::string::npos &&
                  app.loopbackLatencyReadModel().diagnostic.find(
                      "restored sample rate differs") != std::string::npos &&
                  !app.deviceLatencyReadModel().configurationAvailable &&
                  !Access::monitoringDemand(adapter),
              "inexact rollback preserves its causal diagnostic in the safe device-error state");
        Access::refreshState(adapter);
        check(adapter.state().errorMessage.find("restored sample rate differs") !=
                  std::string::npos,
              "later lifecycle refresh cannot replace the causal rollback diagnostic");
        adapter.shutdown();
    }
}

} // namespace

int main() {
    juce::ScopedJuceInitialiser_GUI initialiseJuce;
    outputOnlyEnableAndIdempotence();
    monoFallbackAndRollback();
    rejectedRtPublicationRollsBackPreflight();
    bufferLifecycleAndForcedLossIntent();
    deviceLossClearsToggleIntentAndErrorIsDeferred();
    controlledReconfigureAndLoss();
    recordingAndMonitoringShareInput();
    recordingFinalizationPreservesMonitoring();
    recordingStopsWithoutMonitoringDemand();
    disablingMonitoringDuringRecordingAndAbortAreIndependent();
    recordingAndMonitoringShutdown();
    recordingPlacementSnapshotIsPreparedOutsideRt();
    recordingPlacementIsMonitoringAndWavIndependent();
    recordingPlacementSnapshotsProductiveBufferChanges();
    unavailableInputLatencyAppliesManualPlacementProductively();
    bufferControlPublishesEffectiveConfigurationAndLatency();
    bufferControlRollbackFailureAndRecordingRejection();
    bufferControlVerifiesRollbackCheckpointAndStagingPreparation();
    loopbackPreconditionsCompletionCancelAndRecordingRecovery();
    failedLoopbackRestoresDefaultOutputAndNormalWork();
    loopbackDeviceLossAndRollbackFailureAreSafe();
}
