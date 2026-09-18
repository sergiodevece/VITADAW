#include "vitadaw/application/DawApplication.h"
#include "vitadaw/commands/CommandDispatcher.h"
#include "vitadaw/platform/juce/JuceAudioDeviceAdapter.h"

#include <array>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <thread>

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
    VirtualAudioDevice(int inputChannels, bool rejectInputs)
        : AudioIODevice("VitaDAW monitoring test device", deviceTypeName),
          inputChannelsAvailable_(inputChannels), rejectInputOpen(rejectInputs) {}

    juce::StringArray getOutputChannelNames() override { return {"Left", "Right"}; }
    juce::StringArray getInputChannelNames() override {
        return inputChannelsAvailable_ == 2 ? juce::StringArray{"Input 1", "Input 2"}
                                            : juce::StringArray{"Input 1"};
    }
    juce::Array<double> getAvailableSampleRates() override { return {48000.0, 44100.0}; }
    juce::Array<int> getAvailableBufferSizes() override { return {128, 256}; }
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

    void render(float sample, std::size_t frames = 256) {
        check(frames <= 256, "virtual callback buffer capacity");
        std::array<float, 256> input0{}, input1{}, output0{}, output1{};
        input0.fill(sample);
        input1.fill(sample * 0.5F);
        std::array<const float*, 2> input{input0.data(), input1.data()};
        std::array<float*, 2> output{output0.data(), output1.data()};
        if (callback_ != nullptr) {
            callback_->audioDeviceIOCallbackWithContext(
                input.data(), inputChannels_.countNumberOfSetBits(), output.data(), 2,
                static_cast<int>(frames), {});
        }
        lastOutput = output0[frames - 1];
    }

    void setReportedBufferSize(int buffer) noexcept { bufferSize_ = buffer; }
    void setReportedSampleRate(double rate) noexcept { sampleRate_ = rate; }
    void restart() { start(callback_); }
    void reportError() {
        if (callback_ != nullptr) callback_->audioDeviceError("virtual test error");
    }

    bool rejectInputOpen{};
    int openCount{};
    float lastOutput{};

private:
    int inputChannelsAvailable_{};
    juce::AudioIODeviceCallback* callback_{};
    juce::BigInteger inputChannels_;
    juce::BigInteger outputChannels_;
    double sampleRate_{48000.0};
    int bufferSize_{256};
    bool open_{};
    bool playing_{};
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
        auto* device = new VirtualAudioDevice(inputs_, rejectInputOpen);
        lastDevice = device;
        return device;
    }

    VirtualAudioDevice* lastDevice{};
    bool rejectInputOpen{};

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
        return adapter.callbackRegistered_ &&
               adapter.realtimeEngine_.deviceState() == audio::DeviceProcessingState::operational;
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
              afterBuffer.monitoringRouteSupported && device.lastOutput > 0.01F,
          "buffer-only divergence reprepares staging and preserves monitoring after success");

    // Invalid current hardware is a real controlled-reprepare failure. The
    // productive polling path must force OFF and clear desired application intent.
    device.setReportedBufferSize(0);
    adapter.pollDeviceLifecycle();
    app.synchroniseTransport();
    check(!app.inputMonitoringEnabled() && !Access::monitoringDemand(adapter) &&
              Access::snapshot(adapter).monitoringLifecycleForcedOff &&
              adapter.state().status == audio::AudioDeviceStatus::error,
          "failed buffer lifecycle reprepare forces monitoring off without auto-reactivation");
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
}
