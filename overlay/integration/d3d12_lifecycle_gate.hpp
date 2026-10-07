#pragma once
#include "d3d12_transition_wait.hpp"
#include <atomic>

namespace gtg::overlay::integration {
// Per-renderer admission, not a lock around original game API calls.
// Transition reservation is published before waiting for an active draw.
class D3d12LifecycleGate {
public:
    bool TryDraw() noexcept {
        if (disabled_.load(std::memory_order_acquire) ||
            transition_.load(std::memory_order_acquire) ||
            owner_.test_and_set(std::memory_order_acquire)) return false;
        if (disabled_.load(std::memory_order_acquire) ||
            transition_.load(std::memory_order_acquire)) {
            owner_.clear(std::memory_order_release);
            return false;
        }
        return true;
    }
    void EndDraw() noexcept { owner_.clear(std::memory_order_release); }
    // Only approved resize/color callers may wait here. Pass Remaining(clock) to the fence drain;
    // admission and GPU waiting share one deadline, not two250 ms budgets.
    template<class Clock> bool BeginTransition(Clock& clock, DWORD budget = kTransitionWaitMs) noexcept {
        if (disabled_.load(std::memory_order_acquire)) return false;
        const auto start = clock.Now();
        bool expected = false;
        if (!transition_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
            disabled_.store(true, std::memory_order_release);
            return false;
        }
        if (disabled_.load(std::memory_order_acquire)) return false;
        if (budget > kTransitionWaitMs) budget = kTransitionWaitMs;
        start_ = start;
        budget_ = budget;
        bool first_poll = true;
        for (;;) {
            if (disabled_.load(std::memory_order_acquire)) return false;
            if (!first_poll && clock.Now() - start_ >= budget) {
                disabled_.store(true, std::memory_order_release);
                return false;
            }
            first_poll = false;
            if (!owner_.test_and_set(std::memory_order_acquire)) {
                if (disabled_.load(std::memory_order_acquire)) {
                    owner_.clear(std::memory_order_release);
                    return false;
                }
                return true;
            }
            if (clock.Now() - start_ >= budget) {
                disabled_.store(true, std::memory_order_release);
                return false;
            }
            clock.Pause();
        }
    }
    template<class Clock> DWORD Remaining(Clock& clock) const noexcept {
        const auto elapsed = clock.Now() - start_;
        return elapsed < budget_ ? static_cast<DWORD>(budget_ - elapsed) : 0;
    }
    // Caller releases/retains resources and forwards original Resize first.
    // Commit only after successful native resize and full requalification.
    void EndTransition(bool qualified) noexcept {
        if (!qualified) disabled_.store(true, std::memory_order_release);
        owner_.clear(std::memory_order_release);
        transition_.store(false, std::memory_order_release);
    }
    bool Disabled() const noexcept { return disabled_.load(std::memory_order_acquire); }
private:
    std::atomic_flag owner_ = ATOMIC_FLAG_INIT;
    std::atomic<bool> transition_{false};
    std::atomic<bool> disabled_{false};
    // Read only by successfully admitted transition owner, using the same clock.
    std::uint64_t start_ = 0;
    DWORD budget_ = 0;
};
}  // namespace gtg::overlay::integration
