#include "vitadaw/platform/lifecycle/ApplicationShutdown.h"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {

void check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::exit(1);
    }
}

struct Owner {
    Owner(std::string label, std::vector<std::string>& events,
          bool* alive = nullptr)
        : label(std::move(label)), events(events), alive(alive) {
        if (alive != nullptr) {
            *alive = true;
        }
    }

    ~Owner() {
        events.push_back(label + " destroyed");
        if (alive != nullptr) {
            *alive = false;
        }
    }

    std::string label;
    std::vector<std::string>& events;
    bool* alive{};
};

struct AudioOwner {
    explicit AudioOwner(std::vector<std::string>& events) : events(events) {}

    void clearStateChangedCallback() noexcept {
        events.push_back("callback unregistered");
        callbackRegistered = false;
        if (callbackClearedWhileReceiverAlive != nullptr) {
            *callbackClearedWhileReceiverAlive =
                receiverAlive == nullptr || *receiverAlive;
        }
    }

    void shutdown() noexcept {
        events.push_back("audio stopped");
        ++shutdownCalls;
        active = false;
    }

    ~AudioOwner() { events.push_back("audio adapter destroyed"); }

    std::vector<std::string>& events;
    bool* receiverAlive{};
    bool callbackRegistered{};
    bool* callbackClearedWhileReceiverAlive{};
    bool active{};
    int shutdownCalls{};
};

struct Fixture {
    void createAudio(bool active = false) {
        audio = std::make_unique<AudioOwner>(events);
        audio->active = active;
        audio->callbackClearedWhileReceiverAlive =
            &callbackClearedWhileReceiverAlive;
    }

    void createApplication() {
        application = std::make_unique<Owner>("application", events);
    }

    void createDispatcher() {
        dispatcher = std::make_unique<Owner>("dispatcher", events);
    }

    void createWindow() {
        window = std::make_unique<Owner>("window", events, &windowAlive);
    }

    void registerCallback() {
        audio->callbackRegistered = true;
        audio->receiverAlive = &windowAlive;
    }

    void shutdown() noexcept {
        vitadaw::platform::lifecycle::shutdownApplicationOwners(
            audio, window, dispatcher, application,
            [this] { events.emplace_back("event producers stopped"); });
    }

    std::vector<std::string> events;
    bool windowAlive{};
    bool callbackClearedWhileReceiverAlive{};
    std::unique_ptr<AudioOwner> audio;
    std::unique_ptr<Owner> window;
    std::unique_ptr<Owner> dispatcher;
    std::unique_ptr<Owner> application;
};

void completeInitialiseThenShutdown() {
    Fixture fixture;
    fixture.createAudio(true);
    fixture.createApplication();
    fixture.createDispatcher();
    fixture.createWindow();
    fixture.registerCallback();
    fixture.shutdown();
    const std::vector<std::string> expected{
        "event producers stopped", "callback unregistered", "audio stopped",
        "window destroyed", "dispatcher destroyed", "application destroyed",
        "audio adapter destroyed"};
    check(fixture.events == expected, "complete shutdown order must be explicit");
}

void failureBeforeAdapter() {
    Fixture fixture;
    fixture.shutdown();
    check(fixture.events == std::vector<std::string>{"event producers stopped"},
          "shutdown before adapter construction must be valid");
}

void failureAfterAdapter() {
    Fixture fixture;
    fixture.createAudio();
    fixture.shutdown();
    check(fixture.events == std::vector<std::string>{
              "event producers stopped", "callback unregistered", "audio stopped",
              "audio adapter destroyed"},
          "partially constructed adapter must be stopped and destroyed");
}

void adapterWithoutWindow() {
    Fixture fixture;
    fixture.createAudio(true);
    fixture.createApplication();
    fixture.createDispatcher();
    fixture.shutdown();
    check(std::find(fixture.events.begin(), fixture.events.end(),
                    "window destroyed") == fixture.events.end(),
          "missing window must not be treated as constructed");
    check(fixture.events.back() == "audio adapter destroyed",
          "adapter must outlive application reference on partial shutdown");
}

void activeAdapterAndWindow() {
    Fixture fixture;
    fixture.createAudio(true);
    fixture.createApplication();
    fixture.createDispatcher();
    fixture.createWindow();
    fixture.registerCallback();
    fixture.shutdown();
    check(!fixture.windowAlive, "window must be destroyed");
    check(std::find(fixture.events.begin(), fixture.events.end(), "audio stopped") !=
              fixture.events.end(),
          "active adapter must be stopped");
}

void callbackIsUnregisteredBeforeReceiverDestruction() {
    Fixture fixture;
    fixture.createAudio(true);
    fixture.createApplication();
    fixture.createDispatcher();
    fixture.createWindow();
    fixture.registerCallback();
    fixture.shutdown();
    check(fixture.callbackClearedWhileReceiverAlive,
          "callback must be removed while its receiver is alive");
}

void shutdownTwice() {
    Fixture fixture;
    fixture.createAudio(true);
    fixture.createApplication();
    fixture.createDispatcher();
    fixture.createWindow();
    fixture.registerCallback();
    fixture.shutdown();
    const auto firstSize = fixture.events.size();
    fixture.shutdown();
    check(fixture.events.size() == firstSize + 1 &&
              fixture.events.back() == "event producers stopped",
          "second shutdown may only repeat the idempotent producer stop");
    check(std::count(fixture.events.begin(), fixture.events.end(),
                     "audio adapter destroyed") == 1,
          "idempotent shutdown must not destroy owners twice");
}

void shutdownAfterStartupFailure() {
    Fixture fixture;
    fixture.createAudio(true);
    fixture.createApplication();
    fixture.createDispatcher();
    fixture.createWindow();
    // Simulate failure immediately before callback registration/timer start.
    fixture.shutdown();
    check(fixture.events.front() == "event producers stopped" &&
              fixture.events.back() == "audio adapter destroyed",
          "startup failure must use the same partial cleanup path");
}

} // namespace

int main() {
    completeInitialiseThenShutdown();
    failureBeforeAdapter();
    failureAfterAdapter();
    adapterWithoutWindow();
    activeAdapterAndWindow();
    callbackIsUnregisteredBeforeReceiverDestruction();
    shutdownTwice();
    shutdownAfterStartupFailure();
    std::cout << "All application lifecycle tests passed\n";
}
