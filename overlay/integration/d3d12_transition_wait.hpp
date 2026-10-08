#pragma once
#include <windows.h>
#include <cstdint>
#include <limits>

namespace gtg::overlay::integration {
enum class DrainResult { Complete, Timeout, Removed, NotifyFailed, WaitFailed, Unproven };
inline constexpr DWORD kTransitionWaitMs = 250;

// Transition-only policy. Ops owns a persistent event and a private GPU fence;
// a failed/uncertain notification must not be destroyed or reused afterward.
template <class Ops>
DrainResult DrainPrivateFence(Ops& ops, std::uint64_t target, DWORD budget) noexcept {
    constexpr auto removed = std::numeric_limits<std::uint64_t>::max();
    const auto start = ops.Now();
    auto completed = ops.Completed();
    if (completed == removed) return DrainResult::Removed;
    if (target == removed) return DrainResult::Unproven;
    if (completed >= target) return DrainResult::Complete;
    if (budget > kTransitionWaitMs) budget = kTransitionWaitMs;
    auto elapsed = ops.Now() - start;
    if (elapsed >= budget) return DrainResult::Timeout;
    if (FAILED(ops.Notify(target))) return DrainResult::NotifyFailed;
    elapsed = ops.Now() - start;
    if (elapsed >= budget) return DrainResult::Timeout;
    const DWORD result = ops.Wait(static_cast<DWORD>(budget - elapsed));
    if (result == WAIT_TIMEOUT) return DrainResult::Timeout;
    if (result != WAIT_OBJECT_0) return DrainResult::WaitFailed;
    completed = ops.Completed();
    if (completed == removed) return DrainResult::Removed;
    return completed >= target ? DrainResult::Complete : DrainResult::Unproven;
}
}  // namespace gtg::overlay::integration
