#pragma once

#include <cstdint>
#include <algorithm>
#include <array>
#include <cstddef>
#include <optional>
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

// The five thermal records cannot be switched off, so five is both their
// count and the smallest first row any arrangement may have.
inline constexpr int kCellsPerRow = 5;
inline constexpr int kCellStrideDip = 76;
inline constexpr int kCellWidthDip = 72;
inline constexpr int kCellsMarginDip = 6;
// Tuned collapsed geometry: rows are pitched 55 dip apart and two
// rows occupy 142 dip in total (30 header + 55 + 51 cell + 6 margin). These are
// deliberately two separate numbers, not one stride -- do not merge them.
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

// Expanded-lane geometry. Every record -- the five telemetry lanes, RAM and
// FPS -- must derive its plot rectangle from here. Duplicating these numbers at
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

// ---------------------------------------------------------------------------
// Arrangement.
//
// An arrangement is an order over every record plus a count of records in the
// first row. Placement walks the order, skips records whose preference is off,
// and gives the first `row1` of the remainder to row one. Backfill when a
// record is disabled, and the forced single row at five records, both fall out
// of that rule rather than needing their own logic.
//
// A stored arrangement must never be able to hide a record, so the stored form
// is one 32-bit value validated by pure integer arithmetic: seven three-bit
// record indices and a three-bit row count. Anything that is not a permutation
// of all seven with a legal row count is discarded whole.
// ---------------------------------------------------------------------------

inline constexpr int kRecordCount = 7;

using Order = std::array<Metric, kRecordCount>;

constexpr Order DefaultOrder() noexcept {
    return {Metric::Temperature, Metric::Power, Metric::Vram, Metric::Gpu,
            Metric::Cpu, Metric::Ram, Metric::Fps};
}

constexpr bool IsValidOrder(const Order& order) noexcept {
    unsigned seen = 0;
    for (const Metric metric : order) {
        const auto index = static_cast<unsigned>(metric);
        if (index >= static_cast<unsigned>(kRecordCount)) return false;
        const unsigned bit = 1u << index;
        if (seen & bit) return false;
        seen |= bit;
    }
    return seen == (1u << kRecordCount) - 1u;
}

// The dashboard starts as a single row of every enabled record. `Resolve`
// clamps a first row longer than the records shown, so one value covers all
// three counts: 1x5, 1x6 and 1x7. A second row exists only once a reader
// arranges one.
inline constexpr int kDefaultRowOne = kRecordCount;

struct Layout {
    Order order{DefaultOrder()};
    int row1{kDefaultRowOne};
};

constexpr bool IsValidLayout(const Layout& layout) noexcept {
    return IsValidOrder(layout.order) && layout.row1 >= kCellsPerRow &&
           layout.row1 <= kRecordCount;
}

struct Placement {
    std::array<Metric, kRecordCount> cells{};
    int count{};
    int row1{};
    int rows{1};
};

constexpr bool IsOptional(Metric metric) noexcept {
    return metric == Metric::Ram || metric == Metric::Fps;
}

// The general form: `enabled` answers whether a record is shown. The five
// thermal records cannot be switched off, which is why row one can never fall
// below `kCellsPerRow`.
template <class Enabled>
constexpr Placement Resolve(const Layout& layout, Enabled&& enabled) noexcept {
    Placement placed;
    for (const Metric metric : layout.order) {
        if (!enabled(metric)) continue;
        placed.cells[static_cast<std::size_t>(placed.count++)] = metric;
    }
    placed.row1 = layout.row1 < kCellsPerRow ? kCellsPerRow : layout.row1;
    if (placed.row1 > placed.count) placed.row1 = placed.count;
    placed.rows = placed.count > placed.row1 ? 2 : 1;
    return placed;
}

constexpr Placement Resolve(const Layout& layout, bool show_ram,
                            bool show_fps) noexcept {
    return Resolve(layout, [show_ram, show_fps](const Metric metric) {
        if (metric == Metric::Ram) return show_ram;
        if (metric == Metric::Fps) return show_fps;
        return true;
    });
}

// Overload used by tests to disable a mandatory record and prove that backfill
// is a property of the rule rather than of the two optional preferences.
template <class Enabled>
constexpr Placement Resolve(const Layout& layout, bool, bool,
                            Enabled&& enabled) noexcept {
    return Resolve(layout, static_cast<Enabled&&>(enabled));
}

// Move-insert: the dropped record takes the target slot and the rest shift.
// Out-of-range indices are refused rather than clamped -- a bad index is a bug,
// not an intent to move to the nearest legal slot.
constexpr Order MoveInsert(Order order, int from, int to) noexcept {
    if (from < 0 || from >= kRecordCount || to < 0 || to >= kRecordCount ||
        from == to)
        return order;
    const Metric moved = order[static_cast<std::size_t>(from)];
    if (from < to) {
        for (int i = from; i < to; ++i)
            order[static_cast<std::size_t>(i)] =
                order[static_cast<std::size_t>(i + 1)];
    } else {
        for (int i = from; i > to; --i)
            order[static_cast<std::size_t>(i)] =
                order[static_cast<std::size_t>(i - 1)];
    }
    order[static_cast<std::size_t>(to)] = moved;
    return order;
}

// Dropping a cell. Row membership is a consequence of the order position and
// the row-one count rather than state of its own, so each requested behaviour
// reduces to one rule:
//
//   dragged within a row   -> reorder only, rows untouched
//   dragged row 1 -> row 2 -> the first row shrinks by one, opening a second
//   dragged row 2 -> row 1 -> the first row grows by one
//   first row reaches the record count -> the second row disappears
//   the first row never falls below kCellsPerRow
constexpr Layout ApplyDrop(const Layout& current, const Placement& placed,
                           int from_slot, int drop_row,
                           int drop_column) noexcept {
    if (from_slot < 0 || from_slot >= placed.count) return current;
    if (drop_row < 0 || drop_row > 1 || drop_column < 0) return current;

    const int from_row = from_slot < placed.row1 ? 0 : 1;
    Layout next = current;

    if (drop_row != from_row) {
        next.row1 = drop_row == 1 ? current.row1 - 1 : current.row1 + 1;
        if (next.row1 < kCellsPerRow) return current;   // row one may not shrink further
        if (next.row1 > placed.count) return current;   // nor exceed the records shown
    }

    const int row_base = drop_row == 0 ? 0 : next.row1;
    const int row_size = drop_row == 0
        ? next.row1
        : placed.count - next.row1;
    if (row_size <= 0) return current;
    const int clamped_column = drop_column >= row_size ? row_size - 1 : drop_column;
    int target_slot = row_base + clamped_column;
    if (target_slot >= placed.count) target_slot = placed.count - 1;

    // Translate visual slots to order positions: disabled records sit between
    // them and must keep their relative places.
    const auto order_index = [&](const Metric metric) {
        for (int i = 0; i < kRecordCount; ++i)
            if (current.order[static_cast<std::size_t>(i)] == metric) return i;
        return 0;
    };
    next.order = MoveInsert(
        current.order,
        order_index(placed.cells[static_cast<std::size_t>(from_slot)]),
        order_index(placed.cells[static_cast<std::size_t>(target_slot)]));
    return next;
}

// ---------------------------------------------------------------------------
// Cell geometry. Rendering and hit testing both derive from here: the expanded
// lanes once kept two copies of their plot rectangle and drifted apart, which
// is a mistake worth not repeating one level down.
// ---------------------------------------------------------------------------

inline constexpr float kCellHeightDip = 51.0F;
inline constexpr float kCellsTopDip = 30.0F;

struct CellGrid {
    float cell_width_dip{static_cast<float>(kCellWidthDip)};
    float stride_dip{static_cast<float>(kCellStrideDip)};
    int per_row{kCellsPerRow};
    float scale{1.0F};
};

constexpr CellGrid MakeCellGrid(int columns, int width_px, float scale) noexcept {
    if (columns == 3) {
        const float narrow =
            (static_cast<float>(width_px) / scale - 20.0F) / 3.0F;
        return {narrow, narrow + 4.0F, 3, scale};
    }
    return {static_cast<float>(kCellWidthDip),
            static_cast<float>(kCellStrideDip), columns < 1 ? 1 : columns,
            scale};
}

struct CellOrigin {
    float x{};
    float y{};
};

constexpr CellOrigin CellOriginAt(const CellGrid& grid, int slot) noexcept {
    return {(static_cast<float>(kCellsMarginDip) +
             static_cast<float>(slot % grid.per_row) * grid.stride_dip) *
                grid.scale,
            (kCellsTopDip + static_cast<float>(slot / grid.per_row) *
                                static_cast<float>(kCollapsedRowPitch)) *
                grid.scale};
}

// Which cell is under a point? -1 for none.
constexpr int CellSlotAt(const CellGrid& grid, int count, int x, int y) noexcept {
    for (int slot = 0; slot < count; ++slot) {
        const CellOrigin origin = CellOriginAt(grid, slot);
        if (static_cast<float>(x) >= origin.x &&
            static_cast<float>(x) < origin.x + grid.cell_width_dip * grid.scale &&
            static_cast<float>(y) >= origin.y &&
            static_cast<float>(y) < origin.y + kCellHeightDip * grid.scale)
            return slot;
    }
    return -1;
}

// Where a release lands. Rows are bands rather than strict rectangles, so a
// release in the gap between two cells still resolves to a slot instead of
// being discarded -- a reader aiming between cells means the nearer one, not
// nothing. A release above the cells or below the second row is refused.
struct DropTarget {
    int row{-1};
    int column{-1};
};

constexpr DropTarget DropTargetAt(const CellGrid& grid, int x, int y) noexcept {
    const float local_y = static_cast<float>(y) / grid.scale - kCellsTopDip;
    if (local_y < 0.0F) return {};
    const int row =
        static_cast<int>(local_y / static_cast<float>(kCollapsedRowPitch));
    if (row > 1) return {};
    const float local_x =
        static_cast<float>(x) / grid.scale - static_cast<float>(kCellsMarginDip);
    const int column =
        local_x < 0.0F ? 0 : static_cast<int>(local_x / grid.stride_dip);
    return {row, column};
}

inline constexpr unsigned kRecordBits = 3;
inline constexpr unsigned kRecordMask = (1u << kRecordBits) - 1u;

constexpr std::uint32_t PackLayout(const Layout& layout) noexcept {
    std::uint32_t packed = 0;
    for (int i = 0; i < kRecordCount; ++i) {
        packed |= (static_cast<std::uint32_t>(layout.order[
                       static_cast<std::size_t>(i)]) &
                   kRecordMask)
                  << (static_cast<unsigned>(i) * kRecordBits);
    }
    packed |= (static_cast<std::uint32_t>(layout.row1) & kRecordMask)
              << (kRecordCount * kRecordBits);
    return packed;
}

constexpr std::optional<Layout> UnpackLayout(std::uint32_t packed) noexcept {
    Layout layout;
    for (int i = 0; i < kRecordCount; ++i) {
        layout.order[static_cast<std::size_t>(i)] = static_cast<Metric>(
            (packed >> (static_cast<unsigned>(i) * kRecordBits)) & kRecordMask);
    }
    layout.row1 = static_cast<int>(
        (packed >> (kRecordCount * kRecordBits)) & kRecordMask);
    if (!IsValidLayout(layout)) return std::nullopt;
    return layout;
}

// A stored value that cannot be trusted is discarded whole. The result always
// contains every record exactly once.
constexpr Layout SanitizeLayout(std::uint32_t packed) noexcept {
    const auto parsed = UnpackLayout(packed);
    return parsed ? *parsed : Layout{};
}

// Left margin, then (n-1) strides, then the last cell, then the right margin:
// 6 + (n-1)*76 + 72 + 6, which is n*76 + 8. That reproduces the shipped 388
// for five records and 464 for six, and gives 540 for seven.
//
// Collapsed width therefore follows the first row. The default arrangement is
// a single row, so a preference toggle moves the width by one cell stride and
// leaves the height alone. Once the reader has arranged a second row, a toggle
// moves the height instead and the width holds.
constexpr int CollapsedWidth(int row1) noexcept {
    return row1 * kCellStrideDip + kCellWidthDip - kCellStrideDip +
           kCellsMarginDip * 2;
}

constexpr Footprint ChooseFootprint(bool collapsed, const Placement& placed,
                                    int work_width_dip,
                                    int work_height_dip) noexcept {
    if (collapsed) {
        // Prefer the reader's arrangement. A work area too narrow for its
        // first row falls back to the narrowest legal arrangement -- two rows
        // at the five-record minimum -- before the cells themselves shrink.
        int row1 = placed.row1;
        if (work_width_dip < CollapsedWidth(row1) && row1 > kCellsPerRow)
            row1 = placed.count > kCellsPerRow ? kCellsPerRow : placed.count;
        const int rows = placed.count > row1 ? 2 : 1;
        const int width = CollapsedWidth(row1);
        const int height = 88 + (rows - 1) * kCollapsedRowHeightIncrement;
        if (work_width_dip >= width) return {width, height, row1, rows};
        return {std::min(320, std::max(1, work_width_dip)), height, 3, rows};
    }
    // Expanded is a vertical list, so the arrangement changes the order of the
    // lanes but not the shape of the window.
    const int height = Height(false) + (placed.count - kCellsPerRow) * 25;
    if (work_height_dip >= height * 2 && work_width_dip >= 388)
        return {388, height, 1, 1};
    return {std::min(320, std::max(1, work_width_dip)),
            190 + (placed.count - kCellsPerRow) * 25, 2, 1};
}

// Convenience for callers that have no stored arrangement yet.
constexpr Footprint ChooseFootprint(bool collapsed, bool show_ram, bool show_fps,
                                    int work_width_dip,
                                    int work_height_dip) noexcept {
    return ChooseFootprint(collapsed, Resolve(Layout{}, show_ram, show_fps),
                           work_width_dip, work_height_dip);
}
inline std::pair<double, double> SparkRange(double low, double high, double minimum_span) noexcept {
    const double span = std::max(minimum_span, (high - low) * 1.2);
    const double bottom = std::max(0.0, (low + high - span) / 2.0);
    return {bottom, bottom + span};
}
constexpr bool ToggleHit(int x, int y, int width, int header_height) noexcept {
    return x >= width - header_height && x < width && y >= 0 && y < header_height;
}

// The lock sits immediately left of the chevron and is the same size, so the
// two never overlap and neither can swallow the other's press.
constexpr bool LockHit(int x, int y, int width, int header_height) noexcept {
    return x >= width - header_height * 2 && x < width - header_height &&
           y >= 0 && y < header_height;
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
