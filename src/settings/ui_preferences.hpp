#pragma once

#include <windows.h>

#include <cstdint>
#include <string_view>

// Main-window preferences added by AF-20261004-main-ui-redesign, as pure rules
// so they can be tested without a registry or a window.
namespace gtg::settings {
inline constexpr std::uint32_t kFeatureNoticeSerial = 1;
[[nodiscard]] constexpr bool NeedsFeatureNotice(bool valid, std::uint32_t seen) noexcept {
    return !valid || seen != kFeatureNoticeSerial;
}

// ─── Theme ──────────────────────────────────────────────────────────────────

enum class UiTheme { Auto, Dark, Light };

// Stored as text (REG_SZ), like UiLanguage, so a value is readable in regedit
// and a future choice does not renumber the old ones.
[[nodiscard]] constexpr std::wstring_view RegistryValue(const UiTheme theme) noexcept {
    switch (theme) {
    case UiTheme::Dark: return L"dark";
    case UiTheme::Light: return L"light";
    case UiTheme::Auto: break;
    }
    return L"auto";
}

// Absent or unrecognised means Auto: a value written by a later version should
// follow Windows rather than force a look nobody chose here.
[[nodiscard]] constexpr UiTheme UiThemeFromRegistryValue(const std::wstring_view value) noexcept {
    if (value == L"dark") return UiTheme::Dark;
    if (value == L"light") return UiTheme::Light;
    return UiTheme::Auto;
}

// What the window is actually drawn as. Light is today's window, drawn by the
// system ("classic"); there is no third look.
enum class Appearance { Classic, Dark };

// The whole decision, in one place:
//   - high contrast always gets the system's colours, whatever was chosen;
//   - dark needs darkmodelib's undocumented uxtheme exports, and without them
//     a dark request would paint its own dark over light system parts, so it
//     falls back to classic rather than half-dark;
//   - Auto follows the Windows app mode.
[[nodiscard]] constexpr Appearance ResolveAppearance(const UiTheme preference,
                                                     const bool system_dark,
                                                     const bool dark_supported,
                                                     const bool high_contrast) noexcept {
    if (high_contrast || !dark_supported) return Appearance::Classic;
    switch (preference) {
    case UiTheme::Dark: return Appearance::Dark;
    case UiTheme::Light: return Appearance::Classic;
    case UiTheme::Auto: break;
    }
    return system_dark ? Appearance::Dark : Appearance::Classic;
}

// ─── Overlay hotkey ─────────────────────────────────────────────────────────
//
// Stored only. Nothing registers it until the in-game overlay lands on master
// (owner decision, 2026-10-04); the main window says so.

struct OverlayHotkey {
    UINT modifiers{};   // MOD_ALT | MOD_CONTROL | MOD_SHIFT | MOD_WIN
    UINT key{};         // virtual-key code

    friend constexpr bool operator==(const OverlayHotkey&, const OverlayHotkey&) = default;
};

// ALT+F10. The owner asked for ALT+F12, but RegisterHotKey's documentation
// reserves F12 for the debugger "at all times".
inline constexpr OverlayHotkey kDefaultOverlayHotkey{MOD_ALT, VK_F10};

inline constexpr UINT kHotkeyModifierMask = MOD_ALT | MOD_CONTROL | MOD_SHIFT | MOD_WIN;

// A hotkey this program would accept: at least one modifier (a bare key would
// steal that key from every game), nothing outside the four modifiers, a real
// key that is not itself a modifier, and never F12.
[[nodiscard]] constexpr bool IsAcceptableOverlayHotkey(const OverlayHotkey& hotkey) noexcept {
    if ((hotkey.modifiers & kHotkeyModifierMask) == 0) return false;
    if ((hotkey.modifiers & ~kHotkeyModifierMask) != 0) return false;
    if (hotkey.key == 0 || hotkey.key > 0xFE) return false;
    switch (hotkey.key) {
    case VK_F12:
    case VK_SHIFT: case VK_CONTROL: case VK_MENU:
    case VK_LSHIFT: case VK_RSHIFT: case VK_LCONTROL: case VK_RCONTROL:
    case VK_LMENU: case VK_RMENU: case VK_LWIN: case VK_RWIN:
        return false;
    default:
        return true;
    }
}

// One REG_DWORD: modifiers in the high word, key in the low word.
[[nodiscard]] constexpr std::uint32_t PackOverlayHotkey(const OverlayHotkey& hotkey) noexcept {
    return (static_cast<std::uint32_t>(hotkey.modifiers & 0xFFFFU) << 16) |
           (hotkey.key & 0xFFFFU);
}

// Anything absent or unacceptable reads as the default, never as a half-valid
// combination.
[[nodiscard]] constexpr OverlayHotkey ResolveOverlayHotkey(const bool has_valid_value,
                                                           const std::uint32_t packed) noexcept {
    if (!has_valid_value) return kDefaultOverlayHotkey;
    const OverlayHotkey hotkey{packed >> 16, packed & 0xFFFFU};
    return IsAcceptableOverlayHotkey(hotkey) ? hotkey : kDefaultOverlayHotkey;
}

// Default enables the hotkey only; actual injection still requires user action.
[[nodiscard]] constexpr bool ResolveOverlayEnabled(const bool has_valid_value,
                                                   const std::uint32_t value) noexcept {
    return !has_valid_value || value != 0;
}

// A compatibility assumption, not HDR detection. Only new sessions read it.
[[nodiscard]] constexpr bool ResolveOverlayAutoSdr(const bool has_valid_value,
                                                   const std::uint32_t value) noexcept {
    return !has_valid_value || value != 0;
}

}  // namespace gtg::settings
