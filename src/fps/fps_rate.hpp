#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace gtg::fps {

struct Identity {
    std::uint32_t pid{};
    std::uint64_t creation_time{};

    [[nodiscard]] constexpr bool operator==(const Identity&) const noexcept = default;
};

enum class Status { Unavailable, Warmup, Ready };

struct Snapshot {
    Status status{Status::Unavailable};
    double displayed_fps{};
    Identity identity{};
    std::uint64_t surface{};
    std::uint64_t sampled_at_us{};
};

// Display-outcome-only, single-writer rate state. Neither this type nor its
// consumers are permitted in the protection deadline path.
class RateTracker {
public:
    void SetTarget(Identity identity, std::uint64_t now_us) noexcept {
        if (has_target_ && target_ == identity) return;
        target_ = identity;
        has_target_ = identity.pid != 0 && identity.creation_time != 0;
        ResetWindow(now_us);
    }

    void ClearTarget() noexcept {
        target_ = {};
        has_target_ = false;
        ResetWindow(0);
    }

    void MarkLost(std::uint64_t now_us) noexcept { ResetWindow(now_us); }

    void RecordDisplayed(Identity identity, std::uint64_t surface,
                         std::uint64_t timestamp_us) noexcept {
        if (!has_target_ || identity != target_ || surface == 0 ||
            timestamp_us < window_started_us_) return;
        if (too_many_surfaces_ && timestamp_us >= capacity_lost_at_us_ &&
            timestamp_us - capacity_lost_at_us_ > kSurfaceStaleUs) {
            bool another_recent_surface = false;
            for (const auto& entry : surfaces_) {
                if (entry.key != 0 && entry.key != surface &&
                    timestamp_us >= entry.last_us &&
                    timestamp_us - entry.last_us <= kSurfaceStaleUs) {
                    another_recent_surface = true;
                    break;
                }
            }
            if (!another_recent_surface) ResetWindow(timestamp_us);
        }
        Surface* selected = nullptr;
        Surface* oldest_stale = nullptr;
        for (auto& entry : surfaces_) {
            if (entry.key == surface) {
                selected = &entry;
                break;
            }
            if (entry.key == 0 && selected == nullptr) selected = &entry;
            if (entry.key != 0 && timestamp_us >= entry.last_us &&
                timestamp_us - entry.last_us > kSurfaceStaleUs &&
                (oldest_stale == nullptr || entry.last_us < oldest_stale->last_us))
                oldest_stale = &entry;
        }
        if (selected == nullptr && oldest_stale != nullptr) {
            *oldest_stale = {};
            selected = oldest_stale;
        }
        if (selected == nullptr) {
            too_many_surfaces_ = true;
            capacity_lost_at_us_ = timestamp_us;
            return;
        }
        if (selected->key == 0) selected->key = surface;
        if (selected->count != 0 && timestamp_us < selected->last_us) return;
        selected->last_us = timestamp_us;
        selected->times[selected->next] = timestamp_us;
        selected->next = (selected->next + 1) % kFrameCapacity;
        if (selected->count < kFrameCapacity) {
            ++selected->count;
        } else {
            selected->wrapped = true;
        }
    }

    [[nodiscard]] Snapshot Read(std::uint64_t now_us,
                                bool observer_healthy) const noexcept {
        Snapshot result{};
        result.identity = target_;
        result.sampled_at_us = now_us;
        if (!has_target_ || !observer_healthy || too_many_surfaces_ ||
            now_us < window_started_us_) return result;
        if (now_us - window_started_us_ < kWindowUs) {
            result.status = Status::Warmup;
            return result;
        }

        std::uint64_t latest_event_us = 0;
        for (const auto& entry : surfaces_) {
            if (entry.count != 0 && entry.last_us > latest_event_us)
                latest_event_us = entry.last_us;
        }
        if (latest_event_us > now_us) return result;
        if (latest_event_us == 0 || now_us - latest_event_us > kBufferHoldUs) {
            return result;
        }
        // Real-time ETW events arrive in buffers. Use the latest event's
        // timestamp for rate math and hold it only through a bounded gap.
        const std::uint64_t cutoff = latest_event_us > kWindowUs
            ? latest_event_us - kWindowUs : 0;
        std::size_t best_count = 0;
        std::size_t second_count = 0;
        std::uint64_t first_us = 0;
        std::uint64_t last_us = 0;
        bool best_truncated = false;
        for (const auto& entry : surfaces_) {
            if (entry.key == 0) continue;
            std::size_t count = 0;
            std::uint64_t first = 0;
            std::uint64_t last = 0;
            for (std::size_t i = 0; i < entry.count; ++i) {
                const auto time = entry.times[i];
                if (time < cutoff || time > latest_event_us) continue;
                if (count == 0 || time < first) first = time;
                if (time > last) last = time;
                ++count;
            }
            if (count > best_count) {
                second_count = best_count;
                best_count = count;
                first_us = first;
                last_us = last;
                best_truncated = entry.wrapped &&
                    entry.times[entry.next] > cutoff;
                result.surface = entry.key;
            } else if (count > second_count) {
                second_count = count;
            }
        }
        // A full fixed ring can truncate the one-second interval. Do not show
        // a confident number until a subsequent clean target/window begins.
        if (best_truncated || (second_count != 0 && best_count < second_count * 2)) {
            result.surface = 0;
            return result;
        }
        if (best_count == 0) {
            return result;
        }
        if (best_count < 2 || last_us <= first_us ||
            last_us - first_us < kMinMeasuredSpanUs) {
            result.status = Status::Warmup;
            return result;
        }
        result.status = Status::Ready;
        result.displayed_fps = static_cast<double>(best_count - 1) *
            static_cast<double>(kWindowUs) / static_cast<double>(last_us - first_us);
        return result;
    }

private:
    static constexpr std::uint64_t kWindowUs = 1'000'000;
    // Session warm-up alone does not make a tiny burst representative of a
    // sustained rate. Require enough of the frame-time window to be observed.
    static constexpr std::uint64_t kMinMeasuredSpanUs = 500'000;
    static constexpr std::uint64_t kBufferHoldUs = 2'000'000;
    static constexpr std::uint64_t kSurfaceStaleUs = 2'000'000;
    static constexpr std::size_t kSurfaceCapacity = 4;
    static constexpr std::size_t kFrameCapacity = 1024;

    struct Surface {
        std::uint64_t key{};
        std::array<std::uint64_t, kFrameCapacity> times{};
        std::size_t next{};
        std::size_t count{};
        std::uint64_t last_us{};
        bool wrapped{};
    };

    void ResetWindow(std::uint64_t now_us) noexcept {
        surfaces_ = {};
        too_many_surfaces_ = false;
        capacity_lost_at_us_ = 0;
        window_started_us_ = now_us;
    }

    Identity target_{};
    std::array<Surface, kSurfaceCapacity> surfaces_{};
    std::uint64_t window_started_us_{};
    bool has_target_{};
    bool too_many_surfaces_{};
    std::uint64_t capacity_lost_at_us_{};
};

}  // namespace gtg::fps
