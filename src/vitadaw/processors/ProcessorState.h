#pragma once

#include <compare>
#include <cstdint>
#include <string>
#include <vector>

namespace vitadaw::processors {

struct ProcessorInstanceId {
    std::uint64_t value{};

    [[nodiscard]] constexpr bool isValid() const noexcept { return value != 0; }
    auto operator<=>(const ProcessorInstanceId&) const = default;
};

struct ParameterId {
    std::uint32_t value{};

    [[nodiscard]] constexpr bool isValid() const noexcept { return value != 0; }
    auto operator<=>(const ParameterId&) const = default;
};

inline constexpr ParameterId gainParameterId{1};
inline constexpr const char* internalGainProcessorType = "internal.gain";

struct ProcessorType {
    std::string identifier;

    [[nodiscard]] bool isValid() const noexcept { return !identifier.empty(); }
    bool operator==(const ProcessorType&) const = default;
};

struct ProcessorParameterState {
    ParameterId id;
    float value{};

    bool operator==(const ProcessorParameterState&) const = default;
};

struct ProcessorState {
    ProcessorInstanceId id;
    ProcessorType type;
    std::vector<ProcessorParameterState> parameters;
    std::vector<std::uint8_t> serializedState;
    bool bypassed{};

    bool operator==(const ProcessorState&) const = default;
};

struct InsertChain {
    std::vector<ProcessorState> processors;

    bool operator==(const InsertChain&) const = default;
};

} // namespace vitadaw::processors
