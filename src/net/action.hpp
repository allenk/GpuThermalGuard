#pragma once

#include <cstdint>
#include <format>
#include <string>

#include "net/estats_win32.hpp"
#include "net/verdict.hpp"

// What the network action shows, and for how long.
//
// Separated from `verdict.hpp` because that file judges and this one presents:
// the thresholds are a claim about networks, these are a claim about what a
// reader can take in from a cell 72 dip wide. Both are pure, so both are
// testable without a network or a window.
namespace gtg::net::action {

// A window, not an instant. SmoothedRtt is an estimate the stack already
// maintains and is available the moment the connection is found, but the loss
// counters mean nothing until they have been differenced against something --
// so the action watches for a while before it reads.
//
// 2.5 s is the compromise between the two: long enough for a connection
// carrying traffic to take several RTT samples and to show a retransmission if
// the path is dropping, short enough that nobody in a game watches rain where
// they expect a number. The research harness accumulated for 8 s, which was
// right for observing and would be wrong here.
inline constexpr std::uint64_t kMeasureWindowMs = 2500;

// The floor, for the same reason the reclaim has one: work that finishes
// faster than a reader perceives must not flash past and invite a second
// click while the first is still settling. Below the window above, so it only
// ever matters when the probe fails early and there is nothing to wait for.
inline constexpr std::uint64_t kMinimumVisibleMs = 900;

// Longer than the reclaim's 1600 ms. That one reports a single number; this
// reports a number, a colour and a mark, and a reader who has to learn what
// the mark means needs a moment more than a reader reading "+2.5 GB".
inline constexpr std::uint64_t kResultHoldMs = 2200;

// The failsafe, not the plan. The worker is bounded by the window above plus
// two enumerations of the connection table; if it ever overran even this, the
// indicator must still come down rather than live on the reader's screen.
inline constexpr std::uint64_t kWorkBudgetMs = 6000;

// How many bars of three are filled. Chosen over an emoji: signal bars are
// already the universal shorthand for this, they survive being 11 dip wide,
// and they say which way is better without the reader learning a palette.
// Zero means the mark is drawn empty -- nothing was measured, which is a
// different statement from "measured, and bad".
[[nodiscard]] constexpr int BarsFor(const Grade grade) noexcept {
    switch (grade) {
        case Grade::Good: return 3;
        case Grade::Fair: return 2;
        case Grade::Poor: return 1;
        case Grade::Unknown: break;
    }
    return 0;
}

// The same three colours the reclaim result uses, so green keeps meaning
// healthy across the dashboard instead of meaning one thing per cell.
[[nodiscard]] constexpr std::uint32_t TintFor(const Grade grade) noexcept {
    switch (grade) {
        case Grade::Good: return 0xFF4BD68BU;
        case Grade::Fair: return 0xFFE2C15AU;
        case Grade::Poor: return 0xFFFF5B5EU;
        case Grade::Unknown: break;
    }
    return 0xB4CDD6E2U;
}

// Why there is no verdict, in the fewest words that are still true.
//
// Each of these is a different situation and the reader can act on the
// difference, so each gets its own words. An earlier draft spelled the first
// two both as "idle"; the first real use hit one of them and the cell could
// not say which, so the reader had nothing to act on. Collapsing them all to
// "--" would be tidier still and would tell them even less.
//
// "no TCP" rather than "no NET", deliberately. What was established is that
// the program holds no IPv4 TCP connection -- it may well be sending hard over
// UDP, which is what most games do, or over IPv6, which this does not yet
// look at. Saying it has no network would claim something that was never
// measured, and that is the same rule the attempt is held to.
[[nodiscard]] inline std::wstring StatusText(const ProbeStatus status) {
    switch (status) {
        case ProbeStatus::Ready: return L"--";
        case ProbeStatus::NoLibrary: return L"n/a";
        case ProbeStatus::NoForeground: return L"no app";
        case ProbeStatus::NoConnections: return L"no TCP";
        case ProbeStatus::NotPermitted: return L"denied";
    }
    return L"--";
}

// The figure. Latency in milliseconds, because that is the number a player
// already has an opinion about -- and it is the latency of the connection the
// grade came from, so the mark and the number describe the same path.
//
// No decimals at any magnitude. SmoothedRtt is an estimate with its own
// variance, so a tenth of a millisecond on it would be precision the reading
// does not have; and the cell has already established that its numbers are
// whole.
[[nodiscard]] inline std::wstring ResultText(const Verdict& verdict,
                                             const ProbeStatus status) {
    if (status != ProbeStatus::Ready || verdict.grade == Grade::Unknown) {
        return StatusText(status);
    }
    return std::format(L"{} ms", verdict.rtt_ms);
}

}  // namespace gtg::net::action
