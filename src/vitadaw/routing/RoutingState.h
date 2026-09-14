#pragma once

#include "vitadaw/tracks/AudioTrack.h"
#include "vitadaw/processors/ProcessorState.h"

#include <compare>
#include <cstdint>
#include <string>
#include <variant>
#include <vector>

namespace vitadaw::routing {

struct BusId {
    std::uint64_t value{};

    [[nodiscard]] constexpr bool isValid() const noexcept { return value != 0; }
    auto operator<=>(const BusId&) const = default;
};

enum class DestinationKind : std::uint8_t { master, bus };

struct OutputDestination {
    DestinationKind kind{DestinationKind::master};
    BusId bus;

    [[nodiscard]] static constexpr OutputDestination master() noexcept {
        return {};
    }
    [[nodiscard]] static constexpr OutputDestination toBus(BusId id) noexcept {
        return {DestinationKind::bus, id};
    }
    [[nodiscard]] constexpr bool isValid() const noexcept {
        return kind == DestinationKind::master
                   ? !bus.isValid()
                   : kind == DestinationKind::bus && bus.isValid();
    }
    bool operator==(const OutputDestination&) const = default;
};

// Compatibility name for code which still describes the source node. The
// destination itself is now common to tracks and buses.
using TrackOutputDestination = OutputDestination;

struct AudioBus {
    BusId id;
    std::string name;
    mixer::BusMixState mix;
    OutputDestination outputDestination{OutputDestination::master()};
    processors::InsertChain inserts;
};

struct TrackRoute {
    tracks::TrackId track;
    OutputDestination destination;
};

struct SendId {
    std::uint64_t value{};

    [[nodiscard]] constexpr bool isValid() const noexcept { return value != 0; }
    auto operator<=>(const SendId&) const = default;
};

using SendSource = std::variant<tracks::TrackId, BusId>;

enum class SendTapPoint : std::uint8_t {
    preFaderPrePan,
    postFaderPostPan,
};

[[nodiscard]] constexpr bool isValid(SendTapPoint tap) noexcept {
    return tap == SendTapPoint::preFaderPrePan ||
           tap == SendTapPoint::postFaderPostPan;
}

struct SendRoute {
    SendId id;
    SendSource source;
    BusId destination;
    SendTapPoint tapPoint{SendTapPoint::postFaderPostPan};
    mixer::SendMixState mix;
};

class RoutingState {
public:
    [[nodiscard]] const std::vector<AudioBus>& buses() const noexcept;
    [[nodiscard]] const std::vector<TrackRoute>& trackRoutes() const noexcept;
    [[nodiscard]] const std::vector<SendRoute>& sends() const noexcept;
    [[nodiscard]] const TrackRoute* findTrackRoute(tracks::TrackId track) const noexcept;
    [[nodiscard]] bool containsBus(BusId bus) const noexcept;
    [[nodiscard]] const AudioBus* findBus(BusId bus) const noexcept;
    [[nodiscard]] AudioBus* findBusMutable(BusId bus) noexcept;
    [[nodiscard]] const SendRoute* findSend(SendId send) const noexcept;

    void addTrack(tracks::TrackId track);
    [[nodiscard]] BusId addBus(std::string name);
    [[nodiscard]] bool setTrackDestination(
        tracks::TrackId track, OutputDestination destination) noexcept;
    [[nodiscard]] bool setBusMix(BusId bus,
                                 mixer::BusMixState state) noexcept;
    [[nodiscard]] bool setBusDestination(
        BusId bus, OutputDestination destination) noexcept;
    [[nodiscard]] SendId addSend(SendSource source, BusId destination,
                                 SendTapPoint tapPoint,
                                 mixer::SendMixState mix);
    [[nodiscard]] bool setSendRoute(SendId send, BusId destination,
                                    SendTapPoint tapPoint) noexcept;
    [[nodiscard]] bool removeSend(SendId send) noexcept;
    [[nodiscard]] bool setSendMix(SendId send,
                                  mixer::SendMixState mix) noexcept;

private:
    std::vector<AudioBus> buses_;
    std::vector<TrackRoute> trackRoutes_;
    std::vector<SendRoute> sends_;
    BusId nextBusId_{1};
    SendId nextSendId_{1};
};

} // namespace vitadaw::routing
