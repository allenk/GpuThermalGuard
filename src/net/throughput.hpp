#pragma once

#include <array>
#include <cstdint>
#include <optional>

// Network throughput, as arithmetic.
//
// Counters in, bytes per second out, plus the unit a pair of rates should be
// shown in. No Win32 here: the counters arrive from `throughput_win32.hpp` and
// everything that decides what a reader sees is testable without an adapter.
namespace gtg::net {

// What the adapter reports. Both counters are cumulative since the interface
// came up, so a rate is a difference between two readings.
//
// The interface and its link speed travel with the counters because both are
// needed to decide whether a difference means anything, and a caller holding
// only two numbers cannot know.
struct Counters {
    std::uint64_t received_bytes{};
    std::uint64_t sent_bytes{};
    std::uint64_t monotonic_ms{};
    // Which adapter these came from. A difference across two adapters is a
    // number no link ever carried.
    std::uint32_t interface_index{};
    // Bits per second the adapter says it can carry. Zero means it did not
    // say, and then no ceiling is applied.
    std::uint64_t receive_link_speed_bps{};
    std::uint64_t transmit_link_speed_bps{};
};

// A reading above what the hardware can carry did not happen.
//
// Measured on this machine: the default route adapter reports 1 Gb/s, which is
// 119.2 MB/s. The owner saw the cell show hundreds of MB/s for the first few
// seconds after launch -- a rate that link is physically incapable of. The
// link speed is the adapter's own statement of what is possible, which makes
// it the right oracle: not a guess about what looks reasonable.
//
// The tolerance covers measurement jitter, not disbelief. The elapsed time is
// a 15.6 ms-resolution tick and the counters update on their own schedule, so
// a difference can straddle slightly more traffic than the window it is
// divided by.
inline constexpr double kLinkSpeedTolerance = 1.25;
// Some adapters report nothing, or a sentinel. Neither is a ceiling.
inline constexpr std::uint64_t kImplausibleLinkSpeedBps = 10'000'000'000'000ULL;

[[nodiscard]] constexpr bool ExceedsLink(const double bytes_per_second,
                                         const std::uint64_t link_speed_bps) noexcept {
    if (link_speed_bps == 0 || link_speed_bps >= kImplausibleLinkSpeedBps)
        return false;
    return bytes_per_second >
           static_cast<double>(link_speed_bps) / 8.0 * kLinkSpeedTolerance;
}

struct Rates {
    double received_bytes_per_second{};
    double sent_bytes_per_second{};
};

// A reading is only a rate once there is a previous one to subtract, and only
// a trustworthy rate if nothing happened in between that makes the difference
// meaningless.
//
// Three things make it meaningless, and all three are refused rather than
// papered over:
//
//  - no time passed, so the division has no answer;
//  - the counter went backwards, which means the adapter reset or was
//    replaced, and the difference would be a number the link never carried;
//  - the gap is so long that the average says nothing about now. A reader
//    glancing at the cell is asking about the last moment, not the last
//    minute;
//  - the two readings came from different adapters, which happens while the
//    routing table settles after launch and whenever a link changes;
//  - the result is faster than the adapter says it can go.
inline constexpr std::uint64_t kMaximumGapMs = 5'000;

[[nodiscard]] constexpr std::optional<Rates> RateBetween(
    const Counters& previous, const Counters& current) noexcept {
    if (current.monotonic_ms <= previous.monotonic_ms) return std::nullopt;
    const std::uint64_t elapsed = current.monotonic_ms - previous.monotonic_ms;
    if (elapsed > kMaximumGapMs) return std::nullopt;
    if (current.received_bytes < previous.received_bytes ||
        current.sent_bytes < previous.sent_bytes)
        return std::nullopt;
    // Two adapters' counters have nothing to do with each other. This is not a
    // theoretical case: the route can resolve to a different interface while
    // the stack settles after launch, and the difference would be a rate the
    // link never carried.
    if (current.interface_index != previous.interface_index) return std::nullopt;
    const double seconds = static_cast<double>(elapsed) / 1000.0;
    const Rates rates{
        static_cast<double>(current.received_bytes - previous.received_bytes) / seconds,
        static_cast<double>(current.sent_bytes - previous.sent_bytes) / seconds};
    if (ExceedsLink(rates.received_bytes_per_second, current.receive_link_speed_bps) ||
        ExceedsLink(rates.sent_bytes_per_second, current.transmit_link_speed_bps))
        return std::nullopt;
    return rates;
}

// The unit both directions are shown in.
//
// One unit for the pair, not one each: two numbers carrying their own suffixes
// need 79 dip of a 64 dip cell row and do not fit. Measured, not assumed.
enum class Unit { Bytes, Kilobytes, Megabytes, Gigabytes };

inline constexpr std::array<double, 4> kUnitDivisor{1.0, 1024.0, 1024.0 * 1024.0,
                                                    1024.0 * 1024.0 * 1024.0};

[[nodiscard]] constexpr const wchar_t* UnitSuffix(const Unit unit) noexcept {
    switch (unit) {
        case Unit::Bytes: return L"B/s";
        case Unit::Kilobytes: return L"KB/s";
        case Unit::Megabytes: return L"MB/s";
        case Unit::Gigabytes: return L"GB/s";
    }
    return L"B/s";
}

// One unit, always, everywhere network is shown.
//
// An adaptive unit was tried first, chosen from the peak over the drawn window
// so it would at least be stable. It still fails, and the owner named exactly
// why: 「這麼小的 CELL 沒人會注意到單位不同。只會覺得很怪」.
//
// That is the whole argument, and it generalises past this record. Nobody
// re-reads a four-character suffix in a 72 dip cell. They read the number and
// compare it against the number they saw a moment ago. When the unit changes
// underneath that comparison, the reading silently becomes a thousand times
// wrong -- and nothing on the cell tells them, because the thing that changed
// is the part they stopped looking at after the first glance.
//
// A fixed unit removes the reading task instead of making it easier. Megabytes
// per second, because that is the scale of the traffic that matters during a
// game: a background download eating the link shows as 12.4, and a quiet link
// shows as 0.1, and both of those are read correctly by someone who never
// looks at the suffix at all.
inline constexpr Unit kDisplayUnit = Unit::Megabytes;

// Tenths of the chosen unit, which is what the cell prints with one decimal.
//
// Rounded **up**, never down: at a shared unit a small reading can round to
// zero, and a cell reading `0.0` on a live link says nothing is happening,
// which is false. The overstatement is bounded at one tenth of one unit, and
// the curve below the number is not rounded at all.
//
// Exactly zero still prints zero. The rule is about not losing a real reading,
// not about inventing one -- the same distinction the telemetry code already
// makes when it refuses to invent zero for missing data.
[[nodiscard]] constexpr int TenthsIn(const double bytes_per_second,
                                     const Unit unit) noexcept {
    if (!(bytes_per_second > 0.0)) return 0;
    const double scaled =
        bytes_per_second * 10.0 / kUnitDivisor[static_cast<std::size_t>(unit)];
    const auto whole = static_cast<int>(scaled);
    const int rounded = static_cast<double>(whole) < scaled ? whole + 1 : whole;
    return rounded < 1 ? 1 : rounded;
}

// Whole units, for the compact cell.
//
// The cell has 64 dip for both numbers and a reader glancing at it is asking
// roughly how much, not exactly -- 「CI 其實不用那麼較真」. A decimal costs
// four dip per direction and buys precision nobody is reading at that size.
//
// Same ceiling as `TenthsIn`, and for the same reason: a live direction never
// renders as zero.
[[nodiscard]] constexpr int WholeIn(const double bytes_per_second,
                                    const Unit unit) noexcept {
    if (!(bytes_per_second > 0.0)) return 0;
    const double scaled =
        bytes_per_second / kUnitDivisor[static_cast<std::size_t>(unit)];
    const auto whole = static_cast<int>(scaled);
    const int rounded = static_cast<double>(whole) < scaled ? whole + 1 : whole;
    return rounded < 1 ? 1 : rounded;
}

// The span both curves are drawn against.
//
// One span for the pair. Given their own ranges, 12 MB/s of receive and
// 800 B/s of send would be drawn the same height, which is the one lie this
// cell exists to avoid telling.
[[nodiscard]] constexpr double SharedPeak(const Rates& rates) noexcept {
    return rates.received_bytes_per_second > rates.sent_bytes_per_second
               ? rates.received_bytes_per_second
               : rates.sent_bytes_per_second;
}

}  // namespace gtg::net
