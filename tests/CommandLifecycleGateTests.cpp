#include "vitadaw/audio/CommandLifecycleGate.h"

#include <cstdlib>
#include <iostream>
#include <semaphore>
#include <string_view>
#include <thread>

namespace {

void check(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

void makeOperational(vitadaw::audio::CommandLifecycleGate& gate) {
    static_cast<void>(gate.close(
        vitadaw::audio::DeviceProcessingState::initializing));
    gate.consumerStarted();
    check(gate.state() == vitadaw::audio::DeviceProcessingState::operational,
          "consumer confirmation should open a new generation");
}

} // namespace

int main() {
    using namespace vitadaw::audio;

    {
        CommandLifecycleGate gate;
        makeOperational(gate);
        const auto closure = gate.close(DeviceProcessingState::stopped);
        check(!gate.tryClaim().active && closure.cancellationWatermark == 0,
              "closure before claim must reject without reserving a sequence");
    }

    {
        // Exact former counterexample: lifecycle closes after the producer has
        // claimed the old generation but before it reserves a sequence.
        CommandLifecycleGate gate;
        makeOperational(gate);
        std::binary_semaphore claimed{0};
        std::binary_semaphore lifecycleClosed{0};
        AudioCommandSequence sequence{};
        bool accepted{};
        std::thread producer([&] {
            const auto claim = gate.tryClaim();
            check(claim.active, "producer should claim the operational generation");
            claimed.release();
            lifecycleClosed.acquire();
            sequence = gate.reserveSequence(claim);
            accepted = gate.tryAccept(claim);
        });
        claimed.acquire();
        const auto closure = gate.close(DeviceProcessingState::stopped);
        lifecycleClosed.release();
        producer.join();
        check(sequence != 0 && !accepted &&
                  closure.cancellationWatermark < sequence,
              "late reservation may miss the watermark only when acceptance fails");
    }

    {
        CommandLifecycleGate gate;
        makeOperational(gate);
        std::binary_semaphore reserved{0};
        std::binary_semaphore lifecycleClosed{0};
        AudioCommandSequence sequence{};
        bool accepted{};
        std::thread producer([&] {
            const auto claim = gate.tryClaim();
            sequence = gate.reserveSequence(claim);
            reserved.release();
            lifecycleClosed.acquire();
            accepted = gate.tryAccept(claim);
        });
        reserved.acquire();
        const auto closure = gate.close(DeviceProcessingState::error);
        lifecycleClosed.release();
        producer.join();
        check(!accepted && closure.cancellationWatermark >= sequence,
              "closure between reservation and acceptance must reject the command");
    }

    {
        CommandLifecycleGate gate;
        makeOperational(gate);
        std::binary_semaphore acceptedPoint{0};
        std::binary_semaphore lifecycleMayClose{0};
        AudioCommandSequence sequence{};
        bool accepted{};
        std::thread producer([&] {
            const auto claim = gate.tryClaim();
            sequence = gate.reserveSequence(claim);
            accepted = gate.tryAccept(claim);
            acceptedPoint.release();
            lifecycleMayClose.acquire();
        });
        acceptedPoint.acquire();
        const auto closure = gate.close(DeviceProcessingState::stopped);
        lifecycleMayClose.release();
        producer.join();
        check(accepted && closure.cancellationWatermark >= sequence,
              "closure after acceptance must cancel through the accepted sequence");

        static_cast<void>(gate.close(DeviceProcessingState::initializing));
        gate.consumerStarted();
        const auto nextClaim = gate.tryClaim();
        const auto nextSequence = gate.reserveSequence(nextClaim);
        check(nextClaim.active && nextClaim.generation != 0 &&
                  nextSequence > sequence && gate.tryAccept(nextClaim),
              "a confirmed later generation should accept a later FIFO sequence");
    }

    {
        CommandLifecycleGate gate;
        makeOperational(gate);
        const auto fullQueueClaim = gate.tryClaim();
        gate.reject(fullQueueClaim);
        const auto nextClaim = gate.tryClaim();
        const auto firstSequence = gate.reserveSequence(nextClaim);
        check(firstSequence == 1 && gate.tryAccept(nextClaim),
              "queue-full rejection must not reserve an unresolved sequence");
    }

    std::cout << "All command lifecycle gate tests passed\n";
    return EXIT_SUCCESS;
}
