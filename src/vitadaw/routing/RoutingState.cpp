#include "vitadaw/routing/RoutingState.h"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <utility>

namespace vitadaw::routing {

const std::vector<AudioBus>& RoutingState::buses() const noexcept { return buses_; }

const std::vector<TrackRoute>& RoutingState::trackRoutes() const noexcept {
    return trackRoutes_;
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

} // namespace vitadaw::routing
