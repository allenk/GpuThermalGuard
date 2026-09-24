#pragma once

#include <cstdint>

// Busy animations for the OSD, as arithmetic.
//
// Every effect answers the same question: how bright is the block at
// (column, row) at this many milliseconds in, from 0 to 1. The painter then
// fills those blocks in the record's own accent colour and knows nothing else.
// Adding an effect means adding a function, not a branch inside a paint
// routine -- which is the shape the compact cells had before they were
// rewritten, and it did not scale past two cases.
//
// Keeping the state here rather than in the window buys the same thing it
// bought `osd_compact.hpp`: it is testable without a desktop, and a future
// portable UI layer would keep it unchanged.
//
// Nothing here holds state between frames. Each effect is a pure function of
// (block, elapsed), so a frame can be drawn at any time, out of order, or not
// at all, and it will still look right. That also makes the tests exhaustive
// rather than sampled.
namespace gtg::tray::animation {

// The look, selected by the caller. Deliberately one knob: a parallel enum
// naming the *activity* and mapping it onto a look reads well, but it is a
// second way to choose the same property, and two are worse than either.
enum class Effect {
    Sweep,      // one band travelling left to right
    Rain,       // columns of blocks falling at their own speeds
};

// What an indicator draws unless its caller asks for something else. Named
// rather than repeated at each call site, so changing the house style is one
// edit and not a search.
inline constexpr Effect kDefaultEffect = Effect::Rain;

namespace detail {

// A cheap integer hash, so every column gets its own speed, phase and trail
// length without a generator and without storing anything. The same column is
// always the same column, which is why a frame can be drawn at any time.
[[nodiscard]] constexpr std::uint32_t Hash(std::uint32_t value) noexcept {
    value ^= value >> 16;
    value *= 0x7feb352dU;
    value ^= value >> 15;
    value *= 0x846ca68bU;
    value ^= value >> 16;
    return value;
}

}  // namespace detail

// How far the head of a column has fallen, in blocks, wrapped to its cycle.
// Integer arithmetic throughout: std::floor is not constexpr before C++23, and
// this has to be usable in a constant expression to be worth testing.
[[nodiscard]] constexpr int RainTrail(int column) noexcept {
    // Three to ten blocks. Uneven trails are what stop the columns reading as
    // one moving line.
    return 3 + static_cast<int>((detail::Hash(static_cast<std::uint32_t>(column) + 1u) >> 24) & 0x7u);
}

[[nodiscard]] constexpr float RainIntensity(int column, int row, int rows,
                                            std::uint64_t elapsed_ms) noexcept {
    if (rows <= 0 || row < 0 || row >= rows || column < 0) return 0.0F;
    const std::uint32_t hash = detail::Hash(static_cast<std::uint32_t>(column) + 1u);
    const int trail = RainTrail(column);
    const int period = rows + trail;
    // Thousandths of a block per millisecond: 40 is a slow drip, 103 is brisk.
    const int speed = 40 + static_cast<int>(hash & 0x3Fu);
    const int phase = static_cast<int>((hash >> 8) % static_cast<std::uint32_t>(period));
    const long long advanced =
        static_cast<long long>(phase) * 1000LL +
        static_cast<long long>(elapsed_ms) * static_cast<long long>(speed) / 10LL;
    const int head = static_cast<int>((advanced / 1000LL) % static_cast<long long>(period));
    const int behind = head - row;
    if (behind < 0 || behind >= trail) return 0.0F;
    // Brightest at the head, fading along the trail. Never reaches zero at the
    // last block, or the tail would end on an invisible step.
    return 1.0F - static_cast<float>(behind) / static_cast<float>(trail);
}

// The sweep expressed on the same grid: one band crossing a single row, so a
// painter that can draw rain can draw this too without knowing which it has.
inline constexpr std::uint64_t kSweepPeriodMs = 1200;

[[nodiscard]] constexpr float SweepIntensity(int column, int row, int columns,
                                             int rows,
                                             std::uint64_t elapsed_ms) noexcept {
    if (columns <= 0 || column < 0 || column >= columns) return 0.0F;
    // The band lives on the last row; everything above it stays dark.
    if (rows <= 0 || row != rows - 1) return 0.0F;
    const int width = columns / 3 + 1;
    const int travel = columns + width;
    const auto phase = static_cast<int>(
        (elapsed_ms % kSweepPeriodMs) * static_cast<std::uint64_t>(travel) /
        kSweepPeriodMs);
    const int head = phase - width;
    const int behind = column - head;
    if (behind < 0 || behind >= width) return 0.0F;
    return 1.0F - static_cast<float>(behind) / static_cast<float>(width) * 0.4F;
}

[[nodiscard]] constexpr float IntensityAt(Effect effect, int column, int row,
                                          int columns, int rows,
                                          std::uint64_t elapsed_ms) noexcept {
    switch (effect) {
        case Effect::Sweep:
            return SweepIntensity(column, row, columns, rows, elapsed_ms);
        case Effect::Rain:
            return RainIntensity(column, row, rows, elapsed_ms);
    }
    return 0.0F;
}

// Block size in dip. A compact cell is 72 x 51, so four gives eighteen columns
// by twelve rows -- coarse enough to read as falling blocks rather than noise,
// fine enough not to look like a progress bar with delusions.
inline constexpr float kBlockDip = 4.0F;

[[nodiscard]] constexpr int BlocksAcross(float width_dip) noexcept {
    return width_dip <= 0.0F ? 0 : static_cast<int>(width_dip / kBlockDip);
}

}  // namespace gtg::tray::animation
