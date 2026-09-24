#pragma once

#include <algorithm>

// Initial power settings, derived from the card rather than assumed.
//
// The defaults in `ProtectionConfig` are constants, and constants cannot be
// right on two different cards: 350 W is unwritable on a 320 W card and is a
// 42 % cut on a 600 W one. Everything here is arithmetic over four numbers
// NVML already reports, so it is testable without a GPU.
namespace gtg {

// What the card says about its own power limit, in watts.
struct PowerEnvelope {
    int minimum_w{};
    int maximum_w{};
    int current_w{};   // the limit in force right now
    int default_w{};   // the limit the card ships with
};

struct PowerDefaults {
    int working_w{};
    int safe_w{};
};

// Four readings that must all be present and sane before anything is derived.
// A partial envelope is not repaired here: a caller that cannot answer these
// keeps the built-in defaults and says so, rather than inventing a limit.
[[nodiscard]] constexpr bool IsUsable(const PowerEnvelope& envelope) noexcept {
    return envelope.minimum_w > 0 && envelope.maximum_w >= envelope.minimum_w &&
           envelope.current_w > 0 && envelope.default_w > 0;
}

// Working is the limit already in force, never the default.
//
// Deriving it from `default` would *raise* the limit of anyone who had
// deliberately lowered it, on first run, without being asked. Taking what the
// card is already set to cannot surprise anyone.
//
// Safe is the lower of default and current, which is not the same as "safe =
// default". On a card nobody has touched the two are equal, and the pair comes
// out identical -- protection with no headroom, which is the intended
// first-run state. But a card whose default sits *above* its current limit
// (measured: 600 W default, 350 W current) would otherwise get a safe power
// higher than its working limit, which is worse than equal.
//
// Both are clamped into the card's own range, and safe can never exceed
// working, so the result is always a configuration the validator accepts.
[[nodiscard]] constexpr PowerDefaults Derive(const PowerEnvelope& envelope) noexcept {
    const int working = std::clamp(envelope.current_w, envelope.minimum_w,
                                   envelope.maximum_w);
    const int lower = envelope.default_w < envelope.current_w ? envelope.default_w
                                                              : envelope.current_w;
    const int safe = std::clamp(lower, envelope.minimum_w, working);
    return {working, safe};
}

// The firmware's own thermal ladder, in Celsius. Only the slowdown point is
// used; the other two are carried because a reader comparing numbers wants to
// see the ladder that one sits in.
struct ThermalEnvelope {
    int max_c{};        // vendor's highest normal operating temperature
    int slowdown_c{};   // where the firmware begins throttling
    int shutdown_c{};   // where it cuts power
};

// One degree below the point at which the firmware would act.
//
// This tool exists to get in before the driver and firmware strategy does, and
// `slowdown` is precisely where that strategy starts. A degree earlier is the
// latest moment that is still "before".
//
// On both cards measured this lands *above* the vendor's stated maximum
// operating temperature (93 max / 95 slowdown, and 90 / 94). That is
// deliberate and was the owner's decision: the guard should not interfere with
// ordinary use, only pre-empt the firmware.
//
// `fallback` is returned when the card reports no slowdown point, which is the
// only honest answer -- a threshold invented from nothing would be worse than
// the constant it replaced.
[[nodiscard]] constexpr int DeriveTriggerTemperature(const ThermalEnvelope& envelope,
                                                     int fallback) noexcept {
    if (envelope.slowdown_c <= 0) return fallback;
    // ValidateConfig accepts 40..100. A card reporting a slowdown outside that
    // is reporting nonsense, and the constant is safer than the reading.
    const int derived = envelope.slowdown_c - 1;
    return derived >= 40 && derived <= 100 ? derived : fallback;
}

// The highest trigger a reader may set. At or above the slowdown point a
// trigger cannot pre-empt anything, because the firmware is already acting --
// so that is the line, rather than the maximum operating temperature.
[[nodiscard]] constexpr int TriggerCeiling(const ThermalEnvelope& envelope) noexcept {
    return envelope.slowdown_c > 0 ? envelope.slowdown_c - 1 : 0;
}

// Whether a derived pair leaves any power to give up. Equal limits mean the
// guard can observe and record but has nothing to apply -- see
// `IsMonitorOnly` in core/protection.hpp, which is the same question asked of
// a config rather than of a card.
[[nodiscard]] constexpr bool HasHeadroom(const PowerDefaults& defaults) noexcept {
    return defaults.safe_w < defaults.working_w;
}

}  // namespace gtg
