#pragma once

#include <cstdint>
#include <deque>
#include <optional>

#include "net/throughput.hpp"

// Network throughput over the retained window.
//
// Its own ring, like RAM's and FPS's, and for the same reason: the 200 ms
// thermal ring is the protection path's record and nothing presentational
// belongs in it.
namespace gtg::net {

struct HistorySample {
    std::uint64_t monotonic_ms{};
    // Bytes per second, as floats. Absent means the reading was refused --
    // an adapter that reset, a gap too long to average over -- and a gap in
    // the curve is the honest drawing of that.
    std::optional<float> received_bytes_per_second;
    std::optional<float> sent_bytes_per_second;
};

class History final {
public:
    static constexpr std::uint64_t kRetainedDurationMs = 60ULL * 60ULL * 1000ULL;
    // Sampled on the same 200 ms tick as RAM, so its curve begins at the same
    // instant as the records drawn beside it rather than visibly later.
    static constexpr std::size_t kMaxSamples = 18'001;

    // Takes counters rather than rates, because a rate needs the previous
    // reading and this is the only place that has it. A refused difference is
    // stored as an absent sample rather than dropped: the reader should see
    // that the link stopped reporting, not a curve that quietly joins across
    // the hole.
    void Record(const std::uint64_t monotonic_ms, const Counters& counters) {
        if (!samples_.empty() && monotonic_ms < samples_.back().monotonic_ms) return;

        HistorySample sample{};
        sample.monotonic_ms = monotonic_ms;
        if (previous_) {
            if (const auto rate = RateBetween(*previous_, counters)) {
                sample.received_bytes_per_second =
                    static_cast<float>(rate->received_bytes_per_second);
                sample.sent_bytes_per_second =
                    static_cast<float>(rate->sent_bytes_per_second);
            }
        }
        previous_ = counters;

        if (!samples_.empty() && monotonic_ms == samples_.back().monotonic_ms) {
            samples_.back() = sample;
        } else {
            samples_.push_back(sample);
        }
        while (!samples_.empty() &&
               (monotonic_ms - samples_.front().monotonic_ms > kRetainedDurationMs ||
                samples_.size() > kMaxSamples)) {
            samples_.pop_front();
        }
    }

    // The adapter went away. The next counter reading belongs to a different
    // interface, so the difference across the gap would be a number no link
    // ever carried; forgetting the previous reading is what prevents it.
    void Interrupt() noexcept { previous_.reset(); }

    void Clear() noexcept {
        samples_.clear();
        previous_.reset();
    }

    [[nodiscard]] const std::deque<HistorySample>& Samples() const noexcept {
        return samples_;
    }

    // The largest rate in either direction over the window, which is what the
    // unit and both curves are scaled by. Zero when nothing has been recorded,
    // which reads as bytes per second and draws flat -- both correct.
    [[nodiscard]] double PeakWithin(const std::uint64_t start,
                                    const std::uint64_t end) const noexcept {
        double peak = 0.0;
        for (auto it = samples_.rbegin(); it != samples_.rend(); ++it) {
            if (it->monotonic_ms < start) break;
            if (it->monotonic_ms > end) continue;
            if (it->received_bytes_per_second &&
                *it->received_bytes_per_second > peak)
                peak = *it->received_bytes_per_second;
            if (it->sent_bytes_per_second && *it->sent_bytes_per_second > peak)
                peak = *it->sent_bytes_per_second;
        }
        return peak;
    }

    [[nodiscard]] const HistorySample* Latest() const noexcept {
        return samples_.empty() ? nullptr : &samples_.back();
    }

private:
    std::deque<HistorySample> samples_;
    std::optional<Counters> previous_;
};

}  // namespace gtg::net
