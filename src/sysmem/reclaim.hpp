#pragma once

#include <cstdint>

// Host memory reclaim policy and timing.
//
// Pure arithmetic only: no Win32, no handles, no threads. The Windows calls
// that actually trim a working set live in the tray layer; everything that
// decides *whether* to trim, *what* to skip and *how long* the reader sees a
// busy indicator lives here, so it can be tested exhaustively without a
// desktop. This is the same split that keeps `osd_compact.hpp` testable, and
// the same split that a future portable UI layer would need.
namespace gtg::sysmem::reclaim {

// ---------------------------------------------------------------------------
// What to trim.
//
// Trimming a process working set is documented and reversible -- the pages
// return on demand -- but it is not free: the next time that process runs it
// pays the faults back. So the policy is deliberately conservative. The point
// is to reclaim memory held by things the reader is *not* using, at a moment
// the reader chose, rather than to drive a number down.
// ---------------------------------------------------------------------------

struct Policy {
    // Below this, trimming costs a syscall and an eventual fault storm to
    // reclaim an amount that does not matter. Measured on the owner's machine:
    // 147 of 560 accessible processes hold at least 32 MiB, and together they
    // account for 23.58 GiB of 27.08 GiB total. The long tail is not worth it.
    std::uint64_t minimum_working_set_bytes{32ull * 1024 * 1024};
    // The foreground window's process is the one the reader is looking at --
    // usually the game that prompted this. Trimming it would cause exactly the
    // stutter the reader is trying to avoid.
    bool skip_foreground{true};
    // Walk and decide, but issue no trim and no cache flush. It exists so the
    // window lifecycle can be exercised without modifying the machine running
    // the test, and it doubles as an honest "what would this do" mode.
    bool dry_run{false};
};

struct Candidate {
    std::uint32_t pid{};
    std::uint64_t working_set_bytes{};
    bool is_self{};              // GpuThermalGuard itself
    bool is_foreground{};        // owns the foreground window
    bool is_system_critical{};   // pid 0 and 4, and anything we must not touch
};

// Never trim ourselves: the overlay is mid-render and would fault its own
// bitmaps straight back in. Never trim pid 0 or 4.
[[nodiscard]] constexpr bool ShouldTrim(const Candidate& candidate,
                                        const Policy& policy) noexcept {
    if (candidate.is_self) return false;
    if (candidate.is_system_critical) return false;
    if (policy.skip_foreground && candidate.is_foreground) return false;
    return candidate.working_set_bytes >= policy.minimum_working_set_bytes;
}

// ---------------------------------------------------------------------------
// How long the busy indicator stays up.
//
// The work can finish faster than a reader can perceive. An indicator that
// appears and vanishes inside one frame reads as a glitch, or as nothing
// having happened at all, which invites a second click. So the indicator has a
// floor: it stays until the work is done *or* the floor has elapsed, whichever
// is later. Owner's requirement, 2026-09-23: 「無論如何，會有個最短動畫區間，
// 好讓動畫效果執行完畢。」
// ---------------------------------------------------------------------------

inline constexpr std::uint64_t kMinimumVisibleMs = 700;

// A hard ceiling on the worker. EmptyWorkingSet against a process that is
// itself paging heavily can take an unbounded time to return. The indicator is
// a topmost window that swallows input over its cell, so a worker that never
// finishes would leave a permanent dead spot on the reader's screen. That is
// the worst failure this feature can have, and the ceiling is what prevents
// it: when the budget is spent the remaining targets are abandoned, not
// waited for.
inline constexpr std::uint64_t kWorkBudgetMs = 5000;

[[nodiscard]] constexpr std::uint64_t VisibleUntilMs(
    std::uint64_t started_ms, std::uint64_t work_finished_ms,
    std::uint64_t minimum_ms = kMinimumVisibleMs) noexcept {
    const std::uint64_t floor_ms = started_ms + minimum_ms;
    // A clock that went backwards, or work that reports finishing before it
    // started, must not produce a window that is already overdue.
    if (work_finished_ms < started_ms) return floor_ms;
    return work_finished_ms > floor_ms ? work_finished_ms : floor_ms;
}

[[nodiscard]] constexpr bool StillVisible(std::uint64_t now_ms,
                                          std::uint64_t until_ms) noexcept {
    return now_ms < until_ms;
}

[[nodiscard]] constexpr bool BudgetSpent(
    std::uint64_t started_ms, std::uint64_t now_ms,
    std::uint64_t budget_ms = kWorkBudgetMs) noexcept {
    if (now_ms <= started_ms) return false;
    return now_ms - started_ms >= budget_ms;
}

// ---------------------------------------------------------------------------
// What happened.
// ---------------------------------------------------------------------------

struct Outcome {
    int considered{};
    int trimmed{};       // actually trimmed; zero in a dry run, never inflated
    int would_trim{};    // accepted by the policy but deliberately not trimmed
    int failed{};      // opened but the call refused; expected and not an error
    int skipped{};     // policy said no
    bool timed_out{};
    bool cache_flushed{};
    std::uint64_t available_before_bytes{};
    std::uint64_t available_after_bytes{};
};

// Signed on purpose. Available memory can fall across the operation if other
// processes allocate while it runs, and reporting that honestly is better than
// clamping to zero and implying the work always helps.
[[nodiscard]] constexpr std::int64_t FreedBytes(const Outcome& outcome) noexcept {
    return static_cast<std::int64_t>(outcome.available_after_bytes) -
           static_cast<std::int64_t>(outcome.available_before_bytes);
}

[[nodiscard]] constexpr double FreedGib(const Outcome& outcome) noexcept {
    return static_cast<double>(FreedBytes(outcome)) / 1073741824.0;
}

// True when the operation did something worth telling the reader about.
// A run that trimmed nothing is not a failure -- it means there was nothing
// worth trimming -- but it should not claim a result either.
[[nodiscard]] constexpr bool Reportable(const Outcome& outcome) noexcept {
    // A dry run is never reportable: it changed nothing, and saying otherwise
    // would be the same lie as counting its targets as trimmed.
    return outcome.trimmed > 0 || outcome.cache_flushed;
}

// ---------------------------------------------------------------------------
// Telling the reader what happened.
//
// The constraint that shapes all of this: the reader is in a game. Nothing may
// pop up, steal focus, or interrupt. So the result is shown where the request
// was made -- in the cell itself, briefly -- and then the cell goes back to
// being a cell. Owner, 2026-09-23: 「不能卡住用戶的體驗」.
// ---------------------------------------------------------------------------

// How long the figure stays up after the work is done. Long enough to read a
// short number at a glance, short enough not to hide a live reading during a
// game.
inline constexpr std::uint64_t kResultHoldMs = 1600;

// Below this the delta is noise: other processes allocate and release while
// the operation runs, and a number that small says nothing about whether the
// work helped. Claiming "+0.0 GB" would be worse than claiming nothing.
inline constexpr std::int64_t kMeaningfulFreedBytes = 64ll * 1024 * 1024;

// How many decimals the freed figure carries.
//
// Measured: the cell's text box is 64 dip and "+3.9 GB" at the result font is
// 63.0, so a single extra digit truncates. "+10.0 GB" is 71.9 and does not
// fit at all.
//
// Dropping the decimal above ten is not only shorter, it is more honest. Other
// processes allocate and release while the operation runs, so the figure's own
// uncertainty at that magnitude is far larger than a tenth of a gigabyte.
// Keeping it would be precision the measurement does not have.
[[nodiscard]] constexpr int FreedDecimals(double gib) noexcept {
    return (gib > -10.0 && gib < 10.0) ? 1 : 0;
}

enum class ResultTone {
    Gain,      // memory was freed, and enough of it to mean something
    Neutral,   // nothing measurable changed
    Loss,      // less is available than before; said plainly, not hidden
};

[[nodiscard]] constexpr ResultTone ToneOf(const Outcome& outcome) noexcept {
    const std::int64_t freed = FreedBytes(outcome);
    if (freed >= kMeaningfulFreedBytes) return ResultTone::Gain;
    if (freed <= -kMeaningfulFreedBytes) return ResultTone::Loss;
    return ResultTone::Neutral;
}

// A dry run changed nothing, so it has no result to show whatever the numbers
// happen to say.
[[nodiscard]] constexpr bool HasResultToShow(const Outcome& outcome,
                                             bool dry_run) noexcept {
    return !dry_run && Reportable(outcome);
}

}  // namespace gtg::sysmem::reclaim
