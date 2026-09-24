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
enum class Metric { Temperature, Power, Vram, Gpu, Cpu, Ram, Fps, Network };

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
inline constexpr float kCellHeightDip = 51.0F;
inline constexpr float kCellsTopDip = 30.0F;
// A second row adds exactly one row pitch to the window, which is the only
// value that keeps the margin below the last cell the same whatever the shape.
// This used to be its own constant, one dip short of the pitch: harmless at two
// rows, and by seven it had eaten six of the seven dip below the bottom cell,
// leaving it pressed against the frame. Derived now, so the two cannot drift.
inline constexpr int kCollapsedRowHeightIncrement = kCollapsedRowPitch;

// Height of a single-row compact window, and the margin left under the last
// cell. Named so the relationship is checkable rather than implied by 88.
inline constexpr int kCollapsedSingleRowHeight = 88;
[[nodiscard]] constexpr int CollapsedBottomMargin(int rows) noexcept {
    const int height =
        kCollapsedSingleRowHeight + (rows - 1) * kCollapsedRowHeightIncrement;
    const int last_cell_bottom = static_cast<int>(kCellsTopDip) +
                                 (rows - 1) * kCollapsedRowPitch +
                                 static_cast<int>(kCellHeightDip);
    return height - last_cell_bottom;
}

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

inline constexpr float kLaneBandGapDip = 8.0F;
// Narrower than this and even a stepped-down figure has nowhere to go, so the
// caller is better off drawing it over the gap than into nothing.
inline constexpr float kLaneValueMinimumDip = 40.0F;

// The current reading's box on a lane's top band: everything the annotation
// beside it did not take.
//
// `annotation_width` is what that text MEASURED, not what was set aside for
// it. The box used to be a constant 132 dip regardless -- and `scale` here is
// LaneScale, a VERTICAL ratio, because the lanes divide a fixed window height
// between them. So the box narrows every time a record is added, while the
// value face does not: it is sized from the DPI scale by the caller. At seven
// lanes the 132 became 89 dip; the eighth record took it to 78, and
// "349.5 W (350 W)" -- an ordinary reading on a 350 W card -- stopped fitting
// and was drawn as "349.5 W (350...". A figure that has been cut is a
// different figure.
//
// Taking the whole free band instead of a constant is what makes that
// independent of how many records exist. Pass 0 when the lane has no
// annotation.
constexpr LaneSpan LaneValueSpan(float row_x, float row_width,
                                 float annotation_width, float scale) noexcept {
    const float left = LaneGraphSpan(row_x, row_width, scale).x +
                       (annotation_width > 0.0F
                            ? annotation_width + kLaneBandGapDip * scale
                            : 0.0F);
    const float right = row_x + row_width - kLaneBandGapDip * scale;
    const float width = right - left;
    return {left, width > kLaneValueMinimumDip * scale
                      ? width
                      : kLaneValueMinimumDip * scale};
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

// Eight, and eight is the ceiling this stored form can reach -- see the format
// tag below, which had to shrink to one bit to make room for the eighth.
inline constexpr int kRecordCount = 8;

using Order = std::array<Metric, kRecordCount>;

constexpr Order DefaultOrder() noexcept {
    return {Metric::Temperature, Metric::Power, Metric::Vram, Metric::Gpu,
            Metric::Cpu, Metric::Ram, Metric::Fps, Metric::Network};
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

// Row composition is a set of breaks, not a first-row count.
//
// N placed records have N-1 gaps between them, and each gap either ends its
// row or does not. Every composition is reachable and none of them needs a
// special case: "a row need not be full before the next begins" is not a rule
// to enforce here, it is simply what this representation says.
//
// The breaks are indexed by *placed* slot, not by position in the order, so
// disabling a record reflows the rows the same way disabling one used to
// backfill the first row.
inline constexpr int kMaxBreaks = kRecordCount - 1;

// No composition is forbidden. Width follows the widest row, so a narrow
// trailing row costs nothing -- (5,1,1) is as wide as (5,2) -- and a single
// column is a shape a reader can ask for deliberately, to dock the dashboard
// against a screen edge.
//
// A single column was refused at first, on the grounds that 84 dip could not
// hold the lock, the chevron and a grabbable drag strip. The arithmetic
// behind that refusal was wrong: the header
// band is 25 dip tall and the two buttons take 50 dip of the 84, leaving
// 34 x 25 dip of HTCAPTION -- 9.0 x 6.6 mm at 100 %, taller than a Windows
// title bar. The header drops the `GTG` label and moves the status dot left at
// this width, which is what makes the room; see `HeaderShowsTitle`.
//
// The floor is kept as a named constant rather than deleted so the header
// degradation and the arrangement rule stay visibly tied to the same number.
inline constexpr int kMinimumWidestRow = 1;

struct Layout {
    Order order{DefaultOrder()};
    // Bit i means "end the row after placed slot i". Only the low kMaxBreaks
    // bits are meaningful; zero is the default single row.
    std::uint8_t breaks{0};
};

constexpr bool IsValidLayout(const Layout& layout) noexcept {
    return IsValidOrder(layout.order) &&
           (layout.breaks >> kMaxBreaks) == 0;
}

struct Placement {
    std::array<Metric, kRecordCount> cells{};
    int count{};
    // Length of each row, rows[0..rows-1].
    std::array<int, kRecordCount> row_length{};
    int rows{1};
};

constexpr bool IsOptional(Metric metric) noexcept {
    return metric == Metric::Ram || metric == Metric::Fps ||
           metric == Metric::Network;
}

[[nodiscard]] constexpr int WidestRow(const Placement& placed) noexcept {
    int widest = 0;
    for (int row = 0; row < placed.rows; ++row) {
        if (placed.row_length[static_cast<std::size_t>(row)] > widest)
            widest = placed.row_length[static_cast<std::size_t>(row)];
    }
    return widest;
}

// Which row a placed slot sits in, and how far along that row it is.
[[nodiscard]] constexpr int RowOf(const Placement& placed, int slot) noexcept {
    int seen = 0;
    for (int row = 0; row < placed.rows; ++row) {
        seen += placed.row_length[static_cast<std::size_t>(row)];
        if (slot < seen) return row;
    }
    return placed.rows > 0 ? placed.rows - 1 : 0;
}

[[nodiscard]] constexpr int ColumnOf(const Placement& placed, int slot) noexcept {
    int seen = 0;
    for (int row = 0; row < placed.rows; ++row) {
        const int length = placed.row_length[static_cast<std::size_t>(row)];
        if (slot < seen + length) return slot - seen;
        seen += length;
    }
    return 0;
}

// First slot of a row.
[[nodiscard]] constexpr int RowStart(const Placement& placed, int row) noexcept {
    int seen = 0;
    for (int r = 0; r < row && r < placed.rows; ++r)
        seen += placed.row_length[static_cast<std::size_t>(r)];
    return seen;
}

// The general form: `enabled` answers whether a record is shown.
template <class Enabled>
constexpr Placement Resolve(const Layout& layout, Enabled&& enabled) noexcept {
    Placement placed;
    for (const Metric metric : layout.order) {
        if (!enabled(metric)) continue;
        placed.cells[static_cast<std::size_t>(placed.count++)] = metric;
    }
    if (placed.count <= 0) return placed;

    int rows = 0;
    int current = 0;
    for (int slot = 0; slot < placed.count; ++slot) {
        ++current;
        const bool last = slot == placed.count - 1;
        const bool cut = slot < kMaxBreaks &&
                         ((layout.breaks >> slot) & 1u) != 0u;
        if (last || cut) {
            placed.row_length[static_cast<std::size_t>(rows++)] = current;
            current = 0;
        }
    }
    placed.rows = rows;

    // A stored value can always be read back; it cannot always be obeyed. An
    // arrangement whose widest row is one cell collapses to a single row
    // rather than leaving the reader somewhere they cannot get out of.
    if (placed.count >= kMinimumWidestRow && WidestRow(placed) < kMinimumWidestRow) {
        placed.row_length = {};
        placed.row_length[0] = placed.count;
        placed.rows = 1;
    }
    return placed;
}

// `show_network` defaults to off, which is both the shipped default and what
// keeps every existing caller meaning what it meant before the eighth record
// existed. The five thermal records have no switch: falling through to `true`
// is their answer.
constexpr Placement Resolve(const Layout& layout, bool show_ram, bool show_fps,
                            bool show_network = false) noexcept {
    return Resolve(layout, [show_ram, show_fps, show_network](const Metric metric) {
        if (metric == Metric::Ram) return show_ram;
        if (metric == Metric::Fps) return show_fps;
        if (metric == Metric::Network) return show_network;
        return true;
    });
}

// Overload used by tests to disable a mandatory record and prove that reflow
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

// Recompute the break set for a target composition of row lengths.
constexpr std::uint8_t BreaksFromRows(const std::array<int, kRecordCount>& rows,
                                      int row_count) noexcept {
    std::uint8_t breaks = 0;
    int slot = 0;
    for (int row = 0; row + 1 < row_count; ++row) {
        slot += rows[static_cast<std::size_t>(row)];
        if (slot - 1 >= 0 && slot - 1 < kMaxBreaks)
            breaks = static_cast<std::uint8_t>(breaks | (1u << (slot - 1)));
    }
    return breaks;
}

// Drop rules.
//
// Two targets and no others: a release on a row inserts into that row at that
// column, and a release below the last row starts a new one. A third target
// between two rows was refused -- it needs its own gap bands in hit testing
// and every arrangement it could reach is already reachable.
//
// Out of range is refused, never clamped.
constexpr Layout ApplyDrop(const Layout& current, const Placement& placed,
                           int from_slot, int drop_row,
                           int drop_column) noexcept {
    if (from_slot < 0 || from_slot >= placed.count) return current;
    if (drop_row < 0 || drop_row > placed.rows) return current;
    if (drop_column < 0) return current;

    // Rebuild the row lengths with the cell removed, then insert it.
    std::array<int, kRecordCount> rows = placed.row_length;
    int row_count = placed.rows;
    const int from_row = RowOf(placed, from_slot);
    --rows[static_cast<std::size_t>(from_row)];

    int target_row = drop_row;
    if (drop_row == placed.rows) {
        // A new row below the last. Dropping the only cell of the last row
        // below itself would just rebuild the same arrangement.
        if (row_count >= kRecordCount) return current;
        rows[static_cast<std::size_t>(row_count++)] = 0;
        target_row = row_count - 1;
    }
    const int capacity = rows[static_cast<std::size_t>(target_row)];
    const int column = drop_column > capacity ? capacity : drop_column;
    ++rows[static_cast<std::size_t>(target_row)];

    // Drop any row the move emptied, keeping the rest in order.
    std::array<int, kRecordCount> packed_rows{};
    int packed_count = 0;
    int removed_before_target = 0;
    for (int row = 0; row < row_count; ++row) {
        if (rows[static_cast<std::size_t>(row)] <= 0) {
            if (row < target_row) ++removed_before_target;
            continue;
        }
        packed_rows[static_cast<std::size_t>(packed_count++)] =
            rows[static_cast<std::size_t>(row)];
    }
    if (packed_count <= 0) return current;
    const int packed_target = target_row - removed_before_target;

    // Where the record lands in the flat order.
    int destination = column;
    for (int row = 0; row < packed_target && row < packed_count; ++row)
        destination += packed_rows[static_cast<std::size_t>(row)];
    if (destination >= placed.count) destination = placed.count - 1;

    Placement probe;
    probe.count = placed.count;
    probe.row_length = packed_rows;
    probe.rows = packed_count;
    if (WidestRow(probe) < kMinimumWidestRow) return current;

    // Translate placed slots back into positions in the full order, which also
    // carries the records this placement is not showing.
    int order_from = -1;
    int order_to = -1;
    int seen = 0;
    for (int i = 0; i < kRecordCount; ++i) {
        bool shown = false;
        for (int s = 0; s < placed.count; ++s) {
            if (placed.cells[static_cast<std::size_t>(s)] ==
                current.order[static_cast<std::size_t>(i)]) {
                shown = true;
                break;
            }
        }
        if (!shown) continue;
        if (seen == from_slot) order_from = i;
        if (seen == destination) order_to = i;
        ++seen;
    }
    if (order_from < 0 || order_to < 0) return current;

    Layout next = current;
    next.order = MoveInsert(current.order, order_from, order_to);
    next.breaks = BreaksFromRows(packed_rows, packed_count);
    return next;
}

// ---------------------------------------------------------------------------
// Cell geometry. Rendering and hit testing both derive from here: the expanded
// lanes once kept two copies of their plot rectangle and drifted apart, which
// is a mistake worth not repeating one level down.
// ---------------------------------------------------------------------------



struct CellGrid {
    float cell_width_dip{static_cast<float>(kCellWidthDip)};
    float stride_dip{static_cast<float>(kCellStrideDip)};
    int per_row{kCellsPerRow};   // the widest row; the narrow fallback uses 3
    float scale{1.0F};
};

// `columns` is the widest row. A negative value means the work area could not
// hold the arrangement at full size, so the cells shrink to fit it instead.
//
// The shrunken width is derived from the row that is actually widest, not from
// a fixed three. The previous model could only ever wrap to five per row, so
// three was safe; with arbitrary compositions a row of seven in a 320 dip
// window would have been laid out at the old stride and run off the end.
constexpr CellGrid MakeCellGrid(int columns, int width_px, float scale) noexcept {
    if (columns < 0) {
        const int widest = -columns;
        const float narrow =
            (static_cast<float>(width_px) / scale - 20.0F) /
            static_cast<float>(widest < 1 ? 1 : widest);
        return {narrow, narrow + 4.0F, widest < 1 ? 1 : widest, scale};
    }
    return {static_cast<float>(kCellWidthDip),
            static_cast<float>(kCellStrideDip), columns < 1 ? 1 : columns,
            scale};
}

struct CellOrigin {
    float x{};
    float y{};
};

// Rows have different lengths now, so a slot's position cannot come from a
// modulus. It comes from the placement, which is the one place that knows how
// the rows were cut.
constexpr CellOrigin CellOriginAt(const CellGrid& grid, const Placement& placed,
                                  int slot) noexcept {
    const int row = RowOf(placed, slot);
    const int column = ColumnOf(placed, slot);
    return {(static_cast<float>(kCellsMarginDip) +
             static_cast<float>(column) * grid.stride_dip) * grid.scale,
            (kCellsTopDip + static_cast<float>(row) *
                                static_cast<float>(kCollapsedRowPitch)) *
                grid.scale};
}

// Which cell is under a point? -1 for none.
constexpr int CellSlotAt(const CellGrid& grid, const Placement& placed,
                         int x, int y) noexcept {
    for (int slot = 0; slot < placed.count; ++slot) {
        const CellOrigin origin = CellOriginAt(grid, placed, slot);
        if (static_cast<float>(x) >= origin.x &&
            static_cast<float>(x) < origin.x + grid.cell_width_dip * grid.scale &&
            static_cast<float>(y) >= origin.y &&
            static_cast<float>(y) < origin.y + kCellHeightDip * grid.scale)
            return slot;
    }
    return -1;
}

// Exchange two placed records, leaving the shape alone.
//
// Dropping a cell squarely onto another used to insert it there and push that
// one aside, which moves every record after it and changes what the reader was
// looking at far beyond the two cells under their hand. An exchange is local:
// two cells trade places and nothing else moves.
//
// Breaks are untouched by construction -- row membership follows position in
// the placed sequence, so swapping two positions swaps their rows and leaves
// every row length exactly as it was. Rebalancing rows is still done by
// dropping into the empty space past the end of a row, or below the last one.
constexpr Layout ApplySwap(const Layout& current, const Placement& placed,
                           int from_slot, int to_slot) noexcept {
    if (from_slot < 0 || from_slot >= placed.count) return current;
    if (to_slot < 0 || to_slot >= placed.count) return current;
    if (from_slot == to_slot) return current;

    // Placed slots are positions among the *shown* records; the order carries
    // the hidden ones too.
    int from_index = -1;
    int to_index = -1;
    int seen = 0;
    for (int i = 0; i < kRecordCount; ++i) {
        bool shown = false;
        for (int slot = 0; slot < placed.count; ++slot) {
            if (placed.cells[static_cast<std::size_t>(slot)] ==
                current.order[static_cast<std::size_t>(i)]) {
                shown = true;
                break;
            }
        }
        if (!shown) continue;
        if (seen == from_slot) from_index = i;
        if (seen == to_slot) to_index = i;
        ++seen;
    }
    if (from_index < 0 || to_index < 0) return current;

    Layout next = current;
    const Metric moved = next.order[static_cast<std::size_t>(from_index)];
    next.order[static_cast<std::size_t>(from_index)] =
        next.order[static_cast<std::size_t>(to_index)];
    next.order[static_cast<std::size_t>(to_index)] = moved;
    return next;
}

// Where a release lands. Rows are bands rather than strict rectangles, so a
// release in the gap between two cells still resolves to a slot instead of
// being discarded -- a reader aiming between cells means the nearer one, not
// nothing.
//
// The band immediately below the last row is a target of its own and means
// "start a new row". Without it no arrangement deeper than the current one
// could ever be reached by dragging.
struct DropTarget {
    int row{-1};
    int column{-1};
};

constexpr DropTarget DropTargetAt(const CellGrid& grid, const Placement& placed,
                                  int x, int y) noexcept {
    const float local_y = static_cast<float>(y) / grid.scale - kCellsTopDip;
    if (local_y < 0.0F) return {};
    const int row =
        static_cast<int>(local_y / static_cast<float>(kCollapsedRowPitch));
    // One band past the last row opens a new one; anything beyond that is a
    // release into empty space and is refused.
    if (row > placed.rows) return {};
    const float local_x =
        static_cast<float>(x) / grid.scale - static_cast<float>(kCellsMarginDip);
    const int column =
        local_x < 0.0F ? 0 : static_cast<int>(local_x / grid.stride_dip);
    return {row, column};
}

// Below this many cells across, the header cannot hold a status string and
// keeps only the dot. Measured at the real 9.5 dip status font: three cells
// leaves 124 dip against a longest string of 95.8, and two leaves 48, which
// fits only the shortest Chinese status. The dot already carries the colour
// and already breathes under PulseAlpha, so nothing new has to be invented --
// and detail was never the compact view's job. A reader who arranges the
// dashboard two cells wide has asked to read numbers only.
inline constexpr int kStatusTextMinimumColumns = 3;

// At one cell across, the `GTG` label (x 10-40) and the status dot (x 46-52)
// both sit underneath the two 25-dip buttons. The label is decoration on a
// window nobody mistakes for another; the dot carries the state colour and
// moves to the left margin. Dropping the label is what leaves a drag strip.
[[nodiscard]] constexpr bool HeaderShowsTitle(int widest_row) noexcept {
    return widest_row > kMinimumWidestRow;
}

[[nodiscard]] constexpr bool HeaderShowsStatusText(int widest_row) noexcept {
    return widest_row >= kStatusTextMinimumColumns;
}

inline constexpr unsigned kRecordBits = 3;
inline constexpr unsigned kRecordMask = (1u << kRecordBits) - 1u;
inline constexpr unsigned kBreakShift = kRecordCount * kRecordBits;   // 24
inline constexpr unsigned kBreakMask = (1u << kMaxBreaks) - 1u;       // 7 bits

// A one-bit format tag, at the top of the word.
//
// Silently reinterpreting a reader's arrangement as a different one is worse
// than losing it, so every schema before this one is rejected rather than
// decoded. That has always been the rule; what changed is how much room the
// tag gets.
//
// Eight records need 24 bits of order and 7 of breaks, which leaves exactly
// one. It is enough, because every value the four-bit schema ever wrote
// carried `1 << 28` in the top nibble -- bit 31 clear in all of them. A tag of
// one at bit 31 therefore rejects them as cleanly as four bits did.
//
// An earlier note claimed this scheme would survive one more record than it
// does; it had counted the indices and the breaks but not the tag it had just
// introduced. The word is now full: a ninth record needs four-bit indices,
// which is 36 + 8 + 1, and no
// arrangement of those fits. The record after this one changes the store.
inline constexpr unsigned kFormatShift = 31;
inline constexpr std::uint32_t kFormatVersion = 1;

static_assert(kRecordCount * kRecordBits + kMaxBreaks + 1 <= 32,
              "the arrangement no longer fits a REG_DWORD");

constexpr std::uint32_t PackLayout(const Layout& layout) noexcept {
    std::uint32_t packed = kFormatVersion << kFormatShift;
    for (int i = 0; i < kRecordCount; ++i) {
        packed |= (static_cast<std::uint32_t>(layout.order[
                       static_cast<std::size_t>(i)]) &
                   kRecordMask)
                  << (static_cast<unsigned>(i) * kRecordBits);
    }
    packed |= (static_cast<std::uint32_t>(layout.breaks) & kBreakMask)
              << kBreakShift;
    return packed;
}

constexpr std::optional<Layout> UnpackLayout(std::uint32_t packed) noexcept {
    if ((packed >> kFormatShift) != kFormatVersion) return std::nullopt;
    Layout layout;
    for (int i = 0; i < kRecordCount; ++i) {
        layout.order[static_cast<std::size_t>(i)] = static_cast<Metric>(
            (packed >> (static_cast<unsigned>(i) * kRecordBits)) & kRecordMask);
    }
    layout.breaks =
        static_cast<std::uint8_t>((packed >> kBreakShift) & kBreakMask);
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
// Collapsed width follows the widest row, so a narrow trailing row is free:
// (5,1,1) is exactly as wide as (5,2), and only taller.
constexpr int CollapsedWidth(int widest_row) noexcept {
    return widest_row * kCellStrideDip + kCellWidthDip - kCellStrideDip +
           kCellsMarginDip * 2;
}

constexpr Footprint ChooseFootprint(bool collapsed, const Placement& placed,
                                    int work_width_dip,
                                    int work_height_dip) noexcept {
    if (collapsed) {
        const int widest = WidestRow(placed);
        const int width = CollapsedWidth(widest);
        const int height =
            kCollapsedSingleRowHeight +
            (placed.rows - 1) * kCollapsedRowHeightIncrement;
        if (work_width_dip >= width) return {width, height, widest, placed.rows};
        // Too narrow even for the reader's own arrangement. The cells shrink
        // rather than the arrangement silently changing: the reader chose this
        // shape, and rearranging it behind their back would be a worse answer
        // than a cramped one. Negative columns carry "shrink to this many".
        return {std::min(320, std::max(1, work_width_dip)), height, -widest,
                placed.rows};
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
// The header band, in dip. The buttons are measured against it rather than
// filling it: see `HeaderButtonWidth`.
inline constexpr int kDragBandDip = 25;
inline constexpr int kHeaderButtonDip = 22;   // nine tenths of the band

// Button width for a band of this many pixels. Derived rather than stored so
// it scales with DPI exactly as the band does, and so the hit rectangles and
// the drawn buttons cannot drift apart -- they ask the same function.
[[nodiscard]] constexpr int HeaderButtonWidth(int header_height) noexcept {
    return header_height * kHeaderButtonDip / kDragBandDip;
}

constexpr bool ToggleHit(int x, int y, int width, int header_height) noexcept {
    const int button = HeaderButtonWidth(header_height);
    return x >= width - button && x < width && y >= 0 && y < header_height;
}

// The lock sits immediately left of the chevron and is the same size, so the
// two never overlap and neither can swallow the other's press.
constexpr bool LockHit(int x, int y, int width, int header_height) noexcept {
    const int button = HeaderButtonWidth(header_height);
    return x >= width - button * 2 && x < width - button &&
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

// A new row containing the dragged record, opened at `at_row` and pushing the
// rest down. `at_row == placed.rows` appends, which is what dropping past the
// last row has always meant; any index above that is refused.
constexpr Layout ApplyDropRow(const Layout& current, const Placement& placed,
                              int from_slot, int at_row) noexcept {
    if (from_slot < 0 || from_slot >= placed.count) return current;
    if (at_row < 0 || at_row > placed.rows) return current;

    // The row the record leaves is closed before the new one opens, not after.
    // A single column already has one row per record, so inserting first would
    // need an eighth row that the arrangement cannot hold -- even though the
    // move itself is free, because the row being vacated disappears with it.
    std::array<int, kRecordCount> rows{};
    int row_count = 0;
    int target = at_row;
    const int from_row = RowOf(placed, from_slot);
    for (int row = 0; row < placed.rows; ++row) {
        const int length = placed.row_length[static_cast<std::size_t>(row)] -
                           (row == from_row ? 1 : 0);
        if (length <= 0) {
            if (row < at_row) --target;   // the rows above the target closed up
            continue;
        }
        rows[static_cast<std::size_t>(row_count++)] = length;
    }
    if (row_count >= kRecordCount) return current;   // genuinely no room
    if (target < 0) target = 0;
    if (target > row_count) target = row_count;

    for (int row = row_count; row > target; --row)
        rows[static_cast<std::size_t>(row)] = rows[static_cast<std::size_t>(row - 1)];
    rows[static_cast<std::size_t>(target)] = 1;
    ++row_count;

    int destination = 0;
    for (int row = 0; row < target; ++row)
        destination += rows[static_cast<std::size_t>(row)];
    if (destination >= placed.count) destination = placed.count - 1;

    int order_from = -1;
    int order_to = -1;
    int seen = 0;
    for (int i = 0; i < kRecordCount; ++i) {
        bool shown = false;
        for (int slot = 0; slot < placed.count; ++slot) {
            if (placed.cells[static_cast<std::size_t>(slot)] ==
                current.order[static_cast<std::size_t>(i)]) {
                shown = true;
                break;
            }
        }
        if (!shown) continue;
        if (seen == from_slot) order_from = i;
        if (seen == destination) order_to = i;
        ++seen;
    }
    if (order_from < 0 || order_to < 0) return current;

    Layout next = current;
    next.order = MoveInsert(current.order, order_from, order_to);
    next.breaks = BreaksFromRows(rows, row_count);
    return next;
}

// What a release means.
//
// Every release is read against the cell it is nearest to, and the cell is
// divided into zones rather than compared against a threshold on the gap
// between cells. A gap is four dip wide -- about a millimetre -- which is not
// something anyone can aim at. A zone is a quarter of a cell.
enum class DropAction {
    None,          // released clear of the strip: change nothing
    Swap,          // squarely on another cell: the two trade places
    InsertInRow,   // beside a cell: inserted into its row at that side
    InsertRow,     // above or below a cell: a new row opens there
};

struct DropPlan {
    DropAction action{DropAction::None};
    int slot{-1};     // the cell the release was read against
    int row{};
    int column{};
};

// Half the cell, in each axis, counts as "on top of it". Outside that, the
// axis the release is furthest along decides whether it reads as beside the
// cell or above/below it.
inline constexpr float kSwapZone = 0.5F;
// One whole cell of slack. Further out than this is a release into empty
// space, and empty space means the reader changed their mind.
inline constexpr float kDropReach = 2.0F;

constexpr DropPlan PlanDrop(const CellGrid& grid, const Placement& placed,
                            int origin_x, int origin_y) noexcept {
    const float width = grid.cell_width_dip * grid.scale;
    const float height = kCellHeightDip * grid.scale;
    if (width <= 0.0F || height <= 0.0F || placed.count <= 0) return {};

    // Offsets are normalised into half-cells, so a diagonal release compares
    // like with like: without it a cell 72 wide and 51 tall would make every
    // near-corner release read as vertical.
    int nearest = -1;
    float best = 0.0F;
    float offset_x = 0.0F;
    float offset_y = 0.0F;
    for (int slot = 0; slot < placed.count; ++slot) {
        const CellOrigin cell = CellOriginAt(grid, placed, slot);
        const float nx = (static_cast<float>(origin_x) - cell.x) * 2.0F / width;
        const float ny = (static_cast<float>(origin_y) - cell.y) * 2.0F / height;
        const float distance = nx * nx + ny * ny;
        if (nearest < 0 || distance < best) {
            nearest = slot;
            best = distance;
            offset_x = nx;
            offset_y = ny;
        }
    }
    if (nearest < 0) return {};

    const float away_x = offset_x < 0.0F ? -offset_x : offset_x;
    const float away_y = offset_y < 0.0F ? -offset_y : offset_y;
    if (away_x > kDropReach || away_y > kDropReach) return {};
    if (away_x <= kSwapZone && away_y <= kSwapZone)
        return {DropAction::Swap, nearest, 0, 0};

    const int row = RowOf(placed, nearest);
    const int column = ColumnOf(placed, nearest);
    // Above or below reads as a new row, and it no longer has to be the last
    // one. A single column had no other way to gain a row in the middle: the
    // reader had to drop at the very bottom and then drag the result back up,
    // one position at a time.
    if (away_y > away_x)
        return {DropAction::InsertRow, nearest,
                offset_y < 0.0F ? row : row + 1, 0};
    return {DropAction::InsertInRow, nearest, row,
            offset_x < 0.0F ? column : column + 1};
}

constexpr Layout ApplyPlan(const Layout& current, const Placement& placed,
                           int from_slot, const DropPlan& plan) noexcept {
    switch (plan.action) {
        case DropAction::None:
            return current;
        case DropAction::Swap:
            return ApplySwap(current, placed, from_slot, plan.slot);
        case DropAction::InsertInRow:
            return ApplyDrop(current, placed, from_slot, plan.row, plan.column);
        case DropAction::InsertRow:
            return ApplyDropRow(current, placed, from_slot, plan.row);
    }
    return current;
}


}  // namespace gtg::tray::compact
