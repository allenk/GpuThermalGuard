#pragma once

#include <cstdint>

namespace gtg::topapp::marquee {

// A name too long for its box moves; one that fits never does (owner,
// 2026-10-16: the marquee is only for names that do not fit). Both forms are
// pure functions of the time since the name appeared, so every output drawn
// at one instant agrees, and a frame can be drawn again without remembering
// where the last one left off.
//
// Widths are in whatever unit the caller draws in; the speed and gap are
// given in DIP and scaled by the caller with them.
inline constexpr float kSpeedDipPerSecond = 24.0F;
inline constexpr float kGapDip = 24.0F;
// The start of the name is read first, so each loop begins standing still.
inline constexpr std::uint64_t kPauseMs = 1'200;
// The in-game bitmap is drawn twice a second, which cannot scroll smoothly,
// so it pages instead.
inline constexpr std::uint64_t kPageMs = 2'000;
// Each page repeats a fifth of the previous one, so a word cut at the edge is
// seen whole on one of the two.
inline constexpr float kPageAdvance = 0.8F;

// How far the text has moved left, continuously: zero through the pause, then
// `speed_per_second`, until one whole cycle -- the text and the gap after it --
// has passed; the caller draws the text twice, `text_width + gap` apart, so
// the loop is seamless. Zero when the text fits.
[[nodiscard]] constexpr float ScrollOffset(const std::uint64_t elapsed_ms, const float text_width,
                                           const float box_width, const float gap,
                                           const float speed_per_second,
                                           const std::uint64_t pause_ms) noexcept {
    if (text_width <= box_width || speed_per_second <= 0.0F) return 0.0F;
    const float cycle = text_width + gap;
    const auto scroll_ms = static_cast<std::uint64_t>(cycle / speed_per_second * 1000.0F);
    const std::uint64_t period = pause_ms + (scroll_ms == 0 ? 1 : scroll_ms);
    const std::uint64_t t = elapsed_ms % period;
    if (t < pause_ms) return 0.0F;
    const float moved = static_cast<float>(t - pause_ms) * speed_per_second / 1000.0F;
    return moved < cycle ? moved : cycle;
}

// How far the text has moved left, a page at a time: each page is held for
// `page_ms`, the last one ends flush with the box's right edge, then it starts
// over. Zero when the text fits.
[[nodiscard]] constexpr float PageOffset(const std::uint64_t elapsed_ms, const float text_width,
                                         const float box_width,
                                         const std::uint64_t page_ms) noexcept {
    if (text_width <= box_width || box_width <= 0.0F || page_ms == 0) return 0.0F;
    const float last = text_width - box_width;
    const float step = box_width * kPageAdvance;
    std::uint64_t pages = 1;
    for (float reached = 0.0F; reached < last; reached += step)
        ++pages;
    const std::uint64_t page = (elapsed_ms / page_ms) % pages;
    const float offset = static_cast<float>(page) * step;
    return offset < last ? offset : last;
}

}  // namespace gtg::topapp::marquee
