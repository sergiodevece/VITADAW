#include "vitadaw/platform/juce/MainWindow.h"
#include "vitadaw/platform/juce/TimelineComponent.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
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
        for (auto* label : {&statusLabel_, &transportLabel_, &meterLabel_, &resultLabel_}) {
            label->setColour(juce::Label::textColourId, juce::Colours::white);
            label->setJustificationType(juce::Justification::centredLeft);
            addAndMakeVisible(*label);
        }

        timeline_.commandCompleted = [this](const commands::CommandResult& result) {
            showResult(result);
            updateHistoryControls();
        };
        addAndMakeVisible(timeline_);

        for (const auto& track : application.project().tracks()) {
            auto button = std::make_unique<juce::TextButton>(
                "Import to " + juce::String(track.name));
            const auto id = track.id;
            button->onClick = [this, id] { chooseWav(id); };
            addAndMakeVisible(*button);
            loadButtons_.push_back(std::move(button));
        }

        playButton_.onClick = [this] { dispatch(commands::Play{}); };
        pauseButton_.onClick = [this] { dispatch(commands::Pause{}); };
        stopButton_.onClick = [this] { dispatch(commands::Stop{}); };
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
        for (auto* button : {&tempo100_, &tempoChange_, &signatureChange_}) addAndMakeVisible(*button);
        for (auto* button : {&playButton_, &pauseButton_, &stopButton_, &undoButton_, &redoButton_,
                             &saveButton_, &saveAsButton_, &loadProjectButton_})
            addAndMakeVisible(*button);
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
        auto musicalRow = bounds.removeFromTop(28);
        tempo100_.setBounds(musicalRow.removeFromLeft(160));
        tempoChange_.setBounds(musicalRow.removeFromLeft(240));
        signatureChange_.setBounds(musicalRow.removeFromLeft(240));

        auto commandRow = bounds.removeFromTop(32);
        for (auto* button : {&playButton_, &pauseButton_, &stopButton_, &undoButton_, &redoButton_,
                             &saveButton_, &saveAsButton_, &loadProjectButton_}) {
            const auto width = button == &loadProjectButton_ ? 126 :
                               button == &saveAsButton_ ? 88 : 76;
            button->setBounds(commandRow.removeFromLeft(width));
            commandRow.removeFromLeft(5);
        }
        bounds.removeFromTop(5);
        auto importRow = bounds.removeFromTop(32);
        const auto count = static_cast<int>(loadButtons_.size());
        const auto width = count == 0 ? 0 :
            std::max(90, (importRow.getWidth() - 5 * (count - 1)) / count);
        for (auto& button : loadButtons_) {
            button->setBounds(importRow.removeFromLeft(width));
            importRow.removeFromLeft(5);
        }
        bounds.removeFromTop(8);
        resultLabel_.setBounds(bounds.removeFromBottom(26));
        bounds.removeFromBottom(6);
        timeline_.setBounds(bounds);
    }

private:
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

    void chooseWav(tracks::TrackId track) {
        fileChooser_ = std::make_unique<juce::FileChooser>(
            "Select a WAV file", juce::File{}, "*.wav");
        constexpr auto flags = juce::FileBrowserComponent::openMode |
                               juce::FileBrowserComponent::canSelectFiles;
        fileChooser_->launchAsync(flags, [this, track](const juce::FileChooser& chooser) {
            const auto file = chooser.getResult();
            if (file.existsAsFile()) {
                const auto fullPath = file.getFullPathName();
#if JUCE_WINDOWS
                const std::filesystem::path nativePath{fullPath.toWideCharPointer()};
#else
                const std::filesystem::path nativePath{fullPath.toStdString()};
#endif
                dispatch(commands::ImportAudioToTrack{nativePath, track, {0}});
            }
            fileChooser_.reset();
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
        fileChooser_->launchAsync(flags, [this, save](const juce::FileChooser& chooser) {
            auto file = chooser.getResult();
            if (file == juce::File{}) return;
            if (save) file = file.withFileExtension(".vitadaw");
            const auto path = std::filesystem::path{file.getFullPathName().toStdString()};
            if (save) dispatch(commands::SaveProjectAs{path});
            else dispatch(commands::LoadProject{path, false});
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
            std::holds_alternative<commands::LoadProject>(command)) {
            if (documentLoaded) documentLoaded();
        } else {
            timeline_.refreshModel(false);
        }
        updateHistoryControls();
        showResult(result);
    }

    commands::ICommandDispatcher& dispatcher_;
    application::DawApplication& application_;
    juce::Label statusLabel_, transportLabel_, meterLabel_, resultLabel_;
    TimelineComponent timeline_;
    std::vector<std::unique_ptr<juce::TextButton>> loadButtons_;
    juce::TextButton playButton_{"Play"}, pauseButton_{"Pause"}, stopButton_{"Stop"};
    juce::TextButton undoButton_{"Undo"}, redoButton_{"Redo"};
    juce::TextButton saveButton_{"Save"}, saveAsButton_{"Save As..."};
    juce::TextButton loadProjectButton_{"Load Project..."};
    juce::TextButton tempo100_{"Initial tempo: 100 BPM"};
    juce::TextButton tempoChange_{"Add 60 BPM @ quarter 17"};
    juce::TextButton signatureChange_{"Add 7/8 @ bar 9"};
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
