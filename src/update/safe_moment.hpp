#pragma once

#include <optional>

// When a staged update may be handed to the script. From the old Tray's exit
// to the new one's ready, nothing protects the GPU, so the hand-off waits for a
// moment when that costs nothing: protection idle, the GPU well below its
// trigger, and no game drawing the overlay. Pure, so a test pins every edge.
namespace gtg::update {

// How far below the trigger the GPU must be.
inline constexpr double kSafeMarginC = 15.0;

struct SafeInputs {
    // Armed, or monitoring only; not latched, not near the trigger, not
    // without a sensor, not in fault.
    bool protection_idle{};
    // The latest reading, only if it is fresh.
    std::optional<double> temperature_c;
    int trigger_c{};
    // Overlay sessions installing, visible or hidden.
    unsigned overlay_sessions{};
    // This machine has no NVIDIA GPU: no protection to interrupt and no
    // temperature to wait for.
    bool no_gpu{};
};

enum class Wait {
    None,
    Protection,
    Temperature,
    Overlay,
};

[[nodiscard]] constexpr Wait SafeMoment(const SafeInputs& in) noexcept {
    if (!in.no_gpu) {
        if (!in.protection_idle) return Wait::Protection;
        if (!in.temperature_c || *in.temperature_c > in.trigger_c - kSafeMarginC) {
            return Wait::Temperature;
        }
    }
    if (in.overlay_sessions > 0) return Wait::Overlay;
    return Wait::None;
}

}  // namespace gtg::update
