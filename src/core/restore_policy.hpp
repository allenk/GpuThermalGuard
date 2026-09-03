#pragma once

#include <cstdint>

#include "core/protection.hpp"

namespace gtg {

enum class RestoreInitiator {
    Manual,
    Automatic,
};

[[nodiscard]] constexpr bool ShouldAutomaticallyRestore(
    const bool auto_restore_enabled,
    const ProtectionState state) noexcept {
    return auto_restore_enabled && state == ProtectionState::ReadyToRestore;
}

[[nodiscard]] constexpr bool ShouldResetTriggerCount(
    const RestoreInitiator initiator) noexcept {
    return initiator == RestoreInitiator::Manual;
}

[[nodiscard]] constexpr bool HasNewTriggerCount(
    const std::uint32_t previous_count,
    const std::uint32_t current_count) noexcept {
    return current_count > previous_count;
}

}  // namespace gtg
