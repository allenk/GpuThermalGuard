#pragma once

#include <windows.h>

#include "settings/ui_preferences.hpp"

// Recording a key combination from raw key events, as PowerToys' shortcut
// control does: modifiers are tracked as they go down and up, the first other
// key completes the combination, and pressing a modifier afterwards starts a
// new one. No window and no hook here, so it can be tested.
namespace gtg::tray::hotkey {

// MOD_* for a modifier key, 0 for any other key.
[[nodiscard]] constexpr UINT ModifierFor(const UINT vk) noexcept {
    switch (vk) {
    case VK_CONTROL: case VK_LCONTROL: case VK_RCONTROL: return MOD_CONTROL;
    case VK_MENU: case VK_LMENU: case VK_RMENU: return MOD_ALT;
    case VK_SHIFT: case VK_LSHIFT: case VK_RSHIFT: return MOD_SHIFT;
    case VK_LWIN: case VK_RWIN: return MOD_WIN;
    default: return 0;
    }
}

// Keys the dialog keeps for itself while recording, so it stays usable from
// the keyboard: Tab and Shift+Tab move focus, Enter saves, Escape cancels.
// With Ctrl, Alt or Win held they are recorded like any other key.
[[nodiscard]] constexpr bool PassesThrough(const UINT vk, const UINT held) noexcept {
    if ((held & (MOD_CONTROL | MOD_ALT | MOD_WIN)) != 0) return false;
    if (vk == VK_TAB) return true;
    return held == 0 && (vk == VK_RETURN || vk == VK_ESCAPE);
}

enum class Verdict {
    Empty,           // nothing pressed yet
    Incomplete,      // modifiers held, no key yet
    NeedsModifier,   // a key with no Ctrl/Alt/Shift/Win
    Reserved,        // F12: RegisterHotKey reserves it for the debugger
    Acceptable,      // valid as a combination; registration is checked separately
};

class Capture {
public:
    void KeyDown(const UINT vk) noexcept {
        const UINT modifier = ModifierFor(vk);
        if (modifier != 0) {
            // A fresh modifier after a finished combination starts over; an
            // auto-repeat of one already held does not.
            if (complete_ && (held_ & modifier) == 0) complete_ = false;
            held_ |= modifier;
            return;
        }
        combination_ = {held_, vk};
        complete_ = true;
    }

    void KeyUp(const UINT vk) noexcept { held_ &= ~ModifierFor(vk); }

    // Whatever is to be shown: the finished combination, or the modifiers
    // being held on the way to one.
    [[nodiscard]] settings::OverlayHotkey Shown() const noexcept {
        return complete_ ? combination_ : settings::OverlayHotkey{held_, 0};
    }

    // The window lost the keyboard: whatever was held is released somewhere
    // this capture will never hear about.
    void ReleaseModifiers() noexcept { held_ = 0; }

    [[nodiscard]] UINT Held() const noexcept { return held_; }
    [[nodiscard]] bool Complete() const noexcept { return complete_; }

    [[nodiscard]] Verdict Judge() const noexcept {
        if (!complete_) return held_ == 0 ? Verdict::Empty : Verdict::Incomplete;
        if (combination_.key == VK_F12) return Verdict::Reserved;
        if (!settings::IsAcceptableOverlayHotkey(combination_)) return Verdict::NeedsModifier;
        return Verdict::Acceptable;
    }

    // Starts from a stored combination, as if it had just been pressed.
    void Show(const settings::OverlayHotkey& hotkey) noexcept {
        combination_ = hotkey;
        complete_ = true;
        held_ = 0;
    }

private:
    settings::OverlayHotkey combination_{};
    UINT held_{};
    bool complete_{};
};

}  // namespace gtg::tray::hotkey
