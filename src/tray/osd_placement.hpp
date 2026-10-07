#pragma once

#include <windows.h>

#include <cstdlib>
#include <optional>
#include <span>

// Where the OSD goes, as arithmetic with no window in it, so the rules can be
// tested. AF-20261004-main-ui-redesign.
namespace gtg::tray::placement {

// Each shape remembers its own top-left.
//
// Expanded and collapsed differ in width by hundreds of pixels, so one shared
// coordinate cannot serve both: a compact column docked on a right edge has to
// slide left to expand, and collapsing again kept the slide, so the compact
// window drifted every time the reader looked at the expanded view.
// The owner's rule: compact thinks in compact coordinates.
struct ModePositions {
    std::optional<POINT> expanded;
    std::optional<POINT> collapsed;

    [[nodiscard]] std::optional<POINT>& For(const bool is_collapsed) noexcept {
        return is_collapsed ? collapsed : expanded;
    }
    [[nodiscard]] const std::optional<POINT>& For(const bool is_collapsed) const noexcept {
        return is_collapsed ? collapsed : expanded;
    }
};

// A switch of shape keeps the chevron where it is. The chevron sits at the
// top-right of both shapes, so the top and the right edge stay put and the
// width changes to the left: clicking it again and again expands and collapses
// in place (owner, 2026-10-05).
//
// This replaced "each shape returns to its own remembered position", which
// made the window jump between two places the reader could not predict. The
// stored pair is still what a restart opens at (FromStored).
//
// Fitting the result to a monitor is the caller's next step. Where the fit has
// to move the window -- a compact window on a left edge has no room to expand
// leftwards -- the chevron moves too; nothing can keep it still there.
[[nodiscard]] constexpr POINT ToggleKeepingButton(const POINT here, const int width_before,
                                                  const int width_after) noexcept {
    return {here.x + width_before - width_after, here.y};
}

// The stored pair, read the way an older install wrote it. `OsdX/OsdY` was the
// only position and held whichever shape the reader last exited in, so with no
// compact position of its own the collapsed shape starts from that one, which
// is exactly what every build before this did.
[[nodiscard]] inline ModePositions FromStored(const std::optional<POINT> osd,
                                              const std::optional<POINT> compact) noexcept {
    return {osd, compact.has_value() ? compact : osd};
}

// Magnetic alignment while dragging. The reader asked for the work-area edges
// of the monitor the window is on and the edges of neighbouring monitors; the
// seam between two monitors is the edge of each one's work area, so one list
// of work areas covers both.
inline constexpr int kSnapDistanceDip = 12;

namespace detail {

struct Pull {
    int delta{};
    bool found{};
};

// Keeps the smallest move, within `threshold`, that lays `edge` on `line`.
inline void Consider(Pull& best, const int edge, const int line, const int threshold) noexcept {
    const int delta = line - edge;
    if (std::abs(delta) > threshold) return;
    if (!best.found || std::abs(delta) < std::abs(best.delta)) best = {delta, true};
}

}  // namespace detail

// `proposed` is where the cursor has put the window; the result is where it
// goes. A line counts only where the window lies alongside it -- the left edge
// of a monitor off to the side of the window is not a wall the window is
// standing against.
//
// `proposed` must come from the cursor (FromCursor), never from the rectangle
// WM_MOVING carries. Windows' move loop builds that rectangle from the
// previous one plus the mouse's movement since, so once a window is snapped
// every small movement starts from the edge, lands within the threshold again
// and is pulled back: the window can never be dragged off an edge it touched.
// That shipped once (2026-10-04): the owner's OSD ended up stuck in a corner.
[[nodiscard]] inline RECT Snap(RECT proposed, const std::span<const RECT> work_areas,
                               const int threshold) noexcept {
    if (threshold <= 0) return proposed;
    detail::Pull horizontal;
    detail::Pull vertical;
    for (const RECT& area : work_areas) {
        const bool beside_vertically =
            proposed.top < area.bottom && proposed.bottom > area.top;
        const bool beside_horizontally =
            proposed.left < area.right && proposed.right > area.left;
        if (beside_vertically) {
            for (const int line : {area.left, area.right}) {
                detail::Consider(horizontal, proposed.left, line, threshold);
                detail::Consider(horizontal, proposed.right, line, threshold);
            }
        }
        if (beside_horizontally) {
            for (const int line : {area.top, area.bottom}) {
                detail::Consider(vertical, proposed.top, line, threshold);
                detail::Consider(vertical, proposed.bottom, line, threshold);
            }
        }
    }
    if (horizontal.found) {
        proposed.left += horizontal.delta;
        proposed.right += horizontal.delta;
    }
    if (vertical.found) {
        proposed.top += vertical.delta;
        proposed.bottom += vertical.delta;
    }
    return proposed;
}

// Where the window would be with no snapping: the cursor minus the point of the
// window it was grabbed by, which is fixed for the whole drag.
[[nodiscard]] constexpr RECT FromCursor(const POINT cursor, const POINT grab,
                                        const SIZE size) noexcept {
    const LONG left = cursor.x - grab.x;
    const LONG top = cursor.y - grab.y;
    return {left, top, left + size.cx, top + size.cy};
}

}  // namespace gtg::tray::placement
