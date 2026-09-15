#pragma once

#include <cstdint>

#include "core/protection.hpp"

namespace gtg {

class RestoreRetryBudget {
public:
    void ObserveLatch(bool latched) noexcept {
        if (latched && !latched_) {
            ResetForManualRequest();
            ++episode_;
        }
        latched_ = latched;
    }
    void ResetForManualRequest() noexcept { attempts_ = 0; stopped_ = false; last_attempt_ms_ = 0; }
    bool CanAttempt(std::int64_t now_ms) const noexcept {
        return !exhausted() && (attempts_ == 0 || now_ms - last_attempt_ms_ >= 5'000);
    }
    void BeginAttempt(std::int64_t now_ms) noexcept { ++attempts_; last_attempt_ms_ = now_ms; }
    void StopRetrying() noexcept { stopped_ = true; }
    bool exhausted() const noexcept { return stopped_ || attempts_ >= 3; }
    unsigned int attempts() const noexcept { return attempts_; }
    std::uint64_t episode() const noexcept { return episode_; }
private:
    unsigned int attempts_{};
    std::int64_t last_attempt_ms_{};
    std::uint64_t episode_{};
    bool latched_{};
    bool stopped_{};
};

struct VerifiedRestoreResult {
    ProtectionDecision decision;
    bool attempted{};
    bool verified{};
};

// Check fresh eligibility BEFORE raising the hardware limit; commit the
// controller transition only after the supplied setter/readback succeeds.
template <typename Writer>
VerifiedRestoreResult RestoreWithVerification(ProtectionController& controller,
    const std::int64_t now_ms, const int temperature_c, Writer&& writer) {
    auto candidate = controller;
    const auto proposal = candidate.RequestRestore(now_ms, temperature_c);
    if (proposal.action != ProtectionAction::ApplyNormalPower) {
        controller = candidate;  // Includes revoking stale ReadyToRestore.
        return {proposal, false, false};
    }
    if (!writer()) {
        ProtectionDecision retained;
        retained.state = controller.state();
        retained.safe_latched = controller.safe_latched();
        return {retained, true, false};
    }
    controller = candidate;
    return {proposal, true, true};
}

template <typename Writer, typename SafeWriter>
VerifiedRestoreResult RestoreWithSafeFallback(ProtectionController& controller,
    const std::int64_t now_ms, const int temperature_c, bool& safe_verified,
    Writer&& writer, SafeWriter&& safe_writer) {
    const auto result = RestoreWithVerification(controller, now_ms, temperature_c, [&] {
        // A failed readback does not prove that the normal-power write failed.
        safe_verified = false;
        return writer();
    });
    if (result.attempted && !result.verified) {
        // One bounded attempt; periodic retries handle continued driver failure.
        safe_verified = safe_writer();
    }
    return result;
}

[[nodiscard]] constexpr bool ShouldAutomaticallyRestore(
    const bool auto_restore_enabled,
    const ProtectionState state) noexcept {
    return auto_restore_enabled && state == ProtectionState::ReadyToRestore;
}

[[nodiscard]] constexpr std::uint32_t TestRunTriggerCount(
    const std::uint32_t lifetime_total,
    const std::uint32_t baseline) noexcept {
    return lifetime_total >= baseline ? lifetime_total - baseline : lifetime_total;
}

[[nodiscard]] constexpr bool HasNewTriggerCount(
    const std::uint32_t previous_count,
    const std::uint32_t current_count) noexcept {
    return current_count > previous_count;
}

}  // namespace gtg
