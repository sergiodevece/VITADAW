#include "vitadaw/audio/RecordingRecovery.h"
#include "vitadaw/platform/files/RecoveryInventory.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <span>
#include <string_view>
#include <vector>

namespace {
using namespace vitadaw;

void check(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

void write16(std::array<std::byte, 44>& bytes, std::size_t offset, std::uint16_t value) {
    bytes[offset] = static_cast<std::byte>(value & 0xffU);
    bytes[offset + 1] = static_cast<std::byte>((value >> 8U) & 0xffU);
}

void write32(std::array<std::byte, 44>& bytes, std::size_t offset, std::uint32_t value) {
    for (std::size_t index{}; index < 4; ++index)
        bytes[offset + index] = static_cast<std::byte>((value >> (index * 8U)) & 0xffU);
}

void writeValidWav(const std::filesystem::path& path, std::uint32_t channels = 1,
                   std::uint32_t sampleRate = 48000, std::uint32_t byteRate = 0) {
    constexpr std::uint32_t frames = 4;
    constexpr std::uint32_t bits = 32;
    const auto blockAlign = channels * (bits / 8U);
    const auto dataBytes = frames * blockAlign;
    std::array<std::byte, 44> header{};
    const auto put = [&](std::size_t offset, const char* text) {
        for (std::size_t index{}; index < 4; ++index)
            header[offset + index] = static_cast<std::byte>(text[index]);
    };
    put(0, "RIFF");
    write32(header, 4, 36U + dataBytes);
    put(8, "WAVE");
    put(12, "fmt ");
    write32(header, 16, 16);
    write16(header, 20, 3); // IEEE float32
    write16(header, 22, static_cast<std::uint16_t>(channels));
    write32(header, 24, sampleRate);
    write32(header, 28, byteRate == 0U ? sampleRate * blockAlign : byteRate);
    write16(header, 32, static_cast<std::uint16_t>(blockAlign));
    write16(header, 34, static_cast<std::uint16_t>(bits));
    put(36, "data");
    write32(header, 40, dataBytes);
    std::ofstream output(path, std::ios::binary);
    output.write(reinterpret_cast<const char*>(header.data()), static_cast<std::streamsize>(header.size()));
    std::vector<std::byte> samples(dataBytes);
    output.write(reinterpret_cast<const char*>(samples.data()), static_cast<std::streamsize>(samples.size()));
    check(output.good(), "WAV fixture written");
}

void writeTruncatedWav(const std::filesystem::path& path) {
    std::ofstream output(path, std::ios::binary);
    output.write("RIFF", 4);
}

audio::RecordingRecoveryMarker marker(std::string session, std::string temporary,
                                      std::string published = {}) {
    audio::RecordingRecoveryMarker value;
    value.sessionId = std::move(session);
    value.classification = published.empty() ? audio::RecordingRecoveryClass::temporary
                                              : audio::RecordingRecoveryClass::publishedFinal;
    value.temporaryName = std::move(temporary);
    value.publishedName = std::move(published);
    value.deviceSampleRate = timeline::SampleRate{48000};
    value.acceptedFrames = {4};
    return value;
}

std::filesystem::path markerPath(const std::filesystem::path& root, std::string_view session) {
    return root / (".vitadaw-recording-" + std::string{session} + "-temporary.recovery");
}

void writeMarkerBytes(const std::filesystem::path& path, std::string_view bytes) {
    std::ofstream output(path, std::ios::binary);
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    check(output.good(), "marker fixture written");
}

bool hasDiagnostic(const audio::RecoveryInventory& inventory, const std::filesystem::path& path,
                   std::string_view text) {
    return std::any_of(inventory.diagnostics.begin(), inventory.diagnostics.end(),
        [&](const auto& diagnostic) {
            return diagnostic.path == std::filesystem::absolute(path).lexically_normal() &&
                   diagnostic.message.find(text) != std::string::npos;
        });
}

const audio::RecoveryCandidate* find(const audio::RecoveryInventory& inventory,
                                     const std::filesystem::path& path) {
    const auto absolute = std::filesystem::absolute(path).lexically_normal();
    const auto found = std::find_if(inventory.candidates.begin(), inventory.candidates.end(),
        [&](const auto& candidate) { return candidate.path == absolute; });
    return found == inventory.candidates.end() ? nullptr : &*found;
}

void scanClassifiesKnownAndUnknownArtifacts(const std::filesystem::path& root) {
    const auto recording = root / ".Recording 000001.1.part.wav";
    writeValidWav(recording);
    std::string markerError;
    check(audio::writeRecordingRecoveryMarker(root, marker("recording-valid", recording.filename().string()), markerError),
          "recording marker fixture written");

    const auto exportTemporary = root / ".Solo.wav.export.7.part.wav";
    writeValidWav(exportTemporary, 2);

    const auto final = root / "Published.wav";
    const auto recordingAlias = root / ".Recording 000002.2.part.wav";
    writeValidWav(final);
    std::error_code filesystemError;
    std::filesystem::create_hard_link(final, recordingAlias, filesystemError);
    check(!filesystemError, "recording hard-link fixture created");
    check(audio::writeRecordingRecoveryMarker(
              root, marker("recording-alias", recordingAlias.filename().string(), final.filename().string()), markerError),
          "published recording marker fixture written");

    const auto exportFinal = root / "Mix.wav";
    const auto exportAlias = root / ".Mix.wav.export.8.part.wav";
    writeValidWav(exportFinal, 2);
    std::filesystem::create_hard_link(exportFinal, exportAlias, filesystemError);
    check(!filesystemError, "export hard-link fixture created");

    const auto unknown = root / "foreign.wav";
    writeValidWav(unknown);
    const auto truncated = root / ".Recording 000003.3.part.wav";
    writeTruncatedWav(truncated);
    check(audio::writeRecordingRecoveryMarker(root, marker("recording-truncated", truncated.filename().string()), markerError),
          "truncated recording marker fixture written");

    const std::array roots{root};
    const auto inventory = platform::files::scanRecoveryInventory(roots);
    const auto* recorded = find(inventory, recording);
    check(recorded && recorded->origin == audio::RecoveryOrigin::recording &&
              recorded->classification == audio::RecoveryCandidateClass::recognizedPotentiallyRecoverable &&
              recorded->wavValidation == audio::RecoveryWavValidation::structurallyValid &&
              recorded->recordingSessionId == "recording-valid",
          "valid marker-backed recording is recognized");
    const auto* exported = find(inventory, exportTemporary);
    check(exported && exported->origin == audio::RecoveryOrigin::exportWav &&
              exported->classification == audio::RecoveryCandidateClass::recognizedPotentiallyRecoverable,
          "valid unpaired export temporary is recognized");
    const auto* recordingLinked = find(inventory, recordingAlias);
    check(recordingLinked && recordingLinked->classification == audio::RecoveryCandidateClass::publishedTemporaryAlias &&
              recordingLinked->associatedPath == std::filesystem::absolute(final).lexically_normal(),
          "recording final and temporary hard link are classified as one alias artifact");
    const auto* exportLinked = find(inventory, exportAlias);
    check(exportLinked && exportLinked->classification == audio::RecoveryCandidateClass::publishedTemporaryAlias &&
              exportLinked->associatedPath == std::filesystem::absolute(exportFinal).lexically_normal(),
          "export final and temporary hard link are classified as one alias artifact");
    const auto* recognizedExportFinal = find(inventory, exportFinal);
    check(recognizedExportFinal && recognizedExportFinal->origin == audio::RecoveryOrigin::exportWav &&
              recognizedExportFinal->classification ==
                  audio::RecoveryCandidateClass::recognizedPotentiallyRecoverable,
          "export final is recognized only through the verified temporary alias relationship");
    const auto* foreign = find(inventory, unknown);
    check(foreign && foreign->classification == audio::RecoveryCandidateClass::provenanceUnknown &&
              foreign->wavValidation == audio::RecoveryWavValidation::structurallyValid,
          "unknown WAV is never attributed to VitaDAW by extension alone");
    const auto* damaged = find(inventory, truncated);
    check(damaged && damaged->classification == audio::RecoveryCandidateClass::recognizedIncomplete &&
              damaged->wavValidation == audio::RecoveryWavValidation::truncated,
          "marker-backed truncated WAV is retained as incomplete");
}

void snapshotAndContainmentAreSafe(const std::filesystem::path& root) {
    const auto original = root / ".Recording 000004.4.part.wav";
    writeValidWav(original);
    std::string markerError;
    check(audio::writeRecordingRecoveryMarker(root, marker("snapshot", original.filename().string()), markerError),
          "snapshot marker fixture written");
    const std::array roots{root};
    const auto inventory = platform::files::scanRecoveryInventory(roots);
    const auto* candidate = find(inventory, original);
    check(candidate && platform::files::matchesRecoveryCandidateSnapshot(*candidate),
          "fresh discovery snapshot has matching identity");
    const auto retainedOriginal = root / "retained-original.wav";
    std::error_code filesystemError;
    std::filesystem::create_hard_link(original, retainedOriginal, filesystemError);
    check(!filesystemError, "original retained to prevent inode reuse");
    std::filesystem::rename(original, root / "renamed.wav", filesystemError);
    check(!filesystemError && !platform::files::matchesRecoveryCandidateSnapshot(*candidate),
          "renamed pathname does not pass future identity revalidation");
    writeValidWav(original);
    check(!platform::files::matchesRecoveryCandidateSnapshot(*candidate),
          "different object reused at pathname does not pass identity revalidation");

    const auto outside = root.parent_path() / "vitadaw-recovery-inventory-outside.wav";
    writeValidWav(outside);
    const auto fileLink = root / "outside.wav";
    std::filesystem::create_symlink(outside, fileLink, filesystemError);
    if (!filesystemError) {
        const auto outsideDirectory = root.parent_path() / "vitadaw-recovery-inventory-outside-dir";
        std::filesystem::create_directories(outsideDirectory, filesystemError);
        check(!filesystemError, "outside directory created");
        writeValidWav(outsideDirectory / "escaped.wav");
        std::filesystem::create_directory_symlink(outsideDirectory, root / "outside-dir", filesystemError);
        const auto contained = platform::files::scanRecoveryInventory(roots);
        check(find(contained, fileLink) == nullptr &&
                  std::none_of(contained.candidates.begin(), contained.candidates.end(),
                      [&](const auto& item) { return !item.path.string().starts_with(root.string()); }),
              "symlink files and directories never escape the authorized root");
        std::filesystem::remove_all(outsideDirectory, filesystemError);
    }
    std::filesystem::remove(outside, filesystemError);
}

void scanIsBoundedAndReadOnly(const std::filesystem::path& root) {
    const auto large = root / "foreign-large.wav";
    writeValidWav(large);
    std::error_code error;
    std::filesystem::resize_file(large, 32U * 1024U * 1024U, error);
    check(!error, "large sparse WAV fixture created");
    const auto beforeTime = std::filesystem::last_write_time(large, error);
    check(!error, "fixture timestamp captured");
    std::ifstream beforeInput(large, std::ios::binary);
    std::array<char, 44> before{};
    beforeInput.read(before.data(), static_cast<std::streamsize>(before.size()));
    const std::array roots{root};
    const auto inventory = platform::files::scanRecoveryInventory(roots);
    const auto* candidate = find(inventory, large);
    check(candidate && candidate->sizeBytes == 32U * 1024U * 1024U &&
              candidate->wavValidation == audio::RecoveryWavValidation::structurallyValid,
          "large media is represented by metadata without payload retention");
    const auto afterTime = std::filesystem::last_write_time(large, error);
    check(!error && beforeTime == afterTime, "inventory does not modify file timestamps");
    std::ifstream afterInput(large, std::ios::binary);
    std::array<char, 44> after{};
    afterInput.read(after.data(), static_cast<std::streamsize>(after.size()));
    check(before == after, "inventory does not modify media bytes");

    const std::array missing{root / "missing-authorized-root"};
    const auto missingInventory = platform::files::scanRecoveryInventory(missing);
    check(!missingInventory.rootErrors.empty(), "per-root enumeration failures are reported");

#if !defined(_WIN32)
    const auto denied = root / "permission-denied";
    std::filesystem::create_directories(denied, error);
    check(!error, "permission fixture directory created");
    std::filesystem::permissions(denied, std::filesystem::perms::none,
                                 std::filesystem::perm_options::replace, error);
    check(!error, "permission fixture restricted");
    const auto permissionInventory = platform::files::scanRecoveryInventory(roots);
    std::filesystem::permissions(denied, std::filesystem::perms::owner_all,
                                 std::filesystem::perm_options::replace, error);
    check(!error, "permission fixture restored");
    check(std::any_of(permissionInventory.diagnostics.begin(), permissionInventory.diagnostics.end(),
                      [&](const auto& diagnostic) { return diagnostic.path == denied; }),
          "subdirectory permission denial is reported without aborting the root scan");
#endif
}

void markerReadsAreBoundedAndNoFollow(const std::filesystem::path& root) {
    const auto validMedia = root / ".Recording bounded-valid.part.wav";
    writeValidWav(validMedia);
    std::string markerError;
    check(audio::writeRecordingRecoveryMarker(root,
              marker("bounded-valid", validMedia.filename().string()), markerError),
          "valid bounded marker written");
    const auto validMarker = markerPath(root, "bounded-valid");
    check(audio::readRecordingRecoveryMarker(validMarker).has_value(),
          "historical reader accepts an existing valid marker through the shared parser");

    const auto tooLarge = markerPath(root, "too-large");
    writeMarkerBytes(tooLarge,
                     std::string(audio::maximumRecordingRecoveryMarkerBytes + 1U, 'x'));
    const auto tooLongLine = markerPath(root, "too-long-line");
    writeMarkerBytes(tooLongLine, "session=" +
        std::string(audio::maximumRecordingRecoveryMarkerLineBytes, 'x') + "\n");
    const auto tooManyFields = markerPath(root, "too-many-fields");
    std::string fields;
    for (std::size_t index{}; index <= audio::maximumRecordingRecoveryMarkerFields; ++index)
        fields += "version=1\n";
    writeMarkerBytes(tooManyFields, fields);
    const auto corrupt = markerPath(root, "corrupt");
    writeMarkerBytes(corrupt, "this is not a marker\n");

    const auto symlinkTargetMedia = root / ".Recording symlink-target.part.wav";
    writeValidWav(symlinkTargetMedia);
    const auto outsideMarker = root.parent_path() / "vitadaw-recovery-inventory-outside-marker";
    writeMarkerBytes(outsideMarker,
        "version=1\nsession=symlink-marker\nclass=temporary\ntemporary=" +
        symlinkTargetMedia.filename().string() +
        "\npublished=\nchannels=1\nsampleRate=48000\nframes=4\n");
    const auto symlinkMarker = markerPath(root, "symlink");
    std::error_code linkError;
    std::filesystem::create_symlink(outsideMarker, symlinkMarker, linkError);

    const std::array roots{root};
    const auto inventory = platform::files::scanRecoveryInventory(roots);
    const auto* valid = find(inventory, validMedia);
    check(valid && valid->classification == audio::RecoveryCandidateClass::recognizedPotentiallyRecoverable,
          "a valid neighboring marker remains discoverable after invalid marker entries");
    check(hasDiagnostic(inventory, tooLarge, "bounded metadata size"),
          "oversized marker produces a bounded-size diagnostic");
    check(hasDiagnostic(inventory, tooLongLine, "overlong metadata line"),
          "overlong marker line produces a diagnostic");
    check(hasDiagnostic(inventory, tooManyFields, "too many metadata fields"),
          "excess marker fields produce a diagnostic");
    check(hasDiagnostic(inventory, corrupt, "malformed or incomplete"),
          "corrupt marker produces a diagnostic");
    if (!linkError) {
        check(hasDiagnostic(inventory, symlinkMarker, "symlink was not followed"),
              "marker symlink is rejected by the descriptor-bound reader");
        const auto* target = find(inventory, symlinkTargetMedia);
        check(target && target->classification == audio::RecoveryCandidateClass::provenanceUnknown,
              "content behind a marker symlink is not consumed as recording evidence");
    }
    std::filesystem::remove(outsideMarker, linkError);
}

void wavByteRateOverflowIsRejected(const std::filesystem::path& root) {
    const auto legitimate = root / "byte-rate-legitimate.wav";
    writeValidWav(legitimate, 2);
    const auto overflow = root / "byte-rate-overflow.wav";
    const auto overflowingRate = std::numeric_limits<std::uint32_t>::max();
    const auto wrappedByteRate = static_cast<std::uint32_t>(
        static_cast<std::uint64_t>(overflowingRate) * 8U);
    writeValidWav(overflow, 2, overflowingRate, wrappedByteRate);
    const std::array roots{root};
    const auto inventory = platform::files::scanRecoveryInventory(roots);
    const auto* valid = find(inventory, legitimate);
    check(valid && valid->wavValidation == audio::RecoveryWavValidation::structurallyValid,
          "representable WAV byte rate remains valid");
    const auto* malicious = find(inventory, overflow);
    check(malicious && malicious->wavValidation == audio::RecoveryWavValidation::invalidHeader,
          "WAV byte-rate arithmetic overflow is rejected structurally");
}

} // namespace

int main() {
    const auto root = std::filesystem::temp_directory_path() / "vitadaw-recovery-inventory-tests";
    std::error_code error;
    std::filesystem::remove_all(root, error);
    std::filesystem::create_directories(root, error);
    check(!error, "inventory fixture root created");
    const std::array roots{root};
    check(platform::files::scanRecoveryInventory(roots).candidates.empty(), "empty root has no candidates");
    scanClassifiesKnownAndUnknownArtifacts(root);
    snapshotAndContainmentAreSafe(root);
    scanIsBoundedAndReadOnly(root);
    markerReadsAreBoundedAndNoFollow(root);
    wavByteRateOverflowIsRejected(root);
    std::filesystem::remove_all(root, error);
    check(!error, "inventory fixture root removed");
    std::cout << "Recovery inventory tests passed\n";
}
