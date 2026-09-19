#include "vitadaw/platform/juce/MainWindow.h"
#include "vitadaw/platform/juce/TimelineComponent.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <vector>

namespace vitadaw::platform::juce_adapter {

namespace {
const char* playbackName(transport::PlaybackState state) noexcept {
    if (state == transport::PlaybackState::playing) return "Playing";
    if (state == transport::PlaybackState::paused) return "Paused";
    return "Stopped";
}

juce::String positionText(timeline::Seconds position) {
    const auto totalMilliseconds = static_cast<std::int64_t>(
        std::max(0.0, std::round(position.value * 1000.0)));
    const auto minutes = totalMilliseconds / 60000;
    const auto seconds = (totalMilliseconds / 1000) % 60;
    const auto milliseconds = totalMilliseconds % 1000;
    return juce::String(minutes).paddedLeft('0', 2) + ":" +
           juce::String(seconds).paddedLeft('0', 2) + "." +
           juce::String(milliseconds).paddedLeft('0', 3);
}

juce::String latencyText(const char* name, std::optional<std::uint32_t> frames,
                         std::optional<double> milliseconds, bool estimated = false) {
    juce::String text{name};
    text << ": ";
    if (!frames || !milliseconds) return text + "Unknown";
    if (estimated) text << "~";
    text << static_cast<int>(*frames) << " samples / "
         << juce::String(*milliseconds, 2) << " ms";
    if (estimated) text << " (Estimated)";
    else text << " (Reported)";
    return text;
}

juce::String loopbackFramesText(std::optional<std::uint64_t> frames,
                                double sampleRate) {
    if (!frames) return "Unknown";
    juce::String text{static_cast<juce::int64>(*frames)};
    text << " frames";
    if (std::isfinite(sampleRate) && sampleRate > 0.0)
        text << " / " << juce::String(static_cast<double>(*frames) / sampleRate * 1000.0, 3)
             << " ms";
    return text;
}

juce::String loopbackSignedFramesText(std::optional<std::int64_t> frames,
                                      double sampleRate) {
    if (!frames) return "Unknown";
    juce::String text;
    if (*frames >= 0) text << "+";
    text << static_cast<juce::int64>(*frames) << " frames";
    if (std::isfinite(sampleRate) && sampleRate > 0.0)
        text << " / " << juce::String(static_cast<double>(*frames) / sampleRate * 1000.0, 3)
             << " ms";
    return text;
}

juce::String loopbackPeakText(float peak) {
    if (!std::isfinite(peak) || peak < 0.0F) return "Invalid";
    if (peak == 0.0F) return "-inf dBFS";
    return juce::String(20.0 * std::log10(static_cast<double>(peak)), 1) + " dBFS";
}

juce::String loopbackSnrText(double snr) {
    if (std::isinf(snr) && snr > 0.0) return "inf dB";
    if (!std::isfinite(snr)) return "Unknown";
    return juce::String(snr, 1) + " dB";
}
} // namespace

class AudioStatusComponent final : public juce::Component {
public:
    std::function<void()> documentLoaded;

    AudioStatusComponent(commands::ICommandDispatcher& dispatcher,
                         application::DawApplication& application)
        : dispatcher_(dispatcher), application_(application),
          timeline_(dispatcher, application) {
        for (auto* label : {&statusLabel_, &transportLabel_, &meterLabel_, &inputMeterLabel_,
                            &resultLabel_}) {
            label->setColour(juce::Label::textColourId, juce::Colours::white);
            label->setJustificationType(juce::Justification::centredLeft);
            addAndMakeVisible(*label);
        }
        for (auto* label : {&inputLatencyLabel_, &outputLatencyLabel_,
                            &monitoringLatencyLabel_, &recordingPlacementLabel_,
                            &loopbackInstructionLabel_, &loopbackResultLabel_,
                            &loopbackDetailLabel_, &loopbackTrialsLabel_}) {
            label->setColour(juce::Label::textColourId, juce::Colours::lightgrey);
            label->setJustificationType(juce::Justification::centredLeft);
            addAndMakeVisible(*label);
        }
        loopbackTrialsLabel_.setJustificationType(juce::Justification::topLeft);
        loopbackTrialsLabel_.setFont(juce::FontOptions{11.5F});
        audioBuffer_.onChange = [this] {
            const auto index = audioBuffer_.getSelectedItemIndex();
            if (index < 0 || static_cast<std::size_t>(index) >= displayedBufferSizes_.size()) return;
            dispatch(commands::SetAudioBufferSize{
                displayedBufferSizes_[static_cast<std::size_t>(index)]});
        };
        addAndMakeVisible(audioBuffer_);

        timeline_.commandCompleted = [this](const commands::CommandResult& result) {
            showResult(result);
            updateHistoryControls();
        };
        addAndMakeVisible(timeline_);

        importButton_.onClick = [this] {
            if (application_.project().tracks().empty()) {
                chooseWav(std::nullopt);
            } else if (const auto selected = timeline_.selectedTrackId()) {
                chooseWav(*selected);
            } else {
                showResult({commands::CommandStatus::rejected,
                            "Select a target track before importing audio",
                            commands::CommandError::selectTargetTrack});
            }
        };
        addMonoTrack_.onClick = [this] {
            dispatch(commands::AddAudioTrack{
                {}, media::AudioChannelLayout::mono});
        };
        addStereoTrack_.onClick = [this] {
            dispatch(commands::AddAudioTrack{
                {}, media::AudioChannelLayout::stereo});
        };
        deleteTrack_.onClick = [this] {
            const auto selected = timeline_.selectedTrackId();
            if (!selected) {
                showResult({commands::CommandStatus::rejected,
                            "Select a track before deleting it",
                            commands::CommandError::selectTargetTrack});
                return;
            }
            dispatch(commands::DeleteAudioTrack{*selected});
        };
        moveTrackUp_.onClick = [this] { reorderSelectedTrack(false); };
        moveTrackDown_.onClick = [this] { reorderSelectedTrack(true); };
        for (auto* button : {&importButton_, &addMonoTrack_,
                             &addStereoTrack_, &deleteTrack_,
                             &moveTrackUp_, &moveTrackDown_})
            addAndMakeVisible(*button);

        playButton_.onClick = [this] { dispatch(commands::Play{}); };
        pauseButton_.onClick = [this] { dispatch(commands::Pause{}); };
        stopButton_.onClick = [this] { dispatch(commands::Stop{}); };
        armButton_.onClick = [this] {
            const auto selected = timeline_.selectedTrackId();
            if (!selected) {
                showResult({commands::CommandStatus::rejected,
                            "Select a track before arming it",
                            commands::CommandError::selectTargetTrack});
                return;
            }
            dispatch(commands::SetTrackRecordArmed{
                *selected, application_.armedTrack() != selected});
        };
        recordButton_.onClick = [this] { dispatch(commands::Record{}); };
        undoButton_.onClick = [this] { dispatch(commands::Undo{}); };
        redoButton_.onClick = [this] { dispatch(commands::Redo{}); };
        saveButton_.onClick = [this] { dispatch(commands::SaveProject{}); };
        saveAsButton_.onClick = [this] { chooseProject(true); };
        loadProjectButton_.onClick = [this] { chooseProject(false); };
        tempo100_.onClick = [this] {
            dispatch(commands::SetTempo{application_.project().musicalTime().tempo.events.front().id, {100.0}});
        };
        tempoChange_.onClick = [this] { dispatch(commands::AddTempoChange{{16 * musical::ppq}, {60.0}}); };
        signatureChange_.onClick = [this] { dispatch(commands::AddTimeSignatureChange{{8}, {7,8}}); };
        startBar_.setText("2", false);
        endBar_.setText("4", false);
        startBarLabel_.setText("Start Bar", juce::dontSendNotification);
        endBarLabel_.setText("End Bar", juce::dontSendNotification);
        for (auto* label : {&startBarLabel_, &endBarLabel_}) {
            label->setColour(juce::Label::textColourId, juce::Colours::white);
            addAndMakeVisible(*label);
        }
        startBar_.setInputRestrictions(8, "0123456789");
        endBar_.setInputRestrictions(8, "0123456789");
        applyLoop_.onClick = [this] {
            const auto start = startBar_.getText().getLargeIntValue();
            const auto end = endBar_.getText().getLargeIntValue();
            if (start <= 0 || end <= start) {
                showResult({commands::CommandStatus::rejected,
                            "Loop bars must satisfy 1 <= start < end"});
                return;
            }
            const auto startTick = application_.musicalTime().tickAt(
                {{start - 1}, {0}, {0}});
            const auto endTick = application_.musicalTime().tickAt(
                {{end - 1}, {0}, {0}});
            if (!startTick || !endTick) {
                showResult({commands::CommandStatus::rejected,
                            "Loop bars cannot be represented by the current meter map"});
                return;
            }
            dispatch(commands::SetLoopRangeMusical{startTick.value,
                                                    endTick.value});
        };
        loopEnabled_.setClickingTogglesState(true);
        loopEnabled_.onClick = [this] {
            dispatch(commands::SetLoopEnabled{loopEnabled_.getToggleState()});
        };
        metronomeEnabled_.setClickingTogglesState(true);
        metronomeEnabled_.onClick = [this] {
            dispatch(commands::SetMetronomeEnabled{
                metronomeEnabled_.getToggleState()});
        };
        metronomeLevel_.setRange(-100.0, 0.0, 0.1);
        metronomeLevel_.setValue(-12.0, juce::dontSendNotification);
        metronomeLevel_.setTextValueSuffix(" dB");
        metronomeLevel_.onValueChange = [this] {
            dispatch(commands::SetMetronomeLevel{{
                static_cast<float>(metronomeLevel_.getValue())}});
        };
        monitorButton_.setClickingTogglesState(false);
        monitorButton_.onClick = [this] {
            // The button is set from the confirmed read model only; this
            // action asks Command System to toggle rather than changing audio.
            dispatch(commands::ToggleInputMonitoring{});
        };
        monitorGain_.setRange(-100.0, 0.0, 0.1);
        monitorGain_.setValue(-12.0, juce::dontSendNotification);
        monitorGain_.setTextValueSuffix(" dB");
        monitorGain_.onValueChange = [this] {
            dispatch(commands::SetMonitorGain{{
                static_cast<float>(monitorGain_.getValue())}});
        };
        recordingOffset_.setTextValueSuffix(" frames");
        recordingOffset_.onValueChange = [this] {
            dispatch(commands::SetRecordingOffset{
                static_cast<std::int64_t>(std::llround(recordingOffset_.getValue()))});
        };
        loopbackRun_.onClick = [this] {
            if (loopbackInput_.getSelectedId() <= 0 ||
                loopbackOutput_.getSelectedId() <= 0) {
                showResult({commands::CommandStatus::rejected,
                            "Select one physical output and one physical input"});
                return;
            }
            dispatch(commands::StartLoopbackLatencyTest{
                static_cast<std::uint32_t>(loopbackInput_.getSelectedId() - 1),
                static_cast<std::uint32_t>(loopbackOutput_.getSelectedId() - 1)});
        };
        loopbackCancel_.onClick = [this] {
            dispatch(commands::CancelLoopbackLatencyTest{});
        };
        loopbackInstructionLabel_.setText(
            "Loopback: physically connect the selected Output to Input; Monitoring must be OFF.",
            juce::dontSendNotification);
        for (auto* editor : {&startBar_, &endBar_}) addAndMakeVisible(*editor);
        addAndMakeVisible(applyLoop_);
        addAndMakeVisible(loopEnabled_);
        addAndMakeVisible(metronomeEnabled_);
        addAndMakeVisible(metronomeLevel_);
        addAndMakeVisible(monitorButton_);
        addAndMakeVisible(monitorGain_);
        addAndMakeVisible(recordingOffset_);
        addAndMakeVisible(recordingPlacementLabel_);
        addAndMakeVisible(loopbackInstructionLabel_);
        addAndMakeVisible(loopbackInput_);
        addAndMakeVisible(loopbackOutput_);
        addAndMakeVisible(loopbackRun_);
        addAndMakeVisible(loopbackCancel_);
        addAndMakeVisible(loopbackResultLabel_);
        addAndMakeVisible(loopbackDetailLabel_);
        for (auto* button : {&tempo100_, &tempoChange_, &signatureChange_}) addAndMakeVisible(*button);
        for (auto* button : {&playButton_, &pauseButton_, &stopButton_, &undoButton_, &redoButton_,
                             &saveButton_, &saveAsButton_, &loadProjectButton_})
            addAndMakeVisible(*button);
        addAndMakeVisible(armButton_);
        addAndMakeVisible(recordButton_);
        updateHistoryControls();
    }

    void setTransportState(const transport::TransportState& state,
                           timeline::SampleRate projectSampleRate) {
        const auto position = timeline::projectPositionToSeconds(state.position,
                                                                  projectSampleRate);
        const auto duration = timeline::projectFramesToSeconds(state.duration,
                                                                projectSampleRate);
        juce::String text;
        text << "Transport: "
             << playbackName(state.playback)
             << " | " << positionText(position) << " / "
             << juce::String(duration.value, 3) << " s"
             << " | Frames: " << juce::String(state.position.value)
             << " | Project: " << juce::String(projectSampleRate.hertz(), 0) << " Hz";
        transportLabel_.setText(text, juce::dontSendNotification);
        const auto musicalPosition = application_.musicalTime().musicalPositionAt(state.position);
        if (musicalPosition) {
            const auto& p = musicalPosition.value;
            text << " | Bars: " << juce::String(p.bar.value + 1) << "|" << juce::String(p.beat.value + 1) << "|" << juce::String(p.tick.value);
            transportLabel_.setText(text, juce::dontSendNotification);
        }
        for (auto* button : {&tempo100_, &tempoChange_, &signatureChange_})
            button->setEnabled(state.playback == transport::PlaybackState::stopped);
        const auto stopped = state.playback == transport::PlaybackState::stopped;
        const auto recordingPhase = application_.recordingPhase();
        const auto recordingCanStop = recordingPhase == audio::RecordingPhase::prepared ||
                                      recordingPhase == audio::RecordingPhase::capturing;
        const auto recordingBusy = recordingCanStop ||
                                   recordingPhase == audio::RecordingPhase::finalizing;
        const auto selected = timeline_.selectedTrackId();
        const auto armed = application_.armedTrack();
        armButton_.setButtonText(armed && selected == armed ? "Disarm Track" : "Arm Track");
        armButton_.setEnabled(stopped && !recordingBusy && selected.has_value());
        recordButton_.setButtonText(recordingCanStop ? "Stop Record" : "Record");
        recordButton_.setEnabled(recordingCanStop ||
                                 (!recordingBusy && stopped && armed.has_value()));
        recordButton_.setColour(juce::TextButton::buttonColourId,
                                recordingBusy ? juce::Colours::darkred
                                              : juce::Colour{0xff9d2028});
        if (recordingPhase != displayedRecordingPhase_) {
            displayedRecordingPhase_ = recordingPhase;
            if (recordingPhase == audio::RecordingPhase::failed)
                resultLabel_.setText("Error: " + juce::String(application_.recordingError()),
                                     juce::dontSendNotification);
            else if (recordingPhase == audio::RecordingPhase::complete)
                resultLabel_.setText("OK: Recording committed" +
                                     (application_.recordingError().empty()
                                          ? juce::String{}
                                          : " | " + juce::String(application_.recordingError())),
                                     juce::dontSendNotification);
        }
        addMonoTrack_.setEnabled(stopped);
        addStereoTrack_.setEnabled(stopped);
        deleteTrack_.setEnabled(stopped);
        importButton_.setEnabled(stopped);
        startBar_.setEnabled(stopped);
        endBar_.setEnabled(stopped);
        applyLoop_.setEnabled(stopped);
        loopEnabled_.setEnabled(stopped);
        loopEnabled_.setToggleState(application_.loopEnabled(),
                                    juce::dontSendNotification);
        const auto metronome = application_.metronomeReadModel();
        metronomeEnabled_.setToggleState(metronome.enabled,
                                         juce::dontSendNotification);
        metronomeLevel_.setValue(metronome.level.value,
                                 juce::dontSendNotification);
        updateMonitoringControls();
        updateDeviceLatencyControls();
        updateLoopbackControls(state.playback);
        timeline_.setTransportState(state);
        updateHistoryControls();
    }

    void setAudioDeviceState(const audio::AudioDeviceState& state) {
        juce::String text;
        if (state.status == audio::AudioDeviceStatus::active) {
            text << "Audio: " << state.info.outputDeviceName << " | "
                 << juce::String(state.info.sampleRate, 0) << " Hz | "
                 << static_cast<int>(state.info.bufferSizeFrames) << " frames | "
                 << static_cast<int>(state.info.availableInputChannels) << " in / "
                 << static_cast<int>(state.info.availableOutputChannels) << " out";
        } else if (state.status == audio::AudioDeviceStatus::error) {
            text << "Audio device error: " << state.errorMessage;
        } else {
            text << "Audio device closed";
        }
        statusLabel_.setText(text, juce::dontSendNotification);
    }

    void setMeterState(const mixer::MeterSnapshot& meters) {
        juce::String text{"Meters: Master "};
        text << juce::String(meters.master.left, 3) << " / "
             << juce::String(meters.master.right, 3);
        const auto visibleTracks = std::min<std::size_t>(meters.trackCount, 4);
        for (std::size_t index = 0; index < visibleTracks; ++index) {
            text << " | T" << static_cast<int>(meters.tracks[index].track.value)
                 << " " << juce::String(meters.tracks[index].peak.left, 3)
                 << " / " << juce::String(meters.tracks[index].peak.right, 3);
        }
        meterLabel_.setText(text, juce::dontSendNotification);
    }

    void paint(juce::Graphics& graphics) override {
        graphics.fillAll(juce::Colour{0xff17191d});
    }

    void resized() override {
        auto bounds = getLocalBounds().reduced(12);
        statusLabel_.setBounds(bounds.removeFromTop(26));
        transportLabel_.setBounds(bounds.removeFromTop(26));
        meterLabel_.setBounds(bounds.removeFromTop(22));
        inputMeterLabel_.setBounds(bounds.removeFromTop(22));
        auto deviceRow = bounds.removeFromTop(28);
        audioBuffer_.setBounds(deviceRow.removeFromLeft(210));
        inputLatencyLabel_.setBounds(deviceRow.removeFromLeft(235));
        outputLatencyLabel_.setBounds(deviceRow.removeFromLeft(245));
        monitoringLatencyLabel_.setBounds(deviceRow);
        auto musicalRow = bounds.removeFromTop(28);
        tempo100_.setBounds(musicalRow.removeFromLeft(160));
        tempoChange_.setBounds(musicalRow.removeFromLeft(240));
        signatureChange_.setBounds(musicalRow.removeFromLeft(240));

        auto loopRow = bounds.removeFromTop(30);
        startBarLabel_.setBounds(loopRow.removeFromLeft(62));
        startBar_.setBounds(loopRow.removeFromLeft(54));
        loopRow.removeFromLeft(4);
        endBarLabel_.setBounds(loopRow.removeFromLeft(56));
        endBar_.setBounds(loopRow.removeFromLeft(54));
        loopRow.removeFromLeft(4);
        applyLoop_.setBounds(loopRow.removeFromLeft(130));
        loopRow.removeFromLeft(6);
        loopEnabled_.setBounds(loopRow.removeFromLeft(105));
        loopRow.removeFromLeft(12);
        metronomeEnabled_.setBounds(loopRow.removeFromLeft(110));
        metronomeLevel_.setBounds(loopRow.removeFromLeft(190));

        auto monitoringRow = bounds.removeFromTop(30);
        monitorButton_.setBounds(monitoringRow.removeFromLeft(130));
        monitoringRow.removeFromLeft(8);
        monitorGain_.setBounds(monitoringRow.removeFromLeft(210));
        recordingOffset_.setBounds(monitoringRow.removeFromLeft(230));
        recordingPlacementLabel_.setBounds(bounds.removeFromTop(24));

        loopbackInstructionLabel_.setBounds(bounds.removeFromTop(22));
        auto loopbackRow = bounds.removeFromTop(30);
        loopbackOutput_.setBounds(loopbackRow.removeFromLeft(235));
        loopbackRow.removeFromLeft(6);
        loopbackInput_.setBounds(loopbackRow.removeFromLeft(235));
        loopbackRow.removeFromLeft(6);
        loopbackRun_.setBounds(loopbackRow.removeFromLeft(94));
        loopbackRow.removeFromLeft(6);
        loopbackCancel_.setBounds(loopbackRow.removeFromLeft(82));
        loopbackResultLabel_.setBounds(bounds.removeFromTop(24));
        loopbackDetailLabel_.setBounds(bounds.removeFromTop(24));
        loopbackTrialsLabel_.setBounds(bounds.removeFromTop(88));

        auto commandRow = bounds.removeFromTop(32);
        armButton_.setBounds(commandRow.removeFromLeft(112));
        commandRow.removeFromLeft(5);
        recordButton_.setBounds(commandRow.removeFromLeft(84));
        commandRow.removeFromLeft(10);
        for (auto* button : {&playButton_, &pauseButton_, &stopButton_, &undoButton_, &redoButton_,
                             &saveButton_, &saveAsButton_, &loadProjectButton_}) {
            const auto width = button == &loadProjectButton_ ? 126 :
                               button == &saveAsButton_ ? 88 : 76;
            button->setBounds(commandRow.removeFromLeft(width));
            commandRow.removeFromLeft(5);
        }
        bounds.removeFromTop(5);
        auto importRow = bounds.removeFromTop(32);
        for (auto* button : {&importButton_, &addMonoTrack_,
                             &addStereoTrack_, &deleteTrack_,
                             &moveTrackUp_, &moveTrackDown_}) {
            button->setBounds(importRow.removeFromLeft(
                button == &moveTrackUp_ || button == &moveTrackDown_ ? 125 : 140));
            importRow.removeFromLeft(5);
        }
        bounds.removeFromTop(8);
        resultLabel_.setBounds(bounds.removeFromBottom(26));
        bounds.removeFromBottom(6);
        timeline_.setBounds(bounds);
    }

private:
    void reorderSelectedTrack(bool down) {
        const auto selected = timeline_.selectedTrackId();
        if (!selected) {
            showResult({commands::CommandStatus::rejected,
                        "Select a track before reordering it",
                        commands::CommandError::selectTargetTrack});
            return;
        }
        const auto& tracks = application_.project().tracks();
        const auto found = std::find_if(
            tracks.begin(), tracks.end(),
            [&](const auto& track) { return track.id == *selected; });
        if (found == tracks.end()) return;
        const auto index = static_cast<std::size_t>(found - tracks.begin());
        if ((!down && index == 0) || (down && index + 1 == tracks.size())) {
            showResult({commands::CommandStatus::accepted,
                        "Track order unchanged"});
            return;
        }
        const auto anchor = down ? tracks[index + 1].id : tracks[index - 1].id;
        dispatch(commands::ReorderAudioTrack{
            *selected, anchor,
            down ? commands::TrackPlacement::after
                 : commands::TrackPlacement::before});
    }

    void updateHistoryControls() {
        const auto undo = application_.undoLabel();
        const auto redo = application_.redoLabel();
        undoButton_.setButtonText(undo.empty() ? "Undo" :
                                  "Undo " + juce::String(undo.data(), undo.size()));
        redoButton_.setButtonText(redo.empty() ? "Redo" :
                                  "Redo " + juce::String(redo.data(), redo.size()));
        undoButton_.setEnabled(application_.canUndo());
        redoButton_.setEnabled(application_.canRedo());
    }

    void updateMonitoringControls() {
        const auto monitoring = application_.inputMonitoringReadModel();
        // Do not display a requested Enable as applied while its preflight or
        // RT publication is pending. The next timer tick uses confirmed state.
        monitorButton_.setToggleState(monitoring.enabled, juce::dontSendNotification);
        monitorButton_.setButtonText(monitoring.enabled ? "Monitor ON" : "Monitor OFF");
        monitorGain_.setValue(monitoring.gain.value, juce::dontSendNotification);

        if (monitoring.inputAvailable) {
            inputPeakLeft_ = std::max(monitoring.inputPeak.left, inputPeakLeft_ * 0.82F);
            inputPeakRight_ = std::max(monitoring.inputPeak.right, inputPeakRight_ * 0.82F);
        } else {
            inputPeakLeft_ *= 0.82F;
            inputPeakRight_ *= 0.82F;
        }
        juce::String text{"Input peak (pre-gain): "};
        text << juce::String(inputPeakLeft_, 3) << " / " << juce::String(inputPeakRight_, 3);
        if (!monitoring.routeSupported) text << " | Monitor route limited";
        else if (!monitoring.inputAvailable) text << " | Input unavailable";
        if (monitoring.lifecycleForcedOff) text << " | Monitoring forced OFF";
        inputMeterLabel_.setText(text, juce::dontSendNotification);
    }

    void updateDeviceLatencyControls() {
        const auto device = application_.deviceLatencyReadModel();
        if (device.inputChannelNames != displayedInputChannels_) {
            const auto selected = loopbackInput_.getSelectedId();
            displayedInputChannels_ = device.inputChannelNames;
            loopbackInput_.clear(juce::dontSendNotification);
            for (std::size_t index = 0; index < displayedInputChannels_.size(); ++index)
                loopbackInput_.addItem("Input " + juce::String(static_cast<int>(index + 1U)) +
                                           ": " + juce::String(displayedInputChannels_[index]),
                                       static_cast<int>(index + 1U));
            if (!displayedInputChannels_.empty())
                loopbackInput_.setSelectedId(
                    selected > 0 && selected <= loopbackInput_.getNumItems() ? selected : 1,
                    juce::dontSendNotification);
        }
        if (device.outputChannelNames != displayedOutputChannels_) {
            const auto selected = loopbackOutput_.getSelectedId();
            displayedOutputChannels_ = device.outputChannelNames;
            loopbackOutput_.clear(juce::dontSendNotification);
            for (std::size_t index = 0; index < displayedOutputChannels_.size(); ++index)
                loopbackOutput_.addItem("Output " + juce::String(static_cast<int>(index + 1U)) +
                                            ": " + juce::String(displayedOutputChannels_[index]),
                                        static_cast<int>(index + 1U));
            if (!displayedOutputChannels_.empty())
                loopbackOutput_.setSelectedId(
                    selected > 0 && selected <= loopbackOutput_.getNumItems() ? selected : 1,
                    juce::dontSendNotification);
        }
        if (device.supportedBufferSizeFrames != displayedBufferSizes_) {
            displayedBufferSizes_ = device.supportedBufferSizeFrames;
            audioBuffer_.clear(juce::dontSendNotification);
            for (const auto size : displayedBufferSizes_)
                audioBuffer_.addItem("Audio Buffer: " + juce::String(size) + " samples",
                                     static_cast<int>(audioBuffer_.getNumItems()) + 1);
        }
        auto selected = -1;
        for (std::size_t index = 0; index < displayedBufferSizes_.size(); ++index) {
            if (displayedBufferSizes_[index] == device.confirmedBufferSizeFrames) {
                selected = static_cast<int>(index) + 1;
                break;
            }
        }
        audioBuffer_.setSelectedId(selected, juce::dontSendNotification);
        const auto recordingPhase = application_.recordingPhase();
        const auto recordingBusy = recordingPhase == audio::RecordingPhase::prepared ||
                                   recordingPhase == audio::RecordingPhase::capturing ||
                                   recordingPhase == audio::RecordingPhase::finalizing;
        audioBuffer_.setEnabled(device.configurationAvailable &&
                                !displayedBufferSizes_.empty() && !recordingBusy);
        if (!device.configurationAvailable) {
            audioBuffer_.setText("Audio Buffer: Unknown", juce::dontSendNotification);
        } else if (selected <= 0) {
            // A backend may negotiate an effective size that it does not list
            // as directly selectable. Keep the two concepts distinct while
            // making the certified physical value visible and leaving the
            // reported choices available for the next request.
            audioBuffer_.setText("Audio Buffer: " +
                                     juce::String(device.confirmedBufferSizeFrames) + " samples",
                                 juce::dontSendNotification);
        }

        inputLatencyLabel_.setText(latencyText("Input Latency", device.inputLatencyFrames,
                                                device.inputLatencyMilliseconds),
                                   juce::dontSendNotification);
        outputLatencyLabel_.setText(latencyText("Output Latency", device.outputLatencyFrames,
                                                 device.outputLatencyMilliseconds),
                                    juce::dontSendNotification);
        std::optional<std::uint32_t> estimatedFrames;
        if (device.inputLatencyFrames && device.outputLatencyFrames) {
            const auto total = static_cast<std::uint64_t>(*device.inputLatencyFrames) +
                               static_cast<std::uint64_t>(*device.outputLatencyFrames);
            if (total <= std::numeric_limits<std::uint32_t>::max())
                estimatedFrames = static_cast<std::uint32_t>(total);
        }
        monitoringLatencyLabel_.setText(
            latencyText("Monitoring Latency", estimatedFrames,
                        device.estimatedMonitoringLatencyMilliseconds, true),
                                    juce::dontSendNotification);
        const auto placement = application_.recordingPlacementReadModel();
        const auto maximum = static_cast<double>(application_.project().sampleRate().hertz() * 2.0);
        recordingOffset_.setRange(-maximum, maximum, 1.0);
        recordingOffset_.setValue(static_cast<double>(placement.manualOffsetProjectFrames.value),
                                  juce::dontSendNotification);
        juce::String placementText{"Recording Offset: "};
        placementText << placement.manualOffsetProjectFrames.value << " frames | Effective Compensation: "
                      << placement.effectiveCompensationProjectFrames.value << " project frames";
        if (placement.reportedInputLatencyDeviceFrames) {
            placementText << " | Reported Input: "
                          << static_cast<juce::int64>(placement.reportedInputLatencyDeviceFrames->value)
                          << " device frames";
        } else {
            placementText << " | Reported Input: Unknown";
        }
        if (placement.lastCommittedUnappliedEarlyFrames != 0) {
            placementText << " | Last placement clamped at frame 0: "
                          << static_cast<juce::int64>(placement.lastCommittedUnappliedEarlyFrames)
                          << " frames unapplied";
        }
        recordingPlacementLabel_.setText(placementText, juce::dontSendNotification);
    }

    void updateLoopbackControls(transport::PlaybackState playback) {
        const auto loopback = application_.loopbackLatencyReadModel();
        const auto monitoring = application_.inputMonitoringReadModel();
        const auto recording = application_.recordingPhase();
        const auto recordingBusy = recording == audio::RecordingPhase::prepared ||
                                   recording == audio::RecordingPhase::capturing ||
                                   recording == audio::RecordingPhase::finalizing;
        const auto device = application_.deviceLatencyReadModel();
        const auto ready = !loopback.busy() && device.configurationAvailable &&
                           playback == transport::PlaybackState::stopped && !recordingBusy &&
                           !monitoring.enabled && loopbackInput_.getSelectedId() > 0 &&
                           loopbackOutput_.getSelectedId() > 0;
        loopbackInput_.setEnabled(!loopback.busy() && device.configurationAvailable);
        loopbackOutput_.setEnabled(!loopback.busy() && device.configurationAvailable);
        loopbackRun_.setEnabled(ready);
        loopbackCancel_.setEnabled(loopback.busy());
        audioBuffer_.setEnabled(audioBuffer_.isEnabled() && !loopback.busy());
        monitorButton_.setEnabled(!loopback.busy());
        monitorGain_.setEnabled(!loopback.busy());
        recordingOffset_.setEnabled(!loopback.busy());

        const auto& configuration = loopback.configuration;
        juce::String summary{"Loopback Status: "};
        summary << audio::loopbackStatusName(loopback.status)
                << " | Quality: " << static_cast<int>(loopback.validTrials) << "/"
                << static_cast<int>(audio::loopbackTrialCount) << " valid"
                << " | Buffer: " << static_cast<int>(configuration.bufferSizeFrames)
                << " | Rate: " << juce::String(configuration.sampleRateHz, 0) << " Hz";
        if (loopback.sessionId != 0)
            summary << " | Route: Out " << static_cast<int>(configuration.outputChannel + 1U)
                    << " -> In " << static_cast<int>(configuration.inputChannel + 1U);
        if (!loopback.diagnostic.empty()) summary << " | " << loopback.diagnostic;
        loopbackResultLabel_.setText(summary, juce::dontSendNotification);

        const auto as64 = [](std::optional<std::uint32_t> value)
            -> std::optional<std::uint64_t> {
            return value ? std::optional<std::uint64_t>{*value} : std::nullopt;
        };
        juce::String detail{"Reported Input Latency: "};
        detail << loopbackFramesText(as64(configuration.reportedInputFrames),
                                     configuration.sampleRateHz)
               << " | Reported Output Latency: "
               << loopbackFramesText(as64(configuration.reportedOutputFrames),
                                     configuration.sampleRateHz)
               << " | Reported RTT: "
               << loopbackFramesText(configuration.reportedRoundTripFrames,
                                     configuration.sampleRateHz)
               << " | Measured Physical RTT: "
               << loopbackFramesText(loopback.measuredRoundTripFrames,
                                     configuration.sampleRateHz)
               << " | Residual (Measured - Reported): "
               << loopbackSignedFramesText(loopback.residualFrames,
                                           configuration.sampleRateHz)
               << " | Min/Max/Jitter: "
               << loopbackFramesText(loopback.minimumFrames, configuration.sampleRateHz)
               << " / " << loopbackFramesText(loopback.maximumFrames,
                                               configuration.sampleRateHz)
               << " / " << loopbackFramesText(loopback.jitterFrames,
                                               configuration.sampleRateHz);
        loopbackDetailLabel_.setText(detail, juce::dontSendNotification);

        juce::String trials;
        for (std::size_t index = 0; index < loopback.trials.size(); ++index) {
            const auto& trial = loopback.trials[index];
            if (index != 0) trials << "\n";
            trials << "T" << static_cast<int>(index + 1U) << " "
                   << audio::loopbackQualityName(trial.quality)
                   << " | peak " << loopbackPeakText(trial.peak)
                   << " | score " << juce::String(trial.correlation, 3)
                   << " | second " << juce::String(trial.secondCorrelation, 3)
                   << " | ratio " << juce::String(trial.ambiguityRatio, 3)
                   << " | SNR " << loopbackSnrText(trial.signalToNoiseDb)
                   << " | polarity " << (trial.polarityInverted ? "inverted" : "normal")
                   << " | delay " << loopbackFramesText(trial.measuredFrames,
                                                          configuration.sampleRateHz)
                   << " | clipped " << (trial.clipped ? "yes" : "no");
        }
        loopbackTrialsLabel_.setText(trials, juce::dontSendNotification);
    }

    void chooseWav(std::optional<tracks::TrackId> track) {
        fileChooser_ = std::make_unique<juce::FileChooser>(
            "Select a WAV file", juce::File{}, "*.wav");
        constexpr auto flags = juce::FileBrowserComponent::openMode |
                               juce::FileBrowserComponent::canSelectFiles;
        juce::Component::SafePointer<AudioStatusComponent> safe{this};
        fileChooser_->launchAsync(flags, [safe, track](const juce::FileChooser& chooser) {
            if (!safe) return;
            const auto file = chooser.getResult();
            if (file == juce::File{}) {
                if (track) {
                    safe->dispatch(commands::ImportAudioToTrack{{}, *track, {0}});
                } else {
                    safe->dispatch(commands::ImportAudioFile{{}, {0}});
                }
            } else {
                const auto fullPath = file.getFullPathName();
#if JUCE_WINDOWS
                const std::filesystem::path nativePath{fullPath.toWideCharPointer()};
#else
                const std::filesystem::path nativePath{fullPath.toStdString()};
#endif
                if (track) {
                    safe->dispatch(commands::ImportAudioToTrack{nativePath, *track, {0}});
                } else {
                    safe->dispatch(commands::ImportAudioFile{nativePath, {0}});
                }
            }
            juce::MessageManager::callAsync([safe] {
                if (safe) safe->fileChooser_.reset();
            });
        });
    }

    void chooseProject(bool save) {
        fileChooser_ = std::make_unique<juce::FileChooser>(
            save ? "Save VitaDAW project" : "Load VitaDAW project",
            juce::File{}, "*.vitadaw");
        const auto flags = (save ? juce::FileBrowserComponent::saveMode |
                                      juce::FileBrowserComponent::warnAboutOverwriting
                                 : juce::FileBrowserComponent::openMode) |
                           juce::FileBrowserComponent::canSelectFiles;
        juce::Component::SafePointer<AudioStatusComponent> safe{this};
        fileChooser_->launchAsync(flags, [safe, save](const juce::FileChooser& chooser) {
            if (!safe) return;
            auto file = chooser.getResult();
            if (file != juce::File{}) {
                if (save) file = file.withFileExtension(".vitadaw");
                const auto path = std::filesystem::path{file.getFullPathName().toStdString()};
                if (save) safe->dispatch(commands::SaveProjectAs{path});
                else safe->dispatch(commands::LoadProject{path, false});
            }
            juce::MessageManager::callAsync([safe] {
                if (safe) safe->fileChooser_.reset();
            });
        });
    }

    void showResult(const commands::CommandResult& result) {
        const auto prefix = result.status == commands::CommandStatus::accepted
                                ? juce::String{"OK: "}
                                : juce::String{"Error: "};
        const auto message = result.message.empty()
            ? persistence::codeName(result.persistence.code)
            : result.message.c_str();
        resultLabel_.setText(prefix + juce::String(message), juce::dontSendNotification);
    }

    void dispatch(const commands::Command& command) {
        const auto result = dispatcher_.dispatch(command);
        if (result.persistence.code == persistence::PersistenceCode::unsavedChanges) {
            const auto path = std::get<commands::LoadProject>(command).path;
            juce::Component::SafePointer<AudioStatusComponent> safe{this};
            juce::AlertWindow::showAsync(
                juce::MessageBoxOptions{}.withTitle("Unsaved project")
                    .withMessage("Discard unsaved changes and load this project?")
                    .withButton("Discard and Load").withButton("Cancel"),
                [safe, path](int choice) {
                    if (safe && choice == 1)
                        safe->dispatch(commands::LoadProject{path, true});
                });
        }
        if (result.persistence.code == persistence::PersistenceCode::savePathRequired)
            chooseProject(true);
        if (result.status == commands::CommandStatus::accepted &&
            (std::holds_alternative<commands::LoadProject>(command) ||
             std::holds_alternative<commands::ImportAudioFile>(command))) {
            if (documentLoaded) documentLoaded();
        } else {
            timeline_.refreshModel(false);
        }
        updateHistoryControls();
        showResult(result);
    }

    commands::ICommandDispatcher& dispatcher_;
    application::DawApplication& application_;
    juce::Label statusLabel_, transportLabel_, meterLabel_, inputMeterLabel_, resultLabel_;
    juce::Label inputLatencyLabel_, outputLatencyLabel_, monitoringLatencyLabel_, recordingPlacementLabel_;
    juce::Label loopbackInstructionLabel_, loopbackResultLabel_, loopbackDetailLabel_,
        loopbackTrialsLabel_;
    TimelineComponent timeline_;
    juce::TextButton importButton_{"Import WAV..."};
    juce::TextButton addMonoTrack_{"+ Mono Track"};
    juce::TextButton addStereoTrack_{"+ Stereo Track"};
    juce::TextButton deleteTrack_{"Delete Track"};
    juce::TextButton moveTrackUp_{"Move Track Up"};
    juce::TextButton moveTrackDown_{"Move Track Down"};
    juce::TextButton playButton_{"Play"}, pauseButton_{"Pause"}, stopButton_{"Stop"};
    juce::TextButton armButton_{"Arm Track"}, recordButton_{"Record"};
    juce::TextButton undoButton_{"Undo"}, redoButton_{"Redo"};
    juce::TextButton saveButton_{"Save"}, saveAsButton_{"Save As..."};
    juce::TextButton loadProjectButton_{"Load Project..."};
    juce::TextButton tempo100_{"Initial tempo: 100 BPM"};
    juce::TextButton tempoChange_{"Add 60 BPM @ quarter 17"};
    juce::TextButton signatureChange_{"Add 7/8 @ bar 9"};
    juce::TextEditor startBar_, endBar_;
    juce::Label startBarLabel_, endBarLabel_;
    juce::TextButton applyLoop_{"Apply Loop Range"};
    juce::ToggleButton loopEnabled_{"Enable Loop"};
    juce::ToggleButton metronomeEnabled_{"Metronome"};
    juce::Slider metronomeLevel_{juce::Slider::LinearHorizontal,
                                 juce::Slider::TextBoxRight};
    juce::ToggleButton monitorButton_{"Monitor OFF"};
    juce::Slider monitorGain_{juce::Slider::LinearHorizontal,
                              juce::Slider::TextBoxRight};
    juce::Slider recordingOffset_{juce::Slider::LinearHorizontal,
                                  juce::Slider::TextBoxRight};
    juce::ComboBox audioBuffer_;
    juce::ComboBox loopbackInput_, loopbackOutput_;
    juce::TextButton loopbackRun_{"Run Test"}, loopbackCancel_{"Cancel"};
    std::vector<std::uint32_t> displayedBufferSizes_;
    std::vector<std::string> displayedInputChannels_, displayedOutputChannels_;
    float inputPeakLeft_{};
    float inputPeakRight_{};
    audio::RecordingPhase displayedRecordingPhase_{audio::RecordingPhase::idle};
    std::unique_ptr<juce::FileChooser> fileChooser_;
};

MainWindow::MainWindow(const audio::AudioDeviceState& initialAudioState,
                       commands::ICommandDispatcher& commandDispatcher,
                       application::DawApplication& application)
    : DocumentWindow("VitaDAW", juce::Colour{0xff17191d},
                     DocumentWindow::allButtons),
      dispatcher_(commandDispatcher), application_(application),
      audioState_(initialAudioState) {
    setUsingNativeTitleBar(true);
    content_ = new AudioStatusComponent(dispatcher_, application_);
    content_->documentLoaded = [this] { rebuildPending_ = true; };
    content_->setAudioDeviceState(initialAudioState);
    setContentOwned(content_, true);
    centreWithSize(1280, 820);
    setResizable(true, false);
    setVisible(true);
}

void MainWindow::closeButtonPressed() { juce::JUCEApplicationBase::quit(); }

void MainWindow::setAudioDeviceState(const audio::AudioDeviceState& state) {
    audioState_ = state;
    content_->setAudioDeviceState(state);
}

void MainWindow::setTransportState(const transport::TransportState& state,
                                   timeline::SampleRate projectSampleRate) {
    if (rebuildPending_) {
        auto next = std::make_unique<AudioStatusComponent>(dispatcher_, application_);
        next->documentLoaded = [this] { rebuildPending_ = true; };
        next->setAudioDeviceState(audioState_);
        content_ = next.get();
        setContentOwned(next.release(), false);
        rebuildPending_ = false;
    }
    content_->setTransportState(state, projectSampleRate);
}

void MainWindow::setMeterState(const mixer::MeterSnapshot& meters) {
    content_->setMeterState(meters);
}

} // namespace vitadaw::platform::juce_adapter
