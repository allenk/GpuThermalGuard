#pragma once
// When the in-game overlay replaces the desktop OSD. Pure, so every case the
// owner listed is a unit test rather than a manual run.
// AF-20261008-overlay-embedded-osd; specs/changes/2026-10-08-overlay-embedded-osd/.
#include "session_policy.hpp"
#include <cstdint>
#include <span>

namespace gtg::overlay::integration {

// One session as the rule sees it.
struct EmbedSession {
    Identity target;
    bool visible{};    // State::Visible: the user has the overlay on in this game
    bool published{};  // it has drawn at least once, so hiding loses nothing
};

// The game the user is in: some session is really drawing in the process that
// owns the foreground window. A null foreground (the secure desktop behind
// Ctrl+Alt+Del) or an unrelated one is not a game. Up to four sessions; any
// one of them in front is enough.
[[nodiscard]] constexpr bool OverlayOwnsForeground(std::span<const EmbedSession> sessions,
                                                   const Identity& foreground) noexcept {
    if (foreground.pid == 0) return false;
    for (const auto& s : sessions)
        if (s.visible && s.published && s.target == foreground) return true;
    return false;
}

// How long the program in front has stayed in front. The OSD hides only once
// the game has held the foreground for kSettleMs; it shows again the moment
// that stops being true. The settle exists for ALT+TAB: the switcher takes the
// foreground as soon as it appears, so a quick ALT+TAB back into the game
// would otherwise flash the OSD. Measured on the foreground, not on the rule:
// ALT+F10 pressed inside a game the user has been in for a while hides the
// OSD as soon as the overlay draws (AF-20261008-overlay-o-button). By pid
// alone, because a game's creation time is read only once it has a session,
// and gaining one must not look like a change of foreground.
class ForegroundSettle {
public:
    static constexpr std::uint64_t kSettleMs = 500;

    [[nodiscard]] bool Settled(const std::uint32_t foreground_pid,
                               const std::uint64_t now_ms) noexcept {
        if (foreground_pid != pid_ || since_ms_ == 0) {
            pid_ = foreground_pid;
            since_ms_ = now_ms == 0 ? 1 : now_ms;
        }
        return now_ms >= since_ms_ && now_ms - since_ms_ >= kSettleMs;
    }

private:
    std::uint32_t pid_{};
    std::uint64_t since_ms_{};
};

}  // namespace gtg::overlay::integration
