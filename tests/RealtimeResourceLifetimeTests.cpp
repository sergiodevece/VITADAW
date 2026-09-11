#include "vitadaw/audio/RealtimeAudioEngine.h"

#include <array>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <mutex>
#include <semaphore>
#include <string_view>
#include <thread>
#include <vector>

namespace {

struct LifetimeProbe {
    std::atomic<int> activeRealtimeUsers{};
    std::atomic<int> destructions{};
    std::atomic<int> destructionsWhileRealtimeActive{};
};

struct InstrumentedResource {
    InstrumentedResource(float sample, LifetimeProbe& lifetimeProbe)
        : samples(8, sample), probe(lifetimeProbe) {}

    ~InstrumentedResource() {
        if (published && probe.activeRealtimeUsers.load(std::memory_order_acquire) != 0) {
            probe.destructionsWhileRealtimeActive.fetch_add(1,
                                                            std::memory_order_relaxed);
        }
        probe.destructions.fetch_add(1, std::memory_order_release);
    }

    std::vector<float> samples;
    LifetimeProbe& probe;
    bool published{};
};

void check(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

vitadaw::audio::PreparedTrackView view(const InstrumentedResource& resource) {
    return {{{resource.samples.data(), nullptr}}, 1,
            {static_cast<std::uint64_t>(resource.samples.size())},
            vitadaw::timeline::SampleRate{100.0}};
}

void makeOperational(vitadaw::audio::RealtimeAudioEngine& engine) {
    engine.deviceInitialising();
    std::array<float*, 0> noChannels{};
    engine.processBlock({noChannels.data(), 0, 0},
                        vitadaw::timeline::SampleRate{100.0});
}

} // namespace

int main() {
    using namespace vitadaw;

    std::mutex callbackSerialization;
    audio::RealtimeAudioEngine engine;

    LifetimeProbe oldProbe;
    auto oldResource = std::make_unique<InstrumentedResource>(0.8F, oldProbe);
    oldResource->published = true;
    engine.configure({timeline::SampleRate{100.0}, {8}},
                     {view(*oldResource), {}});
    makeOperational(engine);
    check(engine.tryRequestPlay().accepted,
          "old resource should be playing before replacement");

    LifetimeProbe replacementProbe;
    auto replacement =
        std::make_unique<InstrumentedResource>(0.4F, replacementProbe);
    replacement->published = true;
    std::binary_semaphore oldCallbackEntered{0};
    std::binary_semaphore finishOldCallback{0};
    std::array<float, 1> oldLeft{}, oldRight{};
    std::thread realtimeUse([&] {
        const std::lock_guard callbackLock{callbackSerialization};
        oldProbe.activeRealtimeUsers.fetch_add(1, std::memory_order_acq_rel);
        oldCallbackEntered.release();
        finishOldCallback.acquire();
        std::array<float*, 2> channels{oldLeft.data(), oldRight.data()};
        engine.processBlock({channels.data(), channels.size(), oldLeft.size()},
                            timeline::SampleRate{100.0});
        oldProbe.activeRealtimeUsers.fetch_sub(1, std::memory_order_acq_rel);
    });

    oldCallbackEntered.acquire();
    std::thread replaceOnApplicationThread([&] {
        const std::lock_guard callbackLock{callbackSerialization};
        engine.configure({timeline::SampleRate{100.0}, {8}},
                         {view(*replacement), {}});
        oldResource.reset();
    });
    check(oldProbe.destructions.load(std::memory_order_acquire) == 0,
          "replacement must wait while RT can observe the old view");
    finishOldCallback.release();
    realtimeUse.join();
    replaceOnApplicationThread.join();
    check(std::abs(oldLeft[0] - 0.4F) < 1.0e-6F &&
              oldProbe.destructions.load(std::memory_order_acquire) == 1 &&
              oldProbe.destructionsWhileRealtimeActive.load(
                  std::memory_order_acquire) == 0,
          "old resource must be destroyed only after its final RT use");

    LifetimeProbe failedProbe;
    auto failedCandidate =
        std::make_unique<InstrumentedResource>(0.9F, failedProbe);
    failedCandidate.reset();
    check(failedProbe.destructions.load(std::memory_order_acquire) == 1 &&
              replacementProbe.destructions.load(std::memory_order_acquire) == 0,
          "failed load should destroy only its unpublished candidate");

    makeOperational(engine);
    check(engine.tryRequestPlay().accepted,
          "published replacement should remain playable after failed load");
    std::array<float, 1> replacementLeft{}, replacementRight{};
    std::array<float*, 2> replacementChannels{
        replacementLeft.data(), replacementRight.data()};
    engine.processBlock({replacementChannels.data(), replacementChannels.size(), 1},
                        timeline::SampleRate{100.0});
    check(std::abs(replacementLeft[0] - 0.2F) < 1.0e-6F,
          "failed load must preserve the currently published resource");

    std::binary_semaphore closeCallbackEntered{0};
    std::binary_semaphore finishCloseCallback{0};
    std::array<float, 1> closeLeft{}, closeRight{};
    std::thread finalRealtimeUse([&] {
        const std::lock_guard callbackLock{callbackSerialization};
        replacementProbe.activeRealtimeUsers.fetch_add(1,
                                                        std::memory_order_acq_rel);
        closeCallbackEntered.release();
        finishCloseCallback.acquire();
        std::array<float*, 2> channels{closeLeft.data(), closeRight.data()};
        engine.processBlock({channels.data(), channels.size(), closeLeft.size()},
                            timeline::SampleRate{100.0});
        replacementProbe.activeRealtimeUsers.fetch_sub(1,
                                                        std::memory_order_acq_rel);
    });
    closeCallbackEntered.acquire();
    std::thread closeOnApplicationThread([&] {
        const std::lock_guard callbackLock{callbackSerialization};
        engine.configure({{}, {}}, {});
        replacement.reset();
    });
    check(replacementProbe.destructions.load(std::memory_order_acquire) == 0,
          "close must wait while RT can observe the final resource");
    finishCloseCallback.release();
    finalRealtimeUse.join();
    closeOnApplicationThread.join();
    check(replacementProbe.destructions.load(std::memory_order_acquire) == 1 &&
              replacementProbe.destructionsWhileRealtimeActive.load(
                  std::memory_order_acquire) == 0,
          "close must release the last owner after RT becomes quiescent");

    std::cout << "All realtime resource lifetime tests passed\n";
    return EXIT_SUCCESS;
}
