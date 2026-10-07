#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace gtg::fps {

// Which way the target's frames reach the screen, from Win32k
// TokenStateChanged at InFrame and its IndependentFlip field.
//
// DisplayCorrelator follows composed flip: target token -> DWM frame -> VSync.
// Under independent flip the frames bypass DWM, and what the correlator still
// matches is a trickle that is not the display path. Measured on a DX11 game,
// 2026-10-07: every InFrame of the game's surface independent,
// ~248 kernel presents/s, ~26 correlated "displayed"/s -- and that trickle
// out-ranked the complete presented stream, so FPS read ~26. With a Game Bar
// widget pinned over it the same game was composed and displayed read 120,
// correctly.
//
// So while the target is on independent flip, correlated displayed frames are
// not offered to the rate tracker, and the reading falls back to presented --
// labelled presented, not passed off as displayed.
class PresentPath {
public:
    // Composition surfaces the target owns, from its own Win32k 201 tokens.
    void NoteTargetSurface(const std::uint64_t luid) noexcept {
        if (luid == 0 || IsTarget(luid)) return;
        surfaces_[next_] = luid;
        next_ = (next_ + 1) % surfaces_.size();
    }

    [[nodiscard]] bool IsTarget(const std::uint64_t luid) const noexcept {
        if (luid == 0) return false;
        for (const auto surface : surfaces_)
            if (surface == luid) return true;
        return false;
    }

    // An InFrame for one of the target's surfaces. The latest one decides: a
    // program moves between the paths when something is laid over it.
    void NoteInFrame(const std::uint64_t luid, const bool independent_flip) noexcept {
        if (IsTarget(luid)) independent_flip_ = independent_flip;
    }

    [[nodiscard]] bool IndependentFlip() const noexcept { return independent_flip_; }

    // Whether an InFrame goes on to the DWM correlator. A target frame on
    // independent flip does not: DWM still reports a surface update (196) for
    // it but never flips, so marking it in-frame queued it for a DWM frame that
    // never came, and 32 such frames overflowed the correlator's waiting queue
    // and stopped the whole observer -- presented included. Measured on a
    // DX11 game, 2026-10-07: the correlator failed at DWM 196 within ~34 presents,
    // every session, whenever nothing else on the desktop made DWM compose.
    [[nodiscard]] bool ForwardsInFrame(const std::uint64_t luid,
                                       const bool independent_flip) const noexcept {
        return !(independent_flip && IsTarget(luid));
    }

    // Whether a frame the DWM correlator matched may count as displayed.
    [[nodiscard]] bool CountsAsDisplayed() const noexcept { return !independent_flip_; }

    void Reset() noexcept { *this = PresentPath{}; }

private:
    // A swapchain rarely has more than one surface; a handful covers
    // recreation without growing.
    std::array<std::uint64_t, 4> surfaces_{};
    std::size_t next_{};
    bool independent_flip_{};
};

}  // namespace gtg::fps
