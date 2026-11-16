#pragma once

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "net/throughput.hpp"
#include "topapp/board.hpp"
#include "topapp/details.hpp"
#include "tray/osd_compact.hpp"

// Details: the top apps of one column, drawn in the expanded OSD's own window
// under its header. Everything here is in dip and free of GDI+, so the
// geometry, the hit tests and the figures can be tested without a window.
namespace gtg::tray::details {

using topapp::Column;
using topapp::kColumnCount;

struct Box {
    float x{};
    float y{};
    float width{};
    float height{};

    [[nodiscard]] constexpr bool Contains(const float px, const float py) const noexcept {
        return px >= x && px < x + width && py >= y && py < y + height;
    }

    [[nodiscard]] constexpr float Right() const noexcept { return x + width; }

    [[nodiscard]] constexpr float Bottom() const noexcept { return y + height; }
};

// Under the 31 dip header, where the lanes and cards start.
inline constexpr float kTopDip = compact::kCardsTopDip;
inline constexpr float kMarginDip = compact::kCardsMarginDip;

// The type, in dip, so it scales with the OSD size exactly as the lanes do. A
// row reads at the lanes' size -- their label is 13 and their value 12.5 --
// and the title at the label's. The first version used 9.5 for a row, three
// quarters of the lane beside it, and read as small print (owner,
// 2026-11-13). At 100 % nothing is below the header's own 9.5 px status line.
inline constexpr float kTitleEmDip = 13.0F;
inline constexpr float kReadingEmDip = 12.0F;
inline constexpr float kHeadingEmDip = 10.0F;
inline constexpr float kRowEmDip = 12.0F;
inline constexpr float kFooterEmDip = 9.5F;
inline constexpr float kActionEmDip = 11.0F;

inline constexpr float kTitleHeightDip = 26.0F;
// The back control is a square this size at the title's start, the header
// buttons' size; the action at its end is as tall and wide enough for its
// word ("Reclaim", "Latency", 「回收」, 「測延遲」).
inline constexpr float kControlDip = 22.0F;
inline constexpr float kActionWidthDip = 64.0F;
inline constexpr float kHeadingHeightDip = 18.0F;
// A row is at most this tall; a shorter window gives its rows less, down to
// the minimum, and then lists fewer. Ten rows at the maximum need 405 dip,
// the window of eight records; five records' 330 dip gives them 22.8 each.
inline constexpr float kRowHeightDip = 25.0F;
inline constexpr float kRowMinimumDip = 18.0F;
inline constexpr float kFooterHeightDip = 16.0F;
// The figure columns, in column order: GPU, VRAM, CPU, RAM, NET. At the row's
// size a percentage needs "<0.1%", a size "9.6 GiB" or "850 MiB" -- three
// digits of MiB were cut at 50 dip -- and a rate "1192.3"; NET's heading
// carries its unit. The app's name takes what is left of 388 dip: 122.
inline constexpr std::array<float, kColumnCount> kColumnDip{42.0F, 56.0F, 42.0F, 56.0F, 58.0F};
// The busy indicator of an action started here: a cell's height, over the
// action and the headings under it, so it reads as the cell it is elsewhere.
inline constexpr float kBusyWidthDip = 132.0F;

struct Geometry {
    Box back;
    // The action for the sorted column, when it has one.
    Box action;
    // Between the two controls.
    Box title;
    // The heading row: the app column, then one box per column.
    Box name;
    std::array<Box, kColumnCount> columns;
    float rows_top{};
    float row_height{};
    // How many rows fit, at most kDetailsRows.
    int rows{};
    Box footer;
};

// Details in a window `width` x `height` dip.
[[nodiscard]] constexpr Geometry MakeGeometry(const float width, const float height,
                                              const int wanted_rows) noexcept {
    Geometry g;
    const float inner_left = kMarginDip;
    const float inner_width = width - kMarginDip * 2.0F;
    const float control_y = kTopDip + (kTitleHeightDip - kControlDip) / 2.0F;
    g.back = {inner_left, control_y, kControlDip, kControlDip};
    g.action = {inner_left + inner_width - kActionWidthDip, control_y, kActionWidthDip,
                kControlDip};
    g.title = {g.back.Right() + 4.0F, kTopDip, g.action.x - 4.0F - (g.back.Right() + 4.0F),
               kTitleHeightDip};
    const float heading_y = kTopDip + kTitleHeightDip + 2.0F;
    float figures = 0.0F;
    for (const float column : kColumnDip)
        figures += column;
    g.name = {inner_left, heading_y, inner_width - figures, kHeadingHeightDip};
    float x = g.name.Right();
    for (std::size_t c = 0; c < kColumnCount; ++c) {
        g.columns[c] = {x, heading_y, kColumnDip[c], kHeadingHeightDip};
        x += kColumnDip[c];
    }
    g.footer = {inner_left, height - kMarginDip - kFooterHeightDip, inner_width, kFooterHeightDip};
    g.rows_top = heading_y + kHeadingHeightDip + 1.0F;
    const float room = g.footer.y - 2.0F - g.rows_top;
    const int wanted = wanted_rows < 1 ? 1 : wanted_rows;
    float row = room / static_cast<float>(wanted);
    if (row > kRowHeightDip) row = kRowHeightDip;
    if (row < kRowMinimumDip) row = kRowMinimumDip;
    int fit = room > 0.0F ? static_cast<int>(room / row) : 0;
    if (fit > wanted) fit = wanted;
    g.row_height = row;
    g.rows = fit;
    return g;
}

// Row `index`'s box, across the whole width.
[[nodiscard]] constexpr Box RowAt(const Geometry& g, const int index) noexcept {
    return {g.name.x, g.rows_top + g.row_height * static_cast<float>(index),
            g.columns[kColumnCount - 1].Right() - g.name.x, g.row_height};
}

// Where the busy indicator of an action started from Details stands: ending
// where the action does, from the top of the title down a cell's height.
[[nodiscard]] constexpr Box BusyBox(const Geometry& g) noexcept {
    return {g.action.Right() - kBusyWidthDip, kTopDip, kBusyWidthDip, compact::kCellHeightDip};
}

// What a column's action does. The same two actions the compact cells have
// on a double-click; the other columns have none, and show no control.
enum class Action { None, Reclaim, Latency };

[[nodiscard]] constexpr Action ActionOf(const Column column) noexcept {
    switch (column) {
        case Column::Ram:
            return Action::Reclaim;
        case Column::Network:
            return Action::Latency;
        default:
            return Action::None;
    }
}

// Name: the app's name heading the App column. Sort: a figure's heading.
enum class Target { None, Back, Action, Sort, Name };

struct Hit {
    Target target{Target::None};
    // For Sort: the column.
    Column column{Column::Gpu};

    [[nodiscard]] constexpr bool operator==(const Hit&) const noexcept = default;
};

// What a press at (x, y) dip lands on, with the action offered or not. The
// controls are hit a little outside their boxes, since they sit at the
// window's edges.
[[nodiscard]] constexpr Hit HitTest(const Geometry& g, const float x, const float y,
                                    const bool action) noexcept {
    const auto around = [&](const Box& box) {
        return x >= box.x - 3.0F && x < box.Right() + 3.0F && y >= box.y - 1.0F &&
               y < box.Bottom() + 1.0F;
    };
    if (around(g.back)) return {Target::Back};
    if (action && around(g.action)) return {Target::Action};
    if (g.name.Contains(x, y)) return {Target::Name};
    for (std::size_t c = 0; c < kColumnCount; ++c) {
        if (g.columns[c].Contains(x, y)) return {Target::Sort, static_cast<Column>(c)};
    }
    return {};
}

// The place of the listed row at (x, y) dip, from 0, or -1: the rows' band,
// across the whole width. The caller knows how many are listed.
[[nodiscard]] constexpr int RowIndexAt(const Geometry& g, const float x, const float y) noexcept {
    if (g.rows <= 0 || !(g.row_height > 0.0F)) return -1;
    if (x < g.name.x || x >= g.columns[kColumnCount - 1].Right() || y < g.rows_top) return -1;
    const int index = static_cast<int>((y - g.rows_top) / g.row_height);
    return index < g.rows ? index : -1;
}

// What the App column says of each app, switched by its heading: the name;
// where its process works; its process id. Every opening starts at the name.
enum class NameView { Name, Location, Pid };

[[nodiscard]] constexpr NameView NextView(const NameView view) noexcept {
    switch (view) {
        case NameView::Name:
            return NameView::Location;
        case NameView::Location:
            return NameView::Pid;
        case NameView::Pid:
            break;
    }
    return NameView::Name;
}

// The rows Details lists for `sort`, as indices into the table, largest
// first: none while there is no table, sampling is paused, or the column
// cannot be or has not been measured -- the painter says why instead.
[[nodiscard]] inline std::vector<std::size_t> Listed(const topapp::Details* table,
                                                     const Column sort, const int rows) {
    if (table == nullptr || table->paused || rows <= 0) return {};
    const std::uint32_t bit = topapp::Bit(sort);
    if ((table->available & bit) == 0 || (table->measured & bit) == 0) return {};
    return topapp::Ranked(*table, sort, static_cast<std::size_t>(rows));
}

// The App column's text for a row, and whether it is in the muted ink. The
// process asked about is the one holding most of the sorted column. Its
// location is its working directory's last folder; a row whose directory
// says nothing (Windows' folder, the app's own) keeps its name, muted, so it
// never reads as a place. The id comes first, so a cut never hides it.
struct RowName {
    std::wstring text;
    bool muted{};

    [[nodiscard]] bool operator==(const RowName&) const = default;
};

[[nodiscard]] inline RowName NameFor(const topapp::DetailsRow& row, const Column sort,
                                     const NameView view) {
    const topapp::Representative& who = row.representatives[static_cast<std::size_t>(sort)];
    switch (view) {
        case NameView::Location:
            if (!who.folder.empty()) return {who.folder, false};
            return {row.name, true};
        case NameView::Pid:
            if (who.pid != 0) return {std::format(L"{} · {}", who.pid, row.name), false};
            break;
        case NameView::Name:
            break;
    }
    return {row.name, false};
}

// The footer's start while a row is hovered: its process. The directory
// follows it, cut in its middle to fit.
[[nodiscard]] inline std::wstring HoverLead(const topapp::Representative& who) {
    if (who.pid == 0) return {};
    return who.directory.empty() ? std::format(L"PID {}", who.pid)
                                 : std::format(L"PID {} · ", who.pid);
}

// Whether a double-click may show `directory` in Explorer: a drive path
// ("X:\..."), never a network one -- any process can set its directory to
// \\host\share, and Explorer browsing it would sign in to that host as the
// reader. Whether the drive is fixed and the directory exists is asked when
// it is opened.
[[nodiscard]] constexpr bool ShowableDirectory(const std::wstring_view directory) noexcept {
    if (directory.size() < 3 || directory[1] != L':' || directory[2] != L'\\') return false;
    const wchar_t drive = directory[0];
    return (drive >= L'A' && drive <= L'Z') || (drive >= L'a' && drive <= L'z');
}

// Which column Details opens sorted by when a record asks for it: its own,
// or GPU's for a record with no apps of its own to list -- the app heating
// the card, or behind the frame rate, is the one using the GPU.
[[nodiscard]] constexpr Column ColumnFor(const compact::Metric metric) noexcept {
    switch (metric) {
        case compact::Metric::Vram:
            return Column::Vram;
        case compact::Metric::Cpu:
            return Column::Cpu;
        case compact::Metric::Ram:
            return Column::Ram;
        case compact::Metric::Network:
            return Column::Network;
        default:
            return Column::Gpu;
    }
}

// The column Details opens on: the one asked for, except GPU -- what every
// entry without a column of its own asks for -- on a machine with no GPU
// column (no single NVIDIA adapter: `available`, the sampler's last word,
// without the GPU bit). There the app the reader is after is the one using
// the processor, and GPU would open on a line saying there are no counters.
// VRAM, asked for by name, is kept. Unknown before the first table.
[[nodiscard]] constexpr Column OpeningColumn(
    const Column asked, const std::optional<std::uint32_t> available) noexcept {
    if (asked == Column::Gpu && available && (*available & topapp::Bit(Column::Gpu)) == 0)
        return Column::Cpu;
    return asked;
}

// Whether a compact cell's double-click is its own: RAM reclaims and Network
// probes. Every other cell opens Details, since the gesture would otherwise
// do nothing (owner, 2026-11-13: the cell's own action first, Details only
// where it has none). The lanes and cards have no actions, so every one of
// them opens Details.
[[nodiscard]] constexpr bool CellHasAction(const compact::Metric metric) noexcept {
    return metric == compact::Metric::Ram || metric == compact::Metric::Network;
}

// The expanded header's Details button: the square left of the chevron,
// where the compact header has its lock. The two never show together.
[[nodiscard]] constexpr bool ButtonHit(const int x, const int y, const int width,
                                       const int header_height) noexcept {
    return compact::LockHit(x, y, width, header_height);
}

// The slot of the expanded view's lane or card at (x, y) dip, or -1. Lanes
// share the window's height below the header with a 2 dip gap, as the painter
// draws them; cards are compact::CardAt.
[[nodiscard]] constexpr int ExpandedSlotAt(const bool cards, const int count, const float width,
                                           const float height, const float x,
                                           const float y) noexcept {
    if (count <= 0) return -1;
    if (cards) {
        for (int slot = 0; slot < count; ++slot) {
            const compact::CardBox box = compact::CardAt(slot, count, width, height);
            if (x >= box.x && x < box.x + box.width && y >= box.y && y < box.y + box.height) {
                return slot;
            }
        }
        return -1;
    }
    const float row_height = (height - kTopDip) / static_cast<float>(count) - 2.0F;
    if (row_height <= 0.0F || x < kMarginDip || x >= width - kMarginDip || y < kTopDip) return -1;
    const float stride = row_height + 2.0F;
    const int slot = static_cast<int>((y - kTopDip) / stride);
    if (slot >= count) return -1;
    // The gap between two lanes belongs to neither.
    return y - kTopDip - stride * static_cast<float>(slot) < row_height ? slot : -1;
}

// "87%", "4.2%", "<0.1%", "0%".
[[nodiscard]] inline std::wstring FormatPercent(const double value) {
    if (value >= 9.95) return std::format(L"{:.0f}%", value);
    if (value >= 0.05) return std::format(L"{:.1f}%", value);
    return value > 0.0 ? L"<0.1%" : L"0%";
}

// "60 GiB", "9.6 GiB", "850 MiB", "<1 MiB", in the units the lanes use. Whole
// GiB from ten up, so a size fits the 50 dip a fifth column left it.
[[nodiscard]] inline std::wstring FormatBytes(const double bytes) {
    constexpr double kMiB = 1024.0 * 1024.0;
    constexpr double kGiB = kMiB * 1024.0;
    if (bytes >= 9.95 * kGiB) return std::format(L"{:.0f} GiB", bytes / kGiB);
    if (bytes >= 1000.0 * kMiB) return std::format(L"{:.1f} GiB", bytes / kGiB);
    if (bytes >= kMiB) return std::format(L"{:.0f} MiB", bytes / kMiB);
    return bytes > 0.0 ? L"<1 MiB" : L"0 MiB";
}

// A rate in the one unit the network is shown in everywhere (MB/s, one
// decimal, rounded up so a live app never reads 0.0): the NET record's rule,
// net::kDisplayUnit, kept here so the column and the lane agree.
[[nodiscard]] inline std::wstring FormatRate(const double bytes_per_second) {
    const int tenths = net::TenthsIn(bytes_per_second, net::kDisplayUnit);
    return std::format(L"{}.{}", tenths / 10, tenths % 10);
}

[[nodiscard]] constexpr bool InBytes(const Column column) noexcept {
    return column == Column::Vram || column == Column::Ram;
}

[[nodiscard]] inline std::wstring FormatValue(const Column column, const double value) {
    if (column == Column::Network) return FormatRate(value);
    return InBytes(column) ? FormatBytes(value) : FormatPercent(value);
}

// How strongly a value's cell is shaded, 0 to 1: its share of the whole
// resource, square-rooted so a few percent still shows, as Task Manager's
// heat map does. Nothing for an unknown whole.
[[nodiscard]] inline float Heat(const double value, const double total) noexcept {
    if (!(total > 0.0) || !(value > 0.0)) return 0.0F;
    const double share = value / total;
    return static_cast<float>(std::sqrt(share > 1.0 ? 1.0 : share));
}

}  // namespace gtg::tray::details
