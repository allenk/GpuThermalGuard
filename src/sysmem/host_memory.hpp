#pragma once

// Host memory readings for the presentation layer only.
//
// This unit is auxiliary work and yields to GPU thermal telemetry: it is
// sampled on the Tray message-loop thread, shares no lock, queue, handle or
// NVML session with the protection worker, and allocates nothing in steady
// state. A failed reading is a gap, never a measured zero, and can never reach
// a protection decision, a latch, or a "protected" claim.
//
// The arithmetic is deliberately separated from the operating system so it can
// be tested without one.

#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>

#include <windows.h>

namespace gtg::sysmem {

// No value means unavailable. It never means zero.
struct Usage {
    std::optional<double> physical_percent;
    std::optional<double> physical_used_gib;
    std::optional<double> commit_percent;
    std::optional<double> commit_used_gib;
};

namespace detail {

inline constexpr double kBytesPerGiB = 1024.0 * 1024.0 * 1024.0;

// A total of zero cannot be divided, and an available figure above the total is
// an inconsistent report rather than a negative usage. Both are unavailable.
constexpr bool Usable(const std::uint64_t total,
                      const std::uint64_t available) noexcept {
    return total != 0 && available <= total;
}

constexpr std::optional<double> Percent(const std::uint64_t total,
                                        const std::uint64_t available) noexcept {
    if (!Usable(total, available)) return std::nullopt;
    return static_cast<double>(total - available) * 100.0 /
           static_cast<double>(total);
}

constexpr std::optional<double> UsedGib(const std::uint64_t total,
                                        const std::uint64_t available) noexcept {
    if (!Usable(total, available)) return std::nullopt;
    return static_cast<double>(total - available) / kBytesPerGiB;
}

}  // namespace detail

// `total_commit` is the commit LIMIT: physical memory plus the page file. Its
// denominator is therefore larger than physical total, so the commit
// percentage normally reads below the physical one. A system-managed page file
// can grow the limit and lower the percentage with nothing released; that is
// reported as it happens and is never smoothed away.
[[nodiscard]] constexpr Usage Compute(const std::uint64_t total_phys,
                                      const std::uint64_t avail_phys,
                                      const std::uint64_t total_commit,
                                      const std::uint64_t avail_commit) noexcept {
    return Usage{detail::Percent(total_phys, avail_phys),
                 detail::UsedGib(total_phys, avail_phys),
                 detail::Percent(total_commit, avail_commit),
                 detail::UsedGib(total_commit, avail_commit)};
}

// One `GlobalMemoryStatusEx` call from kernel32. No additional link library,
// service, thread or counter provider is introduced.
[[nodiscard]] inline Usage Query() noexcept {
    MEMORYSTATUSEX status{};
    status.dwLength = sizeof(status);
    if (GlobalMemoryStatusEx(&status) == 0) return Usage{};
    return Compute(status.ullTotalPhys, status.ullAvailPhys,
                   status.ullTotalPageFile, status.ullAvailPageFile);
}

struct HistorySample {
    std::uint64_t monotonic_ms{};
    // Percentages only. The GiB figures are a live readout and are not stored.
    std::optional<float> physical_percent;
    std::optional<float> commit_percent;
};

// Tray/presentation-only history, mirroring fps::History. Never accessed by
// the protection worker.
class History final {
public:
    static constexpr std::uint64_t kRetainedDurationMs = 60ULL * 60ULL * 1000ULL;
    // Sampled on the same 200 ms presentation tick as the thermal telemetry,
    // not at the 2 Hz FPS cadence. RAM is drawn beside GPU % and CPU %, so a
    // sparser grid would make its curve visibly start later than theirs inside
    // the same window. One hour at 5 Hz costs about 430 KB.
    static constexpr std::size_t kMaxSamples = 18'001;

    void Record(const std::uint64_t monotonic_ms, const Usage& usage) {
        if (!samples_.empty() && monotonic_ms < samples_.back().monotonic_ms)
            return;

        HistorySample sample{};
        sample.monotonic_ms = monotonic_ms;
        if (usage.physical_percent)
            sample.physical_percent = static_cast<float>(*usage.physical_percent);
        if (usage.commit_percent)
            sample.commit_percent = static_cast<float>(*usage.commit_percent);

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

}  // namespace gtg::sysmem
