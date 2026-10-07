#pragma once

#include <cstdint>
#include <deque>
#include <optional>

// When protection tripped, for the main window's temperature lane.
// AF-20261004-main-ui-redesign.
//
// In memory only, like the curves the markers sit on: the history does not
// survive a restart, so a persisted marker would have nothing to mark.
namespace gtg::tray {

class TripMarkers {
public:
    // More than the retained hour could plausibly hold. A bound, so a trip
    // storm cannot grow this without limit.
    static constexpr std::size_t kCapacity = 64;

    void Add(const std::uint64_t monotonic_ms) {
        if (times_.size() == kCapacity) times_.pop_front();
        times_.push_back(monotonic_ms);
    }

    // Drops what has scrolled out of the retained history.
    void Prune(const std::uint64_t now_ms, const std::uint64_t retained_ms) {
        const std::uint64_t earliest = now_ms > retained_ms ? now_ms - retained_ms : 0;
        while (!times_.empty() && times_.front() < earliest) times_.pop_front();
    }

    [[nodiscard]] const std::deque<std::uint64_t>& Times() const noexcept { return times_; }

private:
    std::deque<std::uint64_t> times_;
};

// Where along a view a marker falls, 0 at the view's start and 1 at its end;
// nothing when it is outside. A marker is drawn where it happened or not at
// all -- pinning one to the edge would place a trip at a time it did not occur.
[[nodiscard]] constexpr std::optional<double> MarkerFraction(
    const std::uint64_t time_ms, const std::uint64_t view_start_ms,
    const std::uint64_t view_end_ms) noexcept {
    if (view_end_ms <= view_start_ms || time_ms < view_start_ms || time_ms > view_end_ms)
        return std::nullopt;
    return static_cast<double>(time_ms - view_start_ms) /
           static_cast<double>(view_end_ms - view_start_ms);
}

}  // namespace gtg::tray
