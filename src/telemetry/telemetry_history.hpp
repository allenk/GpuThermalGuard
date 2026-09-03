#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>

namespace gtg::telemetry {

struct Sample {
    std::uint64_t monotonic_ms{};
    std::optional<double> temperature_c;
    std::optional<double> power_w;
    std::optional<double> vram_utilization_percent;
    std::optional<double> vram_used_gib;
    std::optional<double> gpu_utilization_percent;
    std::optional<double> cpu_utilization_percent;
};

class History final {
public:
    static constexpr std::uint64_t kRetainedDurationMs = 60ULL * 60ULL * 1000ULL;

    void AddSample(Sample sample);

    [[nodiscard]] const std::deque<Sample>& Samples() const noexcept { return samples_; }
    [[nodiscard]] const Sample* Latest() const noexcept;
    [[nodiscard]] std::size_t CountInRange(std::uint64_t start_ms,
                                           std::uint64_t end_ms) const noexcept;

private:
    std::deque<Sample> samples_;
};

}  // namespace gtg::telemetry
