#include "telemetry/telemetry_history.hpp"

#include <utility>

namespace gtg::telemetry {

void History::AddSample(Sample sample) {
    const std::uint64_t now = sample.monotonic_ms;
    samples_.push_back(std::move(sample));
    const std::uint64_t oldest = now > kRetainedDurationMs ? now - kRetainedDurationMs : 0;
    while (!samples_.empty() && samples_.front().monotonic_ms < oldest) {
        samples_.pop_front();
    }
}

const Sample* History::Latest() const noexcept {
    return samples_.empty() ? nullptr : &samples_.back();
}

std::size_t History::CountInRange(const std::uint64_t start_ms,
                                  const std::uint64_t end_ms) const noexcept {
    std::size_t count = 0;
    for (const auto& sample : samples_) {
        if (sample.monotonic_ms >= start_ms && sample.monotonic_ms <= end_ms) {
            ++count;
        }
    }
    return count;
}

}  // namespace gtg::telemetry
