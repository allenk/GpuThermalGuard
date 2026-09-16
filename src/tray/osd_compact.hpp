#pragma once

#include <cstdint>
#include <algorithm>
#include <utility>

namespace gtg::tray::compact {

// Presentation-only policy shared by rendering, hit testing and native tests.
constexpr int Width(bool) noexcept { return 388; }
constexpr int Height(bool collapsed) noexcept { return collapsed ? 88 : 330; }
inline std::pair<double, double> SparkRange(double low, double high, double minimum_span) noexcept {
    const double span = std::max(minimum_span, (high - low) * 1.2);
    const double bottom = std::max(0.0, (low + high - span) / 2.0);
    return {bottom, bottom + span};
}
constexpr bool ToggleHit(int x, int y, int width, int header_height) noexcept {
    return x >= width - header_height && x < width && y >= 0 && y < header_height;
}

class Gesture {
public:
    void Press(bool inside) noexcept { pressed_ = inside; }
    void Cancel() noexcept { pressed_ = false; }
    bool Release(bool inside) noexcept {
        const bool activate = pressed_ && inside;
        pressed_ = false;
        return activate;
    }
private:
    bool pressed_{};
};

enum class Status { Fault, Protected, Unavailable, Delayed, Warning, Monitoring, Initializing };
constexpr Status Resolve(bool fault, bool protected_state, bool unavailable,
                         bool delayed, bool warning, bool armed) noexcept {
    if (fault) return Status::Fault;
    if (protected_state) return Status::Protected;
    if (unavailable) return Status::Unavailable;
    if (delayed) return Status::Delayed;
    if (warning) return Status::Warning;
    return armed ? Status::Monitoring : Status::Initializing;
}

constexpr unsigned char PulseAlpha(bool attention, bool animations,
                                   std::uint64_t now_ms) noexcept {
    if (!attention || !animations) return 255;
    // Slow four-second breath, sampled by the existing 500 ms presentation timer.
    const auto phase = now_ms % 4000;
    const auto ramp = phase <= 2000 ? phase : 4000 - phase;
    return static_cast<unsigned char>(160 + 95 * ramp / 2000);
}

}  // namespace gtg::tray::compact
