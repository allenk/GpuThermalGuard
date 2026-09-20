#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>

#include "fps/fps_rate.hpp"

namespace gtg::fps {

struct HistorySample {
    std::uint64_t monotonic_ms{};
    // No value is a display-only lowest-waterline marker, never measured zero.
    std::optional<double> displayed_fps;
    Identity identity{};
    std::uint64_t surface{};
};

[[nodiscard]] inline bool CanConnectHistorySamples(
    const HistorySample& older, const HistorySample& newer) noexcept {
    return older.displayed_fps && newer.displayed_fps &&
        older.identity == newer.identity && older.surface == newer.surface &&
        newer.monotonic_ms > older.monotonic_ms &&
        newer.monotonic_ms - older.monotonic_ms <= 1'000;
}

// Presentation-only strokes. Dim strokes bridge unknown intervals but never
// modify samples or imply that an intermediate FPS was measured.
template <class Emit>
void ForEachHistoryStroke(const std::deque<HistorySample>& samples,
                          std::uint64_t start_ms, std::uint64_t end_ms,
                          Emit&& emit) {
    const HistorySample* previous_valid = nullptr;
    const HistorySample* trailing_missing = nullptr;
    bool had_missing = false;
    for (const auto& sample : samples) {
        if (sample.monotonic_ms < start_ms || sample.monotonic_ms > end_ms) continue;
        if (!sample.displayed_fps) {
            if (previous_valid != nullptr) {
                had_missing = true;
                trailing_missing = &sample;
            }
            continue;
        }
        if (previous_valid != nullptr &&
            sample.monotonic_ms > previous_valid->monotonic_ms) {
            emit(*previous_valid, sample,
                 had_missing || !CanConnectHistorySamples(*previous_valid, sample));
        }
        previous_valid = &sample;
        trailing_missing = nullptr;
        had_missing = false;
    }
    if (previous_valid != nullptr && trailing_missing != nullptr &&
        trailing_missing->monotonic_ms > previous_valid->monotonic_ms)
        emit(*previous_valid, *trailing_missing, true);
}

// Tray/presentation-only history. Never accessed by the protection worker.
class History final {
public:
    static constexpr std::uint64_t kRetainedDurationMs = 60ULL * 60ULL * 1000ULL;
    static constexpr std::size_t kMaxSamples = 7'201;  // 2 Hz for one hour.

    void Record(std::uint64_t monotonic_ms, std::optional<Identity> foreground,
                const Snapshot& snapshot) {
        if (!samples_.empty() && monotonic_ms < samples_.back().monotonic_ms) return;

        HistorySample sample{};
        sample.monotonic_ms = monotonic_ms;
        if (foreground && foreground->pid != 0 && foreground->creation_time != 0) {
            sample.identity = *foreground;
            if (snapshot.identity == *foreground) sample.surface = snapshot.surface;
        }
        if (foreground && foreground->pid != 0 &&
            foreground->creation_time != 0 &&
            snapshot.status == Status::Ready &&
            snapshot.identity == *foreground &&
            std::isfinite(snapshot.displayed_fps) &&
            snapshot.displayed_fps >= 0.0) {
            sample.displayed_fps = snapshot.displayed_fps;
        }

        if (!samples_.empty() && monotonic_ms == samples_.back().monotonic_ms) {
            samples_.back() = sample;
        } else {
            samples_.push_back(sample);
        }
        while (!samples_.empty() &&
               (monotonic_ms - samples_.front().monotonic_ms >
                    kRetainedDurationMs ||
                samples_.size() > kMaxSamples)) {
            samples_.pop_front();
        }
    }

    void Clear() noexcept { samples_.clear(); }

    [[nodiscard]] const std::deque<HistorySample>& Samples() const noexcept {
        return samples_;
    }

private:
    std::deque<HistorySample> samples_;
};

}  // namespace gtg::fps
