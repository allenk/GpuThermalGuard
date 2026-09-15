#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

namespace gtg::timing {

struct DeadlineAdvance {
    std::int64_t next_deadline_ms{};
    std::uint64_t missed_periods{};
};

struct CadenceClassification {
    std::int64_t next_deadline_ticks{};
    std::uint64_t wake_missed_periods{};
    std::uint64_t execution_overrun_periods{};
    std::uint64_t total_missed_periods{};
};

[[nodiscard]] constexpr DeadlineAdvance AdvanceDeadline(
    const std::int64_t previous_deadline_ms,
    const std::int64_t completed_at_ms,
    const std::int64_t period_ms) noexcept {
    if (period_ms <= 0) return {completed_at_ms, 0};

    std::int64_t next = previous_deadline_ms + period_ms;
    std::uint64_t missed = 0;
    if (next <= completed_at_ms) {
        const auto elapsed_periods =
            static_cast<std::uint64_t>((completed_at_ms - next) / period_ms) + 1;
        next += static_cast<std::int64_t>(elapsed_periods) * period_ms;
        missed = elapsed_periods;
    }
    return {next, missed};
}

[[nodiscard]] constexpr CadenceClassification ClassifyCadence(
    const std::int64_t scheduled_deadline_ticks,
    const std::int64_t woke_at_ticks,
    const std::int64_t completed_at_ticks,
    const std::int64_t period_ticks) noexcept {
    // A command/event may wake the protection worker before its sampling
    // timer. Process that command without consuming the pending cadence slot.
    if (completed_at_ticks < scheduled_deadline_ticks) {
        return {scheduled_deadline_ticks, 0, 0, 0};
    }
    const DeadlineAdvance wake = AdvanceDeadline(
        scheduled_deadline_ticks, woke_at_ticks, period_ticks);
    const DeadlineAdvance completed = AdvanceDeadline(
        scheduled_deadline_ticks, completed_at_ticks, period_ticks);
    const std::uint64_t wake_missed =
        std::min(wake.missed_periods, completed.missed_periods);
    return {
        completed.next_deadline_ms,
        wake_missed,
        completed.missed_periods - wake_missed,
        completed.missed_periods,
    };
}

struct LatencySummary {
    std::size_t count{};
    std::int64_t minimum_us{};
    std::int64_t p50_us{};
    std::int64_t p95_us{};
    std::int64_t p99_us{};
    std::int64_t maximum_us{};
};

template <std::size_t Capacity>
class LatencyWindow final {
    static_assert(Capacity > 0);

public:
    void Add(const std::int64_t microseconds) noexcept {
        values_[write_index_] = std::max<std::int64_t>(0, microseconds);
        write_index_ = (write_index_ + 1) % Capacity;
        count_ = std::min(count_ + 1, Capacity);
    }

    [[nodiscard]] LatencySummary Summary() const noexcept {
        LatencySummary result;
        result.count = count_;
        if (count_ == 0) return result;

        std::array<std::int64_t, Capacity> sorted{};
        std::copy_n(values_.begin(), count_, sorted.begin());
        std::sort(sorted.begin(), sorted.begin() + static_cast<std::ptrdiff_t>(count_));
        const auto percentile = [&](const std::size_t numerator) {
            const std::size_t rank = (numerator * count_ + 99) / 100;
            return sorted[std::max<std::size_t>(1, rank) - 1];
        };
        result.minimum_us = sorted[0];
        result.p50_us = percentile(50);
        result.p95_us = percentile(95);
        result.p99_us = percentile(99);
        result.maximum_us = sorted[count_ - 1];
        return result;
    }

private:
    std::array<std::int64_t, Capacity> values_{};
    std::size_t write_index_{};
    std::size_t count_{};
};

}  // namespace gtg::timing
