#pragma once

#include <windows.h>

#include <optional>

#include "settings/ui_preferences.hpp"
#include "tray/history_chart.hpp"

// The main window's theme: the only file that talks to darkmodelib, so the rest
// of the tray never includes it. AF-20261004-main-ui-redesign.
//
// Light is "classic" -- darkmodelib stays attached but passes every message
// through, so the window is exactly the one GTG has always drawn. Attaching
// regardless is what lets the choice change while the window is open.
namespace gtg::tray::theme {

// Once per process, before the first themed window exists.
void Initialize() noexcept;

// Resolves the preference against the system (app mode, high contrast,
// whether the undocumented exports exist) and configures darkmodelib for it.
// Returns what the window will actually be drawn as.
settings::Appearance Configure(settings::UiTheme preference) noexcept;

[[nodiscard]] settings::Appearance Current() noexcept;
[[nodiscard]] inline bool IsDark() noexcept {
    return Current() == settings::Appearance::Dark;
}

// For a dialog: subclass it and its children, and follow WM_SETTINGCHANGE.
void AttachDialog(HWND dialog) noexcept;

// After Configure: title bar, child control themes and a full repaint.
void Repaint(HWND dialog) noexcept;

// The surface colours a self-drawn control or a WM_CTLCOLOR* answer needs.
// In classic these are the system colours GTG has always used.
[[nodiscard]] COLORREF DialogBackground() noexcept;
[[nodiscard]] HBRUSH DialogBackgroundBrush() noexcept;
[[nodiscard]] COLORREF ControlBackground() noexcept;
[[nodiscard]] COLORREF HotBackground() noexcept;
[[nodiscard]] COLORREF Edge() noexcept;
[[nodiscard]] COLORREF Text() noexcept;
[[nodiscard]] COLORREF DisabledText() noexcept;

// An accent picked for a light background, lifted for a dark one. The record
// colours keep their hue so a reading is recognisable in either theme.
[[nodiscard]] COLORREF Accent(COLORREF light_accent) noexcept;

// The history chart's surface for the current theme; nullopt is the chart's
// own system look.
[[nodiscard]] std::optional<ChartPalette> HistoryChartPalette() noexcept;

}  // namespace gtg::tray::theme
