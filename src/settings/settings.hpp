#pragma once

#include <cstdint>
#include <string>

#include "core/protection.hpp"
#include "localization/localization.hpp"
#include "settings/ui_preferences.hpp"

namespace gtg::settings {

struct LoadResult {
    ProtectionConfig config{};
    bool loaded{false};
    std::wstring warning;
};

// The range an explicit OSD scale may take. Below this a reading stops being
// legible at any DPI; above it the dashboard cannot be arranged on a small
// display. 0 sits outside deliberately and means "follow the display".
inline constexpr int kMinOsdScalePercent = 50;
inline constexpr int kMaxOsdScalePercent = 300;

struct OsdPreference {
    // Matches ResolveOsdPreference: absent means on.
    bool enabled{true};
    // Absent means expanded, which is what every install does today, so no
    // reader's window changes shape on upgrade.
    bool collapsed{false};
    // OsdX/OsdY: the expanded shape's position. Before each shape had its own
    // it was the shape last exited in, which is why a missing compact
    // position falls back to it (placement::FromStored).
    bool has_position{false};
    int x{};
    int y{};
    // CompactX/CompactY: the collapsed shape's own position.
    // AF-20261004-main-ui-redesign.
    bool has_compact_position{false};
    int compact_x{};
    int compact_y{};
    // The reader's chosen OSD size, in hundredths. 0 means follow the display
    // scale, which is what every install did before this existed and remains
    // the default.
    //
    // Absolute rather than a multiplier over the display scale: a multiplier
    // would mean a different physical size on every monitor, which is the
    // opposite of what somebody setting it once intends.
    int scale_percent{};
};

// The scale the OSD should draw at, resolved from what was stored and the
// display it is on. A pure function because it is the whole of the decision
// and deserves to be tested without a window.
//
// Anything outside the accepted range resolves to the display scale rather
// than being clamped: a number written by a later version should not silently
// become a size nobody chose.
[[nodiscard]] constexpr float EffectiveOsdScale(const int scale_percent,
                                                const unsigned dpi) noexcept {
    if (scale_percent >= kMinOsdScalePercent &&
        scale_percent <= kMaxOsdScalePercent) {
        return static_cast<float>(scale_percent) / 100.0F;
    }
    return static_cast<float>(dpi == 0 ? 96U : dpi) / 96.0F;
}

struct WindowPosition {
    bool has_position{false};
    int x{};
    int y{};
};

struct TriggerTestRun {
    bool loaded{false};
    std::uint32_t baseline{};
    std::uint64_t started_file_time{};
};

[[nodiscard]] constexpr bool ResolveFpsPreference(
    bool has_valid_value, std::uint32_t value) noexcept {
    return !has_valid_value || value != 0;
}

// Same rule as FPS: an absent value enables the record.
[[nodiscard]] constexpr bool ResolveRamPreference(
    bool has_valid_value, std::uint32_t value) noexcept {
    return !has_valid_value || value != 0;
}

// Same rule as RAM and FPS: an absent value enables the overlay. It is the
// feature the product leads with, and everything the compact dashboard
// carries is invisible until it is on, so shipping it off meant shipping it
// hidden behind a checkbox nobody had reason to look for.
[[nodiscard]] constexpr bool ResolveOsdPreference(
    bool has_valid_value, std::uint32_t value) noexcept {
    return !has_valid_value || value != 0;
}

// An absent value leaves the compact dashboard locked, so an arrangement
// cannot be disturbed by a reader who has never opened it.
[[nodiscard]] constexpr bool ResolveCompactLockPreference(
    bool has_valid_value, std::uint32_t value) noexcept {
    return !has_valid_value || value != 0;
}

[[nodiscard]] LoadResult Load();
[[nodiscard]] bool Save(const ProtectionConfig& config, std::wstring& error);
[[nodiscard]] bool LoadSafeLatch() noexcept;
[[nodiscard]] bool SaveSafeLatch(bool latched, std::wstring& error);
[[nodiscard]] unsigned int LoadTriggerCount() noexcept;
[[nodiscard]] bool SaveTriggerCount(unsigned int count, std::wstring& error);
[[nodiscard]] TriggerTestRun LoadTriggerTestRun() noexcept;
[[nodiscard]] bool SaveTriggerTestRun(std::uint32_t baseline,
                                      std::uint64_t started_file_time,
                                      std::wstring& error);
[[nodiscard]] bool LoadCloseToTrayPreference() noexcept;
[[nodiscard]] bool SaveCloseToTrayPreference(bool enabled, std::wstring& error);
[[nodiscard]] OsdPreference LoadOsdPreference() noexcept;
[[nodiscard]] bool SaveOsdEnabled(bool enabled, std::wstring& error);
[[nodiscard]] bool SaveOsdPosition(int x, int y, std::wstring& error);
[[nodiscard]] bool SaveCompactPosition(int x, int y, std::wstring& error);
[[nodiscard]] bool SaveOsdCollapsed(bool collapsed, std::wstring& error);
[[nodiscard]] bool SaveOsdScale(int scale_percent, std::wstring& error);
[[nodiscard]] bool LoadFpsEnabled() noexcept;
[[nodiscard]] bool SaveFpsEnabled(bool enabled, std::wstring& error);
[[nodiscard]] bool LoadRamEnabled() noexcept;
[[nodiscard]] bool SaveRamEnabled(bool enabled, std::wstring& error);
// Absent means on, as RAM and FPS are.
//
// It shipped off by default on the argument that the main window's eighth lane
// costs the other seven 14 % of their height. The owner overruled it: a
// dashboard for gaming that has to be switched on before it shows the network
// is a dashboard most readers will never see the network on.
[[nodiscard]] bool LoadNetEnabled() noexcept;
[[nodiscard]] bool SaveNetEnabled(bool enabled, std::wstring& error);
// The arrangement is returned as the raw stored word, not as a layout: this
// layer must not depend on the tray's presentation headers. The caller
// validates it, and an absent value reads as 0, which sanitizes to the default.
[[nodiscard]] std::uint32_t LoadCompactLayout() noexcept;
[[nodiscard]] bool SaveCompactLayout(std::uint32_t packed, std::wstring& error);
[[nodiscard]] bool LoadCompactLocked() noexcept;
[[nodiscard]] bool SaveCompactLocked(bool locked, std::wstring& error);
[[nodiscard]] WindowPosition LoadMainWindowPosition() noexcept;
[[nodiscard]] bool SaveMainWindowPosition(int x, int y, std::wstring& error);
[[nodiscard]] localization::UiLanguage LoadUiLanguagePreference() noexcept;
[[nodiscard]] bool SaveUiLanguagePreference(localization::UiLanguage language,
                                            std::wstring& error);
// AF-20261004-main-ui-redesign. The rules live in ui_preferences.hpp.
[[nodiscard]] UiTheme LoadUiThemePreference() noexcept;
[[nodiscard]] bool SaveUiThemePreference(UiTheme theme, std::wstring& error);
[[nodiscard]] bool LoadOverlayEnabled() noexcept;
[[nodiscard]] bool SaveOverlayEnabled(bool enabled, std::wstring& error);
[[nodiscard]] bool LoadOverlayAutoSdr() noexcept;
[[nodiscard]] bool SaveOverlayAutoSdr(bool enabled, std::wstring& error);
// AF-20261008-overlay-embedded-osd.
[[nodiscard]] bool LoadOverlayHidesOsd() noexcept;
[[nodiscard]] bool SaveOverlayHidesOsd(bool enabled, std::wstring& error);
[[nodiscard]] OverlayHotkey LoadOverlayHotkey() noexcept;
[[nodiscard]] bool SaveOverlayHotkey(const OverlayHotkey& hotkey, std::wstring& error);
[[nodiscard]] bool LoadFeatureNoticeSerial(std::uint32_t& serial) noexcept;
[[nodiscard]] bool SaveFeatureNoticeSerial(std::uint32_t serial, std::wstring& error);

}  // namespace gtg::settings
