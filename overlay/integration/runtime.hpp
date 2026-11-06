#pragma once
#include <windows.h>

namespace gtg::tray {
class OsdOverlay;
}

namespace gtg::overlay::integration {
// UI-thread-only facade. No protection or MainDialog dependency.
void Start(tray::OsdOverlay& source) noexcept;
void Stop() noexcept;
// Turning the overlay on does not need the hotkey: when another program holds
// the combination it goes on without one, and the O button on the OSD presses
// it instead. False when it cannot be switched at all, or when a hotkey it
// holds cannot be replaced by the one asked for (the held one is kept).
bool Configure(bool enabled, unsigned modifiers = MOD_ALT, unsigned key = VK_F10) noexcept;

struct Status {
    bool enabled{};
    unsigned modifiers{}, key{}, installing{}, visible{}, hidden{}, failed{};
    // Whether Windows gave the overlay `modifiers`+`key`. On without it means
    // another program holds the combination.
    bool hotkey_held{};
};

Status GetStatus() noexcept;
// Whether the desktop OSD steps aside while a drawing session's game is in
// front. On by default.
void SetHidesOsd(bool hides) noexcept;
int HelperMain(const wchar_t* inherited_mapping) noexcept;
}  // namespace gtg::overlay::integration
