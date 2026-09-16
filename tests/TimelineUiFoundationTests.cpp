#include "vitadaw/persistence/ProjectPersistence.h"
#include "vitadaw/history/UndoManager.h"
#include "vitadaw/ui/timeline/TimelineModel.h"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <string>

namespace {
using namespace vitadaw;
using namespace vitadaw::ui::timeline;

void check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::exit(1);
    }
}

const ClipSnapshot& onlyClip(const TimelineSnapshot& snapshot) {
    check(snapshot.tracks.size() == 1 && snapshot.tracks.front().clips.size() == 1,
          "snapshot must contain one clip");
    return snapshot.tracks.front().clips.front();
}

project::ProjectState oneClipProject() {
    project::ProjectState project{timeline::SampleRate{48000.0}, "Timeline"};
    const auto track = project.addAudioTrack("Voice");
    static_cast<void>(project.importAudioToTrack(
        track, media::MediaReference{"/media/voice.wav", {},
            media::MediaFingerprint{std::string(64, 'a'), 768000}}, {192000},
        timeline::SampleRate{48000.0}, media::AudioChannelLayout::mono,
        {48000}));
    return project;
}

void coordinateTests() {
    CoordinateTransform view{timeline::SampleRate{48000.0}, 100.0, 1.0};
    check(std::abs(view.projectFrameToX({48000}) - 0.0) < 1.0e-12 &&
              view.xToProjectFrame(250.0).value == 168000,
          "frame/pixel transform must include logical sample rate and viewport start");
    view.zoomAround(200.0, 250.0);
    check(std::abs(view.projectFrameToX({168000}) - 250.0) < 1.0e-9,
          "zoom must preserve the project time under its anchor");
    view.setVisibleStartSeconds(-100.0);
    view.setPixelsPerSecond(1.0e9);
    check(view.visibleStartSeconds() == 0.0 &&
              view.pixelsPerSecond() == CoordinateTransform::maximumPixelsPerSecond,
          "scroll and zoom limits must be bounded");
}

void snapshotAndSelectionTests() {
    auto project = oneClipProject();
    transport::TransportState transport;
    transport.synchronise(true, {72000}, {240000});
    const auto snapshot = makeTimelineSnapshot(project, 17, transport);
    const auto& clip = onlyClip(snapshot);
    check(snapshot.projectSampleRate == timeline::SampleRate{48000.0} &&
              snapshot.contentDuration.value == 240000 &&
              snapshot.transportPosition.value == 72000 &&
              snapshot.playback == transport::PlaybackState::playing &&
              snapshot.revision == 17 && clip.projectStart.value == 48000 &&
              clip.duration.value == 192000.0 && clip.sourceOffset.value == 0.0 &&
              clip.sourceFramesPerProjectFrame == 1.0 &&
              clip.label == "voice.wav",
          "portable snapshot must contain paint data, transport and no PCM");

    TimelineInteraction interaction;
    interaction.select(clip.id);
    check(interaction.selection() == clip.id, "selection must use ClipId");
    check(bool(project.deleteClip(clip.id)), "test clip deletion");
    interaction.reconcile(makeTimelineSnapshot(project, 18));
    check(!interaction.selection(), "selection must clear when its ClipId disappears");
}

commands::Command gesture(double pixelsPerSecond, double pixelDelta,
                          GestureKind kind) {
    const auto snapshot = makeTimelineSnapshot(oneClipProject(), 1);
    TimelineInteraction interaction;
    const auto& clip = onlyClip(snapshot);
    CoordinateTransform transform{snapshot.projectSampleRate, pixelsPerSecond, 0.0};
    check(interaction.beginGesture(kind, clip, 100.0), "gesture begins");
    interaction.updateGesture(100.0 + pixelDelta, transform);
    check(onlyClip(snapshot).projectStart.value == 48000,
          "preview must not mutate its ProjectState-derived snapshot");
    const auto command = interaction.endGesture();
    check(command.has_value() && !interaction.preview(),
          "mouse-up must produce one command and discard preview");
    return *command;
}

void gestureTests() {
    const auto moved100 = std::get<commands::MoveClip>(
        gesture(100.0, 200.0, GestureKind::move));
    const auto moved200 = std::get<commands::MoveClip>(
        gesture(200.0, 400.0, GestureKind::move));
    check(moved100.projectStart.value == 144000 &&
              moved200.projectStart == moved100.projectStart,
          "a musical two-second move must be zoom-independent");

    const auto clamped = std::get<commands::MoveClip>(
        gesture(100.0, -500.0, GestureKind::move));
    check(clamped.projectStart.value == 0, "move preview must clamp at project zero");

    const auto left100 = std::get<commands::TrimClipLeft>(
        gesture(100.0, 50.0, GestureKind::trimLeft));
    const auto left200 = std::get<commands::TrimClipLeft>(
        gesture(200.0, 100.0, GestureKind::trimLeft));
    check(left100.projectStart.value == 72000 && left200.projectStart == left100.projectStart,
          "left trim conversion must be zoom-independent");
    const auto right = std::get<commands::TrimClipRight>(
        gesture(100.0, -100.0, GestureKind::trimRight));
    check(right.projectEnd.value == 192000,
          "right trim must emit an exclusive project end");

    TimelineInteraction sourcePreview;
    ClipSnapshot resampled{{77}, {1}, {0}, {48000}, {100}, 44100.0 / 48000.0,
                           "resampled.wav"};
    check(sourcePreview.beginGesture(GestureKind::trimLeft, resampled, 0.0),
          "resampled trim preview begins");
    sourcePreview.updateGesture(10.0,
        CoordinateTransform{timeline::SampleRate{48000}, 100.0, 0.0});
    check(std::abs(sourcePreview.preview()->sourceOffset.value - 4510.0) < 1.0e-9,
          "left-trim preview maps project delta to source frames");

    TimelineInteraction boundary;
    const auto maximum = std::numeric_limits<std::int64_t>::max();
    ClipSnapshot extreme{{99}, {1}, {maximum - 100}, {50.0}, {0}, 1.0, "edge"};
    CoordinateTransform view{timeline::SampleRate{48000.0}, 100.0, 0.0};
    check(boundary.beginGesture(GestureKind::move, extreme, 0.0),
          "boundary gesture begins");
    boundary.updateGesture(1.0e300, view);
    const auto saturated = boundary.endGesture();
    check(saturated && std::get<commands::MoveClip>(*saturated).projectStart.value == maximum,
          "preview arithmetic must saturate instead of overflowing frame positions");
}

void crossTrackInteractionTests() {
    auto project = oneClipProject();
    const auto monoTarget = project.addAudioTrack(
        "Mono Target", media::AudioChannelLayout::mono);
    const auto stereoTarget = project.addAudioTrack(
        "Stereo Target", media::AudioChannelLayout::stereo);
    const auto snapshot = makeTimelineSnapshot(project, 20);
    const auto& sourceTrack = snapshot.tracks[0];
    const auto& clip = sourceTrack.clips[0];
    check(TimelineInteraction::trackAtVerticalPosition(
              snapshot, 62.0, 62.0, 58.0, 0.0) == sourceTrack.id &&
          TimelineInteraction::trackAtVerticalPosition(
              snapshot, 62.0, 62.0, 58.0, 58.0) == monoTarget &&
          !TimelineInteraction::trackAtVerticalPosition(
              snapshot, -1.0, 62.0, 58.0, 0.0),
          "portable lane geometry resolves TrackId with vertical scroll");

    TimelineInteraction interaction;
    interaction.selectTrack(monoTarget);
    check(interaction.selectedTrack() == monoTarget && !interaction.selection(),
          "track selection is independent from clip selection");
    interaction.selectClip(sourceTrack.id, clip.id);
    check(interaction.selectedTrack() == sourceTrack.id &&
              interaction.selection() == clip.id,
          "clip selection coordinates its owning track");
    CoordinateTransform transform{snapshot.projectSampleRate, 100.0, 0.0};
    check(interaction.beginGesture(GestureKind::move, sourceTrack, clip, 100.0),
          "cross-track gesture begins without editing the model");
    interaction.updateGesture(300.0, transform, snapshot, monoTarget);
    check(interaction.preview() && interaction.preview()->validTarget &&
              interaction.preview()->track == monoTarget &&
              project.trackContainingClip(clip.id) == sourceTrack.id,
          "compatible vertical preview changes only ephemeral TrackId");
    const auto command = interaction.endGesture();
    const auto& move = std::get<commands::MoveClip>(*command);
    check(move.targetTrack == monoTarget && move.projectStart.value == 144000,
          "mouse-up emits one atomic horizontal and vertical MoveClip");

    check(interaction.beginGesture(GestureKind::move, sourceTrack, clip, 100.0),
          "incompatible gesture begins");
    interaction.updateGesture(100.0, transform, snapshot, stereoTarget);
    check(interaction.preview() && !interaction.preview()->validTarget &&
              !interaction.endGesture(),
          "incompatible lane preview cannot emit a model command");

    interaction.selectTrack(monoTarget);
    check(project.removeAudioTrack(monoTarget).has_value(),
          "selected track deletion fixture succeeds");
    interaction.reconcile(makeTimelineSnapshot(project, 21));
    check(!interaction.selectedTrack(),
          "selection is invalidated when its TrackId disappears");
}

void actionsAndRefreshTests() {
    auto project = oneClipProject();
    auto snapshot = makeTimelineSnapshot(project, 1);
    const auto originalModelClip = *project.findClip(onlyClip(snapshot).id);
    TimelineInteraction interaction;
    interaction.select(onlyClip(snapshot).id);
    const auto duplicate = interaction.duplicateCommand(snapshot);
    check(duplicate &&
              std::get<commands::DuplicateClips>(*duplicate).clips ==
                  std::vector<clips::ClipId>{onlyClip(snapshot).id} &&
              std::get<commands::DuplicateClips>(*duplicate).deltaFrames == 192000,
          "duplicate must be contiguous to the selected clip");
    check(!interaction.splitCommand(snapshot, {48000}) &&
              !interaction.splitCommand(snapshot, {240000}) &&
              interaction.splitCommand(snapshot, {144000}),
          "split must require the playhead strictly inside the selected clip");
    check(interaction.deleteCommand().has_value() &&
              std::get<commands::DeleteClips>(*interaction.deleteCommand()).clips ==
                  std::vector<clips::ClipId>{onlyClip(snapshot).id},
          "delete action must target the complete selection");

    check(bool(project.splitClip(onlyClip(snapshot).id, {144000})), "split model commit");
    auto splitSnapshot = makeTimelineSnapshot(project, 2);
    check(splitSnapshot.tracks[0].clips.size() == 2 &&
              splitSnapshot.tracks[0].clips[0].duration.value == 96000.0 &&
              splitSnapshot.tracks[0].clips[1].projectStart.value == 144000,
          "next snapshot must reflect a split commit");
    const auto leftModelClip = *project.findClip(splitSnapshot.tracks[0].clips[0].id);
    const auto rightModelClip = *project.findClip(splitSnapshot.tracks[0].clips[1].id);
    history::UndoableOperation splitHistory{history::SplitClip{
        project.tracks()[0].id, originalModelClip, leftModelClip, rightModelClip}};
    check(splitHistory.apply(project, false),
          "restore single clip shape for an Undo-equivalent snapshot");
    const auto restored = makeTimelineSnapshot(project, 3);
    check(onlyClip(restored).projectStart.value == 48000 &&
              onlyClip(restored).duration.value == 192000.0,
          "fresh snapshot must reflect restored history geometry");

    const auto encoded = persistence::serializeProject(project, "/tmp/timeline-ui.vitadaw");
    check(encoded.result.success(), "edited timeline saves");
    const auto loaded = persistence::deserializeProject(encoded.bytes);
    check(loaded.result.success(), "edited timeline loads");
    const auto loadedSnapshot = makeTimelineSnapshot(*loaded.project, 4);
    check(loadedSnapshot.tracks == restored.tracks &&
              loadedSnapshot.contentDuration == restored.contentDuration,
          "Load must reconstruct exact timeline positions and durations");
}

void multipleSelectionTests() {
    auto project = oneClipProject();
    const auto source = project.sources().front().id;
    const auto secondTrack = project.addAudioTrack("Second");
    const auto second = project.addClip(secondTrack, source, {288000}, {48000}, {0});
    auto snapshot = makeTimelineSnapshot(project, 1);
    const auto first = snapshot.tracks[0].clips[0].id;
    TimelineInteraction interaction;
    interaction.selectClip(snapshot.tracks[0].id, first);
    interaction.toggleClip(secondTrack, second);
    check(interaction.selections().size() == 2 &&
              interaction.selections()[0] == first &&
              interaction.selections()[1] == second &&
              !interaction.splitCommand(snapshot, {100000}),
          "Cmd/Ctrl-style toggle forms a canonical multi-selection and disables Split");
    interaction.selectClip(snapshot.tracks[0].id, first);
    check(interaction.selections().size() == 2 &&
              interaction.selectedTrack() == snapshot.tracks[0].id,
          "normal click preserves the group and makes its lane the active track");

    CoordinateTransform transform{snapshot.projectSampleRate, 100.0, 0.0};
    check(interaction.beginMoveGesture(
              snapshot, snapshot.tracks[0], snapshot.tracks[0].clips[0], 100.0),
          "group move gesture begins from a selected member");
    interaction.updateGesture(200.0, transform, snapshot, secondTrack);
    const auto command = interaction.endGesture();
    const auto& move = std::get<commands::MoveClips>(*command);
    check(move.clips == std::vector<clips::ClipId>{first, second} &&
              move.deltaFrames == 48000,
          "group drag emits one horizontal MoveClips with a shared delta");

    const auto duplicate = interaction.duplicateCommand(snapshot);
    const auto& duplicateBatch = std::get<commands::DuplicateClips>(*duplicate);
    check(duplicateBatch.clips == std::vector<clips::ClipId>{first, second} &&
              duplicateBatch.deltaFrames == 288000,
          "group duplicate uses the complete temporal span including gaps");

    const auto activeTrack = interaction.selectedTrack();
    const auto duplicated = project.duplicateClips(
        duplicateBatch.clips, duplicateBatch.deltaFrames);
    check(duplicated.succeeded() && duplicated.createdClips.size() == 2,
          "duplicate fixture returns exactly two new ClipIds");
    interaction.selectClips(duplicated.createdClips);
    snapshot = makeTimelineSnapshot(project, 2);
    interaction.reconcile(snapshot);
    check(std::vector<clips::ClipId>{interaction.selections().begin(),
                                     interaction.selections().end()} ==
              duplicated.createdClips &&
              interaction.selectedTrack() == activeTrack,
          "Duplicate selects exactly the new ClipIds and preserves the active track");
    const auto duplicateCreated = interaction.duplicateCommand(snapshot);
    const auto deleteCreated = interaction.deleteCommand();
    check(duplicateCreated && deleteCreated &&
              std::get<commands::DuplicateClips>(*duplicateCreated).clips ==
                  duplicated.createdClips &&
              std::get<commands::DeleteClips>(*deleteCreated).clips ==
                  duplicated.createdClips,
          "clip actions derive from the multi-selection, not the active track");
    const commands::ImportAudioToTrack importForActive{
        {}, *interaction.selectedTrack(), {0}};
    const commands::DeleteAudioTrack deleteActive{*interaction.selectedTrack()};
    const commands::ReorderAudioTrack reorderActive{
        *interaction.selectedTrack(), secondTrack,
        commands::TrackPlacement::before};
    check(importForActive.track == *activeTrack &&
              deleteActive.track == *activeTrack &&
              reorderActive.track == *activeTrack,
          "track actions deliberately use the independent active-track context");

    check(bool(project.deleteClip(first)), "partial selection deletion fixture");
    snapshot = makeTimelineSnapshot(project, 3);
    interaction.reconcile(snapshot);
    check(interaction.selections().size() == 2,
          "reconcile retains selected duplicate IDs when unrelated clips disappear");
    interaction.toggleClip(snapshot.tracks[0].id, duplicated.createdClips[0]);
    interaction.toggleClip(secondTrack, duplicated.createdClips[1]);
    check(interaction.selections().empty(),
          "toggle removes every selected ClipId without conflating track context");
}

void largeSessionTest() {
    project::ProjectState project{timeline::SampleRate{48000.0}, "Large UI"};
    for (int index = 0; index < 64; ++index)
        static_cast<void>(project.addAudioTrack("Track " + std::to_string(index + 1)));
    const auto imported = project.importAudioToTrack(
        project.tracks()[0].id, media::MediaReference{"/media/long.wav", {}},
        {48000000}, timeline::SampleRate{48000.0}, media::AudioChannelLayout::mono);
    for (int index = 1; index < 1000; ++index) {
        const auto track = project.tracks()[static_cast<std::size_t>(index % 64)].id;
        static_cast<void>(project.addClip(track, imported.source,
            {static_cast<std::int64_t>(index * 1000)}, {480.0}, {0.0}));
    }
    const auto snapshot = makeTimelineSnapshot(project, 99);
    std::size_t clips{};
    for (const auto& track : snapshot.tracks) clips += track.clips.size();
    check(snapshot.tracks.size() == 64 && clips == 1000,
          "64-track/1000-clip read model must build without GUI components");
    TimelineInteraction interaction;
    const auto selected = snapshot.tracks[40].clips.front().id;
    interaction.select(selected);
    CoordinateTransform transform{snapshot.projectSampleRate, 20.0, 100.0};
    check(transform.isValid() && interaction.selection() == selected,
          "large-session selection, scroll and zoom remain ID-based and bounded");
}
} // namespace

int main() {
    coordinateTests();
    snapshotAndSelectionTests();
    gestureTests();
    crossTrackInteractionTests();
    actionsAndRefreshTests();
    multipleSelectionTests();
    largeSessionTest();
    std::cout << "Timeline UI foundation tests passed\n";
}
