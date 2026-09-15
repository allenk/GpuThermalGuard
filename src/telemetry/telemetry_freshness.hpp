#pragma once

#include <algorithm>
#include <cstdint>
#include <deque>
#include <optional>
#include <vector>

#include "telemetry/telemetry_history.hpp"

namespace gtg::telemetry {

enum class FreshnessState {
    NoData,
    Fresh,
    Delayed,
    Unavailable,
};

struct Freshness {
    FreshnessState state{FreshnessState::NoData};
    std::uint64_t age_ms{};
    std::optional<std::uint64_t> latest_valid_ms;
};

struct PresentationGap {
    std::uint64_t start_ms{};
    std::uint64_t end_ms{};
    std::uint64_t duration_ms{};
};

inline constexpr std::uint64_t kPresentationDelayedMs = 1'000;
inline constexpr std::uint64_t kPresentationUnavailableMs = 3'000;
inline constexpr std::uint64_t kPresentationGapMs = 1'000;

[[nodiscard]] inline bool HasGpuPresentationData(const Sample& sample) noexcept {
    return sample.temperature_c.has_value();
}

[[nodiscard]] inline Freshness EvaluateFreshness(
    const std::deque<Sample>& samples,
    const std::uint64_t now_ms,
    const std::uint64_t delayed_ms = kPresentationDelayedMs,
    const std::uint64_t unavailable_ms = kPresentationUnavailableMs) noexcept {
    for (auto it = samples.rbegin(); it != samples.rend(); ++it) {
        if (!HasGpuPresentationData(*it)) continue;
        const std::uint64_t age = now_ms >= it->monotonic_ms
            ? now_ms - it->monotonic_ms : 0;
        FreshnessState state = FreshnessState::Fresh;
        if (age >= unavailable_ms) {
            state = FreshnessState::Unavailable;
        } else if (age >= delayed_ms) {
            state = FreshnessState::Delayed;
        }
        return {state, age, it->monotonic_ms};
    }
    return {FreshnessState::NoData, 0, std::nullopt};
}

[[nodiscard]] inline std::vector<PresentationGap> FindPresentationGaps(
    const std::deque<Sample>& samples,
    const std::uint64_t visible_start_ms,
    const std::uint64_t visible_end_ms,
    const std::uint64_t gap_ms = kPresentationGapMs) {
    std::vector<PresentationGap> gaps;
    std::optional<std::uint64_t> previous_valid_ms;
    for (const auto& sample : samples) {
        if (!HasGpuPresentationData(sample)) continue;
        if (previous_valid_ms && sample.monotonic_ms > *previous_valid_ms &&
            sample.monotonic_ms - *previous_valid_ms > gap_ms) {
            const std::uint64_t clipped_start =
                std::max(*previous_valid_ms, visible_start_ms);
            const std::uint64_t clipped_end =
                std::min(sample.monotonic_ms, visible_end_ms);
            if (clipped_start < clipped_end) {
                gaps.push_back({clipped_start, clipped_end,
                                sample.monotonic_ms - *previous_valid_ms});
            }
        }
        previous_valid_ms = sample.monotonic_ms;
    }
    return gaps;
}

}  // namespace gtg::telemetry
