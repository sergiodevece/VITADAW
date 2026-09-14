#pragma once

#include "vitadaw/routing/RoutingState.h"
#include "vitadaw/tracks/AudioTrack.h"

#include <variant>

namespace vitadaw::processors {

struct MasterTarget {
    bool operator==(const MasterTarget&) const = default;
};

using InsertTarget = std::variant<tracks::TrackId, routing::BusId, MasterTarget>;

} // namespace vitadaw::processors
