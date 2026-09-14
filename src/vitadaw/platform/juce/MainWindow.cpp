#include "vitadaw/platform/juce/MainWindow.h"

#include <algorithm>

namespace vitadaw::platform::juce_adapter {

class AudioStatusComponent final : public juce::Component {
public:
    AudioStatusComponent(commands::ICommandDispatcher& commandDispatcher,
                         const project::ProjectState& project)
        : commandDispatcher_(commandDispatcher) {
        statusLabel_.setColour(juce::Label::textColourId, juce::Colours::white);
        statusLabel_.setJustificationType(juce::Justification::centredLeft);
        addAndMakeVisible(statusLabel_);

        resultLabel_.setColour(juce::Label::textColourId, juce::Colours::white);
        addAndMakeVisible(resultLabel_);

        transportLabel_.setColour(juce::Label::textColourId, juce::Colours::white);
        addAndMakeVisible(transportLabel_);

        for (std::size_t index = 0; index < project.tracks().size(); ++index) {
            auto controls = std::make_unique<TrackControls>();
            controls->track = project.tracks()[index].id;
            controls->name.setText(
                juce::String(project.tracks()[index].name),
                juce::NotificationType::dontSendNotification);
            controls->name.setColour(juce::Label::textColourId,
                                     juce::Colours::white);
            controls->meter.setColour(juce::Label::textColourId,
                                      juce::Colours::lightgreen);
            controls->meter.setText("L 0.000  R 0.000",
                                    juce::NotificationType::dontSendNotification);
            controls->gain.setRange(mixer::GainDb::silence,
                                    mixer::GainDb::maximum, 0.1);
            controls->gain.setValue(0.0,
                                    juce::NotificationType::dontSendNotification);
            controls->gain.setTextValueSuffix(" dB");
            controls->pan.setRange(mixer::Pan::left, mixer::Pan::right, 0.01);
            controls->pan.setValue(0.0,
                                   juce::NotificationType::dontSendNotification);
            controls->output.addItem("Master", 1);
            for (std::size_t busIndex = 0;
                 busIndex < project.routing().buses().size(); ++busIndex) {
                controls->output.addItem(
                    juce::String(project.routing().buses()[busIndex].name),
                    static_cast<int>(busIndex + 2));
            }
            const auto* route = project.routing().findTrackRoute(controls->track);
            auto selected = 1;
            if (route != nullptr &&
                route->destination.kind == routing::DestinationKind::bus) {
                const auto found = std::find_if(
                    project.routing().buses().begin(),
                    project.routing().buses().end(),
                    [id = route->destination.bus](const auto& bus) {
                        return bus.id == id;
                    });
                if (found != project.routing().buses().end()) {
                    selected = static_cast<int>(
                        found - project.routing().buses().begin() + 2);
                }
            }
            controls->output.setSelectedId(
                selected, juce::NotificationType::dontSendNotification);
            controls->gain.onValueChange = [this, raw = controls.get()] {
                dispatch(commands::SetTrackGain{
                    raw->track,
                    mixer::GainDb{static_cast<float>(raw->gain.getValue())}});
            };
            controls->pan.onValueChange = [this, raw = controls.get()] {
                dispatch(commands::SetTrackPan{
                    raw->track,
                    mixer::Pan{static_cast<float>(raw->pan.getValue())}});
            };
            controls->mute.onClick = [this, raw = controls.get()] {
                dispatch(commands::SetTrackMute{raw->track,
                                                raw->mute.getToggleState()});
            };
            controls->solo.onClick = [this, raw = controls.get()] {
                dispatch(commands::SetTrackSolo{raw->track,
                                                raw->solo.getToggleState()});
            };
            controls->output.onChange =
                [this, raw = controls.get(),
                 buses = project.routing().buses()] {
                    const auto selectedId = raw->output.getSelectedId();
                    const auto destination = selectedId <= 1
                        ? routing::TrackOutputDestination::master()
                        : routing::TrackOutputDestination::toBus(
                              buses[static_cast<std::size_t>(selectedId - 2)].id);
                    dispatch(commands::SetTrackOutputDestination{
                        raw->track, destination});
                };
            auto button = std::make_unique<juce::TextButton>(
                "Load Audio " + juce::String(static_cast<int>(index + 1)));
            const auto track = controls->track;
            button->onClick = [this, track] { chooseWav(track); };
            addAndMakeVisible(controls->name);
            addAndMakeVisible(controls->gain);
            addAndMakeVisible(controls->pan);
            addAndMakeVisible(controls->mute);
            addAndMakeVisible(controls->solo);
            addAndMakeVisible(controls->output);
            addAndMakeVisible(controls->meter);
            addAndMakeVisible(*button);
            trackControls_.push_back(std::move(controls));
            loadButtons_.push_back(std::move(button));
        }
        for (const auto& bus : project.routing().buses()) {
            auto controls = std::make_unique<BusControls>();
            controls->bus = bus.id;
            controls->name.setText(juce::String(bus.name),
                                   juce::NotificationType::dontSendNotification);
            controls->name.setColour(juce::Label::textColourId,
                                     juce::Colours::white);
            controls->meter.setText("L 0.000  R 0.000",
                                    juce::NotificationType::dontSendNotification);
            controls->meter.setColour(juce::Label::textColourId,
                                      juce::Colours::lightgreen);
            controls->gain.setRange(mixer::GainDb::silence,
                                    mixer::GainDb::maximum, 0.1);
            controls->gain.setValue(bus.mix.gain.value,
                                    juce::NotificationType::dontSendNotification);
            controls->gain.setTextValueSuffix(" dB");
            controls->balance.setRange(mixer::Pan::left, mixer::Pan::right,
                                       0.01);
            controls->balance.setValue(
                bus.mix.balance.value,
                juce::NotificationType::dontSendNotification);
            controls->output.addItem("Master", 1);
            for (const auto& destinationBus : project.routing().buses()) {
                if (destinationBus.id != bus.id) {
                    controls->destinationBuses.push_back(destinationBus.id);
                    controls->output.addItem(
                        juce::String(destinationBus.name),
                        static_cast<int>(controls->destinationBuses.size() + 1));
                }
            }
            auto selectedOutput = 1;
            if (bus.outputDestination.kind == routing::DestinationKind::bus) {
                const auto found = std::find(
                    controls->destinationBuses.begin(),
                    controls->destinationBuses.end(),
                    bus.outputDestination.bus);
                if (found != controls->destinationBuses.end()) {
                    selectedOutput = static_cast<int>(
                        found - controls->destinationBuses.begin() + 2);
                }
            }
            controls->output.setSelectedId(
                selectedOutput, juce::NotificationType::dontSendNotification);
            controls->gain.onValueChange = [this, raw = controls.get()] {
                dispatch(commands::SetBusGain{
                    raw->bus,
                    mixer::GainDb{static_cast<float>(raw->gain.getValue())}});
            };
            controls->balance.onValueChange = [this, raw = controls.get()] {
                dispatch(commands::SetBusPan{
                    raw->bus,
                    mixer::Pan{static_cast<float>(raw->balance.getValue())}});
            };
            controls->mute.onClick = [this, raw = controls.get()] {
                dispatch(commands::SetBusMute{raw->bus,
                                              raw->mute.getToggleState()});
            };
            controls->solo.onClick = [this, raw = controls.get()] {
                dispatch(commands::SetBusSolo{raw->bus,
                                              raw->solo.getToggleState()});
            };
            controls->output.onChange = [this, raw = controls.get()] {
                const auto selectedId = raw->output.getSelectedId();
                const auto destination = selectedId <= 1
                    ? routing::OutputDestination::master()
                    : routing::OutputDestination::toBus(
                          raw->destinationBuses[
                              static_cast<std::size_t>(selectedId - 2)]);
                dispatch(commands::SetBusOutputDestination{raw->bus,
                                                           destination});
            };
            addAndMakeVisible(controls->name);
            addAndMakeVisible(controls->gain);
            addAndMakeVisible(controls->balance);
            addAndMakeVisible(controls->mute);
            addAndMakeVisible(controls->solo);
            addAndMakeVisible(controls->output);
            addAndMakeVisible(controls->meter);
            busControls_.push_back(std::move(controls));
        }
        for (const auto& send : project.routing().sends()) {
            auto controls = std::make_unique<SendControls>();
            controls->send = send.id;
            const auto* destination = project.findBus(send.destination);
            juce::String source;
            if (const auto* track =
                    std::get_if<tracks::TrackId>(&send.source)) {
                const auto* sourceTrack = project.findTrack(*track);
                source = sourceTrack != nullptr
                             ? juce::String{sourceTrack->name}
                             : "T" + juce::String(
                                         static_cast<int>(track->value));
            } else {
                const auto busId = std::get<routing::BusId>(send.source);
                const auto* sourceBus = project.findBus(busId);
                source = sourceBus != nullptr
                             ? juce::String{sourceBus->name}
                             : "B" + juce::String(
                                         static_cast<int>(busId.value));
            }
            controls->name.setText(
                "Send " + juce::String(static_cast<int>(send.id.value)) +
                    " " + source + " -> " +
                    (destination != nullptr ? juce::String(destination->name)
                                            : juce::String{"?"}) +
                    (send.tapPoint == routing::SendTapPoint::preFaderPrePan
                         ? " [Pre]"
                         : " [Post]"),
                juce::NotificationType::dontSendNotification);
            controls->name.setColour(juce::Label::textColourId,
                                     juce::Colours::white);
            controls->level.setRange(mixer::GainDb::silence,
                                     mixer::GainDb::maximum, 0.1);
            controls->level.setValue(
                send.mix.level.value,
                juce::NotificationType::dontSendNotification);
            controls->level.setTextValueSuffix(" dB send");
            controls->muted.setToggleState(
                send.mix.muted,
                juce::NotificationType::dontSendNotification);
            controls->level.onValueChange = [this, raw = controls.get()] {
                dispatch(commands::SetSendLevel{
                    raw->send,
                    mixer::GainDb{static_cast<float>(raw->level.getValue())}});
            };
            controls->muted.onClick = [this, raw = controls.get()] {
                dispatch(commands::SetSendMute{
                    raw->send, raw->muted.getToggleState()});
            };
            addAndMakeVisible(controls->name);
            addAndMakeVisible(controls->level);
            addAndMakeVisible(controls->muted);
            sendControls_.push_back(std::move(controls));
        }
        masterGain_.setRange(mixer::GainDb::silence,
                             mixer::GainDb::maximum, 0.1);
        masterGain_.setValue(0.0, juce::NotificationType::dontSendNotification);
        masterGain_.setTextValueSuffix(" dB master");
        masterGain_.onValueChange = [this] {
            dispatch(commands::SetMasterGain{
                mixer::GainDb{static_cast<float>(masterGain_.getValue())}});
        };
        addAndMakeVisible(masterGain_);
        masterMeter_.setColour(juce::Label::textColourId,
                               juce::Colours::lightgreen);
        masterMeter_.setText("Master L 0.000  R 0.000",
                             juce::NotificationType::dontSendNotification);
        addAndMakeVisible(masterMeter_);
        playButton_.onClick = [this] { dispatch(commands::Play{}); };
        stopButton_.onClick = [this] { dispatch(commands::Stop{}); };
        addAndMakeVisible(playButton_);
        addAndMakeVisible(stopButton_);
    }

    void setTransportState(const transport::TransportState& state,
                           timeline::SampleRate projectSampleRate) {
        const auto position = timeline::projectPositionToSeconds(
            state.position, projectSampleRate);
        const auto duration = timeline::projectFramesToSeconds(
            state.duration, projectSampleRate);
        juce::String text;
        text << "Transport: "
             << (state.playback == transport::PlaybackState::playing ? "Playing" : "Stopped")
             << " | " << juce::String(position.value, 3) << " / "
             << juce::String(duration.value, 3) << " s"
             << " | Project: " << juce::String(projectSampleRate.hertz(), 0) << " Hz";
        transportLabel_.setText(text, juce::NotificationType::dontSendNotification);
    }

    void setAudioDeviceState(const audio::AudioDeviceState& state) {
        juce::String text;

        if (state.status == audio::AudioDeviceStatus::active) {
            text << "Audio device: active\n"
                 << "Output: " << state.info.outputDeviceName << "\n"
                 << "Sample rate: " << juce::String(state.info.sampleRate, 0) << " Hz\n"
                 << "Buffer size: " << static_cast<int>(state.info.bufferSizeFrames)
                 << " frames\n"
                 << "Available input channels: "
                 << static_cast<int>(state.info.availableInputChannels) << "\n"
                 << "Available output channels: "
                 << static_cast<int>(state.info.availableOutputChannels);
        } else if (state.status == audio::AudioDeviceStatus::error) {
            text << "Audio device: error\n" << state.errorMessage;
        } else {
            text << "Audio device: closed";
        }

        statusLabel_.setText(text, juce::NotificationType::dontSendNotification);
    }

    void setMeterState(const mixer::MeterSnapshot& meters) {
        for (auto& controls : trackControls_) {
            mixer::StereoPeak peak;
            for (std::size_t index = 0; index < meters.trackCount; ++index) {
                if (meters.tracks[index].track == controls->track) {
                    peak = meters.tracks[index].peak;
                    break;
                }
            }
            controls->meter.setText(
                "L " + juce::String(peak.left, 3) +
                    "  R " + juce::String(peak.right, 3),
                juce::NotificationType::dontSendNotification);
        }
        for (auto& controls : busControls_) {
            mixer::StereoPeak peak;
            for (std::size_t index = 0; index < meters.busCount; ++index) {
                if (meters.buses[index].bus == controls->bus) {
                    peak = meters.buses[index].peak;
                    break;
                }
            }
            controls->meter.setText(
                "L " + juce::String(peak.left, 3) +
                    "  R " + juce::String(peak.right, 3),
                juce::NotificationType::dontSendNotification);
        }
        masterMeter_.setText(
            "Master L " + juce::String(meters.master.left, 3) +
                "  R " + juce::String(meters.master.right, 3),
            juce::NotificationType::dontSendNotification);
    }

    void paint(juce::Graphics& graphics) override {
        graphics.fillAll(juce::Colours::darkgrey);
    }

    void resized() override {
        auto bounds = getLocalBounds().reduced(24);
        statusLabel_.setBounds(bounds.removeFromTop(150));
        bounds.removeFromTop(12);
        transportLabel_.setBounds(bounds.removeFromTop(32));
        bounds.removeFromTop(12);
        for (std::size_t index = 0; index < loadButtons_.size(); ++index) {
            auto row = bounds.removeFromTop(42);
            auto& controls = *trackControls_[index];
            controls.name.setBounds(row.removeFromLeft(64));
            loadButtons_[index]->setBounds(row.removeFromLeft(112));
            row.removeFromLeft(8);
            controls.gain.setBounds(row.removeFromLeft(180));
            controls.pan.setBounds(row.removeFromLeft(150));
            controls.output.setBounds(row.removeFromLeft(110));
            controls.mute.setBounds(row.removeFromLeft(64));
            controls.solo.setBounds(row.removeFromLeft(64));
            controls.meter.setBounds(row);
        }
        for (auto& controls : busControls_) {
            auto row = bounds.removeFromTop(42);
            controls->name.setBounds(row.removeFromLeft(80));
            controls->gain.setBounds(row.removeFromLeft(180));
            controls->balance.setBounds(row.removeFromLeft(150));
            controls->output.setBounds(row.removeFromLeft(110));
            controls->mute.setBounds(row.removeFromLeft(64));
            controls->solo.setBounds(row.removeFromLeft(64));
            controls->meter.setBounds(row);
        }
        for (auto& controls : sendControls_) {
            auto row = bounds.removeFromTop(38);
            controls->name.setBounds(row.removeFromLeft(220));
            controls->level.setBounds(row.removeFromLeft(220));
            controls->muted.setBounds(row.removeFromLeft(80));
        }
        bounds.removeFromTop(8);
        auto masterRow = bounds.removeFromTop(36);
        masterGain_.setBounds(masterRow.removeFromLeft(240));
        masterMeter_.setBounds(masterRow.removeFromLeft(260));
        bounds.removeFromTop(8);
        auto transportButtons = bounds.removeFromTop(32);
        playButton_.setBounds(transportButtons.removeFromLeft(80));
        transportButtons.removeFromLeft(8);
        stopButton_.setBounds(transportButtons.removeFromLeft(80));
        bounds.removeFromTop(12);
        resultLabel_.setBounds(bounds.removeFromTop(32));
    }

private:
    struct TrackControls {
        tracks::TrackId track;
        juce::Label name;
        juce::Slider gain{juce::Slider::LinearHorizontal,
                          juce::Slider::TextBoxRight};
        juce::Slider pan{juce::Slider::LinearHorizontal,
                         juce::Slider::TextBoxRight};
        juce::ToggleButton mute{"Mute"};
        juce::ToggleButton solo{"Solo"};
        juce::ComboBox output;
        juce::Label meter;
    };

    struct BusControls {
        routing::BusId bus;
        juce::Label name;
        juce::Slider gain{juce::Slider::LinearHorizontal,
                          juce::Slider::TextBoxRight};
        juce::Slider balance{juce::Slider::LinearHorizontal,
                             juce::Slider::TextBoxRight};
        juce::ToggleButton mute{"Mute"};
        juce::ToggleButton solo{"Solo"};
        juce::ComboBox output;
        std::vector<routing::BusId> destinationBuses;
        juce::Label meter;
    };

    struct SendControls {
        routing::SendId send;
        juce::Label name;
        juce::Slider level{juce::Slider::LinearHorizontal,
                           juce::Slider::TextBoxRight};
        juce::ToggleButton muted{"Send Mute"};
    };

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
                dispatch(commands::LoadAudioFile{nativePath, track});
            }
            fileChooser_.reset();
        });
    }

    void dispatch(const commands::Command& command) {
        const auto result = commandDispatcher_.dispatch(command);
        const auto prefix = result.status == commands::CommandStatus::accepted
                                ? juce::String{"OK: "}
                                : juce::String{"Error: "};
        resultLabel_.setText(prefix + juce::String(result.message),
                             juce::NotificationType::dontSendNotification);
    }

    commands::ICommandDispatcher& commandDispatcher_;
    juce::Label statusLabel_;
    juce::Label transportLabel_;
    juce::Label resultLabel_;
    std::vector<std::unique_ptr<juce::TextButton>> loadButtons_;
    std::vector<std::unique_ptr<TrackControls>> trackControls_;
    std::vector<std::unique_ptr<BusControls>> busControls_;
    std::vector<std::unique_ptr<SendControls>> sendControls_;
    juce::Slider masterGain_{juce::Slider::LinearHorizontal,
                             juce::Slider::TextBoxRight};
    juce::Label masterMeter_;
    juce::TextButton playButton_{"Play"};
    juce::TextButton stopButton_{"Stop"};
    std::unique_ptr<juce::FileChooser> fileChooser_;
};

MainWindow::MainWindow(const audio::AudioDeviceState& initialAudioState,
                       commands::ICommandDispatcher& commandDispatcher,
                       const project::ProjectState& project)
    : DocumentWindow("VitaDAW",
                     juce::Colours::darkgrey,
                     DocumentWindow::allButtons) {
    setUsingNativeTitleBar(true);
    content_ = new AudioStatusComponent(commandDispatcher, project);
    content_->setAudioDeviceState(initialAudioState);
    setContentOwned(content_, true);
    centreWithSize(1040, 980);
    setResizable(true, false);
    setVisible(true);
}

void MainWindow::closeButtonPressed() {
    juce::JUCEApplicationBase::quit();
}

void MainWindow::setAudioDeviceState(const audio::AudioDeviceState& state) {
    content_->setAudioDeviceState(state);
}

void MainWindow::setTransportState(const transport::TransportState& state,
                                   timeline::SampleRate projectSampleRate) {
    content_->setTransportState(state, projectSampleRate);
}

void MainWindow::setMeterState(const mixer::MeterSnapshot& meters) {
    content_->setMeterState(meters);
}

} // namespace vitadaw::platform::juce_adapter
