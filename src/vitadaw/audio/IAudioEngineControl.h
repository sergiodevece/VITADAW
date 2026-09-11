#pragma once

#include "vitadaw/audio/RealtimeTransportExchange.h"
#include "vitadaw/timeline/Time.h"

#include <cstdint>
#include <filesystem>
#include <string>

namespace vitadaw::audio {

struct AudioFileMetadata {
    timeline::SampleRate sourceSampleRate;
    std::uint32_t channelCount{};
    timeline::SourceFrameCount sourceFrameCount;
    timeline::Seconds duration;

    bool operator==(const AudioFileMetadata&) const = default;
};

struct AudioFileLoadResult {
    bool success{};
    AudioFileMetadata metadata;
    std::string errorMessage;
};

// Called from the application thread. Realtime control methods must enqueue
// bounded, non-blocking requests. loadWav performs file I/O explicitly outside
// the realtime thread.
class IAudioEngineControl {
public:
    virtual ~IAudioEngineControl() = default;
    // File I/O and decoding are allowed here because this method is never called
    // by the realtime thread.
    [[nodiscard]] virtual AudioFileLoadResult loadWav(
        const std::filesystem::path& file,
        timeline::SampleRate projectSampleRate) = 0;
    [[nodiscard]] virtual AudioControlRequestResult tryRequestPlay() noexcept = 0;
    [[nodiscard]] virtual AudioControlRequestResult tryRequestStop() noexcept = 0;
    [[nodiscard]] virtual RealtimeTransportSnapshot transportSnapshot() const noexcept = 0;
};

} // namespace vitadaw::audio
