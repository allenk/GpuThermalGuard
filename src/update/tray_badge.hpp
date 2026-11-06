#pragma once

#include <windows.h>

#include <cstdint>
#include <optional>
#include <span>

#include "update/decision.hpp"

// The tray icon's update badge: a violet dot with a dark rim in the top right
// corner, half the icon wide, drawn at run time on whichever state icon the
// tray shows. Violet is a colour no state uses, and the top right keeps the
// thermometer's bulb and the state symbol clear.
namespace gtg::update {

struct BadgeState {
    // What the last check decided; nullopt when there is nothing to compare.
    std::optional<Decision> decision;
    // The updater reports a problem: the source could not be read, or this
    // machine cannot be updated.
    bool problem{};
    // An update is staging, waiting for a safe moment or handing off; and
    // whether it is a reinstall, and whether a paired host asked for it.
    bool updating{};
    bool reinstall{};
    bool requested{};
};

// A newer release is offered, or the reader's update to it is on its way. A
// paired host's request has a balloon of its own.
[[nodiscard]] constexpr bool ShowsBadge(const BadgeState& state) noexcept {
    if (state.updating) return !state.reinstall && !state.requested;
    return !state.problem && state.decision.has_value() && OffersUpdate(*state.decision);
}

// 0xAARRGGBB, the order of a 32-bit DIB's pixels in memory.
inline constexpr std::uint32_t kBadgeFill = 0xFFC084FC;
inline constexpr std::uint32_t kBadgeRim = 0xFF141418;

struct BadgeBox {
    int left{};
    int diameter{};
    int rim{};
};

// Where the dot goes on a square icon of `size` pixels; its top is row 0.
[[nodiscard]] constexpr BadgeBox BadgeBoxFor(const int size) noexcept {
    BadgeBox box;
    box.diameter = (size + 1) / 2;
    box.rim = (size + 8) / 16 > 1 ? (size + 8) / 16 : 1;
    box.left = size - box.diameter;
    return box;
}

// Paints the badge over a square of straight-alpha 0xAARRGGBB pixels, rows top
// down. Pixels outside BadgeBoxFor(size) are not touched.
void PaintBadge(std::span<std::uint32_t> pixels, int size) noexcept;

// A copy of the icon with the badge, or nullptr if the icon's pixels could not
// be read. The caller owns both icons.
[[nodiscard]] HICON WithBadge(HICON icon) noexcept;

}  // namespace gtg::update
