// Spike 4 T-01 -- where the dashboard goes, and how big, in a frame that is not
// the desktop. The owner's rule (specs/changes/2026-10-02-overlay-spike-4/
// proposal.md), as arithmetic and nothing else:
//
//   1. The reference is the monitor the desktop OSD is on.
//   2. Size is proportional: the dashboard takes the same share of the frame as
//      the desktop OSD takes of its monitor.
//   3. Alignment is kept, not the position: the edge the OSD sits nearest to
//      (design §4: left / centre / right, top / centre / bottom) stays its edge
//      in the frame, and the margin from it scales with the frame.
//   4. Never outside the frame: nothing below 0, nothing past the frame's size;
//      too big, and it shrinks until it fits, keeping its anchor.
//   5. Small frames make a small dashboard, and that is accepted.
//   6. At a frame the size of the monitor, the result is the desktop OSD's own
//      rectangle, exactly.
//
// Pure: no Windows calls, no state, so every case can be checked against a
// rectangle worked out by hand. The caller supplies rectangles in desktop
// coordinates, physical pixels (the OSD window's, and its monitor's bounds --
// question (b) of the proposal), and the frame's size from OsdReq.v1.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace gtg::overlay::geometry {

struct Rect {
    std::int32_t left = 0;
    std::int32_t top = 0;
    std::int32_t right = 0;
    std::int32_t bottom = 0;

    std::int32_t Width() const { return right - left; }

    std::int32_t Height() const { return bottom - top; }
};

enum class Edge : std::uint32_t {
    kNear = 0,
    kCentre = 1,
    kFar = 2
};  // left/top, centre, right/bottom

// The answer for one frame size: how big to render, and the anchoring that
// Place turns into a rectangle once the real bitmap size is known.
struct Mapping {
    // Relative to the desktop OSD's own size: render at desktop scale x this.
    double scale = 1.0;
    Edge horizontal = Edge::kFar;
    Edge vertical = Edge::kNear;
    // Distance from the anchored edge, in frame pixels. For a centre anchor,
    // the signed offset of the dashboard's centre from the frame's centre.
    double margin_x = 0.0;
    double margin_y = 0.0;
    std::int32_t frame_width = 0;
    std::int32_t frame_height = 0;
};

namespace detail {

// Nearest of near edge, centre, far edge; a tie goes to an edge, because an
// edge is what a user means by "put it in the corner" (design §4).
inline Edge NearestEdge(std::int32_t osd_lo, std::int32_t osd_hi, std::int32_t mon_lo,
                        std::int32_t mon_hi, double& margin) {
    const double near_gap = static_cast<double>(osd_lo - mon_lo);
    const double far_gap = static_cast<double>(mon_hi - osd_hi);
    const double centre_offset =
        (static_cast<double>(osd_lo) + osd_hi) / 2.0 - (static_cast<double>(mon_lo) + mon_hi) / 2.0;
    const double near_d = std::abs(near_gap);
    const double far_d = std::abs(far_gap);
    const double centre_d = std::abs(centre_offset);
    if (near_d <= far_d && near_d <= centre_d) {
        margin = near_gap;
        return Edge::kNear;
    }
    if (far_d <= centre_d) {
        margin = far_gap;
        return Edge::kFar;
    }
    margin = centre_offset;
    return Edge::kCentre;
}

inline std::int32_t Round(double v) {
    return static_cast<std::int32_t>(std::lround(v));
}

// One axis of Place: the start coordinate of `size` pixels in `extent`.
inline std::int32_t PlaceAxis(Edge edge, double margin, std::int32_t size, std::int32_t extent) {
    std::int32_t start = 0;
    switch (edge) {
        case Edge::kNear:
            start = Round(margin);
            break;
        case Edge::kFar:
            start = extent - Round(margin) - size;
            break;
        case Edge::kCentre:
            start =
                Round(static_cast<double>(extent) / 2.0 + margin - static_cast<double>(size) / 2.0);
            break;
    }
    // Rule 4. A dashboard wider than the frame cannot happen after Map's
    // shrink, but a bitmap a pixel larger than estimated can; it starts at 0.
    return std::clamp(start, 0, std::max(0, extent - size));
}

}  // namespace detail

// Rules 1-5 for one frame size. `osd` is the desktop OSD's rectangle and
// `monitor` the bounds of the monitor it is on.
inline Mapping Map(const Rect& osd, const Rect& monitor, std::int32_t frame_width,
                   std::int32_t frame_height) {
    Mapping m;
    m.frame_width = frame_width;
    m.frame_height = frame_height;
    if (osd.Width() <= 0 || osd.Height() <= 0 || monitor.Width() <= 0 || monitor.Height() <= 0 ||
        frame_width <= 0 || frame_height <= 0) {
        m.scale = 0.0;  // nothing sensible to draw; the caller draws nothing
        return m;
    }
    const double rx = static_cast<double>(frame_width) / monitor.Width();
    const double ry = static_cast<double>(frame_height) / monitor.Height();
    // Question (a): one scale for the size, so the dashboard keeps its shape --
    // the smaller axis ratio, so it cannot outgrow the frame on either axis.
    double s = std::min(rx, ry);
    // Rule 4's shrink, for an OSD larger than its own monitor (dragged across
    // an edge, or spanning two): it must still fit the frame.
    s = std::min(s, static_cast<double>(frame_width) / osd.Width());
    s = std::min(s, static_cast<double>(frame_height) / osd.Height());
    m.scale = s;

    double mx = 0.0;
    double my = 0.0;
    m.horizontal = detail::NearestEdge(osd.left, osd.right, monitor.left, monitor.right, mx);
    m.vertical = detail::NearestEdge(osd.top, osd.bottom, monitor.top, monitor.bottom, my);
    // Question (a), continued: each margin by its own axis's ratio, so the
    // relation to each edge is kept on a frame of a different shape.
    m.margin_x = mx * rx;
    m.margin_y = my * ry;
    return m;
}

// The size Map expects the bitmap to come out at, for a desktop OSD of
// `osd_width` x `osd_height`.
inline void ExpectedSize(const Mapping& m, std::int32_t osd_width, std::int32_t osd_height,
                         std::int32_t& width, std::int32_t& height) {
    width = detail::Round(osd_width * m.scale);
    height = detail::Round(osd_height * m.scale);
}

// The destination rectangle, in frame pixels, for a bitmap of the size the
// rasteriser actually produced.
inline Rect Place(const Mapping& m, std::int32_t bitmap_width, std::int32_t bitmap_height) {
    const std::int32_t w = std::min(bitmap_width, m.frame_width);
    const std::int32_t h = std::min(bitmap_height, m.frame_height);
    const std::int32_t x = detail::PlaceAxis(m.horizontal, m.margin_x, w, m.frame_width);
    const std::int32_t y = detail::PlaceAxis(m.vertical, m.margin_y, h, m.frame_height);
    return Rect{x, y, x + w, y + h};
}

}  // namespace gtg::overlay::geometry
