#pragma once

#include <utility>

namespace vitadaw::platform::lifecycle {

// Portable ownership-order primitive used by the native application. Owner
// presence, rather than a duplicated startup-stage flag, is the cleanup
// authority, so every partially constructed state is valid.
template <typename AudioOwner, typename WindowOwner, typename DispatcherOwner,
          typename ApplicationOwner, typename StopEventProducers>
void shutdownApplicationOwners(AudioOwner& audio,
                               WindowOwner& window,
                               DispatcherOwner& dispatcher,
                               ApplicationOwner& application,
                               StopEventProducers&& stopEventProducers) noexcept {
    std::forward<StopEventProducers>(stopEventProducers)();
    if (audio) {
        // The callback captures the outer application and addresses the window.
        // Clear it while both the adapter and every possible receiver are alive.
        audio->clearStateChangedCallback();
        if constexpr (requires { application->shutdownRecording(); }) {
            if (application) application->shutdownRecording();
        }
        // Quiesce the device/RT consumer before destroying any session owner.
        // The adapter remains alive until after DawApplication, whose audio
        // control reference is non-owning.
        audio->shutdown();
    }
    window.reset();
    dispatcher.reset();
    application.reset(); // Holds a non-owning reference to audio.
    audio.reset();       // Final owner release; adapter shutdown is idempotent.
}

} // namespace vitadaw::platform::lifecycle
