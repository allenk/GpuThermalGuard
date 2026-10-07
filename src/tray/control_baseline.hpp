#pragma once
#include <windows.h>

namespace gtg::tray {
[[nodiscard]] constexpr int ButtonCornerDiameter(unsigned dpi) noexcept {
    return static_cast<int>((8ULL * (dpi ? dpi : 96U) + 48) / 96);
}
inline bool ControlTextMetrics(HWND window, TEXTMETRICW& metric) noexcept {
    const auto dc = GetDC(window);
    if (!dc) return false;
    const auto font = reinterpret_cast<HFONT>(SendMessageW(window, WM_GETFONT, 0, 0));
    const auto previous = font ? SelectObject(dc, font) : nullptr;
    if (font && (!previous || previous == HGDI_ERROR)) {
        ReleaseDC(window, dc);
        return false;
    }
    const bool valid = GetTextMetricsW(dc, &metric) != FALSE;
    if (previous && previous != HGDI_ERROR) SelectObject(dc, previous);
    ReleaseDC(window, dc);
    return valid;
}
inline bool AlignEditToLabel(HWND parent, HWND label, HWND edit) noexcept {
    if (!parent || !label || !edit || GetParent(label) != parent || GetParent(edit) != parent)
        return false;
    TEXTMETRICW label_metric{}, edit_metric{};
    if (!ControlTextMetrics(label, label_metric) || !ControlTextMetrics(edit, edit_metric)) return false;
    RECT text{}, label_rect{}, edit_rect{}, client{};
    SendMessageW(edit, EM_GETRECT, 0, reinterpret_cast<LPARAM>(&text));
    if (text.bottom <= text.top || !GetWindowRect(label, &label_rect) ||
        !GetWindowRect(edit, &edit_rect) || !GetClientRect(edit, &client)) return false;
    MapWindowPoints(nullptr, parent, reinterpret_cast<POINT*>(&label_rect), 2);
    MapWindowPoints(nullptr, parent, reinterpret_cast<POINT*>(&edit_rect), 2);
    // Native single-line edit has no vertical-alignment style or EM_SETRECT.
    // Fit one font line plus measured border/client insets, not a DPI pixel guess.
    const int border = edit_rect.bottom - edit_rect.top - (client.bottom - client.top);
    const int padding = text.top > 0 ? text.top : 0;
    const int height = edit_metric.tmHeight + border + 2 * padding;
    if (height <= 0 || !SetWindowPos(edit, nullptr, edit_rect.left, edit_rect.top,
            edit_rect.right - edit_rect.left, height, SWP_NOZORDER | SWP_NOACTIVATE)) return false;
    SendMessageW(edit, EM_GETRECT, 0, reinterpret_cast<LPARAM>(&text));
    MapWindowPoints(edit, parent, reinterpret_cast<POINT*>(&text), 2);
    const int offset = label_rect.top + label_metric.tmAscent - (text.top + edit_metric.tmAscent);
    if (!offset) return true;
    return SetWindowPos(edit, nullptr, edit_rect.left, edit_rect.top + offset, 0, 0,
                        SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE) != FALSE;
}
}
