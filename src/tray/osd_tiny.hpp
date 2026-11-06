#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <vector>

#include "fps/fps_history.hpp"
#include "topapp/alert_rule.hpp"
#include "topapp/board.hpp"
#include "tray/osd_compact.hpp"

// Tiny: the compact dashboard folded into its header and one cell, whatever
// the record count (owner, 2026-10-18: even a single row covered the reader's
// UI). A form of the compact dashboard, not a shape of its own -- the chevron
// still opens the expanded view. The cell carries one bar per placed record,
// each with its initial under it, and no values: a reader who wants numbers
// unfolds it.
namespace gtg::tray::tiny {

// The one-column compact header over one cell, a little taller than a compact
// cell so the bars have room to move and their letters a row of their own,
// and the compact bottom margin.
inline constexpr int kColumns = 1;
inline constexpr int kWidthDip = compact::CollapsedWidth(kColumns);
inline constexpr float kCellHeightDip = 64.0F;
inline constexpr int kHeightDip =
    static_cast<int>(compact::kCellsTopDip + kCellHeightDip) + compact::CollapsedBottomMargin(1);

[[nodiscard]] constexpr compact::Footprint Footprint() noexcept {
    return {kWidthDip, kHeightDip, kColumns, 1};
}

// The bars sit inside the cell with this inset on every side, at most 9 dip
// wide so five records do not read as blocks, and 2 dip apart so eight still
// fit (6 dip each).
inline constexpr float kBarInsetDip = 5.0F;
inline constexpr float kBarGapDip = 2.0F;
inline constexpr float kBarMaxWidthDip = 9.0F;
// Bars alone were hard to tell apart (owner, 2026-10-18), so each has its
// record's initial under it: below rather than inside, which would cover the
// low readings an idle machine lives in, or above, where the bars would grow
// into them. 7 dip letters, compared at 100 and 175 % against 6 dip, which
// was barely legible at 100 %.
inline constexpr float kLetterRowDip = 9.0F;
inline constexpr float kLetterEmDip = 7.0F;

// T P G V C R F N H: Temp, Power, GPU, VRAM, CPU, RAM, FPS, Network, Hosts.
[[nodiscard]] constexpr wchar_t Letter(const compact::Metric metric) noexcept {
    switch (metric) {
        case compact::Metric::Temperature:
            return L'T';
        case compact::Metric::Power:
            return L'P';
        case compact::Metric::Vram:
            return L'V';
        case compact::Metric::Gpu:
            return L'G';
        case compact::Metric::Cpu:
            return L'C';
        case compact::Metric::Ram:
            return L'R';
        case compact::Metric::Fps:
            return L'F';
        case compact::Metric::Network:
            return L'N';
        case compact::Metric::Hosts:
            return L'H';
    }
    return L'?';
}

struct BarSlot {
    float x{};   // dip, window coordinates
    float y{};
    float width{};
    float height{};
};

// Bar `index` of `count`, the group centred in the cell, above the letter row.
[[nodiscard]] constexpr BarSlot BarAt(const int index, const int count) noexcept {
    const int n = count < 1 ? 1 : count;
    const float left = static_cast<float>(compact::kCellsMarginDip) + kBarInsetDip;
    const float room = static_cast<float>(compact::kCellWidthDip) - kBarInsetDip * 2.0F;
    const float fitted = (room - kBarGapDip * static_cast<float>(n - 1)) / static_cast<float>(n);
    const float width = fitted < kBarMaxWidthDip ? fitted : kBarMaxWidthDip;
    const float group = width * static_cast<float>(n) + kBarGapDip * static_cast<float>(n - 1);
    return {left + (room - group) / 2.0F + static_cast<float>(index) * (width + kBarGapDip),
            compact::kCellsTopDip + kBarInsetDip, width,
            kCellHeightDip - kBarInsetDip * 2.0F - kLetterRowDip};
}

// A reading's height on its bar, 0 to 1, on an axis from `bottom` to `top`.
// Clamped: a bar never leaves its track, and a reading that is not a number
// stands at the bottom rather than anywhere it could be mistaken for one.
[[nodiscard]] constexpr double Level(const double value, const double bottom,
                                     const double top) noexcept {
    if (!(top > bottom)) return 0.0;
    const double level = (value - bottom) / (top - bottom);
    if (!(level > 0.0)) return 0.0;
    return level < 1.0 ? level : 1.0;
}

// FPS is drawn for its stability, not its rate (owner, 2026-10-18): against
// the median of the same program's last 20 s, never against the refresh rate,
// so a steady 30, 60 or 120 fps game looks the same -- the bar resting on its
// reference -- and a drop is dramatic: the bar moves three times the
// deviation. The median rather than the mean, so a drop does not drag its own
// reference down with it.
inline constexpr std::uint64_t kFpsWindowMs = 20'000;
// The history records at 2 Hz; a newest reading older than this is not now.
inline constexpr std::uint64_t kFpsFreshMs = 1'500;
// Below this the median says nothing yet, and the bar rests on the reference.
inline constexpr std::size_t kFpsMinimumSamples = 4;
inline constexpr double kFpsReferenceLevel = 2.0 / 3.0;
inline constexpr double kFpsGain = 3.0;
inline constexpr double kFpsWarnDeviation = 0.10;
inline constexpr double kFpsAlarmDeviation = 0.25;

// Either direction counts: stability is the question, and a rate that leaps
// is as unsteady as one that falls.
enum class Severity { Steady, Warn, Alarm };

[[nodiscard]] constexpr Severity FpsSeverity(const double deviation) noexcept {
    const double size = deviation < 0.0 ? -deviation : deviation;
    if (size >= kFpsAlarmDeviation) return Severity::Alarm;
    if (size >= kFpsWarnDeviation) return Severity::Warn;
    return Severity::Steady;
}

[[nodiscard]] constexpr double FpsLevel(const double deviation) noexcept {
    return Level(kFpsReferenceLevel * (1.0 + kFpsGain * deviation), 0.0, 1.0);
}

struct FpsStability {
    double current{};
    double reference{};
    double deviation{};   // current / reference - 1
    double level{};       // 0 to 1
    Severity severity{Severity::Steady};
};

// The newest reading against the median of the readings before it from the
// same program and surface, back to 20 s. Nothing when the newest reading is
// missing or old: the bar is then empty, never zero.
[[nodiscard]] inline std::optional<FpsStability> MeasureFpsStability(
    const std::deque<fps::HistorySample>& samples, const std::uint64_t now_ms) {
    if (samples.empty()) return std::nullopt;
    const fps::HistorySample& newest = samples.back();
    if (!newest.displayed_fps) return std::nullopt;
    if (now_ms > newest.monotonic_ms && now_ms - newest.monotonic_ms > kFpsFreshMs)
        return std::nullopt;

    // The history only accepts samples in time order, so walking back from the
    // newest one is walking back in time.
    std::vector<double> window;
    for (auto it = samples.rbegin(); it != samples.rend(); ++it) {
        if (newest.monotonic_ms - it->monotonic_ms > kFpsWindowMs) break;
        // Another program, or the same one presenting elsewhere, starts over:
        // its rate says nothing about this one's.
        if (!(it->identity == newest.identity) || it->surface != newest.surface) break;
        if (it->displayed_fps) window.push_back(*it->displayed_fps);
    }

    FpsStability result;
    result.current = *newest.displayed_fps;
    result.reference = result.current;
    if (window.size() >= kFpsMinimumSamples) {
        const auto middle = window.begin() + static_cast<std::ptrdiff_t>(window.size() / 2);
        std::nth_element(window.begin(), middle, window.end());
        result.reference = *middle;
        if (window.size() % 2 == 0)
            result.reference = (result.reference + *std::max_element(window.begin(), middle)) / 2.0;
    }
    result.deviation = result.reference > 0.0 ? result.current / result.reference - 1.0 : 0.0;
    result.level = FpsLevel(result.deviation);
    result.severity = FpsSeverity(result.deviation);
    return result;
}

// What one tick's bars are drawn from, wherever they are drawn: this
// machine's Tiny, and the state sent to paired hosts, which draw a host's bars
// as that host draws them itself.
struct BarInputs {
    std::optional<double> temperature_c;
    std::optional<double> power_w;
    std::optional<double> vram_percent;
    std::optional<double> gpu_percent;
    std::optional<double> cpu_percent;
    std::optional<double> ram_percent;
    // The larger of received and sent, against the window's peak, as the
    // network curve is drawn.
    std::optional<double> network_bytes_per_second;
    double network_peak{};
    std::optional<FpsStability> fps;
    int trigger_temperature_c{85};
    int safe_power_w{300};
    std::optional<double> maximum_power_w;
    // Protection: a temperature warning raises temperature; safe power raises
    // temperature and power.
    bool warning{};
    bool safe_power{};
    std::uint32_t top_app_alerts{};   // topapp::Bit flags
    bool host_memory_low{};
};

struct BarState {
    std::optional<double> level;   // nothing: the track stays empty, never zero
    std::optional<double> tick;
    bool raised{};
    bool alarm{};   // FPS far from steady: drawn white
};

[[nodiscard]] inline BarState DescribeBar(const compact::Metric metric, const BarInputs& in) {
    BarState bar;
    const auto place = [&](const std::optional<double> value, const double bottom,
                           const double top) {
        if (value) bar.level = Level(*value, bottom, top);
    };
    const auto alert = [&](const topapp::Record record) {
        return (in.top_app_alerts & topapp::Bit(record)) != 0;
    };
    switch (metric) {
        case compact::Metric::Temperature:
            place(in.temperature_c, compact::kTemperatureAxisBottomC,
                  compact::kTemperatureAxisTopC);
            bar.tick = Level(static_cast<double>(in.trigger_temperature_c),
                             compact::kTemperatureAxisBottomC, compact::kTemperatureAxisTopC);
            bar.raised = in.warning || in.safe_power;
            break;
        case compact::Metric::Power: {
            const double top = compact::PowerAxisTop(in.maximum_power_w, in.safe_power_w);
            place(in.power_w, 0.0, top);
            bar.tick = Level(static_cast<double>(in.safe_power_w), 0.0, top);
            bar.raised = in.safe_power;
            break;
        }
        case compact::Metric::Vram:
            place(in.vram_percent, 0.0, 100.0);
            bar.tick = Level(topapp::kVramThresholds.enter, 0.0, 100.0);
            bar.raised = alert(topapp::Record::Vram);
            break;
        case compact::Metric::Gpu:
            place(in.gpu_percent, 0.0, 100.0);
            bar.tick = Level(topapp::kGpuThresholds.enter, 0.0, 100.0);
            bar.raised = alert(topapp::Record::Gpu);
            break;
        case compact::Metric::Cpu:
            place(in.cpu_percent, 0.0, 100.0);
            bar.tick = Level(topapp::kCpuThresholds.enter, 0.0, 100.0);
            bar.raised = alert(topapp::Record::Cpu);
            break;
        case compact::Metric::Ram:
            place(in.ram_percent, 0.0, 100.0);
            bar.tick = Level(topapp::kRamThresholds.enter, 0.0, 100.0);
            bar.raised = alert(topapp::Record::Ram) || (in.host_memory_low && in.ram_percent);
            break;
        case compact::Metric::Network:
            place(in.network_bytes_per_second, 0.0, in.network_peak > 0.0 ? in.network_peak : 1.0);
            break;
        case compact::Metric::Fps:
            if (in.fps) {
                bar.level = in.fps->level;
                bar.tick = kFpsReferenceLevel;
                bar.raised = in.fps->severity != Severity::Steady;
                bar.alarm = in.fps->severity == Severity::Alarm;
            }
            break;
        case compact::Metric::Hosts:
            // Not a reading: its bar is drawn in segments, one per host.
            break;
    }
    return bar;
}

// The fold control, as the owner drew it (design/tiny_mode_1.png and
// tiny_mode_2.png, 175 %): a hairline across the bottom margin, under the
// cells, broken in the middle by a small chevron on a plate -- up over the
// cells, down in Tiny, the header chevron's convention. Shown only while the
// header shows its buttons; hidden, and not pressable, while it is idle.
inline constexpr float kFoldFromBottomDip =
    static_cast<float>(compact::CollapsedBottomMargin(1)) / 2.0F;
inline constexpr float kFoldGlyphWidthDip = 4.0F;     // 7 px at 175 %
inline constexpr float kFoldGlyphHeightDip = 2.25F;   // 4 px
inline constexpr float kFoldPlateWidthDip = 5.0F;     // 9 px
inline constexpr float kFoldPlateHeightDip = 3.5F;    // 6 px
// The rule runs under the cells, from the cells' margin on either side, and
// stops this far either side of the glyph's centre (a 13 dip break).
inline constexpr float kFoldRuleInsetDip = static_cast<float>(compact::kCellsMarginDip);
inline constexpr float kFoldRuleBreakDip = 6.5F;

// A press roughly at the bottom middle is enough (owner): 40 dip wide,
// centred, and 11 dip tall -- the 7 dip margin and the 4 dip at the foot of a
// compact cell, below its curve. So it never takes a press meant
// for a curve, and in Tiny it starts below the bars.
inline constexpr float kFoldPressWidthDip = 40.0F;
inline constexpr float kFoldPressHeightDip = 11.0F;

// `width` and `height` are the window's, in pixels.
[[nodiscard]] constexpr bool FoldHit(const int x, const int y, const int width, const int height,
                                     const float scale) noexcept {
    const float centre = static_cast<float>(width) / 2.0F;
    const float half = kFoldPressWidthDip / 2.0F * scale;
    const float top = static_cast<float>(height) - kFoldPressHeightDip * scale;
    const float px = static_cast<float>(x);
    const float py = static_cast<float>(y);
    return px >= centre - half && px < centre + half && py >= top &&
           py < static_cast<float>(height);
}

}  // namespace gtg::tray::tiny
