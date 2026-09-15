#include "vitadaw/application/DawApplication.h"
#include "vitadaw/persistence/ProjectPersistence.h"
#include "vitadaw/audio/AudioPreparationPolicy.h"
#include <array>
#include <new>

namespace vitadaw::application {
using namespace persistence;

commands::CommandResult DawApplication::persistenceCommand(const commands::Command& command) {
    PersistenceResult result;
    try {
        if (transport_.playback != transport::PlaybackState::stopped)
            result = {PersistenceCode::transportMustBeStopped, PersistencePhase::validate};
        else if (const auto* load = std::get_if<commands::LoadProject>(&command))
            result = loadProject(load->path, load->discardUnsaved);
        else if (const auto* save = std::get_if<commands::SaveProjectAs>(&command))
            result = saveProject(save->path);
        else result = saveProject(session_.projectFilePath);
    } catch (const std::bad_alloc&) {
        result = {PersistenceCode::capacityExceeded, PersistencePhase::prepare};
    } catch (...) {
        result = {PersistenceCode::preparationFailed, PersistencePhase::prepare};
    }
    // No allocation after a successful disk/document commit.
    const auto status = result.success() ? commands::CommandStatus::accepted : commands::CommandStatus::rejected;
    return {status, {}, commands::CommandError::none, std::move(result)};
}

PersistenceResult DawApplication::saveProject(std::filesystem::path path) {
    if (path.empty()) return {PersistenceCode::savePathRequired, PersistencePhase::validate};
    if (!path.is_absolute()) return {PersistenceCode::schemaValidationFailed, PersistencePhase::validate};
    path = path.lexically_normal();
    const auto token = session_.history.currentStateToken();
    auto encoded = serializeProject(session_.project, path);
    if (!encoded.result.success()) return std::move(encoded.result);
    // Path/token are fully staged before native replace. On durabilityUncertain
    // the new bytes may be visible, but session metadata deliberately stays put.
    auto result = files_.replace(path, encoded.bytes);
    if (!result.success()) return result;
    session_.projectFilePath.swap(path);
    session_.savedStateToken = token;
    return {};
}

PersistenceResult DawApplication::loadProject(std::filesystem::path path, bool discard) {
    if (session_.dirty() && !discard)
        return {PersistenceCode::unsavedChanges, PersistencePhase::validate};
    if (!path.is_absolute()) return {PersistenceCode::schemaValidationFailed, PersistencePhase::validate};
    if (!session_.history.canCreateState()) return {PersistenceCode::capacityExceeded, PersistencePhase::prepare};
    path = path.lexically_normal();
    auto read = files_.read(path, maximumDocumentBytes);
    if (!read.result.success()) return std::move(read.result);
    auto decoded = deserializeProject({reinterpret_cast<const char*>(read.bytes.data()), read.bytes.size()});
    if (!decoded.result.success()) return std::move(decoded.result);
    // Release input storage before decoding PCM.
    std::vector<std::byte>{}.swap(read.bytes);
    auto data = decoded.project->documentData();
    decoded.project.reset();
    if (musicalRevision_ == UINT64_MAX) return {PersistenceCode::capacityExceeded, PersistencePhase::prepare};
    auto musicalMap = musical::PreparedMusicalTimeMap::compile(data.musicalTime, data.settings.sampleRate, musicalRevision_ + 1);
    if (!musicalMap) return {PersistenceCode::semanticValidationFailed, PersistencePhase::prepare};
    auto temporal = audioEngine_.prepareTemporalContext(
        data.musicalTime, data.loopRange, data.settings.sampleRate,
        musicalRevision_ + 1);
    if (!temporal.success())
        return {PersistenceCode::semanticValidationFailed, PersistencePhase::prepare};
    std::vector<audio::PreparedSourceAudio> resources;
    resources.reserve(data.sources.size());
    std::size_t candidateBytes{};
    constexpr std::size_t budget = 512U * 1024U * 1024U;
    for (auto& source : data.sources) {
        std::array<std::filesystem::path, 2> paths;
        if (source.media.projectRelativePath)
            paths[0] = (path.parent_path() / *source.media.projectRelativePath).lexically_normal();
        paths[1] = source.media.originalPath;
        PersistenceResult failure{PersistenceCode::missingMedia, PersistencePhase::media};
        bool prepared{};
        for (const auto& mediaPath : paths) {
            if (mediaPath.empty()) continue;
            auto media = audioEngine_.prepareVerifiedWav(mediaPath, *source.media.fingerprint, candidateBytes);
            if (!media.success()) {
                if (media.result.code != PersistenceCode::fileNotFound) {
                    failure = media.result.success() ? PersistenceResult{PersistenceCode::mediaChanged, PersistencePhase::media}
                                                    : std::move(media.result);
                }
                continue;
            }
            const auto& metadata = media.prepared->metadata;
            if (media.prepared->media.fingerprint != source.media.fingerprint ||
                metadata.sourceSampleRate != source.sampleRate ||
                metadata.channelCount != media::channelCount(source.layout) ||
                metadata.sourceFrameCount != source.frameCount) {
                failure = {PersistenceCode::mediaChanged, PersistencePhase::media};
                continue;
            }
            const auto bytes = audio::validatePreparationShape(source.sampleRate,
                metadata.channelCount, source.frameCount.value,
                audioEngine_.preparedAudioBytes() + candidateBytes, budget);
            if (!bytes.isValid()) return {PersistenceCode::capacityExceeded, PersistencePhase::prepare};
            candidateBytes += static_cast<std::size_t>(source.frameCount.value) * metadata.channelCount * sizeof(float);
            source.media.originalPath = mediaPath;
            resources.push_back({source.id, std::move(media.prepared)});
            prepared = true;
            break;
        }
        if (!prepared) {
            failure.entityKind = "source";
            failure.entityId = std::to_string(source.id.value);
            failure.path = source.media.originalPath;
            return failure;
        }
    }
    auto candidate = project::ProjectState::fromDocumentData(std::move(data));
    if (!candidate) return {PersistenceCode::semanticValidationFailed, PersistencePhase::validate};
    auto plan = audioEngine_.prepareProjectReplacement(makePlanSpecification(*candidate), std::move(resources));
    if (!plan.success()) return {PersistenceCode::preparationFailed, PersistencePhase::prepare};
    struct Context {
        DawApplication* app;
        project::ProjectState* project;
        std::filesystem::path* path;
        std::unique_ptr<const musical::PreparedMusicalTimeMap>* musicalMap;
        audio::PreparedAudibilityState audibility;
    } context{this, candidate.get(), &path, &musicalMap.value, resolveAudibility(*candidate)};
    const audio::AudioFileCommitAction commit{&context, [](void* raw) noexcept {
        auto& c = *static_cast<Context*>(raw);
        c.app->session_.adopt(*c.project, *c.path);
        c.app->musicalTime_.swap(*c.musicalMap);
        ++c.app->musicalRevision_;
        c.app->audibility_ = c.audibility;
        c.app->transport_.stopAndRewind();
        c.app->transport_.setDuration(c.app->session_.project.duration());
    }};
    if (!audioEngine_.commitPreparedProjectAndTemporalContext(
            std::move(plan.prepared), std::move(temporal.prepared), commit))
        return {PersistenceCode::preparationFailed, PersistencePhase::commit};
    pendingAudioCommandSequence_ = audioEngine_.transportSnapshot().lastProcessedCommandSequence;
    return {};
}
} // namespace vitadaw::application
