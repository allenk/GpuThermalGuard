#pragma once
#include <windows.h>

namespace gtg::tray {
class OsdOverlay;
}

namespace gtg::overlay::integration {
// UI-thread-only facade. No protection or MainDialog dependency.
void Start(tray::OsdOverlay& source) noexcept;
void Stop() noexcept;
bool Configure(bool enabled, unsigned modifiers = MOD_ALT, unsigned key = VK_F10) noexcept;

struct Status {
    bool enabled{};
    unsigned modifiers{}, key{}, installing{}, visible{}, hidden{}, failed{};
};

Status GetStatus() noexcept;
// Whether the desktop OSD steps aside while a drawing session's game is in
// front (AF-20261008-overlay-embedded-osd). On by default.
void SetHidesOsd(bool hides) noexcept;
int HelperMain(const wchar_t* inherited_mapping) noexcept;
}  // namespace gtg::overlay::integration
