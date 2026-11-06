#pragma once

#include <cstdint>

namespace gtg::topapp {

// What a sweep may cost, and what the sampler does when one costs more.
//
// The cost is the sampler thread's own CPU time, not the time on the clock.
// The thread runs at background priority, so on a saturated machine most of a
// sweep's clock time is spent waiting behind other work; timed by the clock,
// the guard switched the sampler off exactly when CPU was at 100 % -- the load
// it exists to name -- and, stopped until every alert ended, it never came back
// while that load lasted (2026-10-21).
//
// Over budget, the interval doubles up to 8 s; five sweeps in a row over it and
// the sampler pauses for a minute and tries again. That bounds a real overrun
// to five costly sweeps a minute without giving up on a long alert.
class SweepBudget {
public:
    // About three times the measured sweep.
    static constexpr double kBudgetMs = 50.0;
    static constexpr int kOverLimit = 5;
    static constexpr std::uint32_t kIntervalMs = 1'000;
    static constexpr std::uint32_t kMaximumIntervalMs = 8'000;
    static constexpr std::uint32_t kPauseMs = 60'000;

    struct Next {
        std::uint32_t wait_ms{kIntervalMs};
        // This sweep ended a run of overruns: wait out the pause.
        bool paused{};
        // The first pause since the alert began, which is the one worth a line
        // in the log; later ones in the same alert would repeat it every minute.
        bool first_pause{};
        // The first sweep within budget after a pause.
        bool resumed{};
    };

    // `cost_ms` is the sweep's own CPU time.
    [[nodiscard]] constexpr Next Charge(const double cost_ms) noexcept {
        Next next;
        if (cost_ms > kBudgetMs) {
            interval_ = interval_ * 2 < kMaximumIntervalMs ? interval_ * 2 : kMaximumIntervalMs;
            if (++over_ >= kOverLimit) {
                next.paused = true;
                next.first_pause = !paused_in_alert_;
                next.wait_ms = kPauseMs;
                paused_in_alert_ = true;
                after_pause_ = true;
                over_ = 0;
                interval_ = kIntervalMs;
                return next;
            }
            next.wait_ms = interval_;
            return next;
        }
        over_ = 0;
        interval_ = kIntervalMs;
        next.resumed = after_pause_;
        after_pause_ = false;
        next.wait_ms = interval_;
        return next;
    }

    // No record is in alert any more: the next alert starts afresh.
    constexpr void Reset() noexcept { *this = SweepBudget{}; }

private:
    std::uint32_t interval_{kIntervalMs};
    int over_{};
    bool paused_in_alert_{};
    bool after_pause_{};
};

}  // namespace gtg::topapp
