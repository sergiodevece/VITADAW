#include "vitadaw/platform/juce/MainWindow.h"
#include "vitadaw/platform/juce/TimelineComponent.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <functional>
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
        for (auto* editor : {&startBar_, &endBar_}) addAndMakeVisible(*editor);
        addAndMakeVisible(applyLoop_);
        addAndMakeVisible(loopEnabled_);
        addAndMakeVisible(metronomeEnabled_);
        addAndMakeVisible(metronomeLevel_);
        addAndMakeVisible(monitorButton_);
        addAndMakeVisible(monitorGain_);
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
        const auto recordingBusy = recordingPhase == audio::RecordingPhase::capturing ||
                                   recordingPhase == audio::RecordingPhase::finalizing;
        const auto selected = timeline_.selectedTrackId();
        const auto armed = application_.armedTrack();
        armButton_.setButtonText(armed && selected == armed ? "Disarm Track" : "Arm Track");
        armButton_.setEnabled(stopped && !recordingBusy && selected.has_value());
        recordButton_.setEnabled(stopped && !recordingBusy && armed.has_value());
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
