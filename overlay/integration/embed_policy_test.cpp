// The embedded-OSD rule, one check per case the owner listed on 2026-10-08.
// AF-20261008-overlay-embedded-osd.
#include "embed_policy.hpp"

#include <array>
#include <cstdio>
#include <cstdlib>

namespace {
int failures = 0;
void Check(const bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL %s\n", message);
        ++failures;
    }
}
}  // namespace

int main() {
    using namespace gtg::overlay::integration;
    const Identity game{1000, 77};
    const Identity other_game{2000, 88};
    const Identity explorer{3000, 99};
    const EmbedSession drawing{game, true, true};

    // The rule.
    Check(OverlayOwnsForeground(std::array{drawing}, game), "drawing game in front owns it");
    Check(!OverlayOwnsForeground(std::array{drawing}, explorer),
          "ALT+TAB / Start menu / Task Manager: another process in front");
    Check(!OverlayOwnsForeground(std::array{drawing}, Identity{}),
          "Ctrl+Alt+Del: a null foreground is not a game");
    Check(!OverlayOwnsForeground(std::array{EmbedSession{game, false, true}}, game),
          "ALT+F10 off: a Hidden session does not embed");
    Check(!OverlayOwnsForeground(std::array{EmbedSession{game, true, false}}, game),
          "installing: nothing drawn yet, so the OSD stays");
    Check(!OverlayOwnsForeground(std::span<const EmbedSession>{}, game),
          "game exited or session failed: no session left");
    Check(!OverlayOwnsForeground(std::array{drawing}, Identity{1000, 78}),
          "a reused pid is not the same game");

    // Four sessions: whichever game is in front decides.
    const std::array four{EmbedSession{other_game, true, true}, EmbedSession{game, false, true},
                          EmbedSession{Identity{4000, 1}, true, false}, drawing};
    Check(OverlayOwnsForeground(four, game), "four sessions: the front one draws");
    Check(OverlayOwnsForeground(four, other_game), "four sessions: another drawing game");
    Check(!OverlayOwnsForeground(four, Identity{4000, 1}), "four sessions: installing one");

    // Timing is measured on the foreground (AF-20261008-overlay-o-button).
    ForegroundSettle f;
    Check(!f.Settled(1000, 10'000), "a new foreground is not settled");
    Check(!f.Settled(1000, 10'499), "499 ms is not settled");
    Check(f.Settled(1000, 10'500), "settled at 500 ms");
    Check(f.Settled(1000, 60'000),
          "ALT+F10 inside a settled game: already settled, so the OSD hides as soon as the "
          "overlay draws -- no flash");
    Check(!f.Settled(3000, 60'034), "ALT+TAB: the switcher is a new foreground");
    Check(!f.Settled(1000, 60'100), "a quick ALT+TAB back: not settled again yet");
    Check(!f.Settled(1000, 60'599), "still waiting");
    Check(f.Settled(1000, 60'600), "settled 500 ms after returning");
    Check(!f.Settled(0, 61'000), "Ctrl+Alt+Del: a null foreground restarts it too");

    if (failures == 0) std::puts("PASS embedded OSD policy");
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
