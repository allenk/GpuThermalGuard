#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

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
    // CompactTiny: whether the compact dashboard is folded into Tiny. A form of
    // the compact shape, so it means nothing while expanded and is kept for
    // the next collapse. Absent means the cells.
    bool tiny{false};
    // OsdCard: whether the expanded view draws cards instead of lanes. Its
    // counterpart of Tiny: meaningless while collapsed, kept for the next
    // expand. Absent means the lanes.
    bool card{false};
    // OsdX/OsdY: the expanded shape's position. Before each shape had its own
    // it was the shape last exited in, which is why a missing compact
    // position falls back to it (placement::FromStored).
    bool has_position{false};
    int x{};
    int y{};
    // CompactX/CompactY: the collapsed shape's own position.
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

// Same rule again: an absent value names the app behind a sustained high
// load. With no such load nothing runs, so on costs nothing until it matters.
[[nodiscard]] constexpr bool ResolveTopAppPreference(bool has_valid_value,
                                                     std::uint32_t value) noexcept {
    return !has_valid_value || value != 0;
}

// An absent value leaves the compact dashboard locked, so an arrangement
// cannot be disturbed by a reader who has never opened it.
[[nodiscard]] constexpr bool ResolveCompactLockPreference(
    bool has_valid_value, std::uint32_t value) noexcept {
    return !has_valid_value || value != 0;
}

// Hosts: paired GTGs on each other's dashboards.
inline constexpr std::size_t kMaxStoredHosts = 16;

[[nodiscard]] constexpr bool IsValidHostsPort(const std::uint32_t port) noexcept {
    return port >= 1024 && port <= 65535;
}

struct HostsPreference {
    // Absent means off: a network feature is never on until the reader turns
    // it on.
    bool enabled{false};
    // Empty: the computer's name.
    std::wstring name;
    // 0: the default port.
    int port{};
    // This GTG's id among its pairs; 0 until one is made.
    std::uint64_t peer_id{};
    // The Hosts log, for another program on this machine to read. Off unless
    // the reader turns it on; independent of `enabled`.
    bool log{false};
};

// A pair as stored. The key arrives already protected (DPAPI); this layer
// never holds one in the clear.
struct StoredHost {
    std::uint64_t peer{};
    std::wstring name;
    std::uint32_t order{};
    bool issued{};
    bool has_endpoint{};
    std::array<std::uint8_t, 4> address{};
    std::uint16_t port{};
    std::vector<std::uint8_t> protected_key;
};

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
[[nodiscard]] bool SaveCompactTiny(bool tiny, std::wstring& error);
[[nodiscard]] bool SaveOsdCard(bool card, std::wstring& error);
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
[[nodiscard]] bool LoadTopAppEnabled() noexcept;
[[nodiscard]] bool SaveTopAppEnabled(bool enabled, std::wstring& error);
// The arrangement is returned as raw stored values, not as a layout: this
// layer must not depend on the tray's presentation headers, and the caller
// validates them. CompactArrangement (REG_QWORD) is the store since the ninth
// record; nothing when absent. CompactLayout (REG_DWORD) held the first eight
// and is only read, to translate it once; it is never written again, so an
// older build run afterwards still finds its own. Absent, it reads as 0.
[[nodiscard]] std::optional<std::uint64_t> LoadCompactArrangement() noexcept;
[[nodiscard]] bool SaveCompactArrangement(std::uint64_t packed, std::wstring& error);
[[nodiscard]] std::uint32_t LoadCompactLayout() noexcept;
[[nodiscard]] bool LoadCompactLocked() noexcept;
[[nodiscard]] bool SaveCompactLocked(bool locked, std::wstring& error);
[[nodiscard]] WindowPosition LoadMainWindowPosition() noexcept;
[[nodiscard]] bool SaveMainWindowPosition(int x, int y, std::wstring& error);
[[nodiscard]] localization::UiLanguage LoadUiLanguagePreference() noexcept;
[[nodiscard]] bool SaveUiLanguagePreference(localization::UiLanguage language,
                                            std::wstring& error);
// The rules live in ui_preferences.hpp.
[[nodiscard]] UiTheme LoadUiThemePreference() noexcept;
[[nodiscard]] bool SaveUiThemePreference(UiTheme theme, std::wstring& error);
[[nodiscard]] bool LoadOverlayEnabled() noexcept;
[[nodiscard]] bool SaveOverlayEnabled(bool enabled, std::wstring& error);
[[nodiscard]] bool LoadOverlayAutoSdr() noexcept;
[[nodiscard]] bool SaveOverlayAutoSdr(bool enabled, std::wstring& error);
[[nodiscard]] bool LoadOverlayHidesOsd() noexcept;
[[nodiscard]] bool SaveOverlayHidesOsd(bool enabled, std::wstring& error);
[[nodiscard]] OverlayHotkey LoadOverlayHotkey() noexcept;
[[nodiscard]] bool SaveOverlayHotkey(const OverlayHotkey& hotkey, std::wstring& error);
[[nodiscard]] HostsPreference LoadHostsPreference() noexcept;
[[nodiscard]] bool SaveHostsEnabled(bool enabled, std::wstring& error);
[[nodiscard]] bool SaveHostsName(std::wstring_view name, std::wstring& error);
[[nodiscard]] bool SaveHostsPort(int port, std::wstring& error);
[[nodiscard]] bool SaveHostsPeerId(std::uint64_t id, std::wstring& error);
[[nodiscard]] bool SaveHostsLog(bool on, std::wstring& error);
// The pairs in the reader's order, at most kMaxStoredHosts. An entry without a
// key is not a pair and is skipped.
[[nodiscard]] std::vector<StoredHost> LoadHosts();
// A new pair goes last in the reader's order; a known one keeps its place.
[[nodiscard]] bool SaveHost(const StoredHost& host, std::wstring& error);
[[nodiscard]] bool RemoveHost(std::uint64_t peer, std::wstring& error);
[[nodiscard]] bool SaveHostsOrder(const std::vector<std::uint64_t>& order, std::wstring& error);
[[nodiscard]] bool LoadFeatureNoticeSerial(std::uint32_t& serial) noexcept;
[[nodiscard]] bool SaveFeatureNoticeSerial(std::uint32_t serial, std::wstring& error);

}  // namespace gtg::settings
