#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

// The history chart's timeline: which view ends the reader can scroll to and
// where the thumb sits for one of them. Plain arithmetic over GetTickCount64
// values, so it is tested without a window, and the painter and the mouse
// handlers read the same answer.
//
// The track spans the retained hour less one view, newest at the right, with a
// thumb of fixed size -- one view against the hour -- that the reader can drag
// anywhere along it, whether or not samples exist there.
//
// The one exception is the first 55 minutes after boot, when that span would
// reach back before the clock started; the track then runs from boot to now.
// The arithmetic this replaces clamped the start to boot but still divided by
// 55 minutes, so ten minutes after boot the thumb of a chart following live sat
// 18 % of the way along the track, and every drag to its right was pulled back
// to it.
namespace gtg::tray {

struct TimelineTrack {
    std::uint64_t earliest_end = 0;  // the oldest view end the track reaches
    std::uint64_t latest_end = 0;    // now; a view ending here follows live
    double thumb_share = 1.0;        // the thumb's share of the track

    [[nodiscard]] static TimelineTrack For(const std::uint64_t now, const std::uint64_t retained_ms,
                                           const std::uint64_t visible_ms) noexcept {
        const std::uint64_t span = retained_ms > visible_ms ? retained_ms - visible_ms : 0;
        const double share =
            retained_ms == 0 ? 1.0
                             : static_cast<double>(visible_ms) / static_cast<double>(retained_ms);
        return {now > span ? now - span : 0, now, share};
    }

    [[nodiscard]] std::uint64_t ScrollableMs() const noexcept { return latest_end - earliest_end; }

    [[nodiscard]] double ThumbShare() const noexcept { return thumb_share; }

    [[nodiscard]] std::uint64_t Clamp(const std::uint64_t view_end) const noexcept {
        return std::clamp(view_end, earliest_end, latest_end);
    }

    // Where a view end sits along the track: 0 at the oldest, 1 at live.
    [[nodiscard]] double FractionOf(const std::uint64_t view_end) const noexcept {
        const std::uint64_t scrollable = ScrollableMs();
        if (scrollable == 0) return 1.0;
        return static_cast<double>(Clamp(view_end) - earliest_end) /
               static_cast<double>(scrollable);
    }

    [[nodiscard]] std::uint64_t ViewEndAt(const double fraction) const noexcept {
        const double clamped = std::clamp(fraction, 0.0, 1.0);
        return earliest_end + static_cast<std::uint64_t>(
                                  std::llround(clamped * static_cast<double>(ScrollableMs())));
    }
};

}  // namespace gtg::tray
