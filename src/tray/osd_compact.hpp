#pragma once

#include <cstdint>
#include <algorithm>
#include <utility>

namespace gtg::tray::compact {

// Presentation-only policy shared by rendering, hit testing and native tests.
constexpr int Width(bool) noexcept { return 388; }
constexpr int Height(bool collapsed) noexcept { return collapsed ? 88 : 330; }
struct Footprint {
    int width{};
    int height{};
    int columns{};
    int rows{1};
};

// Record order is fixed. FPS is always the final cell and RAM sits immediately
// before it, in every combination of the two preferences.
enum class Metric { Temperature, Power, Vram, Gpu, Cpu, Ram, Fps };

inline constexpr int kCellsPerRow = 5;
// Tuned collapsed geometry: rows are pitched 55 dip apart and two
// rows occupy 142 dip in total (30 header + 55 + 51 cell + 6 margin). These are
// deliberately two separate numbers, not one stride — do not merge them.
inline constexpr int kCollapsedRowPitch = 55;
inline constexpr int kCollapsedRowHeightIncrement = 54;

constexpr int CellCount(bool show_ram, bool show_fps) noexcept {
    return kCellsPerRow + (show_ram ? 1 : 0) + (show_fps ? 1 : 0);
}

// Returns -1 when the record is not shown.
constexpr int CellIndexOf(Metric metric, bool show_ram, bool show_fps) noexcept {
    switch (metric) {
        case Metric::Temperature: return 0;
        case Metric::Power: return 1;
        case Metric::Vram: return 2;
        case Metric::Gpu: return 3;
        case Metric::Cpu: return 4;
        case Metric::Ram: return show_ram ? kCellsPerRow : -1;
        case Metric::Fps:
            return show_fps ? kCellsPerRow + (show_ram ? 1 : 0) : -1;
    }
    return -1;
}

// Expanded-lane geometry. Every record — the five telemetry lanes, RAM and
// FPS — must derive its plot rectangle from here. Duplicating these numbers at
// each call site is what let the lanes drift out of alignment.
inline constexpr float kLaneRowLeftDip = 6.0F;
inline constexpr float kLaneLabelLeftDip = 8.0F;
inline constexpr float kLaneLabelWidthDip = 84.0F;
inline constexpr float kLaneGraphLeftDip = 92.0F;
inline constexpr float kLaneGraphInsetDip = 100.0F;  // left + right inset

// Lane internals scale with the row's own height, not with the window DPI, so
// that a lane shrinks proportionally as records are added. Every record must
// derive its scale here: mixing this with the raw DPI scale is what made the
// RAM and FPS plots start right of the telemetry plots once seven records
// shortened the rows.
inline constexpr float kLaneNominalHeightDip = 60.0F;

constexpr float LaneScale(float row_height) noexcept {
    return row_height / kLaneNominalHeightDip;
}

struct LaneSpan {
    float x{};
    float width{};
};

constexpr LaneSpan LaneGraphSpan(float row_x, float row_width,
                                 float scale) noexcept {
    return {row_x + kLaneGraphLeftDip * scale,
            row_width - kLaneGraphInsetDip * scale};
}

constexpr LaneSpan LaneLabelSpan(float row_x, float scale) noexcept {
    return {row_x + kLaneLabelLeftDip * scale, kLaneLabelWidthDip * scale};
}

constexpr int RowCount(bool show_ram, bool show_fps) noexcept {
    return (CellCount(show_ram, show_fps) + kCellsPerRow - 1) / kCellsPerRow;
}

// The overlay grows downward, never sideways: a preference toggle changes its
// height but never its width, so a window already positioned stays put.
constexpr Footprint ChooseFootprint(bool collapsed, bool show_ram, bool show_fps,
                                    int work_width_dip,
                                    int work_height_dip) noexcept {
    const int cells = CellCount(show_ram, show_fps);
    const int rows = RowCount(show_ram, show_fps);
    if (collapsed) {
        const int height = 88 + (rows - 1) * kCollapsedRowHeightIncrement;
        if (work_width_dip >= 388) return {388, height, kCellsPerRow, rows};
        return {std::min(320, std::max(1, work_width_dip)), height, 3, rows};
    }
    // Expanded stays at most half the work-area height, as before.
    const int height = Height(false) + (cells - kCellsPerRow) * 25;
    if (work_height_dip >= height * 2 && work_width_dip >= 388)
        return {388, height, 1, 1};
    return {std::min(320, std::max(1, work_width_dip)),
            190 + (cells - kCellsPerRow) * 25, 2, 1};
}
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
