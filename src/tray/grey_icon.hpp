#pragma once

#include <windows.h>

#include <cstdint>
#include <span>

// The armed tray icon drawn grey at run time, for a machine with no NVIDIA
// GPU: GTG runs and monitors, but protects nothing. Grey is no state's colour,
// and the shape still says which program it is.
namespace gtg::tray {

// Each straight-alpha 0xAARRGGBB pixel becomes its luminance (Rec. 601) in
// all three channels; alpha is kept.
constexpr void GreyPixels(const std::span<std::uint32_t> pixels) noexcept {
    for (std::uint32_t& pixel : pixels) {
        const std::uint32_t r = (pixel >> 16) & 0xFF;
        const std::uint32_t g = (pixel >> 8) & 0xFF;
        const std::uint32_t b = pixel & 0xFF;
        const std::uint32_t y = (r * 299 + g * 587 + b * 114 + 500) / 1000;
        pixel = (pixel & 0xFF000000) | (y << 16) | (y << 8) | y;
    }
}

// A grey copy of the icon, or nullptr if its pixels could not be read. The
// caller owns both icons.
[[nodiscard]] HICON GreyIcon(HICON icon) noexcept;

}  // namespace gtg::tray
