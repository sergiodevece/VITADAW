#include "vitadaw/routing/RoutingState.h"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace vitadaw::routing {

const std::vector<AudioBus>& RoutingState::buses() const noexcept { return buses_; }

const std::vector<TrackRoute>& RoutingState::trackRoutes() const noexcept {
    return trackRoutes_;
}

const std::vector<SendRoute>& RoutingState::sends() const noexcept {
    return sends_;
}

const TrackRoute* RoutingState::findTrackRoute(tracks::TrackId track) const noexcept {
    const auto found = std::find_if(
        trackRoutes_.begin(), trackRoutes_.end(),
        [track](const auto& candidate) { return candidate.track == track; });
    return found == trackRoutes_.end() ? nullptr : &*found;
}

bool RoutingState::containsBus(BusId bus) const noexcept {
    return bus.isValid() &&
           std::any_of(buses_.begin(), buses_.end(),
                       [bus](const auto& candidate) { return candidate.id == bus; });
}

const AudioBus* RoutingState::findBus(BusId bus) const noexcept {
    const auto found = std::find_if(
        buses_.begin(), buses_.end(),
        [bus](const auto& candidate) { return candidate.id == bus; });
    return found == buses_.end() ? nullptr : &*found;
}

const SendRoute* RoutingState::findSend(SendId send) const noexcept {
    const auto found = std::find_if(
        sends_.begin(), sends_.end(),
        [send](const auto& candidate) { return candidate.id == send; });
    return found == sends_.end() ? nullptr : &*found;
}

void RoutingState::addTrack(tracks::TrackId track) {
    if (!track.isValid() || findTrackRoute(track) != nullptr) {
        throw std::invalid_argument{"Invalid or duplicate routing track identity"};
    }
    trackRoutes_.push_back({track, OutputDestination::master()});
}

BusId RoutingState::addBus(std::string name) {
    if (!nextBusId_.isValid() ||
        nextBusId_.value == std::numeric_limits<std::uint64_t>::max()) {
        throw std::overflow_error{"Bus identity space exhausted"};
    }
    const auto id = nextBusId_;
    buses_.push_back({id, std::move(name), {}, OutputDestination::master()});
    ++nextBusId_.value;
    return id;
}

bool RoutingState::setBusMix(BusId bus,
                             mixer::BusMixState state) noexcept {
    if (!state.isValid()) {
        return false;
    }
    const auto found = std::find_if(
        buses_.begin(), buses_.end(),
        [bus](const auto& candidate) { return candidate.id == bus; });
    if (found == buses_.end()) {
        return false;
    }
    found->mix = state;
    return true;
}

bool RoutingState::setBusDestination(
    BusId bus, OutputDestination destination) noexcept {
    if (!destination.isValid() ||
        (destination.kind == DestinationKind::bus &&
         !containsBus(destination.bus))) {
        return false;
    }
    const auto found = std::find_if(
        buses_.begin(), buses_.end(),
        [bus](const auto& candidate) { return candidate.id == bus; });
    if (found == buses_.end()) {
        return false;
    }
    found->outputDestination = destination;
    return true;
}

bool RoutingState::setTrackDestination(
    tracks::TrackId track, OutputDestination destination) noexcept {
    if (!destination.isValid() ||
        (destination.kind == DestinationKind::bus &&
         !containsBus(destination.bus))) {
        return false;
    }
    const auto found = std::find_if(
        trackRoutes_.begin(), trackRoutes_.end(),
        [track](const auto& candidate) { return candidate.track == track; });
    if (found == trackRoutes_.end()) {
        return false;
    }
    found->destination = destination;
    return true;
}

SendId RoutingState::addSend(SendSource source, BusId destination,
                             SendTapPoint tapPoint,
                             mixer::SendMixState mix) {
    const auto sourceExists = std::visit(
        [this](const auto id) {
            using T = std::decay_t<decltype(id)>;
            if constexpr (std::is_same_v<T, tracks::TrackId>) {
                return findTrackRoute(id) != nullptr;
            } else {
                return containsBus(id);
            }
        },
        source);
    if (!sourceExists || !containsBus(destination) ||
        !routing::isValid(tapPoint) || !mix.isValid()) {
        throw std::invalid_argument{"Invalid send routing state"};
    }
    if (!nextSendId_.isValid() ||
        nextSendId_.value == std::numeric_limits<std::uint64_t>::max()) {
        throw std::overflow_error{"Send identity space exhausted"};
    }
    const auto id = nextSendId_;
    sends_.push_back({id, source, destination, tapPoint, mix});
    ++nextSendId_.value;
    return id;
}

bool RoutingState::setSendRoute(SendId send, BusId destination,
                                SendTapPoint tapPoint) noexcept {
    if (!containsBus(destination) || !routing::isValid(tapPoint)) {
        return false;
    }
    const auto found = std::find_if(
        sends_.begin(), sends_.end(),
        [send](const auto& candidate) { return candidate.id == send; });
    if (found == sends_.end()) {
        return false;
    }
    found->destination = destination;
    found->tapPoint = tapPoint;
    return true;
}

bool RoutingState::removeSend(SendId send) noexcept {
    const auto found = std::find_if(
        sends_.begin(), sends_.end(),
        [send](const auto& candidate) { return candidate.id == send; });
    if (found == sends_.end()) {
        return false;
    }
    sends_.erase(found);
    return true;
}

bool RoutingState::setSendMix(SendId send,
                              mixer::SendMixState mix) noexcept {
    if (!mix.isValid()) {
        return false;
    }
    const auto found = std::find_if(
        sends_.begin(), sends_.end(),
        [send](const auto& candidate) { return candidate.id == send; });
    if (found == sends_.end()) {
        return false;
    }
    found->mix = mix;
    return true;
}

} // namespace vitadaw::routing
