#pragma once
#include "vitadaw/history/UndoManager.h"
#include "vitadaw/audio/PreparedTemporalContext.h"
#include "vitadaw/waveform/WaveformCache.h"
#include <optional>

namespace vitadaw::application {
struct ProjectSession {
    explicit ProjectSession(timeline::SampleRate rate) : project(rate) {}
    project::ProjectState project;
    history::UndoManager history;
    std::filesystem::path projectFilePath;
    history::StateToken savedStateToken{history.currentStateToken()};
    bool loopEnabled{};
    bool metronomeEnabled{};
    audio::MetronomeLevelDb metronomeLevel{};
    waveform::WaveformCache waveforms;
    std::uint64_t appliedTemporalRevision{};
    std::optional<tracks::TrackId> armedTrack;
    [[nodiscard]] bool dirty() const noexcept { return history.currentStateToken() != savedStateToken; }
    void adopt(project::ProjectState& candidate, std::filesystem::path& path,
               waveform::WaveformCache& candidateWaveforms) noexcept {
        project.swap(candidate);
        projectFilePath.swap(path);
        waveforms.swap(candidateWaveforms);
        history.commitBarrier(); // New session identity, monotonic revision, no old Undo.
        savedStateToken = history.currentStateToken();
        loopEnabled = false;
        metronomeEnabled = false;
        metronomeLevel = {};
        appliedTemporalRevision = 0;
        armedTrack.reset();
    }
};
} // namespace vitadaw::application
