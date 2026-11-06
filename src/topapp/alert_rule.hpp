#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>

namespace gtg::topapp {

// When a record is "in alert": its load has stayed high long enough that
// naming the app behind it is worth the reader's attention (owner,
// 2026-10-16). Fed the record's own reading on the presentation tick, the
// same number the cell shows, so the alert can never disagree with the value
// printed beside it.
struct Thresholds {
    double enter{};
    double leave{};
};

// GPU and VRAM name their app from 70 % (owner, 2026-10-16); leave sits a
// hysteresis band below. VRAM moves slowly, so its band is narrower.
inline constexpr Thresholds kGpuThresholds{70.0, 55.0};
inline constexpr Thresholds kVramThresholds{70.0, 60.0};
inline constexpr Thresholds kCpuThresholds{80.0, 60.0};
inline constexpr Thresholds kRamThresholds{85.0, 75.0};

class AlertRule {
public:
    // Eight seconds of sustained load before the search starts. The owner
    // set it: this is not a real-time instrument, and a three-second rule
    // would name whatever happened to load a level.
    static constexpr std::uint64_t kEnterWindowMs = 8'000;
    static constexpr std::uint64_t kLeaveWindowMs = 10'000;
    // A record whose data has been missing this long has no business keeping
    // a name on screen.
    static constexpr std::uint64_t kStaleMs = 3'000;
    // Valid readings further apart than this are not one continuous run. Five
    // ticks of the 200 ms presentation timer.
    static constexpr std::uint64_t kMaxGapMs = 1'000;
    // A window counts as covered when its oldest reading is within this much
    // of the window's start: the tick does not land on exact multiples. Kept
    // well under one tick, so eight seconds does not become 7.8.
    static constexpr std::uint64_t kCoverageSlackMs = 100;
    // Bounds the memory whatever the caller's cadence. Ten seconds of 200 ms
    // ticks is 50 readings.
    static constexpr std::size_t kMaxSamples = 256;

    explicit AlertRule(const Thresholds thresholds) noexcept : thresholds_(thresholds) {}

    // `value` is the record's reading at `now_ms`, or nothing when the record
    // has no valid reading. Returns whether the record is in alert.
    bool Observe(const std::uint64_t now_ms, const std::optional<double> value) {
        if (has_valid_ && now_ms < last_valid_ms_) Reset();
        // A silence longer than the stale limit ends an alert even when no
        // missing reading was reported in between -- a stalled caller says
        // nothing at all.
        if (has_valid_ && now_ms - last_valid_ms_ >= kStaleMs) Reset();

        if (!value || !std::isfinite(*value)) {
            // Entry needs an unbroken run, so a missing reading restarts it.
            // An alert already up survives a short hole: the stale rule above
            // takes it down if the hole lasts.
            if (!active_) samples_.clear();
            return active_;
        }
        if (!active_ && has_valid_ && now_ms - last_valid_ms_ > kMaxGapMs) samples_.clear();

        samples_.push_back({now_ms, *value});
        last_valid_ms_ = now_ms;
        has_valid_ = true;
        while (!samples_.empty() && samples_.front().ms + kLeaveWindowMs < now_ms) {
            samples_.pop_front();
        }
        while (samples_.size() > kMaxSamples)
            samples_.pop_front();

        if (!active_) {
            if (Covers(now_ms, kEnterWindowMs)) {
                const Summary s = Summarize(now_ms - kEnterWindowMs);
                // The mean decides, and the minimum vetoes: utilisation
                // jitters, so every reading above the threshold would rarely
                // hold for eight seconds of a real game, but a dip below the
                // leave line means the load really did let go.
                active_ = s.mean >= thresholds_.enter && s.minimum >= thresholds_.leave;
            }
        } else if (Covers(now_ms, kLeaveWindowMs)) {
            const Summary s = Summarize(now_ms - kLeaveWindowMs);
            // The mirror image: the mean decides, and one reading back at the
            // enter level vetoes. A hard drop therefore leaves exactly ten
            // seconds later; a mean alone would leave after about three.
            if (s.mean < thresholds_.leave && s.maximum < thresholds_.enter) active_ = false;
        }
        return active_;
    }

    [[nodiscard]] bool Active() const noexcept { return active_; }

    void Reset() noexcept {
        samples_.clear();
        active_ = false;
        has_valid_ = false;
        last_valid_ms_ = 0;
    }

private:
    struct Sample {
        std::uint64_t ms{};
        double value{};
    };

    struct Summary {
        double mean{};
        double minimum{};
        double maximum{};
    };

    [[nodiscard]] bool Covers(const std::uint64_t now_ms,
                              const std::uint64_t window_ms) const noexcept {
        return !samples_.empty() && now_ms >= window_ms &&
               samples_.front().ms <= now_ms - window_ms + kCoverageSlackMs;
    }

    // Over the readings at or after `from_ms`; Covers() has already said there
    // is at least one.
    [[nodiscard]] Summary Summarize(const std::uint64_t from_ms) const noexcept {
        Summary s{0.0, INFINITY, -INFINITY};
        std::size_t count = 0;
        for (const Sample& sample : samples_) {
            if (sample.ms < from_ms) continue;
            s.mean += sample.value;
            s.minimum = sample.value < s.minimum ? sample.value : s.minimum;
            s.maximum = sample.value > s.maximum ? sample.value : s.maximum;
            ++count;
        }
        if (count == 0) return {0.0, 0.0, 0.0};
        s.mean /= static_cast<double>(count);
        return s;
    }

    Thresholds thresholds_;
    std::deque<Sample> samples_;
    std::uint64_t last_valid_ms_{};
    bool has_valid_{};
    bool active_{};
};

}  // namespace gtg::topapp
