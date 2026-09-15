#pragma once

#include <windows.h>
#include <optional>

namespace gtg::tray {
// Visibility is deliberately not a prerequisite: a hidden popup retains its
// screen coordinates. Invalid/failed creation must never persist a fake (0,0).
inline std::optional<POINT> ReadWindowPosition(HWND window) noexcept {
    RECT bounds{};
    if (!window || !IsWindow(window) || !GetWindowRect(window, &bounds) ||
        bounds.right <= bounds.left || bounds.bottom <= bounds.top) return std::nullopt;
    return POINT{bounds.left, bounds.top};
}
}  // namespace gtg::tray
