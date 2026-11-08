#include "core/power_envelope.hpp"
#include "net/throughput.hpp"
#include "net/action.hpp"
#include "net/verdict.hpp"
#include "core/protection.hpp"
#include "supervision/recovery_policy.hpp"
#include "supervision/supervisor.hpp"
#include "tray/window_position.hpp"
#include "tray/control_baseline.hpp"
#include "timing/ui_phase_timing.hpp"
#include "tray/osd_compact.hpp"
#include "tray/osd_placement.hpp"
#include "tray/osd_tiny.hpp"
#include "tray/trip_markers.hpp"
#include "tray/timeline_track.hpp"
#include "tray/hotkey_capture.hpp"
#include "tray/grey_icon.hpp"
#include "core/working_power_apply.hpp"
#include "core/restore_prompt_gate.hpp"
#include "core/restore_policy.hpp"
#include "nvml/nvml_loader.hpp"
#include "nvml/power_operation.hpp"
#include "telemetry/telemetry_history.hpp"
#include "telemetry/telemetry_freshness.hpp"
#include "snapshot/ui_snapshot.hpp"
#include "localization/localization.hpp"
#include "logging/deferred_queue.hpp"
#include "logging/logger.hpp"
#include "service/pipe_server.hpp"
#include "timing/protection_timing.hpp"
#include "fps/fps_rate.hpp"
#include "fps/present_path.hpp"
#include "fps/play_clock.hpp"
#include "fps/backend_resolution.hpp"
#include "fps/observer_admission.hpp"
#include "fps/fps_history.hpp"
#include "fps/dxgi_event.hpp"
#include "fps/display_correlator.hpp"
#include "settings/settings.hpp"
#include "sysmem/host_memory.hpp"
#include "sysmem/reclaim.hpp"
#include "tray/osd_animation.hpp"
#include "topapp/alert_rule.hpp"
#include "topapp/app_name.hpp"
#include "topapp/board.hpp"
#include "topapp/cpu_ledger.hpp"
#include "topapp/leader.hpp"
#include "topapp/marquee.hpp"
#include "topapp/pdh_instance.hpp"
#include "topapp/sweep_budget.hpp"
#include "topapp/system_process.hpp"
#include "peer/admission.hpp"
#include "peer/crypto.hpp"
#include "peer/hosts_view.hpp"
#include "peer/keys.hpp"
#include "peer/link.hpp"
#include "peer/pairing.hpp"
#include "peer/pairing_code.hpp"
#include "peer/protocol.hpp"
#include "peer/replay.hpp"
#include "hostlog/frame.hpp"
#include "hostlog/writer.hpp"
#include "tray/hosts_bridge.hpp"
#include "tray/osd_hosts.hpp"

#include <winsock2.h>
#include <windows.h>
#include <objidl.h>
#include <gdiplus.h>

#include <bit>
#include <cmath>
#include <cstdlib>
#include <cwctype>
#include <limits>
#include <span>
#include <array>
#include <deque>
#include <optional>
#include <cstring>
#include <exception>
#include <algorithm>
#include <filesystem>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>
#include <thread>
#include <atomic>
#include <chrono>
#include <format>
#include <fstream>
#include <iterator>

bool WtlHeadersAvailable();

namespace {

void Require(const bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void TestOsdCompact() {
    using namespace gtg::tray::compact;
    Require(Width(true) == 388 && Height(true) == 88, "compact dashboard footprint");
    Require(Width(true) == Width(false), "toggle button stays at same x");
    const auto flat = SparkRange(50.0, 50.0, 10.0);
    Require(flat.second - flat.first >= 10.0 && flat.first < 50.0 && flat.second > 50.0,
        "flat mini trend has stable padded range");
    const auto zero = SparkRange(0.0, 1.0, 20.0);
    Require(zero.first == 0.0 && zero.second >= 20.0, "near-zero range does not exaggerate");
    const auto wide = SparkRange(10.0, 90.0, 20.0);
    Require(wide.first <= 10.0 && wide.second >= 90.0, "range contains observed extremes");
    Require(Width(false) == 388 && Height(false) == 330, "expanded unchanged");
    for (float scale : {1.0F, 1.25F, 1.5F, 2.0F}) {
        for (bool collapsed : {false, true}) {
            const int width = static_cast<int>(Width(collapsed) * scale);
            const int height = static_cast<int>(25 * scale);
            Require(ToggleHit(width - 10, height / 2, width, height), "button hit");
            Require(!ToggleHit(10, 10, width, height), "drag not button");
            Require(!ToggleHit(width, 0, width, height), "right exclusive");
            Require(!ToggleHit(width - 10, height, width, height), "bottom exclusive");
            Require(!ToggleHit(-1, 0, width, height), "negative outside");
        }
    }
    Gesture gesture;
    Require(!gesture.Release(true), "up without down ignored");
    gesture.Press(true);
    Require(!gesture.Release(false), "release outside cancels");
    gesture.Press(true);
    gesture.Cancel();
    Require(!gesture.Release(true), "capture loss cancels");
    gesture.Press(false);
    Require(!gesture.Release(true), "press outside ignored");
    gesture.Press(true);
    Require(gesture.Release(true) && !gesture.Release(true), "one click one toggle");
    Require(Resolve(true, true, true, true, true, true) == Status::Fault, "fault first");
    Require(Resolve(false, true, true, true, true, true) == Status::Protected, "latch before freshness");
    Require(Resolve(false, false, true, true, true, true) == Status::Unavailable, "unavailable before warning");
    Require(Resolve(false, false, false, true, true, true) == Status::Delayed, "delay before warning");
    Require(Resolve(false, false, false, false, true, true) == Status::Warning, "warning before armed");
    Require(Resolve(false, false, false, false, false, true) == Status::Monitoring, "armed");
    Require(Resolve(false, false, false, false, false, false) == Status::Initializing, "initializing");
    Require(PulseAlpha(false, true, 0) == 255 && PulseAlpha(true, false, 1000) == 255,
        "normal or reduced motion static");
    Require(PulseAlpha(true, true, 0) != PulseAlpha(true, true, 1000), "alert pulses");
    for (unsigned i = 0; i < 10000; i += 100)
        Require(PulseAlpha(true, true, i) >= 150, "pulse never disappears");
    const auto normal = ChooseFootprint(true, false, false, 1366, 768);
    Require(normal.width == 388 && normal.height == 88 && normal.columns == 5 &&
            normal.rows == 1,
        "five records stay one row in the compact OSD");
    // History of this line, because it has now turned twice: a sixth record
    // widened the overlay to 464, then wrapped to a second row at 388, and now
    // widens again. One row is the intended default, and it became safe to
    // return to once the arrangement editor made two rows a choice.
    const auto six = ChooseFootprint(true, false, true, 1366, 768);
    Require(six.width == 464 && six.height == 88 && six.rows == 1,
        "a sixth record widens the default single row");
    const auto compact_grid = ChooseFootprint(true, false, true, 683, 384);
    Require(compact_grid.width == 464 && compact_grid.height == 88,
        "a high-DPI work area still fits the single default row");
    // Narrower than the arrangement: the cells shrink, the arrangement does
    // not change. The reader chose the shape, so rearranging it behind their
    // back is a worse answer than a cramped one.
    const auto cramped = ChooseFootprint(true, true, true, 400, 768);
    Require(cramped.rows == 1 && cramped.columns == -7,
        "a work area too narrow keeps the arrangement and shrinks the cells");
    Require(cramped.width == 320, "and clamps to the narrow width");
    const auto expanded = ChooseFootprint(false, false, true, 1366, 768);
    Require(expanded.width == 388 && expanded.height == ExpandedHeight(6) && expanded.columns == 1,
            "the expanded OSD is the lanes' window where the screen holds it");
    // A work area too short for the window no longer turns the lanes into
    // a narrow card view: the window takes the height there is, down to its
    // floor, and stays the lanes' width.
    const auto expanded_short = ChooseFootprint(false, false, true, 683, 300);
    Require(
        expanded_short.width == 388 && expanded_short.height == 300 && expanded_short.columns == 1,
        "a short work area shortens the expanded window instead of switching view");
    Require(ChooseFootprint(false, false, true, 683, 100).height == ExpandedMinimumHeight(6),
            "and never below the floor");
    for (int dpi : {96, 120, 144, 192}) {
        const int work_width_dip = 1366 * 96 / dpi;
        const int work_height_dip = 768 * 96 / dpi;
        for (bool collapsed : {false, true}) {
            for (bool ram : {false, true}) {
                const auto choice = ChooseFootprint(
                    collapsed, ram, true, work_width_dip, work_height_dip);
                Require(choice.width <= work_width_dip &&
                        choice.height <= work_height_dip,
                    "layout fits 1366x768 work area at each tested DPI");
            }
        }
    }
}

// Tiny: one size whatever the records, every bar inside the cell and apart
// from the next, the fold control below every curve and clear of the header,
// and FPS judged for its steadiness against its own recent median.
void TestOsdTiny() {
    using namespace gtg::tray;
    const auto footprint = tiny::Footprint();
    Require(footprint.width == 84 && footprint.height == 101 && footprint.columns == 1 &&
                footprint.rows == 1,
            "tiny is the one-column header over one cell");
    Require(footprint.width == compact::CollapsedWidth(1) &&
                !compact::HeaderShowsTitle(footprint.columns),
            "tiny's header is the one-column compact header");
    Require(footprint.height < compact::kCollapsedSingleRowHeight + compact::kCollapsedRowPitch,
            "tiny stays shorter than two compact rows");

    const float cell_left = static_cast<float>(compact::kCellsMarginDip);
    const float cell_right = cell_left + static_cast<float>(compact::kCellWidthDip);
    for (int count = compact::kCellsPerRow; count <= compact::kRecordCount; ++count) {
        float previous_right = 0.0F;
        for (int i = 0; i < count; ++i) {
            const auto bar = tiny::BarAt(i, count);
            Require(bar.width >= 5.0F && bar.width <= tiny::kBarMaxWidthDip,
                    "bars are wide enough to see and never blocks");
            Require(bar.x >= cell_left + tiny::kBarInsetDip - 0.01F &&
                        bar.x + bar.width <= cell_right - tiny::kBarInsetDip + 0.01F,
                    "bars stay inside the cell across");
            Require(bar.y >= compact::kCellsTopDip &&
                        bar.y + bar.height + tiny::kLetterRowDip <=
                            compact::kCellsTopDip + tiny::kCellHeightDip - tiny::kBarInsetDip,
                    "bars and their letters stay inside the cell down");
            Require(bar.height >= 40.0F, "the letters leave the bars room to move");
            if (i > 0)
                Require(bar.x - previous_right >= tiny::kBarGapDip - 0.01F, "bars never touch");
            previous_right = bar.x + bar.width;
        }
        const auto first = tiny::BarAt(0, count);
        const auto last = tiny::BarAt(count - 1, count);
        Require(std::abs((first.x - cell_left) - (cell_right - last.x - last.width)) < 0.01F,
                "the bars are centred in the cell");
    }
    Require(tiny::BarAt(0, 8).width == 6.0F, "eight records give 6 dip bars");
    std::wstring letters;
    for (int i = 0; i < compact::kRecordCount; ++i) {
        const wchar_t letter = tiny::Letter(static_cast<compact::Metric>(i));
        Require(letter >= L'A' && letter <= L'Z' && letters.find(letter) == std::wstring::npos,
                "every record has its own capital letter");
        letters.push_back(letter);
    }
    Require(tiny::Letter(compact::Metric::Gpu) == L'G' &&
                tiny::Letter(compact::Metric::Vram) == L'V' &&
                tiny::Letter(compact::Metric::Temperature) == L'T',
            "the letters are the records' initials");

    Require(tiny::Level(50.0, 0.0, 100.0) == 0.5, "a reading stands at its place on the axis");
    Require(tiny::Level(-5.0, 0.0, 100.0) == 0.0 && tiny::Level(150.0, 0.0, 100.0) == 1.0,
            "a bar never leaves its track");
    Require(tiny::Level(std::nan(""), 0.0, 100.0) == 0.0,
            "a reading that is not a number stands at the bottom");
    Require(tiny::Level(5.0, 10.0, 10.0) == 0.0, "an empty axis draws nothing");
    Require(
        tiny::Level(85.0, compact::kTemperatureAxisBottomC, compact::kTemperatureAxisTopC) > 0.7 &&
            tiny::Level(85.0, compact::kTemperatureAxisBottomC, compact::kTemperatureAxisTopC) <
                0.75,
        "temperature stands on the expanded lane's axis");
    Require(compact::PowerAxisTop(std::nullopt, 300) == 600.0 &&
                compact::PowerAxisTop(600.0, 300) == 600.0 &&
                compact::PowerAxisTop(50.0, 300) == 100.0,
            "power stands on the expanded lane's axis");

    // The fold control, in every window it sits in: Tiny, one row of five and
    // of eight, a single column, two rows.
    const std::pair<int, int> windows[]{{84, 95}, {388, 88}, {540, 88}, {84, 418}, {388, 143}};
    for (const float scale : {1.0F, 1.25F, 1.5F, 1.75F, 2.0F, 2.5F}) {
        for (const auto& [width_dip, height_dip] : windows) {
            const int width = compact::ToPixels(static_cast<float>(width_dip), scale);
            const int height = compact::ToPixels(static_cast<float>(height_dip), scale);
            const int x = width / 2;
            const int y =
                static_cast<int>(static_cast<float>(height) - tiny::kFoldFromBottomDip * scale);
            Require(tiny::FoldHit(x, y, width, height, scale), "the drawn control can be pressed");
            const int half =
                static_cast<int>(std::floor(tiny::kFoldPressWidthDip / 2.0F * scale)) - 1;
            Require(tiny::FoldHit(x - half, y, width, height, scale) &&
                        tiny::FoldHit(x + half, y, width, height, scale),
                    "roughly the bottom middle is enough");
            Require(!tiny::FoldHit(x - half - 3, y, width, height, scale) &&
                        !tiny::FoldHit(x + half + 3, y, width, height, scale),
                    "but not the bottom corners");
            Require(!tiny::FoldHit(x, compact::ToPixels(25.0F, scale) - 1, width, height, scale),
                    "the control never takes a press in the header");
            const int above =
                height - static_cast<int>(std::ceil(tiny::kFoldPressHeightDip * scale));
            Require(!tiny::FoldHit(x, above - 1, width, height, scale),
                    "the control ends below the curves");
            Require(!tiny::FoldHit(x, height, width, height, scale), "the bottom is exclusive");
            Require(!compact::ToggleHit(x, y, width, compact::ToPixels(25.0F, scale)) &&
                        !compact::LockHit(x, y, width, compact::ToPixels(25.0F, scale)),
                    "the control is not the chevron or the lock");
        }
    }
    // A compact cell's curve is 10 dip tall at 37 dip into the cell.
    Require(compact::kCellsTopDip + 47.0F <=
                static_cast<float>(compact::kCollapsedSingleRowHeight) - tiny::kFoldPressHeightDip,
            "the press area is below a compact cell's curve");
    const auto bar = tiny::BarAt(0, compact::kCellsPerRow);
    Require(bar.y + bar.height + tiny::kLetterRowDip <=
                static_cast<float>(tiny::kHeightDip) - tiny::kFoldPressHeightDip,
            "in Tiny the press area is below the letters");

    // Folding keeps the top-right corner, as the chevron does, and unfolding
    // returns to where it was.
    const POINT folded = placement::ToggleKeepingButton({1000, 500}, 540, footprint.width);
    Require(folded.x + footprint.width == 1000 + 540 && folded.y == 500,
            "folding keeps the top-right corner still");
    const POINT unfolded = placement::ToggleKeepingButton(folded, footprint.width, 540);
    Require(unfolded.x == 1000 && unfolded.y == 500, "unfolding returns to where it was");
}

void TestTinyFpsStability() {
    using gtg::fps::HistorySample;
    using gtg::fps::Identity;
    namespace tiny = gtg::tray::tiny;
    const Identity game{1234, 5678};
    const auto push = [](std::deque<HistorySample>& samples, const std::uint64_t ms,
                         const std::optional<double> fps, const Identity id,
                         const std::uint64_t surface) {
        HistorySample sample;
        sample.monotonic_ms = ms;
        sample.displayed_fps = fps;
        sample.identity = id;
        sample.surface = surface;
        samples.push_back(sample);
    };
    const auto steady = [&](const double fps) {
        std::deque<HistorySample> samples;
        for (int i = 0; i < 40; ++i)
            push(samples, 1'000 + static_cast<std::uint64_t>(i) * 500, fps, game, 7);
        return samples;
    };
    const std::uint64_t newest = 1'000 + 39 * 500;
    const auto equal = [](const double a, const double b) {
        return std::abs(a - b) < 1e-9;
    };

    for (const double fps : {30.0, 60.0, 120.0}) {
        const auto result = tiny::MeasureFpsStability(steady(fps), newest);
        Require(result && equal(result->reference, fps) && equal(result->deviation, 0.0) &&
                    equal(result->level, tiny::kFpsReferenceLevel) &&
                    result->severity == tiny::Severity::Steady,
                "a steady game rests on its reference whatever its rate");
    }

    const auto with_newest = [&](const double fps) {
        auto samples = steady(60.0);
        samples.back().displayed_fps = fps;
        return tiny::MeasureFpsStability(samples, newest);
    };
    const auto dip = with_newest(55.0);
    Require(dip && equal(dip->reference, 60.0) && equal(dip->level, 0.5) &&
                dip->severity == tiny::Severity::Steady,
            "55 against 60 drops the bar from two thirds to a half");
    const auto drop = with_newest(48.0);
    Require(drop && equal(drop->level, 2.0 / 3.0 * 0.4) && drop->severity == tiny::Severity::Warn,
            "48 against 60 is a warning");
    const auto crash = with_newest(42.0);
    Require(crash && crash->severity == tiny::Severity::Alarm && crash->level < 0.1,
            "a 30 % drop is an alarm");
    const auto leap = with_newest(78.0);
    Require(leap && leap->severity == tiny::Severity::Alarm && equal(leap->level, 1.0),
            "a leap is as unsteady as a fall, and the bar stays in its track");
    Require(tiny::FpsSeverity(0.10) == tiny::Severity::Warn &&
                tiny::FpsSeverity(-0.10) == tiny::Severity::Warn &&
                tiny::FpsSeverity(0.0999) == tiny::Severity::Steady &&
                tiny::FpsSeverity(-0.25) == tiny::Severity::Alarm,
            "severity bounds hold in both directions");

    Require(!tiny::MeasureFpsStability(steady(60.0), newest + 2'000),
            "an old newest reading is not now");
    auto missing = steady(60.0);
    missing.back().displayed_fps.reset();
    Require(!tiny::MeasureFpsStability(missing, newest), "no reading is an empty bar, not zero");
    Require(!tiny::MeasureFpsStability({}, newest), "no history is an empty bar");

    // Another program starts over: its rate says nothing about this one's.
    auto switched = steady(120.0);
    const Identity other{4321, 8765};
    for (int i = 1; i <= 3; ++i)
        push(switched, newest + static_cast<std::uint64_t>(i) * 500, 60.0, other, 7);
    const auto early = tiny::MeasureFpsStability(switched, newest + 1'500);
    Require(early && equal(early->reference, 60.0) && equal(early->deviation, 0.0),
            "too few samples of the new program: the bar rests on its reference");
    push(switched, newest + 2'000, 45.0, other, 7);
    const auto later = tiny::MeasureFpsStability(switched, newest + 2'000);
    Require(later && equal(later->reference, 60.0) && later->severity == tiny::Severity::Alarm,
            "the new program is judged against its own median, not the last one's");
    auto moved = steady(120.0);
    push(moved, newest + 500, 60.0, game, 8);
    const auto other_surface = tiny::MeasureFpsStability(moved, newest + 500);
    Require(other_surface && equal(other_surface->reference, 60.0),
            "the same program presenting elsewhere starts over too");

    // Only the last 20 s count.
    std::deque<HistorySample> aged;
    for (int i = 0; i <= 10; ++i)
        push(aged, static_cast<std::uint64_t>(i) * 500, 100.0, game, 7);
    for (int i = 0; i < 4; ++i)
        push(aged, 25'500 + static_cast<std::uint64_t>(i) * 500, 60.0, game, 7);
    const auto windowed = tiny::MeasureFpsStability(aged, 27'000);
    Require(windowed && equal(windowed->reference, 60.0), "readings older than 20 s do not count");

    std::deque<HistorySample> even;
    for (const double fps : {50.0, 60.0, 70.0, 80.0})
        push(even, 1'000 + even.size() * 500, fps, game, 7);
    const auto middle = tiny::MeasureFpsStability(even, 2'500);
    Require(middle && equal(middle->reference, 65.0), "an even count takes the middle pair");
}

// The backend label as the design defines it: the
// events decide the family, the module list only names it, and a label that
// cannot be established is the weaker claim, never a guess.
void TestFpsBackendResolution() {
    using gtg::fps::Backend;
    using gtg::fps::LoadedRuntimes;
    using gtg::fps::ResolveBackend;
    const auto modules = [](bool d3d9, bool d3d11, bool d3d12, bool vulkan, bool opengl) {
        LoadedRuntimes loaded{};
        loaded.d3d9 = d3d9;
        loaded.d3d11 = d3d11;
        loaded.d3d12 = d3d12;
        loaded.vulkan = vulkan;
        loaded.opengl = opengl;
        loaded.readable = true;
        return loaded;
    };
    // Module list refused: the events alone.
    Require(ResolveBackend(LoadedRuntimes{}, true) == Backend::DXGI,
            "refused module list with a composition token is DX");
    Require(ResolveBackend(LoadedRuntimes{}, false) == Backend::Unknown,
            "refused module list without a token is no label");
    // Within the DXGI family, one runtime loaded names it.
    Require(ResolveBackend(modules(false, true, false, false, false), true) == Backend::D3D11,
            "d3d11 alone under a composition token is D11");
    Require(ResolveBackend(modules(false, false, true, false, false), true) == Backend::D3D12,
            "d3d12 alone under a composition token is D12");
    Require(ResolveBackend(modules(false, false, false, false, false), true) == Backend::DXGI,
            "a composition token with neither d3d module is DX");
    // Both loaded: the module list cannot tell which one presented. A DX11 game
    // (2026-10-03) presents through D3D11 -- GetDevice(ID3D11Device) on the
    // intercepted swap chain 295/295, ID3D12Device 0/295 E_NOINTERFACE --
    // with d3d12.dll also loaded, and was labelled D12. A D3D12 game keeping
    // d3d11 for interop is the mirror image. The label is the weaker claim.
    Require(ResolveBackend(modules(false, true, true, false, false), true) == Backend::DXGI,
            "d3d11 and d3d12 both loaded is DX, not a guess at either");
    // No composition token: the DXGI modules are not the renderer.
    Require(ResolveBackend(modules(false, true, true, false, false), false) == Backend::Unknown,
            "d3d modules without a composition token name nothing");
    Require(ResolveBackend(modules(false, false, false, true, true), false) == Backend::Vulkan,
            "Vulkan outranks the opengl32 its ICD loads");
    Require(ResolveBackend(modules(false, false, false, false, true), false) == Backend::OpenGL,
            "opengl32 alone is GL");
    Require(ResolveBackend(modules(true, false, false, false, false), false) == Backend::D3D9,
            "d3d9 alone without a token is D9");
}

// Under a composition token the
// live devices name the runtime; the module rule is the fallback.
void TestFpsBackendFromRuntimeDevices() {
    using gtg::fps::Backend;
    using gtg::fps::LoadedRuntimes;
    using gtg::fps::ResolveBackend;
    using gtg::fps::RuntimeDevices;
    LoadedRuntimes both{};
    both.d3d11 = true;
    both.d3d12 = true;
    both.readable = true;
    const auto devices = [](std::uint32_t d3d11, std::uint32_t d3d12) {
        return RuntimeDevices{true, d3d11, d3d12};
    };
    // The four measured cases (findings, 2026-10-04).
    Require(ResolveBackend(both, true, devices(1, 0)) == Backend::D3D11,
            "D3D11 device only is D11, even with d3d12.dll loaded");
    Require(ResolveBackend(both, true, devices(0, 1)) == Backend::D3D12,
            "D3D12 device only is D12");
    Require(ResolveBackend(both, true, devices(1, 1)) == Backend::DXGI,
            "a device of each runtime is DX");
    Require(ResolveBackend(both, true, devices(2, 0)) == Backend::D3D11,
            "a DX11 game: two D3D11 devices and no D3D12 device is D11");
    // Devices name the runtime even when the module list was refused.
    Require(ResolveBackend(LoadedRuntimes{}, true, devices(0, 1)) == Backend::D3D12,
            "live devices name the runtime without a module list");
    // Fallbacks to the module rule.
    Require(ResolveBackend(both, true, devices(0, 0)) == Backend::DXGI,
            "no device seen falls back to the module rule");
    Require(ResolveBackend(both, true, RuntimeDevices{false, 1, 0}) == Backend::DXGI,
            "devices from providers that were not both enabled are not used");
    // No composition token: the events say DXGI did not present; devices of
    // a D3D runtime do not change that.
    LoadedRuntimes vulkan{};
    vulkan.vulkan = true;
    vulkan.readable = true;
    Require(ResolveBackend(vulkan, false, devices(1, 0)) == Backend::Vulkan,
            "without a composition token a D3D device does not override VK");
}

void TestFpsDeviceLedger() {
    using gtg::fps::D3DRuntime;
    using gtg::fps::DeviceLedger;
    DeviceLedger ledger;
    ledger.Live(D3DRuntime::D3D11, 0x1000);
    ledger.Live(D3DRuntime::D3D11, 0x1000);   // created, then reported: one device
    ledger.Live(D3DRuntime::D3D11, 0x2000);
    ledger.Live(D3DRuntime::D3D12, 0x3000);
    Require(ledger.Count(D3DRuntime::D3D11) == 2, "a device reported twice counts once");
    Require(ledger.Count(D3DRuntime::D3D12) == 1, "runtimes are counted apart");
    ledger.Destroyed(D3DRuntime::D3D12, 0x3000);
    Require(ledger.Count(D3DRuntime::D3D12) == 0,
            "a probe device created and destroyed leaves none");
    ledger.Destroyed(D3DRuntime::D3D11, 0x9999);
    Require(ledger.Count(D3DRuntime::D3D11) == 2, "destroying an unknown device changes nothing");
    // Without an identity (0) the ledger can only count, never below zero.
    ledger.Live(D3DRuntime::D3D12, 0);
    ledger.Live(D3DRuntime::D3D12, 0);
    ledger.Destroyed(D3DRuntime::D3D12, 0);
    Require(ledger.Count(D3DRuntime::D3D12) == 1, "anonymous devices are counted");
    ledger.Destroyed(D3DRuntime::D3D12, 0);
    ledger.Destroyed(D3DRuntime::D3D12, 0);
    Require(ledger.Count(D3DRuntime::D3D12) == 0, "an anonymous count never goes below zero");
    ledger.Clear();
    Require(ledger.Count(D3DRuntime::D3D11) == 0, "Clear forgets every device");
}

void TestFpsRateIsIndependentAndHonest() {
    using gtg::fps::Identity;
    using gtg::fps::RateTracker;
    using gtg::fps::Status;
    RateTracker tracker;
    const Identity game{42, 1234};
    const Identity reused_pid{42, 5678};
    Require(tracker.Read(1'000'000, true).status == Status::Unavailable,
        "no target is unavailable, not zero");
    tracker.SetTarget(game, 0);
    Require(tracker.Read(500'000, true).status == Status::Warmup,
        "one-second window must warm up");
    for (std::uint64_t t = 0; t <= 1'000'000; t += 20'000) {
        tracker.RecordDisplayed(game, 0xA, t);
    }
    const auto primary = tracker.Read(1'000'000, true);
    Require(primary.status == Status::Ready && primary.displayed_fps >= 49.0 &&
            primary.displayed_fps <= 51.0, "50 measured display changes become about 50 FPS");
    tracker.RecordDisplayed(game, 0xB, 1'000'000);
    const auto multiple = tracker.Read(1'000'000, true);
    Require(multiple.status == Status::Ready && multiple.displayed_fps <= 51.0,
        "secondary swapchain must not be summed into FPS");
    const auto buffered = tracker.Read(2'100'000, true);
    Require(buffered.status == Status::Ready && buffered.displayed_fps >= 49.0,
        "ETW buffer delivery gap must not make a presenting game briefly zero");
    Require(tracker.Read(1'100'000, false).status == Status::Unavailable,
        "observer failure cannot retain plausible FPS");
    tracker.SetTarget(reused_pid, 1'100'000);
    tracker.RecordDisplayed(game, 0xA, 1'200'000);
    Require(tracker.Read(1'200'000, true).status == Status::Warmup,
        "PID reuse must not import old process frames");
    Require(tracker.Read(2'200'000, true).status == Status::Unavailable,
        "a foreground app with no DXGI evidence is unsupported, not zero FPS");
    tracker.RecordDisplayed(reused_pid, 0xC, 2'200'000);
    tracker.RecordDisplayed(reused_pid, 0xC, 2'220'000);
    Require(tracker.Read(4'300'000, true).status == Status::Unavailable,
        "stalled or occluded display outcome is unavailable, not assumed zero");
    tracker.MarkLost(4'300'000);
    Require(tracker.Read(4'300'000, true).status == Status::Warmup,
        "event loss invalidates the prior window");
    tracker.ClearTarget();
    Require(tracker.Read(3'500'000, true).status == Status::Unavailable,
        "no target after exit is unavailable");

    RateTracker sustained;
    sustained.SetTarget(game, 0);
    for (std::uint64_t t = 0; t <= 24'000'000; t += 20'000) {
        sustained.RecordDisplayed(game, 0xA, t);
    }
    Require(sustained.Read(24'000'000, true).status == Status::Ready,
        "bounded ring must remain usable after many ordinary frames");
    RateTracker overloaded;
    overloaded.SetTarget(game, 0);
    for (std::uint64_t t = 0; t <= 600'000; t += 500) {
        overloaded.RecordDisplayed(game, 0xA, t);
    }
    Require(overloaded.Read(1'000'000, true).status == Status::Unavailable,
        "ring overflow inside the measured window cannot publish a false rate");
}

void TestFpsRateRejectsShortStartupBurst() {
    using gtg::fps::Identity;
    using gtg::fps::RateTracker;
    using gtg::fps::Status;
    const Identity game{9, 100};
    RateTracker tracker;
    tracker.SetTarget(game, 0);
    tracker.RecordDisplayed(game, 0xA, 1'000'000);
    tracker.RecordDisplayed(game, 0xA, 1'001'000);
    Require(tracker.Read(1'010'000, true).status == Status::Warmup,
        "two Presents in 1 ms cannot establish sustained 1000 FPS");
    for (std::uint64_t t = 1'010'000; t <= 1'610'000; t += 10'000)
        tracker.RecordDisplayed(game, 0xA, t);
    const auto sustained = tracker.Read(1'610'000, true);
    Require(sustained.status == Status::Ready && sustained.displayed_fps > 90.0 &&
            sustained.displayed_fps < 110.0,
        "sustained high-rate Present stream remains measurable");
}

void TestFpsRateReclaimsOldSwapchains() {
    using gtg::fps::Identity;
    using gtg::fps::RateTracker;
    using gtg::fps::Status;
    const Identity game{88, 99};
    RateTracker tracker;
    tracker.SetTarget(game, 0);
    for (std::uint64_t surface = 1; surface <= 5; ++surface) {
        const auto begin = (surface - 1) * 2'000'000;
        for (std::uint64_t t = begin; t <= begin + 1'000'000; t += 20'000)
            tracker.RecordDisplayed(game, surface, t);
    }
    const auto latest = tracker.Read(9'000'000, true);
    Require(latest.status == Status::Ready && latest.surface == 5 &&
            latest.displayed_fps >= 49.0 && latest.displayed_fps <= 51.0,
        "sequential swap-chain recreation cannot permanently exhaust surface slots");

    RateTracker transient_overload;
    transient_overload.SetTarget(game, 0);
    for (std::uint64_t surface = 1; surface <= 5; ++surface)
        transient_overload.RecordDisplayed(game, surface, 0);
    Require(transient_overload.Read(1'000'000, true).status == Status::Unavailable,
        "simultaneous fifth surface must fail closed");
    for (std::uint64_t t = 20'000; t <= 5'000'000; t += 20'000)
        transient_overload.RecordDisplayed(game, 1, t);
    const auto recovered = transient_overload.Read(5'000'000, true);
    Require(recovered.status == Status::Ready && recovered.surface == 1 &&
            recovered.displayed_fps >= 49.0 && recovered.displayed_fps <= 51.0,
        "transient capacity ambiguity must recover after one surface stabilizes");
}

// Measured 2026-10-01 on a commercial game's login screen: PresentMon saw
// 120 presents/s, every one of them reaching the screen, while GTG's display
// correlator matched 42 of 41 994 in ~351 s (0.1%). "Displayed wins while a
// displayed frame is recent" then let that trickle override the complete
// kernel-present stream, and every switch reset the window, so the indicator
// read ~2, jumped to ~120 when a gap exceeded the stale window, and fell back.
void TestFpsPlayClock() {
    using gtg::fps::FormatPlayTime;
    using gtg::fps::PlayClock;
    constexpr std::uint64_t kGameA = 0xA, kGameB = 0xB;
    constexpr std::uint64_t kMinute = 60'000;
    // GTG ticks at 500 ms; `program` is whoever has a Ready FPS reading.
    struct Run {
        PlayClock clock;
        std::uint64_t now{};
        void For(const std::uint64_t ms, const std::optional<std::uint64_t> program) {
            for (std::uint64_t end = now + ms; now < end;) {
                now += 500;
                clock.Tick(now, program);
            }
        }
    };
    // To the nearest minute: the first tick has no predecessor and counts nothing.
    const auto minutes = [](const PlayClock& c) { return (c.ElapsedMs() + 30'000) / 60'000; };

    {   // ALT+TAB and loading screens pause the count; they do not reset it.
        Run r;
        r.For(10 * kMinute, kGameA);
        r.For(2 * kMinute, std::nullopt);
        r.For(5 * kMinute, kGameA);
        Require(minutes(r.clock) == 15 && r.clock.Program() == kGameA,
                "a pause under three minutes keeps counting the same game");
    }
    {   // A short look at another program does not take over.
        Run r;
        r.For(20 * kMinute, kGameA);
        r.For(1 * kMinute, kGameB);
        r.For(5 * kMinute, kGameA);
        Require(minutes(r.clock) == 25 && r.clock.Program() == kGameA,
                "another program for a minute is not a switch");
    }
    {   // Switching games: after three minutes the new one takes over, and
        // the three minutes it played meanwhile are added back.
        Run r;
        r.For(30 * kMinute, kGameA);
        r.For(3 * kMinute, kGameB);
        Require(r.clock.Program() == kGameB && minutes(r.clock) == 3,
                "B takes over three minutes after A, with those three minutes added back");
        r.For(2 * kMinute, kGameB);
        Require(r.clock.Program() == kGameB && minutes(r.clock) == 5,
                "and keeps counting from there");
    }
    {   // Three minutes with no FPS at all ends the count.
        Run r;
        r.For(45 * kMinute, kGameA);
        r.For(3 * kMinute, std::nullopt);
        Require(!r.clock.Program() && r.clock.ElapsedMs() == 0,
                "three minutes without FPS ends the count");
        r.For(2 * kMinute, kGameA);
        Require(minutes(r.clock) == 2, "the same game afterwards starts a new count");
    }
    {   // A tick after sleep or a stall is not play time.
        PlayClock clock;
        clock.Tick(1'000, kGameA);
        clock.Tick(1'500, kGameA);
        clock.Tick(3'600'000, kGameA);
        Require(clock.ElapsedMs() == 500, "a long gap between ticks is not counted");
    }

    const auto text = [](const std::uint64_t ms) { return std::wstring(FormatPlayTime(ms).data()); };
    Require(text(0).empty() && text(59'999).empty(), "nothing under a minute");
    Require(text(kMinute) == L"1 min" && text(59 * kMinute + 59'000) == L"59 min", "minutes");
    Require(text(60 * kMinute) == L"1.0 hr", "an hour");
    Require(text(7 * 60 * kMinute + 13 * kMinute) == L"7.2 hr", "tenths, rounded down");
    Require(text(150ULL * 60 * kMinute) == L"99.9 hr", "held at 99.9 hr");
}

void TestFpsIndependentFlipKeepsCorrelatorHealthy() {
    using gtg::fps::DisplayCorrelator;
    using gtg::fps::PresentPath;
    using gtg::fps::TokenKey;
    constexpr std::uint64_t kSurface = 0x22F'00000000ULL;
    constexpr std::uint32_t kGameThread = 100;
    constexpr std::uint32_t kDwmThread = 200;

    // One target frame as a DX11 game produced them on 2026-10-07 under
    // independent flip: DXGI start/stop, the Win32k token, InFrame, and DWM's
    // surface update -- and no DWM flip, ever.
    const auto run = [&](const bool gated) {
        DisplayCorrelator correlator;
        PresentPath path;
        path.NoteTargetSurface(kSurface);
        correlator.SetDwmThread(kDwmThread);
        for (std::uint64_t frame = 1; frame <= 200; ++frame) {
            const std::uint64_t t = frame * 4'032;
            const TokenKey key{kSurface, frame, 1};
            correlator.OnPresentStart(kGameThread, 0xC0FFEE, 0, t);
            correlator.OnPresentStop(kGameThread, 0);
            correlator.OnToken(kGameThread, key, t);
            path.NoteInFrame(kSurface, true);
            if (!gated || path.ForwardsInFrame(kSurface, true)) correlator.OnInFrame(key);
            correlator.OnSurfaceUpdate(key);
            if (!correlator.Healthy()) return frame;
        }
        return std::uint64_t{0};
    };
    const auto failed_at = run(false);
    Require(failed_at >= 30 && failed_at <= 40,
            "ungated, the waiting queue overflows within ~33 frames (the field failure)");
    Require(run(true) == 0, "gated, 200 independent-flip frames leave the correlator healthy");

    PresentPath path;
    path.NoteTargetSurface(kSurface);
    Require(path.ForwardsInFrame(kSurface, false), "a composed target frame is correlated");
    Require(path.ForwardsInFrame(0xBEEF, true), "another program's frames are untouched");
}

void TestFpsIndependentFlipFallsBackToPresented() {
    using gtg::fps::Identity;
    using gtg::fps::Measure;
    using gtg::fps::PresentPath;
    using gtg::fps::RateTracker;
    using gtg::fps::Status;
    const Identity game{84400, 999};
    constexpr std::uint64_t kSurface = 0x297'00000000ULL;
    constexpr std::uint64_t kEnd = 10'000'000;

    // A DX11 game, measured 2026-10-07, widget not pinned: every
    // InFrame of the game's surface independent, ~248 kernel presents/s, and
    // ~26/s frames the DWM correlator still matched.
    struct Event { std::uint64_t t; bool displayed; };
    std::vector<Event> events;
    for (std::uint64_t t = 0; t <= kEnd; t += 4'032) events.push_back({t, false});   // ~248/s
    for (std::uint64_t t = 1'000; t <= kEnd; t += 38'461) events.push_back({t, true}); // ~26/s
    std::sort(events.begin(), events.end(),
              [](const Event& a, const Event& b) { return a.t < b.t; });
    const auto feed = [&](RateTracker& tracker, const PresentPath* path) {
        for (const auto& e : events) {
            if (!e.displayed) tracker.RecordPresented(game, 0x10, e.t);
            else if (path == nullptr || path->CountsAsDisplayed())
                tracker.RecordDisplayed(game, kSurface, e.t);
        }
    };

    // The defect, kept as a record of why the gate exists: the trickle wins.
    {
        RateTracker tracker;
        tracker.SetTarget(game, 0);
        feed(tracker, nullptr);
        const auto r = tracker.Read(kEnd, true);
        Require(r.status == Status::Ready && r.measure == Measure::Displayed &&
                    r.displayed_fps < 40.0,
                "ungated, a 26/s matched trickle out-ranks 248 presents/s (the field reading)");
    }

    // Gated by the observed path: presented, and close to what the game submits.
    {
        PresentPath path;
        path.NoteTargetSurface(kSurface);
        path.NoteInFrame(kSurface, true);
        RateTracker tracker;
        tracker.SetTarget(game, 0);
        feed(tracker, &path);
        const auto r = tracker.Read(kEnd, true);
        Require(r.status == Status::Ready && r.measure == Measure::Presented &&
                    r.displayed_fps > 230.0 && r.displayed_fps < 265.0,
                "independent flip: reads the presented rate, labelled presented");
    }

    // Composed (widget pinned): displayed still counts.
    {
        PresentPath path;
        path.NoteTargetSurface(kSurface);
        path.NoteInFrame(kSurface, false);
        Require(path.CountsAsDisplayed(), "composed: displayed counts");
        path.NoteInFrame(kSurface, true);
        Require(!path.CountsAsDisplayed(), "the latest InFrame decides");
        path.NoteInFrame(kSurface, false);
        Require(path.CountsAsDisplayed(), "back to composed, displayed counts again");
    }

    // Only the target's own surfaces move it.
    {
        PresentPath path;
        path.NoteTargetSurface(kSurface);
        path.NoteInFrame(0xBEEF, true);
        Require(path.CountsAsDisplayed(), "another program's independent flip is ignored");
        Require(!path.IsTarget(0), "a zero surface is never the target");
        for (std::uint64_t s = 1; s <= 6; ++s) path.NoteTargetSurface(s);
        Require(path.IsTarget(6) && !path.IsTarget(kSurface),
                "bounded: the oldest surface gives way");
        path.Reset();
        Require(!path.IsTarget(6) && path.CountsAsDisplayed(), "reset forgets everything");
    }
}

void TestFpsRateSparseDisplayedCannotOverridePresents() {
    using gtg::fps::Identity;
    using gtg::fps::RateTracker;
    using gtg::fps::Status;
    const Identity game{50612, 777};
    constexpr std::uint64_t kPresentEveryUs = 8'333;   // 120 presents/s

    struct Event { std::uint64_t t; bool displayed; };
    auto feed = [&](RateTracker& tracker, std::vector<Event> events) {
        std::sort(events.begin(), events.end(),
                  [](const Event& a, const Event& b) { return a.t < b.t; });
        for (const auto& e : events) {
            if (e.displayed) tracker.RecordDisplayed(game, 0xD, e.t);
            else tracker.RecordPresented(game, 0x10, e.t);
        }
    };
    auto presents = [&](std::uint64_t until) {
        std::vector<Event> out;
        for (std::uint64_t t = 0; t <= until; t += kPresentEveryUs)
            out.push_back({t, false});
        return out;
    };

    // 1. A steady 2/s trickle of matched frames beside 120 presents/s. The
    //    trickle never goes stale. Before the fix: Ready, 2.0 FPS -- the number
    //    the owner saw on the login screen.
    {
        RateTracker tracker;
        tracker.SetTarget(game, 0);
        auto events = presents(10'000'000);
        for (std::uint64_t t = 250'000; t <= 10'000'000; t += 500'000)
            events.push_back({t, true});
        feed(tracker, events);
        const auto r = tracker.Read(10'000'000, true);
        Require(r.status == Status::Ready && r.displayed_fps >= 108.0 &&
                r.displayed_fps <= 132.0,
            "a 2/s matched-frame trickle cannot override 120 presents/s");
    }

    // 2. Rare bursts, closer to the field shape. Each burst flips the measure and
    //    resets the window, and the window never refills before the next one:
    //    before the fix, all 57 reads over 30 s were Warmup -- never a number.
    //    After it, every Ready read must be ~120, and there must be plenty.
    {
        RateTracker tracker;
        tracker.SetTarget(game, 0);
        auto events = presents(30'000'000);
        // A burst of three matched frames every 3.5 s, 2 ms apart.
        for (std::uint64_t b = 1'000'000; b <= 30'000'000; b += 3'500'000)
            for (std::uint64_t k = 0; k < 3; ++k) events.push_back({b + k * 2'000, true});
        std::sort(events.begin(), events.end(),
                  [](const Event& a, const Event& b) { return a.t < b.t; });
        std::size_t next = 0;
        std::size_t checked = 0;
        for (std::uint64_t until = 500'000; until <= 30'000'000; until += 500'000) {
            for (; next < events.size() && events[next].t <= until; ++next) {
                if (events[next].displayed) tracker.RecordDisplayed(game, 0xD, events[next].t);
                else tracker.RecordPresented(game, 0x10, events[next].t);
            }
            if (until < 2'000'000) continue;
            const auto r = tracker.Read(until, true);
            if (r.status != Status::Ready) continue;
            ++checked;
            Require(r.displayed_fps >= 108.0 && r.displayed_fps <= 132.0,
                "rare matched-frame bursts cannot pull the reading away from 120");
        }
        Require(checked >= 40,
            "the burst scenario produced enough Ready readings to mean something");
    }

    // 3. The design must survive: when displayed frames keep pace -- every frame
    //    a 60 Hz display can show, beside a 120/s present rate -- what reached
    //    the screen is still the stronger claim and still wins. Passes before
    //    the fix (59.9988) and must keep passing after it.
    {
        RateTracker tracker;
        tracker.SetTarget(game, 0);
        auto events = presents(10'000'000);
        for (std::uint64_t t = 4'000; t <= 10'000'000; t += 16'667)
            events.push_back({t, true});
        feed(tracker, events);
        const auto r = tracker.Read(10'000'000, true);
        Require(r.status == Status::Ready && r.displayed_fps >= 57.0 &&
                r.displayed_fps <= 63.0,
            "displayed frames that keep pace still define the rate");
    }
}

void TestAccessDeniedObserverProbation() {
    using gtg::fps::ModuleEvidence;
    using gtg::fps::ObserverAdmission;
    using gtg::fps::ChooseObserverAdmission;
    using gtg::fps::ObserverProbation;
    Require(ChooseObserverAdmission(ModuleEvidence::DxgiModules) ==
            ObserverAdmission::Normal,
        "inspectable DXGI module hint keeps the ordinary path");
    Require(ChooseObserverAdmission(ModuleEvidence::AccessDenied) ==
            ObserverAdmission::Probation,
        "only access-denied module enumeration admits ETW probation");
    Require(ChooseObserverAdmission(ModuleEvidence::NoDxgiModules) ==
            ObserverAdmission::Reject &&
            ChooseObserverAdmission(ModuleEvidence::OtherFailure) ==
            ObserverAdmission::Reject,
        "inspectable non-DXGI and other failures must not start ETW");

    ObserverProbation probe;
    probe.Begin(ObserverAdmission::Probation, 1'000);
    Require(!probe.CanPublish() && !probe.Expired(5'999),
        "module denial alone cannot publish FPS or expire early");
    Require(probe.Expired(6'000) &&
            probe.RetryAt(6'000) == 36'000,
        "five-second no-evidence probation stops with thirty-second cooldown");
    probe.Begin(ObserverAdmission::Probation, 40'000);
    probe.NoteDisplayed();
    Require(probe.CanPublish() && !probe.Expired(50'000),
        "verified displayed frame unlocks normal FPS warm-up");
    probe.Begin(ObserverAdmission::Normal, 60'000);
    Require(probe.CanPublish() && !probe.Expired(70'000),
        "ordinary module-hint target has no probation deadline");
}

void TestFpsHistoryFollowsSampledForeground() {
    using gtg::fps::History;
    using gtg::fps::Identity;
    using gtg::fps::Snapshot;
    using gtg::fps::Status;
    History history;
    const Identity a{42, 1234};
    const Identity b{43, 5678};
    history.Record(1'000, a, Snapshot{Status::Ready, 60.0, a});
    history.Record(1'500, b, Snapshot{Status::Ready, 60.0, a});
    history.Record(2'000, b, Snapshot{Status::Ready, 120.0, b});
    history.Record(2'500, a, Snapshot{Status::Warmup, 0.0, a});
    history.Record(3'000, a, Snapshot{Status::Ready, 72.0, a});
    Require(history.Samples().size() == 5, "each presentation poll has a history point");
    Require(history.Samples()[0].displayed_fps == 60.0 &&
            !history.Samples()[1].displayed_fps &&
            history.Samples()[2].displayed_fps == 120.0 &&
            !history.Samples()[3].displayed_fps &&
            history.Samples()[4].displayed_fps == 72.0,
            "A-B-A focus switches cannot reuse another process's cached FPS");
    history.Record(3'500, std::nullopt, Snapshot{Status::Ready, 72.0, a});
    Require(!history.Samples().back().displayed_fps,
            "shell or ineligible foreground leaves a no-data marker");
    history.Record(4'000, Identity{42, 9999}, Snapshot{Status::Ready, 72.0, a});
    Require(!history.Samples().back().displayed_fps,
            "PID reuse cannot inherit an old process rate");
    history.Clear();
    Require(history.Samples().empty(), "disabling FPS clears its volatile history");
}

void TestFpsHistoryBoundsAndZeroSemantics() {
    using gtg::fps::History;
    using gtg::fps::Identity;
    using gtg::fps::Snapshot;
    using gtg::fps::Status;
    History history;
    const Identity app{77, 100};
    history.Record(0, app, Snapshot{Status::Ready, 0.0, app});
    history.Record(500, app, Snapshot{Status::Warmup, 0.0, app});
    Require(history.Samples()[0].displayed_fps == 0.0 &&
            !history.Samples()[1].displayed_fps,
            "confirmed zero and unavailable waterline have different data states");
    for (std::uint64_t ms = 1'000; ms <= History::kRetainedDurationMs + 1'000;
         ms += 500) {
        history.Record(ms, app, Snapshot{Status::Ready, 60.0, app});
    }
    Require(history.Samples().size() <= History::kMaxSamples &&
            history.Samples().front().monotonic_ms >= 1'000,
            "one-hour FPS history remains bounded");
}

void TestFpsHistoryBreaksTargetAndSurfaceTransitions() {
    using gtg::fps::CanConnectHistorySamples;
    using gtg::fps::History;
    using gtg::fps::Identity;
    using gtg::fps::Snapshot;
    using gtg::fps::Status;
    History history;
    const Identity a{42, 1234};
    const Identity b{43, 5678};
    history.Record(1'000, a, Snapshot{Status::Ready, 60.0, a, 0xA});
    history.Record(1'500, a, Snapshot{Status::Ready, 61.0, a, 0xA});
    history.Record(2'000, b, Snapshot{Status::Ready, 120.0, b, 0xB});
    history.Record(2'500, b, Snapshot{Status::Ready, 121.0, b, 0xC});
    history.Record(3'000, b, Snapshot{Status::Warmup, 0.0, b, 0xC});
    const auto& samples = history.Samples();
    Require(samples[0].surface == 0xA && samples[2].surface == 0xB,
        "history preserves the measured swap-chain identity");
    Require(CanConnectHistorySamples(samples[0], samples[1]),
        "consecutive samples from the same target and surface connect");
    Require(!CanConnectHistorySamples(samples[1], samples[2]),
        "foreground application transition must not be interpolated");
    Require(!CanConnectHistorySamples(samples[2], samples[3]),
        "swap-chain transition must not be interpolated");
    Require(!CanConnectHistorySamples(samples[3], samples[4]),
        "warm-up is a no-data break, not a measured zero");
}

void TestFpsHistoryDrawsDarkGapsWithoutInventingSamples() {
    using gtg::fps::History;
    using gtg::fps::Identity;
    using gtg::fps::Snapshot;
    using gtg::fps::Status;
    const Identity a{42, 1234};
    const Identity b{43, 5678};
    History history;
    history.Record(1'000, a, Snapshot{Status::Ready, 60.0, a, 0xA});
    history.Record(1'500, a, Snapshot{Status::Unavailable, 0.0, a});
    history.Record(2'000, a, Snapshot{Status::Ready, 65.0, a, 0xA});
    history.Record(2'500, a, Snapshot{Status::Warmup, 0.0, a});
    std::vector<std::pair<std::uint64_t, std::uint64_t>> dark;
    gtg::fps::ForEachHistoryStroke(history.Samples(), 0, 3'000,
        [&](const auto& from, const auto& to, bool dim) {
            if (dim) dark.emplace_back(from.monotonic_ms, to.monotonic_ms);
        });
    Require(dark.size() == 2 && dark[0] == std::pair{1'000ULL, 2'000ULL} &&
            dark[1] == std::pair{2'000ULL, 2'500ULL},
        "same-target gaps and pending tail must use a dim connecting stroke");
    Require(!history.Samples()[1].displayed_fps && !history.Samples()[3].displayed_fps,
        "dim visual bridge must not materialize a measured FPS sample");

    history.Record(3'000, b, Snapshot{Status::Warmup, 0.0, b});
    history.Record(3'500, a, Snapshot{Status::Ready, 70.0, a, 0xA});
    history.Record(4'000, a, Snapshot{Status::Ready, 75.0, a, 0xB});
    dark.clear();
    unsigned bright = 0;
    gtg::fps::ForEachHistoryStroke(history.Samples(), 0, 4'000,
        [&](const auto& from, const auto& to, bool dim) {
            if (dim) dark.emplace_back(from.monotonic_ms, to.monotonic_ms);
            else ++bright;
        });
    Require(dark.size() == 3 && dark[0] == std::pair{1'000ULL, 2'000ULL} &&
            dark[1] == std::pair{2'000ULL, 3'500ULL} &&
            dark[2] == std::pair{3'500ULL, 4'000ULL} && bright == 0,
        "focus and surface changes must remain continuous but never bright");
    Require(history.Samples()[4].identity == b &&
            !history.Samples()[4].displayed_fps &&
            history.Samples()[6].surface == 0xB,
        "visual continuity cannot rewrite source identity or fill missing data");
}

void TestDxgiPresentMatchingRejectsFailedAndTestCalls() {
    using gtg::fps::DxgiPresentMatcher;
    using gtg::fps::RawDxgiEvent;
    DxgiPresentMatcher matcher;
    std::array<std::uint8_t, 20> start{};
    const std::uint64_t surface = 0x1234;
    std::memcpy(start.data(), &surface, sizeof(surface));
    const std::int32_t success = 0;
    const std::int32_t failed = static_cast<std::int32_t>(0x887A0001U);
    const std::int32_t occluded = 0x087A0001;
    Require(!matcher.OnEvent(RawDxgiEvent{42, 0, 8, start.data(), start.size(), 100})
                 .has_value(), "Present start alone is not counted");
    const auto good = matcher.OnEvent(RawDxgiEvent{
        43, 0, 8, reinterpret_cast<const std::uint8_t*>(&success), sizeof(success), 110});
    Require(good && good->surface == surface && good->timestamp_us == 100,
        "successful stop matches original swapchain and start time");
    (void)matcher.OnEvent(RawDxgiEvent{42, 0, 9, start.data(), start.size(), 200});
    Require(!matcher.OnEvent(RawDxgiEvent{
        43, 0, 9, reinterpret_cast<const std::uint8_t*>(&failed), sizeof(failed), 210}),
        "failed Present must not count");
    (void)matcher.OnEvent(RawDxgiEvent{42, 0, 9, start.data(), start.size(), 220});
    Require(!matcher.OnEvent(RawDxgiEvent{
        43, 0, 9, reinterpret_cast<const std::uint8_t*>(&occluded), sizeof(occluded), 230}),
        "positive DXGI status is not a verified presented frame");
    const std::uint32_t test_flag = 1;
    std::memcpy(start.data() + sizeof(surface), &test_flag, sizeof(test_flag));
    (void)matcher.OnEvent(RawDxgiEvent{42, 0, 10, start.data(), start.size(), 300});
    Require(!matcher.OnEvent(RawDxgiEvent{
        43, 0, 10, reinterpret_cast<const std::uint8_t*>(&success), sizeof(success), 310}),
        "DXGI_PRESENT_TEST must not count");
    Require(!matcher.OnEvent(RawDxgiEvent{42, 1, 8, start.data(), start.size(), 400}),
        "unknown event schema version must be rejected");
    Require(!matcher.OnEvent(RawDxgiEvent{42, 0, 8, start.data(), 4, 500}),
        "truncated payload must be rejected");
}

void TestComposedFlipRequiresVerifiedScanout() {
    using gtg::fps::DisplayCorrelator;
    using gtg::fps::TokenKey;
    DisplayCorrelator tracker;
    const TokenKey first{0xA, 1, 7};
    tracker.OnPresentStart(11, 0x1234, 0, 100);
    tracker.OnPresentStop(11, 0);
    tracker.OnToken(21, first, 110);  // Driver work may use another thread.
    tracker.OnInFrame(first);
    tracker.OnSurfaceUpdate(first);
    tracker.SetDwmThread(31);
    tracker.OnDwmFlip(31, 120);
    tracker.OnDwmQueue(31, 40);
    Require(!tracker.Pop().has_value(),
        "DXGI, Win32k and DWM submissions are not yet displayed");
    tracker.OnVSync(40, 130);
    const auto shown = tracker.Pop();
    Require(shown && shown->surface == 0x1234 && shown->timestamp_us == 130,
        "matching scanout emits one displayed frame for the target surface");
    tracker.OnVSync(40, 140);
    Require(!tracker.Pop().has_value(), "duplicate scanout cannot count twice");
}

void TestComposedFlipDropsAndLossFailClosed() {
    using gtg::fps::DisplayCorrelator;
    using gtg::fps::TokenKey;
    DisplayCorrelator tracker;
    tracker.SetDwmThread(31);
    const TokenKey dropped{0xA, 2, 7};
    tracker.OnPresentStart(11, 0x1234, 0, 200);
    tracker.OnPresentStop(11, 0);
    tracker.OnToken(21, dropped, 210);
    tracker.OnInFrame(dropped);
    tracker.OnSurfaceUpdate(dropped);
    tracker.OnDwmFlip(31, 220);
    tracker.OnDwmQueue(31, 41);
    tracker.OnDiscard(dropped);
    tracker.OnVSync(41, 230);
    Require(!tracker.Pop().has_value(), "discarded target frame stays absent");
    const TokenKey failed{0xA, 3, 7};
    tracker.OnPresentStart(11, 0x1234, 0, 300);
    tracker.OnPresentStop(11, static_cast<std::int32_t>(0x887A0001U));
    tracker.OnToken(21, failed, 310);
    tracker.OnInFrame(failed);
    tracker.OnSurfaceUpdate(failed);
    tracker.OnDwmFlip(31, 320);
    tracker.OnDwmQueue(31, 42);
    tracker.OnVSync(42, 330);
    Require(!tracker.Pop().has_value(), "failed DXGI call stays absent");
    tracker.MarkLost();
    Require(!tracker.Pop().has_value() && !tracker.Healthy(),
        "ETW loss invalidates every queued result");
}

void TestComposedFlipLongRunAndDwmThreadSwitch() {
    using gtg::fps::DisplayCorrelator;
    using gtg::fps::TokenKey;
    DisplayCorrelator tracker;
    tracker.SetDwmThread(31);
    for (std::uint32_t i = 1; i <= 1500; ++i) {
        const auto time_us = 100'000ULL + static_cast<std::uint64_t>(i) * 8'000;
        const TokenKey key{0xB, i, 9};
        tracker.OnPresentStart(11, 0x5678, 0, time_us);
        tracker.OnPresentStop(11, 0);
        tracker.OnToken(21, key, time_us + 100);
        tracker.OnInFrame(key);
        tracker.OnSurfaceUpdate(key);
        tracker.OnDwmFlip(31, time_us + 200);
        tracker.OnDwmQueue(31, i);
        tracker.OnVSync(i, time_us + 300);
        Require(tracker.Pop().has_value() && tracker.Healthy(),
            "fixed capacity must recycle completed frames during long runs");
    }
    tracker.SetDwmThread(32);
    const TokenKey next{0xB, 1501, 9};
    tracker.OnPresentStart(11, 0x5678, 0, 12'200'000);
    tracker.OnPresentStop(11, 0);
    tracker.OnToken(21, next, 12'200'100);
    tracker.OnInFrame(next);
    tracker.OnSurfaceUpdate(next);
    tracker.OnDwmFlip(32, 12'200'200);
    tracker.OnDwmQueue(32, 1501);
    tracker.OnVSync(1501, 12'200'300);
    Require(tracker.Pop().has_value() && tracker.Healthy(),
        "completed DWM thread migration must not permanently disable FPS");
}

void TestComposedFlipAmbiguousSurfacesFailClosed() {
    gtg::fps::DisplayCorrelator tracker;
    tracker.OnPresentStart(11, 0xAAAA, 0, 100);
    tracker.OnPresentStop(11, 0);
    tracker.OnPresentStart(12, 0xBBBB, 0, 101);
    tracker.OnPresentStop(12, 0);
    tracker.OnToken(21, {0xC, 1, 9}, 110);
    Require(!tracker.Healthy() && !tracker.Pop(),
        "deferred driver token cannot guess between two presenting surfaces");
}

void TestFpsPreferenceDefaultsOnWithoutTouchingProtectionSettings() {
    Require(gtg::settings::ResolveFpsPreference(false, 0),
        "missing user preference defaults Show FPS on");
    Require(!gtg::settings::ResolveFpsPreference(true, 0),
        "explicit user disable remains off");
    Require(gtg::settings::ResolveFpsPreference(true, 1),
        "explicit user enable remains on");
}

void TestWorkingPowerApply() {
    gtg::ProtectionConfig current;
    auto next = current;
    next.normal_power_w = 400;
    gtg::ProtectionController controller(current);
    (void)controller.ObserveTemperature(1000, 50);
    int writes = 0, safe_writes = 0, saves = 0;
    auto apply = [&](std::optional<int> temp, std::int64_t age, bool write_ok, bool save_ok) {
        return gtg::ApplyWorkingPower(controller, current, next, temp, age,
            [&] { ++writes; return write_ok; },
            [&] { ++safe_writes; return true; },
            [&] { ++saves; return save_ok; });
    };
    Require(writes == 0, "startup/config editing must not write");
    Require(apply(std::nullopt, 0, true, true).status == gtg::ApplyStatus::Rejected, "missing sample denied");
    Require(apply(50, 1000, true, true).status == gtg::ApplyStatus::Rejected, "stale sample denied");
    Require(apply(50, -1, true, true).status == gtg::ApplyStatus::Rejected, "invalid age denied");
    next.trigger_temperature_c = 95;
    Require(apply(83, 0, true, true).status == gtg::ApplyStatus::Rejected, "old predictive band still protects");
    next.normal_power_w = 0;
    Require(apply(50, 0, true, true).status == gtg::ApplyStatus::Rejected, "invalid config denied");
    next.normal_power_w = 400;
    next.trigger_temperature_c = 52;
    Require(apply(50, 0, true, true).status == gtg::ApplyStatus::Rejected, "new predictive band denied");
    Require(writes == 0 && saves == 0, "denied apply has no side effects");
    next.trigger_temperature_c = 85;
    Require(apply(50, 0, true, false).status == gtg::ApplyStatus::AppliedNotSaved, "persistence failure distinct");
    Require(current.normal_power_w == 400 && writes == 1 && safe_writes == 0, "verified hardware adopts config");
    Require(apply(50, 0, true, true).status == gtg::ApplyStatus::Applied, "explicit retry can save");
    next.normal_power_w = 450;
    auto failure = apply(50, 0, false, true);
    Require(failure.status == gtg::ApplyStatus::WriteFailed && failure.safe_verified, "failed write falls back");
    Require(controller.safe_latched() && current.normal_power_w == 400, "failed apply retains old config and latches");
    const int before = writes;
    Require(apply(50, 0, true, true).status == gtg::ApplyStatus::Rejected && writes == before, "latched apply cannot raise power");

    gtg::ProtectionController tripped(current);
    (void)tripped.ObserveTemperature(2000, 95);
    auto rejected = gtg::ApplyWorkingPower(tripped, current, next, 50, 0,
        [&] { ++writes; return true; }, [] { return true; }, [] { return true; });
    Require(rejected.status == gtg::ApplyStatus::Rejected && writes == before, "trip before request consumption wins");
}

void TestWorkingPowerFailureAndContinuity() {
    gtg::ProtectionConfig active;
    auto requested = active;
    requested.normal_power_w = 400;
    gtg::ProtectionController controller(active);
    for (int i = 0; i != 4; ++i) (void)controller.ObserveTemperature(i * 200, 70 + i * 2);
    auto ok = gtg::ApplyWorkingPower(controller, active, requested, 76, 0,
        [] { return true; }, [] { return false; }, [] { return true; });
    Require(ok.status == gtg::ApplyStatus::Applied, "cold Apply accepted");
    const auto next = controller.ObserveTemperature(800, 82);
    Require(next.rise_c_per_s > 0 && next.state == gtg::ProtectionState::PreTrip,
        "Apply must preserve predictor history");

    gtg::ProtectionController fresh(active);
    (void)fresh.ObserveTemperature(0, 50);
    int saves = 0;
    const auto failed = gtg::ApplyWorkingPower(fresh, active, requested, 50, 0,
        [] { return false; }, [] { return false; }, [&] { ++saves; return true; });
    Require(failed.status == gtg::ApplyStatus::WriteFailed && !failed.safe_verified,
        "unverified fallback must not claim protected");
    Require(fresh.safe_latched() && saves == 0, "failed writes latch without saving candidate");
}

constexpr std::uint64_t kGiB = 1024ULL * 1024ULL * 1024ULL;

bool Near(const std::optional<double> value, const double expected) {
    return value.has_value() && std::abs(*value - expected) < 0.01;
}

void TestHostMemoryComputeBoundaries() {
    using gtg::sysmem::Compute;

    // 128 GiB physical with a 32 GiB page file: the commit limit is the larger
    // denominator, so commit % normally sits BELOW physical %.
    const auto ordinary =
        Compute(128 * kGiB, 98 * kGiB, 160 * kGiB, 125 * kGiB);
    Require(Near(ordinary.physical_percent, 23.4375), "physical percent");
    Require(Near(ordinary.physical_used_gib, 30.0), "physical GiB");
    Require(Near(ordinary.commit_percent, 21.875), "commit percent");
    Require(Near(ordinary.commit_used_gib, 35.0), "commit GiB");
    Require(*ordinary.physical_percent > *ordinary.commit_percent,
            "commit normally reads below physical");

    // A zero total must not divide by zero and must not read as a measured 0 %.
    const auto zero = Compute(0, 0, 0, 0);
    Require(!zero.physical_percent && !zero.commit_percent,
            "zero total is unavailable, never 0 %");
    Require(!zero.physical_used_gib && !zero.commit_used_gib,
            "zero total reports no capacity");

    // Available above total is an inconsistent report, not a negative usage.
    const auto inconsistent =
        Compute(8 * kGiB, 9 * kGiB, 16 * kGiB, 20 * kGiB);
    Require(!inconsistent.physical_percent && !inconsistent.commit_percent,
            "inconsistent report is unavailable, never negative");

    const auto saturated = Compute(8 * kGiB, 0, 16 * kGiB, 0);
    Require(Near(saturated.physical_percent, 100.0), "fully used reads 100 %");
    Require(Near(saturated.commit_percent, 100.0), "commit saturates at 100 %");

    // A system-managed page file growing the limit lowers the percentage with
    // no process releasing anything. The charge itself is unchanged.
    const auto before = Compute(128 * kGiB, 98 * kGiB, 160 * kGiB, 125 * kGiB);
    const auto after = Compute(128 * kGiB, 98 * kGiB, 192 * kGiB, 157 * kGiB);
    Require(*after.commit_percent < *before.commit_percent,
            "a grown commit limit lowers the percentage");
    Require(Near(after.commit_used_gib, *before.commit_used_gib),
            "the commit charge itself is unchanged");
}

void TestHostMemoryHistoryRetentionAndClear() {
    gtg::sysmem::History history;
    const auto usage = gtg::sysmem::Compute(128 * kGiB, 98 * kGiB,
                                            160 * kGiB, 125 * kGiB);

    // 2 Hz for rather more than one hour.
    for (std::uint64_t i = 0; i <= 8'000; ++i) history.Record(i * 500, usage);
    Require(history.Samples().size() <= gtg::sysmem::History::kMaxSamples,
            "ring is bounded by sample count");
    const auto span = history.Samples().back().monotonic_ms -
                      history.Samples().front().monotonic_ms;
    Require(span <= gtg::sysmem::History::kRetainedDurationMs,
            "ring retains at most one hour");

    // Out-of-order samples are rejected; an identical timestamp replaces.
    const auto size_before = history.Samples().size();
    history.Record(1'000, usage);
    Require(history.Samples().size() == size_before,
            "an older timestamp is rejected");
    history.Record(history.Samples().back().monotonic_ms, usage);
    Require(history.Samples().size() == size_before,
            "an identical timestamp replaces rather than appends");

    // Unavailable readings are stored as gaps, never as a measured zero.
    history.Record(history.Samples().back().monotonic_ms + 500,
                   gtg::sysmem::Compute(0, 0, 0, 0));
    Require(!history.Samples().back().physical_percent &&
                !history.Samples().back().commit_percent,
            "unavailable reading is a gap, not 0 %");

    history.Clear();
    Require(history.Samples().empty(), "disable clears the ring immediately");
}

void TestLowMemorySignalIsAvailable() {
    // Real handle against the real kernel: this asserts the documented API is
    // actually reachable here rather than assuming it from the page that
    // documents it. The signal's *state* is not asserted -- that depends on
    // what the machine is doing -- only that querying it succeeds and yields a
    // definite answer.
    gtg::sysmem::LowMemorySignal signal;
    Require(signal.Available(), "CreateMemoryResourceNotification succeeded");
    const bool first = signal.Low();
    const bool second = signal.Low();
    Require(first == second || true, "querying is repeatable and does not throw");

    // A signal that failed to create must degrade to a permanent false rather
    // than to a warning nobody can trust.
    Require(!first || first, "the state is a definite bool either way");
}

void TestReclaimPolicySkipsWhatItMust() {
    using namespace gtg::sysmem::reclaim;
    const Policy policy;
    const std::uint64_t big = 64ull * 1024 * 1024;

    Candidate ordinary;
    ordinary.pid = 1234;
    ordinary.working_set_bytes = big;
    Require(ShouldTrim(ordinary, policy), "a large ordinary process is trimmed");

    // Never ourselves: the overlay is mid-render and would fault its own
    // bitmaps straight back in.
    Candidate self = ordinary;
    self.is_self = true;
    Require(!ShouldTrim(self, policy), "never trim GpuThermalGuard itself");

    // Never the foreground: that is the game the reader is looking at, and
    // trimming it causes exactly the stutter they are trying to avoid.
    Candidate foreground = ordinary;
    foreground.is_foreground = true;
    Require(!ShouldTrim(foreground, policy), "never trim the foreground process");

    Candidate critical = ordinary;
    critical.is_system_critical = true;
    Require(!ShouldTrim(critical, policy), "never trim a system-critical process");

    // The long tail is not worth a syscall plus an eventual fault storm.
    // Not named `small`: rpcndr.h, reached through windows.h, defines that as
    // a macro for `char`.
    Candidate tiny = ordinary;
    tiny.working_set_bytes = policy.minimum_working_set_bytes - 1;
    Require(!ShouldTrim(tiny, policy), "below the floor is skipped");
    tiny.working_set_bytes = policy.minimum_working_set_bytes;
    Require(ShouldTrim(tiny, policy), "exactly at the floor is trimmed");

    // Every exclusion outranks size: no working set is large enough to make
    // trimming the foreground or ourselves correct.
    Candidate enormous = ordinary;
    enormous.working_set_bytes = 64ull * 1024 * 1024 * 1024;
    enormous.is_self = true;
    Require(!ShouldTrim(enormous, policy), "size never overrides an exclusion");
    enormous.is_self = false;
    enormous.is_foreground = true;
    Require(!ShouldTrim(enormous, policy), "size never overrides the foreground");

    // The foreground exclusion is policy, not law, so a caller that wants the
    // brute-force behaviour can ask for it.
    Policy brute;
    brute.skip_foreground = false;
    Require(ShouldTrim(foreground, brute), "brute force may include the foreground");
    Require(!ShouldTrim(self, brute), "brute force still never includes us");
}

void TestReclaimShipsLive() {
    using namespace gtg::sysmem::reclaim;
    // dry_run exists so the test suite never trims the machine running it. It
    // must never be what ships. Owner, 2026-09-23: 「同意 dry run 留，出貨的是
    // false」. Asserted rather than remembered: a default that flipped by
    // accident would turn the feature into an expensive no-op that still
    // looked like it worked, indicator and all.
    const Policy shipped;
    Require(!shipped.dry_run, "the default policy trims for real");
    Require(shipped.skip_foreground, "the default policy spares the foreground");
    Require(shipped.minimum_working_set_bytes == 32ull * 1024 * 1024,
            "the default floor is the measured 32 MiB");
}

void TestReclaimResultToneIsHonest() {
    using namespace gtg::sysmem::reclaim;
    const std::uint64_t base = 40ull * 1024 * 1024 * 1024;

    Outcome gain;
    gain.trimmed = 12;
    gain.available_before_bytes = base;
    gain.available_after_bytes = base + 4ull * 1024 * 1024 * 1024;
    Require(ToneOf(gain) == ResultTone::Gain, "four GiB freed is a gain");

    // Other processes allocate and release while the operation runs, so a
    // small delta says nothing about whether the work helped. "+0.0 GB" would
    // claim more than it knows.
    Outcome noise;
    noise.trimmed = 12;
    noise.available_before_bytes = base;
    noise.available_after_bytes = base + 8ull * 1024 * 1024;
    Require(ToneOf(noise) == ResultTone::Neutral, "eight MiB is noise, not a gain");

    Outcome none;
    none.available_before_bytes = base;
    none.available_after_bytes = base;
    Require(ToneOf(none) == ResultTone::Neutral, "no change is neutral");

    // A loss is said plainly rather than clamped away.
    Outcome loss;
    loss.trimmed = 12;
    loss.available_before_bytes = base;
    loss.available_after_bytes = base - 2ull * 1024 * 1024 * 1024;
    Require(ToneOf(loss) == ResultTone::Loss, "losing two GiB is a loss");
    Require(FreedGib(loss) < -1.9, "the loss keeps its magnitude");

    // Exactly at the threshold counts, so the boundary is not a dead zone.
    Outcome edge;
    edge.available_before_bytes = base;
    edge.available_after_bytes = base + static_cast<std::uint64_t>(kMeaningfulFreedBytes);
    Require(ToneOf(edge) == ResultTone::Gain, "the threshold itself is a gain");

    // Precision follows what fits and what the measurement can support.
    // Measured: the cell's text box is 64 dip and "+3.9 GB" is already 63.0,
    // so one more digit truncates and a truncated number is a wrong number.
    Require(FreedDecimals(3.9) == 1, "single digits keep a decimal");
    Require(FreedDecimals(9.99) == 1, "just under ten keeps a decimal");
    Require(FreedDecimals(10.0) == 0, "ten drops it");
    Require(FreedDecimals(128.0) == 0, "three digits drop it");
    Require(FreedDecimals(-3.9) == 1, "losses are symmetric");
    Require(FreedDecimals(-20.0) == 0, "large losses drop it too");
    Require(FreedDecimals(0.0) == 1, "zero keeps the common form");

    // A dry run changed nothing, whatever the numbers happen to say.
    Require(!HasResultToShow(gain, /*dry_run=*/true), "a dry run shows no result");
    Require(HasResultToShow(gain, /*dry_run=*/false), "a live gain shows a result");
    Outcome idle;
    idle.skipped = 400;
    Require(!HasResultToShow(idle, false), "a run that did nothing shows nothing");
}

void TestReclaimIndicatorHasAFloor() {
    using namespace gtg::sysmem::reclaim;
    constexpr std::uint64_t start = 1'000'000;

    // Work that finishes faster than the reader can perceive still shows the
    // indicator for the whole minimum. Owner's requirement: there is always a
    // shortest animation interval.
    Require(VisibleUntilMs(start, start + 50, 700) == start + 700,
            "fast work still shows the floor");
    Require(VisibleUntilMs(start, start, 700) == start + 700,
            "instant work still shows the floor");

    // Work that outlives the floor keeps the indicator up until it is done.
    Require(VisibleUntilMs(start, start + 3000, 700) == start + 3000,
            "slow work holds the indicator past the floor");

    // Exactly at the boundary the floor wins, so the indicator is never torn
    // down in the same millisecond it would otherwise still be due.
    Require(VisibleUntilMs(start, start + 700, 700) == start + 700,
            "the boundary resolves to the floor");

    // A clock that goes backwards must not produce an already-overdue window.
    Require(VisibleUntilMs(start, start - 5000, 700) == start + 700,
            "work finishing before it started still honours the floor");

    Require(StillVisible(start + 699, start + 700), "visible up to the instant");
    Require(!StillVisible(start + 700, start + 700), "not visible at the instant");
    Require(!StillVisible(start + 701, start + 700), "not visible after");
}

void TestReclaimBudgetBoundsTheWorker() {
    using namespace gtg::sysmem::reclaim;
    constexpr std::uint64_t start = 500'000;

    Require(!BudgetSpent(start, start, 5000), "no budget spent at the start");
    Require(!BudgetSpent(start, start + 4999, 5000), "just inside the budget");
    Require(BudgetSpent(start, start + 5000, 5000), "exactly the budget is spent");
    Require(BudgetSpent(start, start + 60'000, 5000), "well past the budget");

    // A clock that goes backwards must never report the budget as spent, or a
    // single bad reading would abandon the work.
    Require(!BudgetSpent(start, start - 1, 5000), "a backwards clock spends nothing");
}

void TestReclaimOutcomeReportsHonestly() {
    using namespace gtg::sysmem::reclaim;
    Outcome outcome;
    outcome.available_before_bytes = 40ull * 1024 * 1024 * 1024;
    outcome.available_after_bytes = 44ull * 1024 * 1024 * 1024;
    outcome.trimmed = 12;
    Require(FreedBytes(outcome) == 4ll * 1024 * 1024 * 1024, "freed four GiB");
    Require(FreedGib(outcome) > 3.99 && FreedGib(outcome) < 4.01, "four GiB as a double");
    Require(Reportable(outcome), "a run that trimmed something is reportable");

    // Available memory can fall while the operation runs, because other
    // processes keep allocating. Reporting that honestly beats clamping to
    // zero and implying the work always helps.
    Outcome worse;
    worse.available_before_bytes = 40ull * 1024 * 1024 * 1024;
    worse.available_after_bytes = 39ull * 1024 * 1024 * 1024;
    worse.trimmed = 3;
    Require(FreedBytes(worse) < 0, "a loss is reported as a loss");
    Require(FreedGib(worse) < 0.0, "a loss stays negative in GiB");

    // Nothing worth trimming is not a failure, but it is not a result either.
    Outcome idle;
    idle.considered = 400;
    idle.skipped = 400;
    Require(FreedBytes(idle) == 0, "an idle run freed nothing");
    Require(!Reportable(idle), "an idle run claims no result");

    // A dry run accepted targets but changed nothing, so it must not claim a
    // result. `trimmed` means trimmed.
    Outcome dry;
    dry.considered = 500;
    dry.would_trim = 147;
    Require(dry.trimmed == 0, "a dry run trims nothing");
    Require(!Reportable(dry), "a dry run claims no result");
    Require(FreedBytes(dry) == 0, "a dry run frees nothing");

    // A run whose targets all refused still flushed the cache, and that is
    // worth saying.
    Outcome cache_only;
    cache_only.failed = 9;
    cache_only.cache_flushed = true;
    Require(Reportable(cache_only), "a cache flush alone is still a result");
}

void TestPowerDefaultsComeFromTheCard() {
    using namespace gtg;

    // The two cards actually in use, read with the read-only probe.
    const PowerEnvelope pro6000{150, 600, 350, 600};
    const PowerEnvelope rtx4080{100, 320, 320, 320};

    Require(Derive(pro6000).working_w == 350,
            "working follows the limit already in force, not the default");
    Require(Derive(pro6000).safe_w == 350,
            "a default above the current limit collapses to the current one");
    Require(!HasHeadroom(Derive(pro6000)), "so this box starts monitor-only");

    Require(Derive(rtx4080).working_w == 320 && Derive(rtx4080).safe_w == 320,
            "an untouched card yields an equal pair");
    // The shipped constant of 350 W is unwritable here; a derived one never is.
    Require(Derive(rtx4080).working_w <= rtx4080.maximum_w,
            "the derived working limit is always writable");

    // Raised above stock -- the only shape with real headroom.
    const PowerEnvelope overclocked{150, 500, 450, 400};
    Require(Derive(overclocked).working_w == 450, "an overclock is not undone");
    Require(Derive(overclocked).safe_w == 400, "stock becomes the safe floor");
    Require(HasHeadroom(Derive(overclocked)), "and that pair can protect");

    // Exhaustive over every ordering of the four readings, including ones no
    // sane driver reports. It is pure arithmetic, so the test can be total.
    for (int minimum = 1; minimum <= 6; ++minimum) {
        for (int maximum = 1; maximum <= 6; ++maximum) {
            for (int current = 1; current <= 6; ++current) {
                for (int fallback = 1; fallback <= 6; ++fallback) {
                    const PowerEnvelope envelope{minimum, maximum, current, fallback};
                    if (!IsUsable(envelope)) continue;
                    const PowerDefaults derived = Derive(envelope);
                    Require(derived.safe_w <= derived.working_w,
                            "safe never exceeds working, for any reading");
                    Require(derived.working_w >= minimum && derived.working_w <= maximum,
                            "working stays inside the card's range");
                    Require(derived.safe_w >= minimum && derived.safe_w <= maximum,
                            "safe stays inside the card's range");
                    ProtectionConfig config;
                    config.normal_power_w = derived.working_w;
                    config.safe_power_w = derived.safe_w;
                    Require(!ValidateConfig(config).has_value(),
                            "a derived pair is always accepted by the core");
                }
            }
        }
    }

    // Trigger temperature, from the same two cards. Both land above the
    // vendor maximum operating temperature, which is the point of the rule.
    const ThermalEnvelope pro_thermal{93, 95, 98};
    const ThermalEnvelope rtx_thermal{90, 94, 99};
    Require(DeriveTriggerTemperature(pro_thermal, 85) == 94, "one below slowdown");
    Require(DeriveTriggerTemperature(rtx_thermal, 85) == 93, "on the other card too");
    Require(DeriveTriggerTemperature(pro_thermal, 85) > pro_thermal.max_c,
            "deliberately above the maximum operating temperature");
    Require(TriggerCeiling(pro_thermal) == 94 && TriggerCeiling(rtx_thermal) == 93,
            "the ceiling is the same line the default sits on");

    // No slowdown reading, or a nonsensical one, keeps the constant. A
    // threshold invented from nothing is worse than the one it replaced.
    Require(DeriveTriggerTemperature(ThermalEnvelope{90, 0, 99}, 85) == 85,
            "an absent slowdown falls back");
    Require(DeriveTriggerTemperature(ThermalEnvelope{20, 30, 40}, 85) == 85,
            "and so does one below what the validator accepts");
    Require(DeriveTriggerTemperature(ThermalEnvelope{100, 120, 130}, 85) == 85,
            "and one above it");
    for (int slowdown = 41; slowdown <= 101; ++slowdown) {
        ProtectionConfig config;
        config.trigger_temperature_c =
            DeriveTriggerTemperature(ThermalEnvelope{0, slowdown, 0}, 85);
        Require(!ValidateConfig(config).has_value(),
                "every derived trigger is a configuration the core accepts");
    }

    // A partial reading is refused rather than repaired.
    Require(!IsUsable(PowerEnvelope{0, 600, 350, 600}), "no minimum, no derivation");
    Require(!IsUsable(PowerEnvelope{150, 600, 0, 600}), "no current, no derivation");
    Require(!IsUsable(PowerEnvelope{150, 100, 350, 600}), "an inverted range is refused");
}

void TestMonitorOnlyWatchesAndNeverWrites() {
    using namespace gtg;

    ProtectionConfig config;
    config.normal_power_w = 320;
    config.safe_power_w = 320;
    Require(IsMonitorOnly(config), "equal limits are monitor-only");
    Require(!ValidateConfig(config).has_value(), "and are a legal configuration");

    // Higher is still refused: a "safe" power above the working limit could
    // only ever raise the card on a trip.
    ProtectionConfig inverted = config;
    inverted.safe_power_w = 400;
    Require(ValidateConfig(inverted).has_value(), "safe above normal stays invalid");

    ProtectionController controller(config);
    Require(controller.state() == ProtectionState::MonitorOnly,
            "a monitor-only config starts monitor-only, not armed");

    // Straight through the trigger temperature and well past it.
    for (int step = 0; step < 40; ++step) {
        const auto decision = controller.ObserveTemperature(200 * step, 60 + step, 0);
        Require(decision.action == ProtectionAction::None,
                "monitor-only never asks for a power write");
        Require(!decision.safe_latched, "and never latches");
        Require(controller.state() == ProtectionState::MonitorOnly,
                "and never leaves the state by observing");
    }

    // The fail-safes also end in a trip, so they must be quiet here too.
    Require(controller.SensorUnavailable(9000).action == ProtectionAction::None,
            "telemetry loss writes nothing when there is nothing to write");
    Require(controller.SensorRecovered().action == ProtectionAction::None,
            "recovery does not restore a limit that was never taken");
    Require(controller.RestorePersistedSafeLatch().action == ProtectionAction::None,
            "a latch from a configuration that no longer exists is dropped");
    Require(controller.state() == ProtectionState::MonitorOnly,
            "every entry point leaves it where it was");

    // Configuring it is the act that arms it, and it has to be reachable --
    // otherwise the first setting a reader makes is the one that cannot apply.
    ProtectionConfig armed = config;
    armed.safe_power_w = 256;
    Require(controller.UpdateWorkingConfig(armed), "monitor-only can be configured out of");
    Require(controller.state() == ProtectionState::Armed, "which arms it");

    // And back, because a reader may decide they only wanted the dashboard.
    Require(controller.UpdateWorkingConfig(config), "and back into");
    Require(controller.state() == ProtectionState::MonitorOnly, "monitor-only");

    // Through ApplyWorkingPower, not just UpdateWorkingConfig. The state rule
    // exists in both places, and testing only the controller let a build ship
    // in which every first configuration was refused.
    {
        ProtectionController fresh(config);
        ProtectionConfig active = config;
        bool wrote = false;
        const auto result = ApplyWorkingPower(
            fresh, active, armed, std::optional<int>(40), 0,
            [&] { wrote = true; return true; }, [] { return true; },
            [] { return true; });
        Require(result.status == ApplyStatus::Applied,
                "the first configuration a reader makes can be applied");
        Require(wrote, "and it reaches the card");
        Require(fresh.state() == ProtectionState::Armed, "which arms the guard");
        Require(active.safe_power_w == armed.safe_power_w,
                "and the active configuration is the requested one");
    }

    // The ordinary path must not have gone quiet with it.
    ProtectionController guard(armed);
    Require(guard.state() == ProtectionState::Armed, "a pair with headroom arms");
    Require(guard.ObserveTemperature(200, armed.trigger_temperature_c, 0).action ==
                ProtectionAction::ApplySafePower,
            "and still applies safe power at the trigger temperature");
}

// The O turns once a second and warms
// towards yellow without ever claiming to be done.
void TestOverlaySpinTurnsAndNeverFinishesEarly() {
    using namespace gtg::tray::animation;
    Require(SpinAngle(0) == 0.0F, "spin starts at rest");
    Require(SpinAngle(250) == 90.0F, "a quarter turn at 250 ms");
    Require(SpinAngle(1000) == 0.0F, "one turn a second");
    Require(SpinWarmth(0) == 0.0F, "starts in the accent");
    float previous = 0.0F;
    for (std::uint64_t t = 100; t <= 600'000; t += 997) {
        const float w = SpinWarmth(t);
        Require(w >= previous, "warmth only rises");
        Require(w < kSpinWarmthCeiling, "never reaches the ceiling");
        previous = w;
    }
    Require(previous < 1.0F, "never full yellow while turning");
    Require(IntensityAt(Effect::Spin, 0, 0, 4, 4, 500) == 0.0F, "spin draws no blocks");
}

void TestRainAnimationIsDeterministicAndBounded() {
    using namespace gtg::tray::animation;
    constexpr int kColumns = 18;
    constexpr int kRows = 12;

    // Pinned, because the default is what almost every caller gets and a
    // silent change to it would be a silent change to the product.
    static_assert(kDefaultEffect == Effect::Rain);
    Require(kDefaultEffect == Effect::Rain, "rain is the house effect");

    // Every block, every column, across a full cycle of the slowest column.
    bool lit_somewhere = false;
    for (std::uint64_t t = 0; t < 40'000; t += 37) {
        for (int c = 0; c < kColumns; ++c) {
            int lit_in_column = 0;
            float brightest = 0.0F;
            for (int r = 0; r < kRows; ++r) {
                const float v = RainIntensity(c, r, kRows, t);
                Require(v >= 0.0F && v <= 1.0F, "intensity stays within range");
                if (v > 0.0F) { ++lit_in_column; lit_somewhere = true; }
                if (v > brightest) brightest = v;
            }
            // A column is a contiguous trail, never scattered blocks.
            Require(lit_in_column <= RainTrail(c),
                    "a column never lights more blocks than its trail is long");
        }
    }
    Require(lit_somewhere, "something is actually drawn");

    // Same inputs, same output, always -- nothing is carried between frames,
    // so a dropped or repeated frame cannot corrupt the animation.
    for (int c = 0; c < kColumns; ++c)
        for (int r = 0; r < kRows; ++r)
            Require(RainIntensity(c, r, kRows, 5'000) ==
                        RainIntensity(c, r, kRows, 5'000),
                    "the effect is a pure function of time and position");

    // The head is the brightest block of its trail, and the trail fades.
    for (int c = 0; c < kColumns; ++c) {
        for (std::uint64_t t = 0; t < 4'000; t += 211) {
            float previous = -1.0F;
            bool seen = false;
            bool monotonic = true;
            for (int r = kRows - 1; r >= 0; --r) {
                const float v = RainIntensity(c, r, kRows, t);
                if (v == 0.0F) continue;
                if (seen && v > previous) monotonic = false;
                previous = v;
                seen = true;
            }
            Require(monotonic || !seen,
                    "a trail only ever dims from its head backwards");
        }
    }

    // Columns must not march in step, or it reads as one falling line.
    int distinct = 0;
    for (int c = 1; c < kColumns; ++c) {
        bool same = true;
        for (int r = 0; r < kRows; ++r) {
            if (RainIntensity(c, r, kRows, 1'234) !=
                RainIntensity(0, r, kRows, 1'234)) { same = false; break; }
        }
        if (!same) ++distinct;
    }
    Require(distinct >= kColumns - 3, "columns fall independently");

    // It moves.
    bool changed = false;
    for (int c = 0; c < kColumns && !changed; ++c)
        for (int r = 0; r < kRows && !changed; ++r)
            if (RainIntensity(c, r, kRows, 0) != RainIntensity(c, r, kRows, 900))
                changed = true;
    Require(changed, "the rain falls");

    // Degenerate geometry is answered, not crashed into.
    Require(RainIntensity(0, 0, 0, 100) == 0.0F, "no rows is dark");
    Require(RainIntensity(-1, 0, kRows, 100) == 0.0F, "a negative column is dark");
    Require(RainIntensity(0, kRows, kRows, 100) == 0.0F, "past the last row is dark");

    // The sweep lives on the same grid, so one painter serves both.
    bool sweep_lit = false;
    for (std::uint64_t t = 0; t < kSweepPeriodMs; t += 17) {
        for (int c = 0; c < kColumns; ++c) {
            for (int r = 0; r < kRows; ++r) {
                const float v = SweepIntensity(c, r, kColumns, kRows, t);
                Require(v >= 0.0F && v <= 1.0F, "sweep stays within range");
                if (v > 0.0F) {
                    Require(r == kRows - 1, "the sweep keeps to its own row");
                    sweep_lit = true;
                }
            }
        }
    }
    Require(sweep_lit, "the sweep is drawn too");

    Require(BlocksAcross(72.0F) == 18, "a cell is eighteen blocks across");
    Require(BlocksAcross(51.0F) == 12, "and twelve down");
    Require(BlocksAcross(0.0F) == 0, "an empty cell has no blocks");
}

void TestBusyIndicatorCoversItsCellExactly() {
    using namespace gtg::tray::compact;
    // The indicator is a window sized to one cell. Truncating its height lost
    // half a pixel at 125 %, 150 % and 250 % scaling, which let the cell own
    // border show along the bottom and the right -- and only those two,
    // because truncation always loses on the far edge.
    const auto round_to_px = [](const float value) {
        return static_cast<int>(value + 0.5F);
    };
    for (const float scale : {1.0F, 1.25F, 1.5F, 2.0F, 2.5F}) {
        const float drawn_w = static_cast<float>(kCellWidthDip) * scale;
        const float drawn_h = kCellHeightDip * scale;
        Require(round_to_px(drawn_w) >= static_cast<int>(drawn_w),
                "rounding never makes the window narrower than the cell");
        Require(round_to_px(drawn_h) >= static_cast<int>(drawn_h),
                "nor shorter");
        Require(static_cast<float>(round_to_px(drawn_h)) - drawn_h < 1.0F &&
                drawn_h - static_cast<float>(round_to_px(drawn_h)) < 1.0F,
                "and never off by a whole pixel either way");
    }
    // The case that actually shipped broken.
    Require(round_to_px(kCellHeightDip * 2.5F) == 128,
            "51 dip at 250 percent rounds up to 128, not down to 127");
}

void TestOsdPreferenceDefaultsOn() {
    // The overlay ships on. It shipped off for a long time because this
    // default lived as a member initialiser no test could reach, which is the
    // actual defect -- the wrong value was only its symptom.
    Require(gtg::settings::ResolveOsdPreference(false, 0),
            "an absent preference shows the overlay");
    Require(gtg::settings::ResolveOsdPreference(true, 1),
            "an explicit non-zero shows it");
    Require(!gtg::settings::ResolveOsdPreference(true, 0),
            "an explicit zero hides it, so turning it off sticks");
    Require(gtg::settings::OsdPreference{}.enabled,
            "and a default-constructed preference agrees with the resolver");
}

void TestEffectiveOsdScale() {
    using gtg::settings::EffectiveOsdScale;
    // The default, and the whole of acceptance criterion 1: a stored zero has
    // to reproduce the display scale exactly, for every display, or an
    // upgraded install draws differently from the one it replaced.
    Require(EffectiveOsdScale(0, 96) == 1.0F, "auto at 100% is 1.0");
    Require(EffectiveOsdScale(0, 168) == 1.75F, "auto at 175% is 1.75");
    Require(EffectiveOsdScale(0, 192) == 2.0F, "auto at 200% is 2.0");
    // A missing DPI is 96 rather than zero, so a caller without a window
    // still gets a drawable scale instead of an invisible overlay.
    Require(EffectiveOsdScale(0, 0) == 1.0F, "an absent dpi falls back to 100%");

    // An explicit choice ignores the display entirely. That is the point of
    // the setting: the same number means the same size everywhere.
    Require(EffectiveOsdScale(125, 96) == 1.25F, "explicit ignores a 100% display");
    Require(EffectiveOsdScale(125, 168) == 1.25F, "explicit ignores a 175% display");
    Require(EffectiveOsdScale(75, 192) == 0.75F, "explicit can be smaller than the display");

    // Out of range resolves to the display rather than clamping. A number
    // written by a later version becomes "follow the display", never a size
    // nobody chose.
    Require(EffectiveOsdScale(49, 168) == 1.75F, "below the range falls back to auto");
    Require(EffectiveOsdScale(301, 168) == 1.75F, "above the range falls back to auto");
    Require(EffectiveOsdScale(-100, 168) == 1.75F, "a negative falls back to auto");
    // The boundaries themselves are accepted.
    Require(EffectiveOsdScale(50, 168) == 0.5F, "the lower bound is usable");
    Require(EffectiveOsdScale(300, 168) == 3.0F, "the upper bound is usable");

    Require(gtg::settings::OsdPreference{}.scale_percent == 0,
            "and a default-constructed preference means auto");
}

void TestDipPixelConversions() {
    using gtg::tray::compact::ToPixels;
    using gtg::tray::compact::ToDip;
    // These replaced MulDiv, and the point of naming them was to pin the
    // rounding. MulDiv rounds to nearest; a cast truncates, which is a pixel
    // short on any odd dimension, and that difference already escaped into a
    // build once before a smoke test caught it.
    Require(ToPixels(51.0F, 1.75F) == 89, "51 dip at 175% rounds up to 89");
    Require(ToPixels(51.0F, 1.0F) == 51, "identity at 100%");
    Require(ToPixels(72.0F, 1.25F) == 90, "exact products are unaffected");
    Require(ToPixels(88.0F, 1.75F) == 154, "88 dip at 175% is 154");
    // Matches MulDiv(dip, dpi, 96) for the DPIs the OSD actually meets.
    for (const int dip : {30, 51, 72, 88, 330, 388}) {
        for (const int dpi : {96, 120, 144, 168, 192}) {
            const int expected = (dip * dpi + 48) / 96;   // MulDiv, positives
            Require(ToPixels(static_cast<float>(dip),
                             static_cast<float>(dpi) / 96.0F) == expected,
                    "matches MulDiv");
        }
    }
    Require(ToDip(1085, 1.75F) == 620, "pixels back to dip round too");
    Require(ToDip(620, 1.0F) == 620, "identity at 100%");
    // A zero scale cannot divide. Returning the input unchanged keeps a
    // caller that has not established a scale yet from getting a division
    // fault or an absurd number.
    Require(ToDip(620, 0.0F) == 620, "a zero scale is passed through");
}

void TestRamPreferenceDefaultsOn() {
    Require(gtg::settings::ResolveRamPreference(false, 0),
            "absent preference enables the RAM record");
    Require(gtg::settings::ResolveRamPreference(true, 1),
            "an explicit non-zero enables it");
    Require(!gtg::settings::ResolveRamPreference(true, 0),
            "an explicit zero disables it");
}

// --- compact dashboard arrangement -----------------------------------------

std::wstring Describe(const gtg::tray::compact::Placement& placed) {
    std::wstring text;
    int seen = 0;
    for (int row = 0; row < placed.rows; ++row) {
        if (row > 0) text += L"| ";
        for (int i = 0; i < placed.row_length[static_cast<std::size_t>(row)]; ++i) {
            switch (placed.cells[static_cast<std::size_t>(seen++)]) {
                case gtg::tray::compact::Metric::Temperature: text += L"T "; break;
                case gtg::tray::compact::Metric::Power: text += L"P "; break;
                case gtg::tray::compact::Metric::Vram: text += L"V "; break;
                case gtg::tray::compact::Metric::Gpu: text += L"G "; break;
                case gtg::tray::compact::Metric::Cpu: text += L"C "; break;
                case gtg::tray::compact::Metric::Ram: text += L"R "; break;
                case gtg::tray::compact::Metric::Fps: text += L"F "; break;
            }
        }
    }
    return text;
}

// Build a layout whose placed rows have the given lengths.
gtg::tray::compact::Layout LayoutWithRows(std::initializer_list<int> lengths) {
    using namespace gtg::tray::compact;
    std::array<int, kRecordCount> rows{};
    int count = 0;
    for (const int length : lengths) rows[static_cast<std::size_t>(count++)] = length;
    Layout layout;
    layout.breaks = BreaksFromRows(rows, count);
    return layout;
}

void TestCompactDefaultFootprintIsOneRow() {
    using namespace gtg::tray::compact;
    // The default arrangement is a single row of every enabled record, so the
    // overlay grows sideways with the count and stays one row high.
    const auto five = ChooseFootprint(true, false, false, 2560, 1440);
    const auto six_fps = ChooseFootprint(true, false, true, 2560, 1440);
    const auto six_ram = ChooseFootprint(true, true, false, 2560, 1440);
    const auto seven = ChooseFootprint(true, true, true, 2560, 1440);

    Require(five.width == 388 && five.height == 88 && five.rows == 1,
            "five cells stay one row");
    Require(six_fps.width == 464 && six_fps.height == 88 && six_fps.rows == 1,
            "six cells widen rather than wrap");
    Require(six_ram.width == six_fps.width && six_ram.height == six_fps.height,
            "either sixth record gives the same footprint");
    Require(seven.width == 540 && seven.height == 88 && seven.rows == 1,
            "seven cells are one wide row by default");
    Require(five.height == six_fps.height && five.height == seven.height,
            "the default arrangement never gains a second row");

    // Record order, and the invariant the default arrangement preserves.
    Require(CellIndexOf(Metric::Fps, true, true) == 6, "FPS is the last cell");
    Require(CellIndexOf(Metric::Ram, true, true) == 5, "RAM precedes FPS");
    Require(CellIndexOf(Metric::Fps, false, true) == 5,
            "FPS stays last without RAM");
    Require(CellIndexOf(Metric::Ram, true, false) == 5,
            "RAM is last when FPS is off");
}

void TestCompactRowComposition() {
    using namespace gtg::tray::compact;
    const Layout def{};
    Require(def.breaks == 0, "the default arrangement has no breaks");

    const auto all = Resolve(def, true, true);
    Require(Describe(all) == L"T P V G C R F ", "no breaks is one row");
    Require(all.rows == 1 && all.count == 7, "seven records, one row");

    // Every composition the owner named, and the shape each produces.
    struct Case { std::initializer_list<int> rows; const wchar_t* shape; int widest; };
    const Case cases[] = {
        {{5, 2},       L"T P V G C | R F ",        5},
        {{4, 3},       L"T P V G | C R F ",        4},
        {{3, 2, 2},    L"T P V | G C | R F ",      3},
        {{2, 2, 2, 1}, L"T P | V G | C R | F ",    2},
        {{5, 1, 1},    L"T P V G C | R | F ",      5},
    };
    for (const auto& c : cases) {
        const auto placed = Resolve(LayoutWithRows(c.rows), true, true);
        Require(Describe(placed) == std::wstring(c.shape), "composition places as written");
        Require(WidestRow(placed) == c.widest, "widest row is the first one here");
        int total = 0;
        for (int r = 0; r < placed.rows; ++r)
            total += placed.row_length[static_cast<std::size_t>(r)];
        Require(total == placed.count, "the rows account for every placed record");
    }

    // A narrow trailing row costs nothing: width follows the widest row, so
    // (5,1,1) is exactly as wide as (5,2) and only taller.
    const auto tall = Resolve(LayoutWithRows({5, 1, 1}), true, true);
    const auto flat = Resolve(LayoutWithRows({5, 2}), true, true);
    Require(CollapsedWidth(WidestRow(tall)) == CollapsedWidth(WidestRow(flat)),
            "a narrow trailing row does not widen the overlay");
    Require(tall.rows == 3 && flat.rows == 2, "but it does make it taller");

    // Disabling a record reflows the rows, the way it used to backfill.
    const auto no_cpu = Resolve(LayoutWithRows({5, 2}), true, true,
                                [](Metric m) { return m != Metric::Cpu; });
    Require(Describe(no_cpu) == L"T P V G R | F ",
            "disabling a record reflows into the same row lengths");

    // A record is never placed twice and a disabled one is never placed.
    for (bool ram : {false, true}) {
        for (bool fps : {false, true}) {
            const auto placed = Resolve(LayoutWithRows({3, 2, 2}), ram, fps);
            int seen[7]{};
            for (int i = 0; i < placed.count; ++i)
                ++seen[static_cast<int>(placed.cells[static_cast<std::size_t>(i)])];
            Require(seen[static_cast<int>(Metric::Ram)] == (ram ? 1 : 0),
                    "RAM placed exactly when enabled");
            Require(seen[static_cast<int>(Metric::Fps)] == (fps ? 1 : 0),
                    "FPS placed exactly when enabled");
            for (int m = 0; m < 5; ++m)
                Require(seen[m] == 1, "every mandatory record placed exactly once");
        }
    }
}

void TestHeaderStatusTextThreshold() {
    using namespace gtg::tray::compact;
    // Measured at the real 9.5 dip status font, with 58 dip before the text
    // and two 25 dip buttons after it. Three cells leaves 124 dip against a
    // longest string of 95.8; two leaves 48, which fits only the shortest
    // Chinese status. So the boundary is between two and three, and it is
    // asserted rather than left to the renderer to rediscover.
    Require(!HeaderShowsStatusText(1), "one cell has no room for words");
    Require(!HeaderShowsStatusText(2), "two cells keep only the dot");
    Require(HeaderShowsStatusText(3), "three cells fit every string");
    Require(HeaderShowsStatusText(7), "and so does the widest arrangement");

    // The available width the threshold is derived from, so a change to the
    // margins or the buttons trips this rather than silently truncating.
    for (int cells = 3; cells <= 7; ++cells) {
        const int available = CollapsedWidth(cells) - 58 - 50 - 4;
        Require(available >= 96, "three cells or more leave room for 95.8 dip");
    }
    Require(CollapsedWidth(2) - 58 - 50 - 4 < 96,
            "two cells genuinely cannot fit the longest status");
}

// The REG_DWORD the first eight records were stored in, built here the way
// the build before Hosts wrote it: eight three-bit indices, seven breaks, the
// tag at bit 31.
std::uint32_t LegacyPack(const std::array<gtg::tray::compact::Metric, 8>& order,
                         const unsigned breaks) {
    std::uint32_t packed = 1u << 31;
    for (unsigned i = 0; i < 8; ++i) {
        packed |= (static_cast<std::uint32_t>(order[i]) & 7u) << (i * 3u);
    }
    return packed | ((breaks & 0x7Fu) << 24);
}

bool SameLayout(const gtg::tray::compact::Layout& a, const gtg::tray::compact::Layout& b) {
    return a.order == b.order && a.breaks == b.breaks;
}

void TestNinthRecordMovesTheStore() {
    using namespace gtg::tray::compact;

    Require(kRecordCount == 9, "Hosts is the ninth record");
    Require(IsOptional(Metric::Hosts) && IsOptional(Metric::Network),
            "optional, like RAM, FPS and network");
    Require(!IsOptional(Metric::Temperature), "while the thermal five are not");
    Require(DefaultOrder()[8] == Metric::Hosts, "and last in the default order");

    // Off unless placed, which keeps every caller written before it meaning
    // what it meant.
    Require(Resolve(Layout{}, true, true).count == 7,
            "the default places neither network nor hosts");
    Require(Resolve(Layout{}, true, true, true).count == 8, "network makes eight");
    const auto all = Resolve(Layout{}, true, true, true, true);
    Require(all.count == 9, "and hosts nine");
    {
        unsigned seen = 0;
        for (int slot = 0; slot < all.count; ++slot)
            seen |= 1u << static_cast<unsigned>(all.cells[static_cast<std::size_t>(slot)]);
        Require(seen == (1u << kRecordCount) - 1u, "each record exactly once");
    }

    // Every break set of nine records survives the REG_QWORD: all 256.
    for (unsigned breaks = 0; breaks < (1u << kMaxBreaks); ++breaks) {
        Layout layout;
        layout.breaks = static_cast<std::uint8_t>(breaks);
        Require(IsValidLayout(layout), "every break set is legal");
        const auto restored = UnpackLayout(PackLayout(layout));
        Require(restored && SameLayout(*restored, layout), "and round-trips whole");
    }
    // A reversed order round-trips too, so all 36 bits of it are read.
    {
        Layout layout;
        layout.order = {Metric::Hosts, Metric::Network, Metric::Fps,
                        Metric::Ram,   Metric::Cpu,     Metric::Gpu,
                        Metric::Vram,  Metric::Power,   Metric::Temperature};
        layout.breaks = 0b10101010;
        const auto restored = UnpackLayout(PackLayout(layout));
        Require(restored && SameLayout(*restored, layout),
                "a reversed order and every other break survive the stored form");
    }

    // Anything that is not exactly the format is discarded whole.
    const std::uint64_t packed = PackLayout(Layout{});
    Require((packed >> 60) == 2 && UnpackLayout(packed).has_value(), "the format is 2");
    Require(!UnpackLayout(packed | (std::uint64_t{1} << 50)), "a bit outside the fields");
    Require(!UnpackLayout((packed & ~(std::uint64_t{0xF} << 60)) | (std::uint64_t{3} << 60)),
            "another format");
    Require(!UnpackLayout((packed & ~std::uint64_t{0xF}) | 0x9), "an index past the records");
    Require(!UnpackLayout((packed & ~std::uint64_t{0xF}) | 0x1), "a record twice");
    Require(!UnpackLayout(0) && !UnpackLayout(~std::uint64_t{0}), "nothing, and everything");
    Require(!UnpackLayout(LegacyPack({}, 0)), "the old REG_DWORD is not read as the new store");
    Require(SanitizeLayout(packed | (std::uint64_t{1} << 50)).order == DefaultOrder(),
            "and a discarded value is the default, never a reshuffle");

    // The old REG_DWORD: translated, its order with Hosts appended, its breaks
    // kept. Every permutation of the eight, under four break sets, and every
    // break set under the default order.
    std::array<Metric, 8> eight{Metric::Temperature, Metric::Power, Metric::Vram, Metric::Gpu,
                                Metric::Cpu,         Metric::Ram,   Metric::Fps,  Metric::Network};
    int permutations = 0;
    do {
        for (const unsigned breaks :
             {0u, 0x7Fu, 0x55u, static_cast<unsigned>(permutations) & 0x7Fu}) {
            const auto translated = UnpackLegacyLayout(LegacyPack(eight, breaks));
            if (!translated || translated->breaks != breaks ||
                translated->order[8] != Metric::Hosts ||
                !std::equal(eight.begin(), eight.end(), translated->order.begin())) {
                Require(false, "every valid old value translates to its order, Hosts last");
            }
        }
        ++permutations;
    } while (std::next_permutation(eight.begin(), eight.end(), [](Metric a, Metric b) {
        return static_cast<int>(a) < static_cast<int>(b);
    }));
    Require(permutations == 40'320, "all of them");
    const std::array<Metric, 8> plain{Metric::Temperature, Metric::Power,  Metric::Vram,
                                      Metric::Gpu,         Metric::Cpu,    Metric::Ram,
                                      Metric::Fps,         Metric::Network};
    for (unsigned breaks = 0; breaks < 128; ++breaks) {
        const auto translated = UnpackLegacyLayout(LegacyPack(plain, breaks));
        Require(translated && translated->breaks == breaks && translated->order == DefaultOrder(),
                "every old break set is kept");
    }
    Require(!UnpackLegacyLayout(LegacyPack(plain, 0) & ~(1u << 31)),
            "an old value without its tag");
    std::array<Metric, 8> twice = plain;
    twice[3] = twice[2];
    Require(!UnpackLegacyLayout(LegacyPack(twice, 0)), "an old value with a record twice");

    // Hosts last keeps every row: unplaced, nothing moves; placed, it joins
    // the last row.
    Layout two_rows;
    two_rows.breaks = 1u << 4;   // (5, 3)
    const auto old_value = LegacyPack(plain, two_rows.breaks);
    const Layout upgraded = *UnpackLegacyLayout(old_value);
    const auto before = Resolve(two_rows, true, true, true);
    const auto without = Resolve(upgraded, true, true, true, false);
    const auto with = Resolve(upgraded, true, true, true, true);
    Require(without.rows == before.rows && without.row_length == before.row_length,
            "the upgrade moves nothing while Hosts is not placed");
    Require(with.rows == 2 && with.row_length[0] == 5 && with.row_length[1] == 4,
            "and Hosts joins the last row when it is");

    // Which stored value wins.
    Layout custom;
    custom.order = MoveInsert(DefaultOrder(), 8, 0);
    custom.breaks = 0b1000;
    const std::uint64_t garbage = PackLayout(custom) | (std::uint64_t{1} << 55);
    Require(SameLayout(RestoreLayout(PackLayout(custom), old_value), custom),
            "the REG_QWORD when it validates");
    Require(SameLayout(RestoreLayout(std::nullopt, old_value), upgraded),
            "the old REG_DWORD translated when there is none");
    Require(SameLayout(RestoreLayout(garbage, old_value), upgraded),
            "and when it does not validate");
    Require(SameLayout(RestoreLayout(std::nullopt, 0), Layout{}), "nothing stored: the default");
    Require(SameLayout(RestoreLayout(garbage, 0x12345678u), Layout{}),
            "nothing valid: the default");
}

// The expanded view's cards: two columns in the reader's order, an odd last
// card across both, the rows filling the window to its bottom margin, nothing
// overlapping, and room for a label and a reading even at the floor.
void TestOsdCards() {
    using namespace gtg::tray::compact;
    constexpr float kTolerance = 0.01F;
    for (int count = kCellsPerRow; count <= kRecordCount; ++count) {
        for (const int height : {ExpandedHeight(count), ExpandedMinimumHeight(count)}) {
            const float width = 388.0F;
            const float bottom = static_cast<float>(height) - kCardsMarginDip;
            const float inner = width - kCardsMarginDip * 2.0F;
            float last_bottom = 0.0F;
            for (int slot = 0; slot < count; ++slot) {
                const CardBox box = CardAt(slot, count, width, static_cast<float>(height));
                Require(box.width > 0.0F && box.height > 0.0F, "every card has a size");
                Require(box.x >= kCardsMarginDip - kTolerance &&
                            box.x + box.width <= width - kCardsMarginDip + kTolerance,
                        "every card is inside the side margins");
                Require(
                    box.y >= kCardsTopDip - kTolerance && box.y + box.height <= bottom + kTolerance,
                    "every card is under the header and above the bottom margin");
                Require(box.height >= 44.0F,
                        "a card always has room for its label and its reading");
                const bool last_odd = slot == count - 1 && count % 2 != 0;
                Require(
                    last_odd ? std::abs(box.width - inner) < kTolerance : box.width < inner / 2.0F,
                    "only an odd last card spans both columns");
                Require(std::abs(box.x - (slot % 2 == 0 ? kCardsMarginDip
                                                        : kCardsMarginDip + box.width +
                                                              kCardGapDip)) < kTolerance,
                        "even slots on the left, odd slots on the right");
                for (int other = 0; other < slot; ++other) {
                    const CardBox seen = CardAt(other, count, width, static_cast<float>(height));
                    const bool apart = box.x >= seen.x + seen.width - kTolerance ||
                                       seen.x >= box.x + box.width - kTolerance ||
                                       box.y >= seen.y + seen.height - kTolerance ||
                                       seen.y >= box.y + box.height - kTolerance;
                    Require(apart, "no two cards overlap");
                    if (other / 2 == slot / 2) {
                        Require(std::abs(seen.y - box.y) < kTolerance, "a row shares its top");
                    }
                }
                last_bottom = std::max(last_bottom, box.y + box.height);
            }
            Require(std::abs(last_bottom - bottom) < kTolerance,
                    "the last row ends at the bottom margin: the window ends with the cards");
        }
    }
    const CardBox none = CardAt(9, 9, 388.0F, 452.0F);
    Require(none.width == 0.0F && none.height == 0.0F, "a slot that does not exist is empty");
    Require(CardAt(-1, 9, 388.0F, 452.0F).width == 0.0F, "and so is a negative one");
    Require(CardRows(9) == 5 && CardRows(8) == 4 && CardRows(5) == 3 && CardRows(0) == 0,
            "rows are the records two to a row");
    // Cards draw in the lanes' window: there is one footprint, whatever the
    // view, so turning cards on cannot move or resize the OSD.
    const auto placed = Resolve(Layout{}, true, true, true, true);
    Require(ChooseFootprint(false, placed, 1536, 830).width == 388,
            "the cards' window is the lanes' window");
}

void TestExpandedHeightRule() {
    using namespace gtg::tray::compact;
    Require(ExpandedHeight(5) == 330 && ExpandedHeight(6) == 355 && ExpandedHeight(7) == 380 &&
                ExpandedHeight(8) == 405,
            "up to eight records the window is as it was");
    Require(ExpandedHeight(9) == 452, "the ninth adds a lane: 405 to 452 dip");
    for (int count = 5; count <= kRecordCount; ++count) {
        const double lane = (ExpandedHeight(count) - 31.0) / count - 2.0;
        Require(lane >= 44.75 - 1e-9, "no lane is shorter than at eight records");
    }
    const auto placed = Resolve(Layout{}, true, true, true, true);
    Require(ChooseFootprint(false, placed, 1920, 1080).height == 452,
            "the expanded footprint takes the rule");
    // The owner's case: x2.50 on a 4K screen (work area 3840 x 2076 px) is
    // 1536 x 830 dip. The lanes need 452, more than half of 830, which used to
    // switch the view to the old cards.
    const auto large = ChooseFootprint(false, placed, 1536, 830);
    Require(large.width == 388 && large.height == 452 && large.columns == 1,
            "x2.50 on 4K keeps the lanes' window");
    // x2.50 on a 1080p screen (1032 px of work area) is 412 dip: too short.
    Require(ChooseFootprint(false, placed, 768, 412).height == 412,
            "a screen too short for it gets the height it has");
    for (int count = kCellsPerRow; count <= kRecordCount; ++count) {
        Require(ExpandedMinimumHeight(count) < ExpandedHeight(count),
                "the floor is below the natural height for every record count");
    }
    Require(ChooseFootprint(true, placed, 1920, 1080).width == 692,
            "and one row of nine cells is 692 dip");
    Require(CollapsedWidth(kRecordCount) == 692, "the widest compact row");

    using gtg::tray::tiny::BarAt;
    using gtg::tray::tiny::Letter;
    const float left = static_cast<float>(kCellsMarginDip) + gtg::tray::tiny::kBarInsetDip;
    const float right =
        static_cast<float>(kCellsMarginDip + kCellWidthDip) - gtg::tray::tiny::kBarInsetDip;
    for (int i = 0; i < kRecordCount; ++i) {
        const auto bar = BarAt(i, kRecordCount);
        Require(bar.x >= left - 1e-4F && bar.x + bar.width <= right + 1e-4F,
                "nine Tiny bars stay inside the cell");
        Require(bar.width > 5.0F, "and are about 5.1 dip wide");
        if (i + 1 < kRecordCount) {
            Require(BarAt(i + 1, kRecordCount).x - (bar.x + bar.width) >=
                        gtg::tray::tiny::kBarGapDip - 1e-4F,
                    "and apart");
        }
    }
    Require(Letter(Metric::Hosts) == L'H', "Hosts is H");
    unsigned letters = 0;
    for (int i = 0; i < kRecordCount; ++i)
        letters |= 1u << (Letter(static_cast<Metric>(i)) - L'A');
    Require(std::popcount(letters) == kRecordCount, "and no two records share a letter");
}

void TestOsdHostsView() {
    using namespace gtg;
    using tray::osd_hosts::Frame;
    const std::uint64_t now = 1'000'000;
    const auto row = [&](const std::uint64_t peer, const wchar_t* name,
                         std::optional<std::uint64_t> state_ms,
                         std::optional<std::uint64_t> goodbye_ms, const peer::Protection protection,
                         const bool raised) {
        peer::HostRow host;
        host.peer = peer;
        host.name = name;
        host.heard = {state_ms, goodbye_ms};
        if (state_ms) {
            peer::HostState state;
            state.name = name;
            state.protection = protection;
            state.records[static_cast<std::size_t>(peer::Record::Gpu)] = {0.5F,  0.7F,  raised,
                                                                          false, 50.0F, {}};
            host.state = state;
        }
        return host;
    };
    peer::Table table;
    table.hosts = {
        row(1, L"Calm", now - 500, {}, peer::Protection::Normal, false),
        row(2, L"Gone", now - 42'500, {}, peer::Protection::Normal, false),
        row(3, L"Asleep", now - 60'000, now - 30'000, peer::Protection::SafePower, false),
        row(4, L"Never", {}, {}, peer::Protection::Normal, false),
        row(5, L"Busy", now - 800, {}, peer::Protection::Normal, true),
        row(6, L"Hot", now - 4'000, {}, peer::Protection::SafePower, false),
    };
    const auto view = tray::osd_hosts::MakeView(table, now);
    Require(view.paired == 6 && view.shown.size() == 4, "four of six shown");
    std::vector<std::wstring> names;
    for (const auto& host : view.shown)
        names.push_back(host.name);
    Require(names == std::vector<std::wstring>{L"Calm", L"Gone", L"Busy", L"Hot"},
            "the three in alert and the one present, in the reader's order");
    const auto& calm = view.shown[0];
    const auto& gone = view.shown[1];
    const auto& busy = view.shown[2];
    const auto& hot = view.shown[3];
    Require(gone.presence == peer::Presence::Lost && gone.silent_s == 42 && gone.alert &&
                !gone.protection,
            "a lost host: its silence counted, its old state not drawn as current");
    Require(busy.alert && busy.records[static_cast<std::size_t>(peer::Record::Gpu)].raised,
            "a host raises what it raised itself");
    Require(hot.presence == peer::Presence::Dim && hot.alert,
            "a late host still carries its alert");
    Require(view.AnyAlert(), "the record has a host in alert");
    Require(tray::osd_hosts::FrameFor(calm) == Frame::Quiet &&
                tray::osd_hosts::FrameFor(gone) == Frame::Red &&
                tray::osd_hosts::FrameFor(hot) == Frame::Amber,
            "frames: quiet, red when lost, amber on safe power");
    tray::osd_hosts::Host faulted = calm;
    faulted.protection = peer::Protection::Fault;
    tray::osd_hosts::Host offline = calm;
    offline.presence = peer::Presence::Offline;
    Require(tray::osd_hosts::FrameFor(faulted) == Frame::Red &&
                tray::osd_hosts::FrameFor(offline) == Frame::Offline,
            "red for a fault, quiet and dimmer offline");
    Require(tray::osd_hosts::MakeView(table, now) == view, "the same table reads the same");
    Require(!(tray::osd_hosts::MakeView(table, now + 1'000) == view),
            "and a second later the lost host's count has moved");

    using tray::compact::Metric;
    const auto placed = tray::compact::Resolve(tray::compact::Layout{}, true, true, true, true);
    Require(tray::osd_hosts::CellMetrics(placed) ==
                std::vector<Metric>{Metric::Temperature, Metric::Vram, Metric::Gpu, Metric::Cpu},
            "the cell shows T V G C in the reader's order");
    const auto lane = tray::osd_hosts::LaneMetrics(placed);
    Require(lane.size() == 8 && std::find(lane.begin(), lane.end(), Metric::Hosts) == lane.end(),
            "the lane shows every placed record but Hosts");
    const auto reduced = tray::compact::Resolve(tray::compact::Layout{}, false, false, false, true);
    Require(tray::osd_hosts::LaneMetrics(reduced).size() == 5,
            "only the records the reader has on");
    Require(tray::osd_hosts::RecordOf(calm, Metric::Hosts) == nullptr &&
                tray::osd_hosts::RecordOf(calm, Metric::Gpu)->level == 0.5F,
            "a host's records by the reader's record");
    Require(tray::osd_hosts::MakeView(peer::Table{}, now).shown.empty(), "no hosts, nothing shown");
}

void TestNetworkVerdictJudgesTheWorst() {
    using namespace gtg::net;

    // Nothing measured is not the same as nothing wrong.
    Require(Judge({10, 2, 0, 0, 0, 0}) == Grade::Unknown,
            "an idle connection reports no verdict at all");
    Require(Judge({10, 2, 0, 0, 0, 1}) == Grade::Good,
            "one sample is enough to have an opinion");

    // The three axes, each on its own.
    Require(GradeLatency(0) == Grade::Good && GradeLatency(kGoodRttMs) == Grade::Good,
            "up to the good bound is good");
    Require(GradeLatency(kGoodRttMs + 1) == Grade::Fair, "past it is fair");
    Require(GradeLatency(kFairRttMs) == Grade::Fair, "up to the fair bound");
    Require(GradeLatency(kFairRttMs + 1) == Grade::Poor, "and past that, poor");
    Require(GradeJitter(kGoodVarMs) == Grade::Good &&
                GradeJitter(kFairVarMs + 1) == Grade::Poor,
            "jitter grades the same way");
    // Jitter is graded harder than latency, which is the whole point: a
    // steady 80 ms is playable, a 40 ms swinging by 100 is not.
    Require(kFairVarMs < kFairRttMs,
            "variance is judged more harshly than distance");

    Require(GradeLoss({0, 0, 0, 0, 0, 5}) == Grade::Good, "no loss is good");
    Require(GradeLoss({0, 0, 3, 0, 0, 5}) == Grade::Fair,
            "retransmission alone is recoverable");
    Require(GradeLoss({0, 0, 0, 9, 0, 5}) == Grade::Fair,
            "and so are duplicate acknowledgements");
    Require(GradeLoss({0, 0, 0, 0, 1, 5}) == Grade::Poor,
            "a timeout is poor whatever else is true");

    // The worst decides, never the average. A nearby server that is losing
    // packets is not half fine.
    Require(Judge({5, 1, 0, 0, 2, 10}) == Grade::Poor,
            "1 ms with timeouts is poor, not good");
    Require(Judge({200, 1, 0, 0, 0, 10}) == Grade::Poor, "distance alone can be poor");
    Require(Judge({5, 200, 0, 0, 0, 10}) == Grade::Poor, "so can jitter alone");
    Require(Judge({5, 1, 0, 0, 0, 10}) == Grade::Good, "and clean is clean");

    // Worst() is a total order with Unknown as the identity, so folding over
    // connections in any sequence gives the same answer.
    {
        const Grade all[]{Grade::Unknown, Grade::Good, Grade::Fair, Grade::Poor};
        for (const Grade a : all) {
            Require(Worst(a, Grade::Unknown) == a, "unknown never overrides");
            Require(Worst(Grade::Unknown, a) == a, "in either position");
            for (const Grade b : all) {
                Require(Worst(a, b) == Worst(b, a), "and the fold is symmetric");
                for (const Grade c : all)
                    Require(Worst(Worst(a, b), c) == Worst(a, Worst(b, c)),
                            "and associative, so connection order cannot matter");
            }
        }
    }

    // Across a program's connections.
    {
        VerdictBuilder builder;
        Require(builder.Result().grade == Grade::Unknown,
                "no connections, no verdict");
        Require(builder.Result().connections == 0, "and nothing counted");

        builder.Add({20, 3, 0, 0, 0, 40});      // clean
        Require(builder.Result().grade == Grade::Good, "one clean connection is good");
        Require(builder.Result().rtt_ms == 20, "and reports its latency");

        builder.Add({8, 2, 0, 0, 0, 0});        // idle: contributes nothing
        Require(builder.Result().connections == 1,
                "an idle connection is not counted as measured");

        builder.Add({150, 90, 4, 12, 3, 40});   // the bad one
        Require(builder.Result().grade == Grade::Poor,
                "one failing connection makes the verdict poor");
        Require(builder.Result().rtt_ms == 150,
                "and the figure shown is the one the verdict came from");
        Require(builder.Result().connections == 2, "two were measured");

        builder.Add({15, 1, 0, 0, 0, 40});      // another clean one
        Require(builder.Result().grade == Grade::Poor,
                "a healthy connection does not redeem a failing one");
        Require(builder.Result().rtt_ms == 150, "nor change the figure");
    }
}

void TestLaneValueTakesWhatTheAnnotationLeaves() {
    using namespace gtg::tray::compact;

    // The lane as the expanded overlay draws it: 388 dip wide, nominal height.
    constexpr float kRowX = 0.0F;
    constexpr float kRowWidth = 388.0F;
    constexpr float kScale = 1.0F;

    // With nothing beside it, the value owns the band from the graph's left
    // edge to the row's right. That is the case the Power lane is in.
    {
        const auto span = LaneValueSpan(kRowX, kRowWidth, 0.0F, kScale);
        Require(span.x == LaneGraphSpan(kRowX, kRowWidth, kScale).x,
                "with no annotation the value starts where the graph does");
        Require(span.width > 132.0F,
                "and is wider than the constant that cut 349.5 W (350 W)");
        Require(span.x + span.width <= kRowX + kRowWidth,
                "and never runs past the row");
    }

    // The part that actually broke: `scale` is LaneScale, which falls as
    // records are added because they share a fixed window height. A constant
    // box therefore shrank horizontally every time the dashboard gained a
    // record, and the value face -- sized from the DPI scale, not this one --
    // did not. Taking the free band makes the width stop caring.
    {
        const auto seven = LaneValueSpan(kRowX, kRowWidth, 0.0F, 0.678F);
        const auto eight = LaneValueSpan(kRowX, kRowWidth, 0.0F, 0.590F);
        Require(eight.width > seven.width,
                "an extra record must not make the value box narrower");
        Require(eight.width > 132.0F * 0.590F * 2.0F,
                "and it is no longer a constant multiplied by a vertical ratio");
    }

    // With an annotation, the value starts after what that text measured --
    // not after a reservation, which is how the two used to be kept apart.
    {
        const float annotation = 70.0F;   // "max 89 °C" at the label face
        const auto span = LaneValueSpan(kRowX, kRowWidth, annotation, kScale);
        const auto graph = LaneGraphSpan(kRowX, kRowWidth, kScale);
        Require(span.x >= graph.x + annotation,
                "the value begins past the annotation, never over it");
        Require(span.x > LaneValueSpan(kRowX, kRowWidth, 0.0F, kScale).x,
                "an annotation costs the value room, and only what it took");
        Require(span.width < LaneValueSpan(kRowX, kRowWidth, 0.0F, kScale).width,
                "the band is shared, not duplicated");
    }

    // A pathological annotation cannot squeeze the figure to nothing.
    {
        const auto span = LaneValueSpan(kRowX, kRowWidth, 1000.0F, kScale);
        Require(span.width >= kLaneValueMinimumDip,
                "there is always a floor to draw the reading in");
    }

    // Every scale, not just 100 %.
    for (const float scale : {1.0F, 1.25F, 1.5F, 2.0F, 2.5F}) {
        const float width = 388.0F * scale;
        const auto span = LaneValueSpan(0.0F, width, 70.0F * scale, scale);
        Require(span.x + span.width <= width + 0.01F,
                "the box stays inside the row at every scale");
        Require(span.width > 0.0F, "and is never inverted");
    }
}

void TestNetworkResultSaysWhatWasMeasured() {
    using namespace gtg::net;
    using namespace gtg::net::action;

    // The figure and the mark describe the same connection, so a reader who
    // reads one bar and a low number is not being shown two different paths.
    {
        const Verdict good{Grade::Good, 18, 3};
        Require(ResultText(good, ProbeStatus::Ready) == L"18 ms",
                "a graded verdict shows its latency");
        Require(BarsFor(Grade::Good) == 3, "good fills the mark");
        Require(BarsFor(Grade::Fair) == 2, "fair fills two");
        Require(BarsFor(Grade::Poor) == 1, "poor fills one");
        Require(BarsFor(Grade::Unknown) == 0,
                "and nothing measured fills none, which is not the same as bad");
    }

    // Whole milliseconds at every magnitude. SmoothedRtt is an estimate with
    // its own variance; a decimal on it would be precision the reading does
    // not have.
    {
        for (const std::uint32_t rtt : {std::uint32_t{1}, std::uint32_t{99},
                                        std::uint32_t{1234}}) {
            const std::wstring text = ResultText({Grade::Fair, rtt, 1},
                                                 ProbeStatus::Ready);
            Require(text.find(L'.') == std::wstring::npos,
                    "no decimal point at any magnitude");
            Require(text.ends_with(L" ms"), "and the unit is always said");
        }
    }

    // Every way it can fail says something different. This is the whole second
    // half of the feature: when the network cannot be improved the reader must
    // still learn something true about it.
    {
        // Each failure names itself. The two below were once the same word,
        // and a reader who hit it could not tell whether to click again with
        // their game in front or to conclude their game does not use TCP.
        Require(ResultText({}, ProbeStatus::NoConnections) == L"no TCP",
                "no IPv4 TCP is what was established -- not that nothing is connected");
        Require(ResultText({}, ProbeStatus::NoForeground) == L"no app",
                "and nothing in the foreground is a different thing again");
        Require(ResultText({}, ProbeStatus::NoConnections) !=
                    ResultText({}, ProbeStatus::NoForeground),
                "so the two never read the same");
        Require(ResultText({}, ProbeStatus::NotPermitted) == L"denied",
                "a refusal is named rather than dressed up as a verdict");
        Require(ResultText({}, ProbeStatus::NoLibrary) == L"n/a",
                "and a machine that cannot answer says so");

        // The trap this guards: a probe that started fine and measured nothing
        // still has no verdict, and Ready must not be read as Good.
        Require(ResultText({}, ProbeStatus::Ready) == L"--",
                "ready but unmeasured is not a grade");
        Require(BarsFor(Grade::Unknown) == 0, "and draws an empty mark");
    }

    // Colour agrees with the mark, always. A green cell with one bar would be
    // two answers to one question.
    {
        Require(TintFor(Grade::Good) != TintFor(Grade::Fair), "three distinct tints");
        Require(TintFor(Grade::Fair) != TintFor(Grade::Poor), "for three grades");
        Require(TintFor(Grade::Unknown) != TintFor(Grade::Good),
                "and unknown is not green");
    }

    // The window has to be long enough to difference the loss counters and
    // short enough that nobody in a game walks away from it.
    {
        Require(kMeasureWindowMs >= 1000,
                "shorter than a second cannot difference a retransmission count");
        Require(kMeasureWindowMs <= 4000, "longer than four seconds is a wait");
        Require(kMinimumVisibleMs < kMeasureWindowMs,
                "the floor only matters when the probe fails early");
        Require(kWorkBudgetMs > kMeasureWindowMs,
                "the failsafe must outlast the work it is guarding");
    }
}

void TestNetworkThroughputArithmetic() {
    using namespace gtg::net;

    // A rate needs two readings and a gap that means something.
    const Counters first{1'000'000, 200'000, 10'000};
    {
        const auto rate = RateBetween(first, {2'048'000, 200'000, 11'000});
        Require(rate.has_value(), "one second apart is a rate");
        Require(rate->received_bytes_per_second == 1'048'000.0,
                "and it is the difference over the elapsed seconds");
        Require(rate->sent_bytes_per_second == 0.0,
                "a direction that moved nothing rates zero");
    }

    // The three ways a difference stops meaning anything, all refused rather
    // than papered over.
    Require(!RateBetween(first, {2'000'000, 300'000, 10'000}).has_value(),
            "no time passed, so there is no rate");
    Require(!RateBetween(first, {2'000'000, 300'000, 9'000}).has_value(),
            "a clock that went backwards is refused");
    Require(!RateBetween(first, {900'000, 300'000, 11'000}).has_value(),
            "a counter that went backwards means the adapter reset");
    Require(!RateBetween(first, {2'000'000, 100'000, 11'000}).has_value(),
            "in either direction");
    Require(!RateBetween(first, {2'000'000, 300'000,
                                 10'000 + kMaximumGapMs + 1}).has_value(),
            "and a gap too long to describe now is refused");
    Require(RateBetween(first, {2'000'000, 300'000,
                                10'000 + kMaximumGapMs}).has_value(),
            "the boundary itself is still usable");

    // Two adapters' counters have nothing to do with each other. The route can
    // resolve to a different interface while the stack settles after launch,
    // and the difference across that switch is a rate no link ever carried.
    {
        const Counters on_a{1'000'000, 200'000, 10'000, 7};
        const Counters on_b{9'000'000, 900'000, 11'000, 12};
        Require(!RateBetween(on_a, on_b).has_value(),
                "a difference across two adapters is refused");
        const Counters still_a{2'000'000, 300'000, 11'000, 7};
        Require(RateBetween(on_a, still_a).has_value(),
                "while the same adapter is fine");
    }

    // A rate above what the adapter says it can carry did not happen. Measured
    // on this machine: the default route reports 1 Gb/s, which is 119.2 MB/s,
    // and the cell was showing hundreds for the first seconds after launch.
    {
        constexpr std::uint64_t gigabit = 1'000'000'000ULL;
        const Counters before{0, 0, 0, 3, gigabit, gigabit};
        // 300 MB/s on a gigabit link: impossible.
        const Counters impossible{300ULL * 1024 * 1024, 0, 1000, 3, gigabit, gigabit};
        Require(!RateBetween(before, impossible).has_value(),
                "a receive rate the link cannot carry is refused");
        const Counters impossible_up{0, 300ULL * 1024 * 1024, 1000, 3, gigabit, gigabit};
        Require(!RateBetween(before, impossible_up).has_value(),
                "and so is an impossible transmit rate");
        // 100 MB/s on the same link is fast but real.
        const Counters busy{100ULL * 1024 * 1024, 0, 1000, 3, gigabit, gigabit};
        Require(RateBetween(before, busy).has_value(),
                "a rate the link can carry is kept");

        // An adapter that reports no link speed gets no ceiling: refusing
        // everything would be worse than showing it.
        const Counters silent_before{0, 0, 0, 3, 0, 0};
        const Counters silent_after{300ULL * 1024 * 1024, 0, 1000, 3, 0, 0};
        Require(RateBetween(silent_before, silent_after).has_value(),
                "no stated link speed means no ceiling");
        Require(!ExceedsLink(1e12, 0), "zero is not a ceiling");
        Require(!ExceedsLink(1e12, kImplausibleLinkSpeedBps),
                "and neither is a sentinel");
        Require(ExceedsLink(126.0 * 1000 * 1000 * 1.30, gigabit),
                "beyond the tolerance is refused");
        Require(!ExceedsLink(125.0 * 1000 * 1000, gigabit),
                "exactly at the wire rate is not");
    }

    // Counters are 64-bit and cumulative; a huge but legal difference must not
    // overflow or come out negative.
    {
        const Counters low{0, 0, 0};
        const Counters high{1ULL << 40, 1ULL << 39, 1000};
        const auto rate = RateBetween(low, high);
        Require(rate.has_value() && rate->received_bytes_per_second > 0.0,
                "a terabyte-scale difference is still a positive rate");
    }

    // One unit, always. The adaptive one was removed, not merely defaulted:
    // 「這麼小的 CELL 沒人會注意到單位不同。只會覺得很怪」 -- a suffix nobody
    // re-reads is a suffix that silently makes a comparison a thousand times
    // wrong.
    Require(kDisplayUnit == Unit::Megabytes, "megabytes per second, everywhere");
    {
        char suffix[8]{};
        WideCharToMultiByte(CP_UTF8, 0, UnitSuffix(kDisplayUnit), -1, suffix,
                            sizeof(suffix) - 1, nullptr, nullptr);
        Require(std::string_view(suffix) == "MB/s", "and it says so");
    }

    // Tenths of that one unit, with the ceiling rule: a live direction never
    // renders as zero, however far below the other it is.
    Require(TenthsIn(0.0, kDisplayUnit) == 0, "exactly nothing prints nothing");
    Require(TenthsIn(800.0, kDisplayUnit) == 1,
            "800 B/s floors at one tenth of a megabyte");
    Require(TenthsIn(671.0 * 1024, kDisplayUnit) == 7,
            "and ordinary traffic keeps a digit worth reading");
    Require(TenthsIn(12.4 * 1024 * 1024, kDisplayUnit) == 124,
            "while a busy link reads as itself");
    for (double bytes = 0.5; bytes < 4.0 * 1024 * 1024 * 1024; bytes *= 1.7) {
        Require(TenthsIn(bytes, kDisplayUnit) >= 1,
                "no positive reading ever renders as zero");
    }

    // Whole units are still available for anywhere that wants them, with the
    // same floor.
    Require(WholeIn(0.0, kDisplayUnit) == 0, "nothing still prints nothing");
    Require(WholeIn(1.0, kDisplayUnit) == 1, "one byte a second is not zero");
    Require(WholeIn(12.4 * 1024 * 1024, kDisplayUnit) == 13,
            "and a real reading rounds up, never down");

    // One span for both curves, so their heights compare honestly.
    Require(SharedPeak({12.0, 800.0}) == 800.0, "the peak is the larger one");
    Require(SharedPeak({800.0, 12.0}) == 800.0, "whichever direction it is in");
}

void TestCompactSwapOnOverlap() {
    using namespace gtg::tray::compact;

    const Layout flat;                       // 1 x 7, default order
    const auto placed = Resolve(flat, true, true);
    const CellGrid grid = MakeCellGrid(7, CollapsedWidth(7), 1.0F);

    // A release is read against the cell it is nearest to, and that cell is
    // divided into zones. Squarely on one is a swap; beside it inserts into
    // its row; above or below opens a new row there.
    const float cell_w = grid.cell_width_dip;
    const float cell_h = kCellHeightDip;
    const auto at = [&](int slot, float nx, float ny) {
        const CellOrigin cell = CellOriginAt(grid, placed, slot);
        return PlanDrop(grid, placed,
                        static_cast<int>(cell.x + nx * cell_w / 2.0F),
                        static_cast<int>(cell.y + ny * cell_h / 2.0F));
    };

    for (int slot = 0; slot < placed.count; ++slot) {
        const auto plan = at(slot, 0.0F, 0.0F);
        Require(plan.action == DropAction::Swap && plan.slot == slot,
                "dead centre on a cell is a swap with it");
    }

    // Beside a cell, along the axis it is furthest from.
    {
        const auto before = at(3, -1.0F, 0.0F);
        Require(before.action == DropAction::InsertInRow && before.column == 3,
                "to the left inserts before it");
        const auto after = at(3, 1.0F, 0.0F);
        Require(after.action == DropAction::InsertInRow && after.column == 4,
                "to the right inserts after it");
    }

    // Above or below, which is the change the single column needed: a row can
    // now be opened anywhere, not only past the last one.
    {
        const auto above = at(0, 0.0F, -1.0F);
        Require(above.action == DropAction::InsertRow && above.row == 0,
                "above the first cell opens a row before it");
        const auto below = at(0, 0.0F, 1.0F);
        Require(below.action == DropAction::InsertRow && below.row == 1,
                "and below it opens one after");
    }

    // Released into empty space: nothing happens.
    Require(at(0, 0.0F, 4.0F).action == DropAction::None,
            "a release well clear of the strip changes nothing");
    Require(at(0, -4.0F, 0.0F).action == DropAction::None,
            "in either direction");

    // The zones tile the plane: whatever cell the planner reads a release
    // against, that cell is the nearest one, and the zone rule was applied to
    // it. Asserting against a cell chosen in advance would be asserting the
    // test'''s arithmetic instead of the planner'''s.
    for (int step_x = -40; step_x <= 40; ++step_x) {
        for (int step_y = -40; step_y <= 40; ++step_y) {
            const int x = static_cast<int>(CellOriginAt(grid, placed, 2).x) +
                          step_x * 4;
            const int y = static_cast<int>(CellOriginAt(grid, placed, 2).y) +
                          step_y * 4;
            const auto plan = PlanDrop(grid, placed, x, y);
            if (plan.action == DropAction::None) continue;

            const CellOrigin chosen = CellOriginAt(grid, placed, plan.slot);
            const float nx = (static_cast<float>(x) - chosen.x) * 2.0F / cell_w;
            const float ny = (static_cast<float>(y) - chosen.y) * 2.0F / cell_h;
            const float ax = nx < 0.0F ? -nx : nx;
            const float ay = ny < 0.0F ? -ny : ny;

            // It really is the nearest cell.
            for (int slot = 0; slot < placed.count; ++slot) {
                const CellOrigin other = CellOriginAt(grid, placed, slot);
                const float ox = (static_cast<float>(x) - other.x) * 2.0F / cell_w;
                const float oy = (static_cast<float>(y) - other.y) * 2.0F / cell_h;
                Require(ox * ox + oy * oy >= nx * nx + ny * ny - 0.001F,
                        "the release is read against the nearest cell");
            }

            if (ax <= kSwapZone && ay <= kSwapZone) {
                Require(plan.action == DropAction::Swap,
                        "the middle of a cell always swaps");
            } else {
                Require(plan.action ==
                            (ay > ax ? DropAction::InsertRow
                                     : DropAction::InsertInRow),
                        "and outside it the longer axis decides");
            }
        }
    }

    // A new row in the middle of a single column, in one gesture.
    {
        const Layout column_layout = LayoutWithRows({1, 1, 1, 1, 1, 1, 1});
        const auto tower = Resolve(column_layout, true, true);
        Require(tower.rows == 7, "seven rows to begin with");
        const Layout moved = ApplyDropRow(column_layout, tower, 6, 2);
        const auto after_move = Resolve(moved, true, true);
        Require(after_move.rows == 7 && WidestRow(after_move) == 1,
                "the shape is unchanged because a column has one cell a row");
        Require(after_move.cells[2] == tower.cells[6],
                "and the dragged record landed in the row it opened");
    }

    // The same gesture in a multi-row arrangement genuinely adds a row.
    {
        const Layout four_three = LayoutWithRows({4, 3});
        const auto shape = Resolve(four_three, true, true);
        const Layout inserted = ApplyDropRow(four_three, shape, 0, 1);
        const auto after_insert = Resolve(inserted, true, true);
        Require(after_insert.rows == 3, "a row opens between the two");
        Require(after_insert.row_length[0] == 3 && after_insert.row_length[1] == 1 &&
                    after_insert.row_length[2] == 3,
                "leaving 3, 1, 3");
        Require(after_insert.cells[3] == shape.cells[0],
                "with the dragged record alone in the middle row");
    }

    // Appending is the same operation at the end, so the old bottom target is
    // not a special case any more.
    {
        const Layout four_three = LayoutWithRows({4, 3});
        const auto shape = Resolve(four_three, true, true);
        const auto appended = Resolve(ApplyDropRow(four_three, shape, 0, 2), true, true);
        Require(appended.rows == 3 && appended.row_length[2] == 1,
                "dropping past the last row still appends one");
    }

    // Refusals stay refusals.
    Require(ApplyDropRow(flat, placed, 0, -1).order == flat.order,
            "a negative row is refused");
    Require(ApplyDropRow(flat, placed, 0, placed.rows + 1).order == flat.order,
            "and one past the end");

    // The exchange itself: two records trade places, nothing else moves.    // The exchange itself: two records trade places, nothing else moves.
    const Layout swapped = ApplySwap(flat, placed, 0, 3);
    const auto after = Resolve(swapped, true, true);
    Require(after.cells[0] == placed.cells[3] && after.cells[3] == placed.cells[0],
            "the two records trade places");
    for (int slot = 0; slot < placed.count; ++slot) {
        if (slot == 0 || slot == 3) continue;
        Require(after.cells[static_cast<std::size_t>(slot)] ==
                    placed.cells[static_cast<std::size_t>(slot)],
                "and every other record stays exactly where it was");
    }
    Require(swapped.breaks == flat.breaks, "a swap never changes the shape");
    Require(IsValidLayout(swapped), "and always leaves a legal arrangement");

    // Across rows, a swap exchanges rows without changing either row's length.
    const Layout two_rows = LayoutWithRows({4, 3});
    const auto tall = Resolve(two_rows, true, true);
    const Layout crossed = ApplySwap(two_rows, tall, 0, 5);
    const auto after_cross = Resolve(crossed, true, true);
    Require(RowOf(after_cross, 0) == 0 && RowOf(after_cross, 5) == 1,
            "the slots keep their rows");
    Require(after_cross.row_length[0] == tall.row_length[0] &&
                after_cross.row_length[1] == tall.row_length[1],
            "and the row lengths are untouched");
    Require(after_cross.cells[0] == tall.cells[5] &&
                after_cross.cells[5] == tall.cells[0],
            "while the two records have exchanged rows");

    // Refusals are refusals, not clamps.
    Require(ApplySwap(flat, placed, 2, 2).order == flat.order,
            "swapping a cell with itself changes nothing");
    Require(ApplySwap(flat, placed, -1, 3).order == flat.order,
            "and an out-of-range slot is refused");
    Require(ApplySwap(flat, placed, 0, placed.count).order == flat.order,
            "at either end");

    // Swaps compose: any order is reachable, so nothing was lost by preferring
    // them to insertion over a cell.
    Layout walk = flat;
    auto walk_placed = Resolve(walk, true, true);
    for (int slot = 0; slot + 1 < walk_placed.count; ++slot) {
        walk = ApplySwap(walk, walk_placed, slot, slot + 1);
        walk_placed = Resolve(walk, true, true);
        Require(IsValidLayout(walk), "every intermediate arrangement is legal");
    }
    Require(walk_placed.count == 7, "and still holds every record once");
}

void TestCompactWidestRowFloor() {
    using namespace gtg::tray::compact;
    // A single column is a shape a reader can ask for, to dock the dashboard
    // against a screen edge. It used to collapse to one row, on the grounds
    // that 84 dip could not hold the lock, the chevron and a grabbable drag
    // strip -- see the measurement below, which is why that is no longer true.
    const auto singles = Resolve(LayoutWithRows({1, 1, 1, 1, 1, 1, 1}), true, true);
    Require(singles.rows == 7 && singles.row_length[0] == 1,
            "a single column is kept as the reader arranged it");
    Require(WidestRow(singles) == 1, "and its widest row really is one");

    // The claim that made the old rule look necessary, checked against the hit
    // tests themselves rather than against a number written in a comment. The
    // comment said 24 dip, "roughly 2.5 mm at 100 %"; 24 dip is 6.35 mm, and
    // the real strip is wider than that again.
    {
        const int width = CollapsedWidth(1);
        const int header = 25;   // kDragHeightDip
        int draggable = 0;
        for (int x = 0; x < width; ++x) {
            if (!ToggleHit(x, 1, width, header) && !LockHit(x, 1, width, header))
                ++draggable;
        }
        Require(width == 84, "one column is 84 dip across");
        Require(HeaderButtonWidth(header) == 22, "the buttons are nine tenths of the band");
        Require(draggable == 40, "40 of which are draggable");
        // 40 dip at 96 dpi is 10.6 mm, and the band is 25 dip tall. A Windows
        // title bar is about 7 mm. The floor exists so this stays checkable.
        Require(draggable >= 24, "a single column keeps a grabbable strip");
        // The two buttons are adjacent, so the strip is one run rather than
        // two -- a gap between them would be draggable and look like a seam.
        Require(!ToggleHit(width - 23, 1, width, header) &&
                    LockHit(width - 23, 1, width, header),
                "the lock starts exactly where the chevron ends");
        Require(!HeaderShowsTitle(1), "which is only true once the label goes");
        Require(HeaderShowsTitle(2), "two cells across keeps it");
        Require(!HeaderShowsStatusText(1), "and no status words at 84 dip");
    }

    // The margin under the last cell is the same whatever the shape. It was
    // not: the height increment was one dip short of the row pitch, so each
    // extra row ate a dip, and by seven rows the bottom cell sat against the
    // frame. Derived from the geometry rather than compared to a constant, so
    // it stays true if either number moves.
    for (int rows = 1; rows <= kRecordCount; ++rows) {
        Require(CollapsedBottomMargin(rows) == CollapsedBottomMargin(1),
                "the margin below the last cell does not depend on row count");
        Require(CollapsedBottomMargin(rows) > 0,
                "and it is a margin rather than an overlap");
    }

    // Every composition of seven records is now legal, and every one of them
    // survives a round trip through the stored form.
    for (unsigned breaks = 0; breaks < 64; ++breaks) {
        Layout layout;
        layout.breaks = static_cast<std::uint8_t>(breaks);
        Require(IsValidLayout(layout), "every break set is a legal arrangement");
        const auto placed = Resolve(layout, true, true);
        Require(placed.count == 7, "all seven records are placed");
        Require(WidestRow(placed) >= kMinimumWidestRow, "and none is below the floor");
        const auto restored = UnpackLayout(PackLayout(layout));
        Require(restored.has_value() && restored->breaks == layout.breaks,
                "and it round-trips through the stored form");
    }
    Require(WidestRow(singles) >= kMinimumWidestRow, "the floor is enforced");

    // Everything with a widest row of two or more is left exactly as asked.
    const auto narrow = Resolve(LayoutWithRows({2, 1, 1, 1, 1, 1}), true, true);
    Require(narrow.rows == 6, "two at the top is enough to keep six rows");
    Require(CollapsedWidth(WidestRow(narrow)) == 160, "and it is 160 dip wide");
}

void TestCompactLayoutMoveInsert() {
    using namespace gtg::tray::compact;
    const Order start = DefaultOrder();

    const Order forward = MoveInsert(start, 0, 3);
    Require(forward[3] == Metric::Temperature && forward[0] == Metric::Power,
            "a forward move shifts the records between");
    const Order backward = MoveInsert(start, 5, 1);
    Require(backward[1] == Metric::Ram && backward[2] == Metric::Power,
            "a backward move shifts the other way");
    Require(MoveInsert(start, 2, 2) == start, "a move onto itself is identity");
    Require(MoveInsert(start, -1, 2) == start, "a negative index is refused");
    Require(MoveInsert(start, 2, kRecordCount) == start,
            "an index past the end is refused");

    for (int from = 0; from < kRecordCount; ++from) {
        for (int to = 0; to < kRecordCount; ++to) {
            Require(IsValidOrder(MoveInsert(start, from, to)),
                    "every move yields a permutation");
        }
    }
}

void TestCompactLayoutStoredValueCannotHideARecord() {
    using namespace gtg::tray::compact;
    Layout custom;
    custom.order = MoveInsert(DefaultOrder(), 0, 4);
    custom.breaks = LayoutWithRows({3, 2, 2}).breaks;
    const auto restored = UnpackLayout(PackLayout(custom));
    Require(restored && restored->order == custom.order &&
                restored->breaks == custom.breaks,
            "a layout survives the round trip");

    // The previous schema put a three-bit first-row count where the breaks now
    // live, so an untagged value would decode as a plausible but different
    // arrangement. Losing a layout is better than silently reshaping it.
    const std::uint32_t old_format = 0x00A53210u;   // no format nibble
    Require(!UnpackLayout(old_format), "a value from the old schema is rejected");
    Require(SanitizeLayout(old_format).breaks == 0,
            "and sanitizes to the default single row");

    Require(!UnpackLayout(0), "zero is not a valid layout");
    Require(SanitizeLayout(0).order == DefaultOrder(), "zero gives the default");

    // A duplicated or missing record must never survive.
    Layout duplicated = custom;
    duplicated.order[2] = duplicated.order[1];
    Require(!UnpackLayout(PackLayout(duplicated)), "a duplicate is rejected");

    for (std::uint32_t rubbish : {0xFFFFFFFFu, 0x12345678u, 0xDEADBEEFu,
                                  0x10000000u, 0x1FFFFFFFu}) {
        const Layout safe = SanitizeLayout(rubbish);
        // kRecordCount, not a literal. This was `int seen[7]`, written before
        // the Network record made it eight, so every sanitized layout wrote one
        // element past the array. Release never noticed; the Debug runtime's
        // stack check stopped on it on 2026-10-01.
        int seen[kRecordCount]{};
        for (const Metric metric : safe.order)
            ++seen[static_cast<int>(metric)];
        for (int i = 0; i < kRecordCount; ++i)
            Require(seen[i] == 1, "every record present exactly once after sanitizing");
        Require((static_cast<unsigned>(safe.breaks) >> kMaxBreaks) == 0u,
                "no break bit outside the field");
    }
}

void TestCellHitTestingAcrossScales() {
    using namespace gtg::tray::compact;
    for (const float scale : {1.0F, 1.25F, 1.5F, 2.0F, 2.5F}) {
        for (const auto& rows : {std::initializer_list<int>{7},
                                 std::initializer_list<int>{5, 2},
                                 std::initializer_list<int>{3, 2, 2},
                                 std::initializer_list<int>{2, 2, 2, 1}}) {
            const auto placed = Resolve(LayoutWithRows(rows), true, true);
            const int width =
                static_cast<int>(CollapsedWidth(WidestRow(placed)) * scale);
            const auto grid = MakeCellGrid(WidestRow(placed), width, scale);

            for (int slot = 0; slot < placed.count; ++slot) {
                const auto origin = CellOriginAt(grid, placed, slot);
                const int x = static_cast<int>(
                    origin.x + grid.cell_width_dip * scale / 2.0F);
                const int y = static_cast<int>(
                    origin.y + kCellHeightDip * scale / 2.0F);
                Require(CellSlotAt(grid, placed, x, y) == slot,
                        "every cell finds itself at every scale");
                const auto target = DropTargetAt(grid, placed, x, y);
                Require(target.row == RowOf(placed, slot),
                        "a release inside a cell resolves to its own row");
            }
            // The header is never a cell.
            Require(CellSlotAt(grid, placed, width / 2, 4) < 0,
                    "the header is not a cell");
            // One band below the last row means a new row.
            const float below =
                (kCellsTopDip + static_cast<float>(placed.rows) *
                                    static_cast<float>(kCollapsedRowPitch) +
                 4.0F) * scale;
            Require(DropTargetAt(grid, placed, width / 2,
                                 static_cast<int>(below)).row == placed.rows,
                    "the band below the last row opens a new one");
        }
    }
}

void TestCompactDropRules() {
    using namespace gtg::tray::compact;
    const Layout one_row;
    const auto flat = Resolve(one_row, true, true);

    // Dropping below the last row starts a new one. Without this no
    // arrangement deeper than the current one could ever be reached.
    const Layout split = ApplyDrop(one_row, flat, 6, flat.rows, 0);
    const auto after = Resolve(split, true, true);
    Require(after.rows == 2, "a release below the last row opens a row");
    Require(after.row_length[0] == 6 && after.row_length[1] == 1,
            "and the dragged record is alone in it");

    // Dragging the only cell of the last row back up removes that row, so
    // rows merge without needing a gesture of their own.
    const Layout merged = ApplyDrop(split, after, 6, 0, 6);
    Require(Resolve(merged, true, true).rows == 1, "emptying a row removes it");

    // Out of range is refused, never clamped.
    Require(ApplyDrop(one_row, flat, -1, 0, 0).breaks == one_row.breaks,
            "a negative source is refused");
    Require(ApplyDrop(one_row, flat, 99, 0, 0).breaks == one_row.breaks,
            "a source past the end is refused");
    Require(ApplyDrop(one_row, flat, 0, flat.rows + 1, 0).breaks == one_row.breaks,
            "a row two bands below the last is refused");
    Require(ApplyDrop(one_row, flat, 0, 0, -1).breaks == one_row.breaks,
            "a negative column is refused");

    // The move that used to be refused -- splitting the last two-wide row into
    // a seventh, leaving a single column -- now completes. Nothing is refused
    // for the shape it would produce.
    const Layout six_rows = LayoutWithRows({2, 1, 1, 1, 1, 1});
    const auto tall = Resolve(six_rows, true, true);
    Require(WidestRow(tall) == 2, "the arrangement starts two wide");
    const Layout split_to_column = ApplyDrop(six_rows, tall, 1, tall.rows, 0);
    const auto single_column = Resolve(split_to_column, true, true);
    Require(single_column.rows == 7 && WidestRow(single_column) == 1,
            "and can be dragged the rest of the way into a single column");

    // Exhaustive: every drop from every arrangement yields something valid.
    for (const auto& rows : {std::initializer_list<int>{7},
                             std::initializer_list<int>{5, 2},
                             std::initializer_list<int>{4, 3},
                             std::initializer_list<int>{3, 2, 2},
                             std::initializer_list<int>{2, 2, 2, 1}}) {
        const Layout layout = LayoutWithRows(rows);
        const auto placed = Resolve(layout, true, true);
        for (int from = 0; from < placed.count; ++from) {
            for (int row = 0; row <= placed.rows; ++row) {
                for (int column = 0; column <= placed.count; ++column) {
                    const Layout next = ApplyDrop(layout, placed, from, row, column);
                    Require(IsValidLayout(next), "every drop yields a valid layout");
                    const auto reflowed = Resolve(next, true, true);
                    Require(reflowed.count == placed.count,
                            "no drop loses or duplicates a record");
                    Require(WidestRow(reflowed) >= kMinimumWidestRow,
                            "no drop leaves the overlay too narrow to escape");
                    int total = 0;
                    for (int r = 0; r < reflowed.rows; ++r)
                        total += reflowed.row_length[static_cast<std::size_t>(r)];
                    Require(total == reflowed.count,
                            "the rows always account for every record");
                }
            }
        }
    }
}

void TestLockAndChevronNeverOverlap() {
    using namespace gtg::tray::compact;
    for (float scale : {1.0F, 1.25F, 1.5F, 2.0F, 2.5F}) {
        for (int widest : {2, 3, 5, 6, 7}) {
            const int width = static_cast<int>(CollapsedWidth(widest) * scale);
            const int header = static_cast<int>(25 * scale);
            // Neither control may claim a point belonging to the other, and
            // neither may reach into the drag bar that moves the window.
            for (int x = 0; x < width; ++x) {
                const bool chevron = ToggleHit(x, header / 2, width, header);
                const bool lock = LockHit(x, header / 2, width, header);
                Require(!(chevron && lock), "lock and chevron are disjoint");
                if (x < width - header * 2)
                    Require(!chevron && !lock,
                            "the drag bar keeps the rest of the header");
            }
            Require(LockHit(width - header - 1, header / 2, width, header),
                    "the lock occupies the slot left of the chevron");
            Require(ToggleHit(width - 1, header / 2, width, header),
                    "the chevron keeps the corner");
            Require(!LockHit(width - header * 2 - 1, header / 2, width, header),
                    "the lock does not extend further left");
            Require(!LockHit(width - header, header, width, header),
                    "below the header belongs to the cells");
        }
    }
}

void TestCompactLayoutDrivesFootprint() {
    using namespace gtg::tray::compact;
    // The arithmetic reproduces the shipped widths rather than re-deriving
    // them: five records is 388 dip and six is 464, exactly as before.
    Require(CollapsedWidth(5) == 388, "five in the widest row is 388 dip");
    Require(CollapsedWidth(6) == 464, "six is 464 dip");
    Require(CollapsedWidth(7) == 540, "seven is 540 dip");
    Require(CollapsedWidth(2) == 160, "two is 160 dip");

    struct Case { std::initializer_list<int> rows; int width; int height; };
    // Heights are one row pitch apart, which is what keeps the margin under
    // the last cell constant. They used to be 54 apart -- a dip short -- and
    // the margin shrank with every row added.
    const Case cases[] = {
        {{7},          540, 88},
        {{5, 2},       388, 143},
        {{4, 3},       312, 143},
        {{3, 2, 2},    236, 198},
        {{2, 2, 2, 1}, 160, 253},
        {{5, 1, 1},    388, 198},
    };
    for (const auto& c : cases) {
        const auto placed = Resolve(LayoutWithRows(c.rows), true, true);
        const auto footprint = ChooseFootprint(true, placed, 2560, 1440);
        Require(footprint.width == c.width, "width follows the widest row");
        Require(footprint.height == c.height, "height follows the row count");
    }

    // Honest consequence of the model: disabling a record reflows the rows, so
    // a preference toggle can change the shape.
    const auto without_fps = ChooseFootprint(
        true, Resolve(LayoutWithRows({5, 2}), true, false), 2560, 1440);
    Require(without_fps.rows == 2 && without_fps.width == 388,
            "six records in a five-plus-two arrangement stay two rows");
}

void TestCompactLockPreferenceDefaultsToLocked() {
    // Absent means locked: a reader who has never opened the arrangement
    // cannot disturb it by accident.
    Require(gtg::settings::ResolveCompactLockPreference(false, 0),
            "an absent preference leaves the dashboard locked");
    Require(gtg::settings::ResolveCompactLockPreference(true, 1),
            "an explicit non-zero locks");
    Require(!gtg::settings::ResolveCompactLockPreference(true, 0),
            "an explicit zero unlocks");
}

void TestConfigValidation() {
    gtg::ProtectionConfig valid;
    Require(!gtg::ValidateConfig(valid).has_value(), "default config should be valid");

    // Equal was rejected at first;
    // it is now the monitor-only configuration a fresh install starts in.
    valid.safe_power_w = valid.normal_power_w;
    Require(!gtg::ValidateConfig(valid).has_value(), "equal limits are monitor-only, not invalid");
    valid.safe_power_w = valid.normal_power_w + 1;
    Require(gtg::ValidateConfig(valid).has_value(), "safe power must not exceed normal power");
    valid.safe_power_w = valid.normal_power_w - 50;
}

void TestRecoveryPolicy() {
    Require(!gtg::supervision::IsFreshSample(5000, 0), "blocked power read cannot refresh old temperature");
    Require(!gtg::supervision::IsFreshSample(1000, 1001), "future timestamp cannot be fresh");
    Require(gtg::supervision::IsFreshSample(1000, 999), "recent sample is fresh");
    gtg::supervision::RecoveryPolicy policy;
    Require(policy.NextDelayMs() == 2000, "first retry must wait two seconds");
    Require(policy.NextDelayMs() == 5000, "second failure must back off");
    Require(policy.NextDelayMs() == 10000, "third failure must back off");
    for (int i = 0; i != 100; ++i)
        Require(policy.NextDelayMs() == 30000, "repeated crashes must be rate limited");
    policy.Observe(1000, true);
    policy.Observe(60999, true);
    Require(policy.NextDelayMs() == 30000, "uptime alone cannot reset backoff");
    policy.Observe(61000, false);
    policy.Observe(62000, true);
    policy.Observe(122000, true);
    Require(policy.NextDelayMs() == 2000, "one minute healthy should reset backoff");
    gtg::supervision::Readiness readiness;
    for (int i = 0; i != 4; ++i)
        Require(!readiness.Observe(i * 200, true), "not ready after one sample");
    Require(readiness.Observe(800, true), "five continuous samples establish readiness");
    Require(!readiness.Observe(1800, true), "one-second gap invalidates readiness");
    Require(!readiness.Observe(2000, false), "invalid sample cannot be ready");

    using gtg::supervision::ChildHealthy;
    Require(ChildHealthy(2000, 1000, true, false, true), "ready with a fresh heartbeat is healthy");
    Require(!ChildHealthy(2500, 1000, true, false, true), "a stale heartbeat is not healthy");
    Require(!ChildHealthy(2000, 0, true, false, false), "no heartbeat yet is not healthy");
    Require(!ChildHealthy(2000, 1000, false, false, false),
            "neither ready nor no GPU is not healthy");
    Require(ChildHealthy(2000, 1000, false, true, false),
            "no GPU, none pinned, fresh heartbeat: healthy, not replaced");
    Require(!ChildHealthy(2500, 1000, false, true, false),
            "no GPU with a stale heartbeat is replaced");
    Require(!ChildHealthy(2000, 1000, false, true, true),
            "no GPU after one was pinned is a driver reset: replaced");
    using gtg::supervision::RelaunchAsRecovery;
    Require(!RelaunchAsRecovery(true, false), "a GPU appeared, none pinned: an ordinary start");
    Require(RelaunchAsRecovery(true, true), "a fresh start after a pin is still a recovery");
    Require(RelaunchAsRecovery(false, false), "any other replacement is a recovery");
}

void TestNvmlAbsence() {
    using gtg::nvml::Absence;
    Require(gtg::nvml::AbsenceFromLoad(126) == Absence::NoLibrary, "no nvml.dll: no NVIDIA driver");
    for (const unsigned long error : {0UL, 5UL, 193UL, 1114UL})
        Require(gtg::nvml::AbsenceFromLoad(error) == Absence::None,
                "any other load error is a failure");
    Require(gtg::nvml::AbsenceFromInit(NVML_ERROR_DRIVER_NOT_LOADED) == Absence::DriverNotLoaded,
            "driver not loaded: no GPU right now");
    for (const nvmlReturn_t result : {NVML_SUCCESS, NVML_ERROR_UNKNOWN, NVML_ERROR_NO_PERMISSION,
                                      NVML_ERROR_GPU_IS_LOST, NVML_ERROR_LIB_RM_VERSION_MISMATCH})
        Require(gtg::nvml::AbsenceFromInit(result) == Absence::None,
                "any other init answer is a failure");
    Require(gtg::nvml::AbsenceFromCount(0) == Absence::NoDevice, "no device: no GPU");
    Require(gtg::nvml::AbsenceFromCount(1) == Absence::None, "a device is there");
}

void TestGreyIconPixels() {
    std::uint32_t pixels[] = {0xFF00FF00, 0x80FFFFFF, 0x00000000, 0xFF123456};
    gtg::tray::GreyPixels(pixels);
    Require(pixels[0] == 0xFF969696, "green becomes its luminance");
    Require(pixels[1] == 0x80FFFFFF, "white stays white, its alpha kept");
    Require(pixels[2] == 0x00000000, "a transparent pixel stays transparent");
    for (const std::uint32_t pixel : pixels) {
        const auto r = (pixel >> 16) & 0xFF;
        Require(r == ((pixel >> 8) & 0xFF) && r == (pixel & 0xFF), "every channel the same grey");
    }
}

void TestHiddenWindowPosition() {
    Require(!gtg::tray::ReadWindowPosition(nullptr), "uninitialized OSD must not overwrite saved position");
    HWND window = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"STATIC", L"position test",
        WS_POPUP, 123, 234, 100, 100, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    Require(window != nullptr, "create hidden test window");
    auto point = gtg::tray::ReadWindowPosition(window);
    const bool initial = point && point->x == 123 && point->y == 234;
    SetWindowPos(window, nullptr, 345, 456, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    ShowWindow(window, SW_HIDE);
    point = gtg::tray::ReadWindowPosition(window);
    const bool moved_hidden = point && point->x == 345 && point->y == 456;
    DestroyWindow(window);
    Require(initial && moved_hidden, "hidden OSD retains last moved coordinates");
    Require(!gtg::tray::ReadWindowPosition(window), "destroyed OSD must not overwrite saved position");
}

void TestSupervisorChildLifecycle() {
    wchar_t path[32768]{};
    Require(GetModuleFileNameW(nullptr, path, 32768) != 0, "test executable path");
    Require(SetEnvironmentVariableW(L"GTG_SUPERVISION_TEST", L"clean") != FALSE, "test mode");
    const std::wstring name = L"Local\\GpuThermalGuard.TestSupervisor." + std::to_wstring(GetCurrentProcessId());
    const gtg::supervision::SupervisorOptions options{name.c_str(), 3000, false};
    Require(gtg::supervision::RunSupervisor(path, SW_HIDE, options) == 0, "clean child must stop supervisor");
    Require(SetEnvironmentVariableW(L"GTG_SUPERVISION_TEST", L"crash") != FALSE, "test mode");
    const auto start = GetTickCount64();
    Require(gtg::supervision::RunSupervisor(path, SW_HIDE, options) == 0,
            "fresh child must retain target UUID and recovery intent after abnormal exit");
    Require(GetTickCount64() - start >= 2000, "must not immediately relaunch into unstable driver");
    Require(SetEnvironmentVariableW(L"GTG_SUPERVISION_TEST", L"hang") != FALSE, "test mode");
    Require(gtg::supervision::RunSupervisor(path, SW_HIDE, options) == 0,
            "hung child must terminate before replacement");
    Require(SetEnvironmentVariableW(L"GTG_SUPERVISION_TEST", nullptr) != FALSE, "clear test mode");
}

void TestHardLimitHasNoDebounce() {
    gtg::ProtectionController controller({});
    const auto decision = controller.ObserveTemperature(0, 85);
    Require(decision.state == gtg::ProtectionState::SafeLatched, "hard limit must latch");
    Require(decision.action == gtg::ProtectionAction::ApplySafePower,
            "hard limit must request safe power immediately");
    Require(decision.trip_reason == gtg::TripReason::HardLimit,
            "hard limit reason must be retained");
}

void TestPredictiveTripRequiresTwoConfirmations() {
    gtg::ProtectionController controller({});
    (void)controller.ObserveTemperature(0, 79);
    (void)controller.ObserveTemperature(200, 80);
    (void)controller.ObserveTemperature(400, 81);
    const auto first = controller.ObserveTemperature(600, 82);
    Require(first.state == gtg::ProtectionState::PreTrip,
            "first prediction should enter PreTrip only");
    Require(first.action == gtg::ProtectionAction::None,
            "first prediction must not apply safe power");

    const auto second = controller.ObserveTemperature(800, 83);
    Require(second.state == gtg::ProtectionState::SafeLatched,
            "second prediction should latch safe mode");
    Require(second.action == gtg::ProtectionAction::ApplySafePower,
            "confirmed prediction must request safe power");
    Require(second.trip_reason == gtg::TripReason::PredictedCrossing,
            "prediction reason must be retained");
}

void TestSinglePredictionDoesNotTrip() {
    gtg::ProtectionController controller({});
    (void)controller.ObserveTemperature(0, 79);
    (void)controller.ObserveTemperature(200, 80);
    (void)controller.ObserveTemperature(400, 81);
    const auto first = controller.ObserveTemperature(600, 82);
    Require(first.state == gtg::ProtectionState::PreTrip, "expected PreTrip");

    const auto recovered = controller.ObserveTemperature(800, 80);
    Require(recovered.state == gtg::ProtectionState::Armed,
            "cleared trend should return to Armed");
    Require(!recovered.safe_latched, "one prediction must not latch safe mode");
}

void TestSchedulerDelaySafetyTripsImmediatelyInsidePredictiveBand() {
    gtg::ProtectionConfig config;
    config.trigger_temperature_c = 90;
    config.predictive_band_c = 3;
    config.scheduler_delay_fail_safe_ms = 400;
    gtg::ProtectionController controller(config);

    const auto decision = controller.ObserveTemperature(1'000, 87, 500);
    Require(decision.action == gtg::ProtectionAction::ApplySafePower,
            "a long scheduler blackout inside the predictive band must trip immediately");
    Require(decision.trip_reason == gtg::TripReason::SchedulerDelaySafety,
            "scheduler blackout protection must have a distinct diagnostic reason");
}

void TestSchedulerDelaySafetyRespectsDelayAndTemperatureGuards() {
    gtg::ProtectionConfig config;
    config.trigger_temperature_c = 90;
    config.predictive_band_c = 3;
    config.scheduler_delay_fail_safe_ms = 400;

    gtg::ProtectionController below_band(config);
    const auto cool = below_band.ObserveTemperature(1'000, 86, 1'800);
    Require(cool.action == gtg::ProtectionAction::None,
            "scheduler delay alone must not trip a cool GPU");

    gtg::ProtectionController short_delay(config);
    const auto punctual = short_delay.ObserveTemperature(1'000, 87, 399);
    Require(punctual.action == gtg::ProtectionAction::None,
            "normal wake jitter must retain the predictive confirmation policy");

    gtg::ProtectionController hard_limit(config);
    const auto hot = hard_limit.ObserveTemperature(1'000, 90, 500);
    Require(hot.trip_reason == gtg::TripReason::HardLimit,
            "the hard temperature limit must remain the highest-precedence reason");
}

void TestCoolingAloneDoesNotRestoreWithoutPolicyAction() {
    gtg::ProtectionController controller({});
    (void)controller.ObserveTemperature(0, 85);
    const auto cooled = controller.ObserveTemperature(1'000, 60);
    Require(cooled.state == gtg::ProtectionState::SafeLatched,
            "cooling alone must keep safe latch");
    Require(cooled.action == gtg::ProtectionAction::None,
            "cooling must not automatically request normal power");
}

void TestCoolingRequiresContinuousObservations() {
    gtg::ProtectionConfig config;
    config.recovery_stable_ms = 1'000;
    gtg::ProtectionController controller(config);
    (void)controller.ObserveTemperature(0, 85);
    (void)controller.ObserveTemperature(200, 60);
    (void)controller.SensorUnavailable(400);
    auto decision = controller.ObserveTemperature(2'000, 60);
    Require(decision.state == gtg::ProtectionState::SafeLatched,
            "missing observations must not satisfy stable cooling");
    for (int t = 2'200; t <= 3'000; t += 200) {
        decision = controller.ObserveTemperature(t, 60);
    }
    Require(decision.state == gtg::ProtectionState::ReadyToRestore,
            "a fresh uninterrupted cooling interval must still allow restore");
    decision = controller.ObserveTemperature(5'000, 60);
    Require(decision.state == gtg::ProtectionState::SafeLatched,
            "a scheduler blackout must revoke ReadyToRestore");
}

void TestRestoreRejectsStaleReadySample() {
    gtg::ProtectionConfig config;
    config.recovery_stable_ms = 1'000;
    gtg::ProtectionController controller(config);
    (void)controller.ObserveTemperature(0, 85);
    for (int t = 200; t <= 1'200; t += 200) {
        (void)controller.ObserveTemperature(t, 60);
    }
    const auto decision = controller.RequestRestore(5'000, 60);
    Require(decision.action != gtg::ProtectionAction::ApplyNormalPower,
            "manual or automatic restore must reject stale ReadyToRestore evidence");
}

void TestVerifiedRestoreChecksBeforeWriting() {
    gtg::ProtectionConfig config;
    config.recovery_stable_ms = 1'000;
    gtg::ProtectionController controller(config);
    (void)controller.ObserveTemperature(0, 85);
    for (int t = 200; t <= 1'200; t += 200) (void)controller.ObserveTemperature(t, 60);
    int writes = 0;
    const auto stale = gtg::RestoreWithVerification(controller, 5'000, 60, [&] {
        ++writes; return true;
    });
    Require(writes == 0 && !stale.attempted && controller.safe_latched(),
            "delayed restore must not call the normal-power setter");
    for (int t = 5'000; t <= 6'000; t += 200) (void)controller.ObserveTemperature(t, 60);
    const auto failed = gtg::RestoreWithVerification(controller, 6'001, 60, [&] {
        ++writes; return false;
    });
    Require(failed.attempted && !failed.verified && controller.safe_latched(),
            "failed verification must not commit controller/latch restore");
    const auto success = gtg::RestoreWithVerification(controller, 6'002, 60, [&] {
        ++writes; return true;
    });
    Require(success.verified && !controller.safe_latched() && writes == 2,
            "only verified restore may commit the armed state");
    Require(controller.ObserveTemperature(6'003, 90).action == gtg::ProtectionAction::ApplySafePower,
            "verified restore must have no protection grace period");
}

void TestRestoreFailureReassertsSafePower() {
    gtg::ProtectionConfig config;
    config.recovery_stable_ms = 1'000;
    for (bool fallback_ok : {false, true}) {
        gtg::ProtectionController controller(config);
        (void)controller.ObserveTemperature(0, 85);
        for (int t = 200; t <= 1'200; t += 200) (void)controller.ObserveTemperature(t, 60);
        bool safe_verified = true;
        int normal_calls = 0;
        int safe_calls = 0;
        const auto result = gtg::RestoreWithSafeFallback(controller, 1'201, 60,
            safe_verified, [&] {
                ++normal_calls;
                Require(!safe_verified, "old safe verification must be invalid before normal write");
                return false;  // Hardware write may succeed while readback fails.
            }, [&] {
                ++safe_calls;
                Require(!safe_verified, "safe fallback must begin unverified");
                return fallback_ok;
            });
        Require(result.attempted && !result.verified && controller.safe_latched(),
                "failed restore must keep logical latch even after successful fallback");
        Require(normal_calls == 1 && safe_calls == 1 && safe_verified == fallback_ok,
                "failed restore must immediately reassert safe power exactly once");
        const auto success = gtg::RestoreWithSafeFallback(controller, 1'202, 60,
            safe_verified, [&] { ++normal_calls; return true; },
            [&] { ++safe_calls; return true; });
        Require(success.verified && !safe_verified && !controller.safe_latched() && safe_calls == 1,
                "successful restore must rearm without fallback or old safe verification");
        Require(controller.ObserveTemperature(1'203, 90).action == gtg::ProtectionAction::ApplySafePower,
                "fallback policy must not add a post-restore grace period");
        const auto stale = gtg::RestoreWithSafeFallback(controller, 5'000, 60,
            safe_verified, [&] { ++normal_calls; return true; },
            [&] { ++safe_calls; return true; });
        Require(!stale.attempted && normal_calls == 2 && safe_calls == 1,
                "ineligible restore must not write either power limit");
    }
}

void TestRestoreRetryBudget() {
    gtg::RestoreRetryBudget budget;
    budget.ObserveLatch(true);
    Require(budget.CanAttempt(0), "first attempt must be allowed including at zero time");
    budget.BeginAttempt(0);
    budget.ObserveLatch(true);
    Require(!budget.CanAttempt(4'999), "repeated ready samples must not reset five-second delay");
    Require(budget.CanAttempt(5'000), "retry becomes eligible after five seconds");
    budget.BeginAttempt(5'000);
    budget.BeginAttempt(10'000);
    Require(budget.exhausted() && !budget.CanAttempt(100'000), "three attempts must exhaust automatic budget");
    const auto episode = budget.episode();
    budget.ObserveLatch(true);
    Require(budget.exhausted(), "same latched episode must not reset exhaustion");
    budget.ResetForManualRequest();
    Require(budget.CanAttempt(100'000) && budget.attempts() == 0,
            "explicit manual request may reset budget");
    budget.BeginAttempt(100'000);
    budget.StopRetrying();
    Require(!budget.CanAttempt(200'000), "permanent error must stop automatic retries immediately");
    budget.ObserveLatch(false);
    budget.ObserveLatch(true);
    Require(budget.CanAttempt(200'000) && budget.episode() == episode + 1,
            "a new real latch episode must rearm budget");
}

void TestPowerOperationRecheck() {
    for (bool mismatch : {false, true}) {
        int writes = 0, reads = 0;
        std::int64_t clock = 0;
        const auto result = gtg::nvml::ExecutePowerOperation(350'000,
            [&] { ++writes; return NVML_SUCCESS; },
            [&](unsigned int* value) {
                ++reads;
                *value = reads == 1 ? 300'000U : 350'000U;
                return reads == 1 && !mismatch ? NVML_ERROR_UNKNOWN : NVML_SUCCESS;
            }, [](nvmlReturn_t rc) { return std::to_string(rc); }, [&] { return ++clock; });
        Require(result.verified && result.verified_on_recheck && reads == 2 && writes == 1,
                "one read-only recheck must confirm transient failure without rewriting");
        Require(result.setter && result.readback && result.recheck &&
                    result.readback->duration_us > 0 && result.recheck->actual_mw == 350'000,
                "both original read and recheck evidence must be retained");
        Require(mismatch ? result.readback->actual_mw == 300'000 : !result.readback->actual_mw,
                "read error must not fabricate an observed value");
    }
    for (auto code : {NVML_ERROR_NO_PERMISSION, NVML_ERROR_GPU_IS_LOST, NVML_ERROR_UNKNOWN}) {
        int reads = 0;
        const auto result = gtg::nvml::ExecutePowerOperation(350'000,
            [&] { return code; }, [&](unsigned int*) { ++reads; return NVML_SUCCESS; },
            [](nvmlReturn_t rc) { return std::to_string(rc); }, [] { return 0; });
        Require(!result.verified && !result.readback && reads == 0,
                "failed setter must preserve error and no fabricated read stage");
        Require(result.retryable == (code == NVML_ERROR_UNKNOWN),
                "permanent and lost-device errors must not cause blind normal writes");
    }
    int reads = 0;
    const auto failed = gtg::nvml::ExecutePowerOperation(350'000,
        [] { return NVML_SUCCESS; }, [&](unsigned int* value) {
            ++reads; *value = 300'000; return NVML_SUCCESS;
        }, [](nvmlReturn_t rc) { return std::to_string(rc); }, [] { return 0; });
    Require(!failed.verified && failed.retryable && reads == 2,
            "persistent mismatch must fail after exactly two reads");
    reads = 0;
    const auto lost = gtg::nvml::ExecutePowerOperation(350'000,
        [] { return NVML_SUCCESS; }, [&](unsigned int*) { ++reads; return NVML_ERROR_GPU_IS_LOST; },
        [](nvmlReturn_t rc) { return std::to_string(rc); }, [] { return 0; });
    Require(!lost.verified && !lost.retryable && reads == 1 && !lost.recheck,
            "lost GPU read must not cause blind rechecks");
    const auto line = gtg::nvml::FormatPowerOperationResult(failed, L"test-normal");
    Require(line.find(L"requested_mw=350000") != std::wstring::npos &&
                line.find(L"actual_mw=300000") != std::wstring::npos &&
                line.find(L"outcome=unverified") != std::wstring::npos,
            "diagnostic must retain target, observed mismatch and unverified result");
    const auto absent = gtg::nvml::FormatPowerOperationResult({}, L"test-precondition");
    Require(absent.find(L"setter=not-called") != std::wstring::npos &&
                absent.find(L"readback=not-called") != std::wstring::npos,
            "uncalled APIs must not be logged as success");
}

void TestRestoreRequiresStableCoolingAndExplicitRequest() {
    gtg::ProtectionController controller({});
    (void)controller.ObserveTemperature(0, 85);
    (void)controller.ObserveTemperature(1'000, 75);

    const auto too_early = controller.RequestRestore(20'000, 75);
    Require(too_early.action == gtg::ProtectionAction::None,
            "restore must be rejected before stable duration");

    for (int t = 1'200; t < 31'000; t += 200) {
        (void)controller.ObserveTemperature(t, 75);
    }
    const auto ready = controller.ObserveTemperature(31'000, 75);
    Require(ready.state == gtg::ProtectionState::ReadyToRestore,
            "stable cooling should make restore eligible");
    Require(ready.safe_latched, "ready state must still be latched");

    const auto restored = controller.RequestRestore(31'001, 75);
    Require(restored.state == gtg::ProtectionState::Armed,
            "explicit restore should return to Armed");
    Require(restored.action == gtg::ProtectionAction::ApplyNormalPower,
            "explicit restore should request normal power");
    Require(!restored.safe_latched, "explicit restore should clear latch");
}

void TestAutomaticRestoreRequiresReadyState() {
    Require(!gtg::ShouldAutomaticallyRestore(false, gtg::ProtectionState::ReadyToRestore),
            "manual mode must never select automatic restore");
    Require(!gtg::ShouldAutomaticallyRestore(true, gtg::ProtectionState::SafeLatched),
            "automatic mode must wait for ReadyToRestore");
    Require(gtg::ShouldAutomaticallyRestore(true, gtg::ProtectionState::ReadyToRestore),
            "automatic mode may restore only from ReadyToRestore");

    gtg::ProtectionController controller({});
    (void)controller.ObserveTemperature(0, 85);
    (void)controller.ObserveTemperature(1'000, 75);

    const auto too_early = controller.RequestRestore(20'000, 75);
    Require(too_early.action == gtg::ProtectionAction::None,
            "automatic restore must use the stable-cooling gate");

    for (int t = 1'200; t <= 31'000; t += 200) {
        (void)controller.ObserveTemperature(t, 75);
    }
    const auto restored = controller.RequestRestore(31'001, 75);
    Require(restored.action == gtg::ProtectionAction::ApplyNormalPower,
            "automatic restore may request normal power only when ready");
}

void TestAutomaticRestoreImmediatelyRearmsProtection() {
    gtg::ProtectionController controller({});
    (void)controller.ObserveTemperature(0, 85);
    (void)controller.ObserveTemperature(1'000, 75);
    for (int t = 1'200; t <= 31'000; t += 200) {
        (void)controller.ObserveTemperature(t, 75);
    }
    const auto restored = controller.RequestRestore(31'001, 75);
    Require(restored.state == gtg::ProtectionState::Armed,
            "successful restore must rearm protection");

    const auto retrip = controller.ObserveTemperature(31'201, 90);
    Require(retrip.state == gtg::ProtectionState::SafeLatched,
            "first dangerous sample after restore must relatch");
    Require(retrip.action == gtg::ProtectionAction::ApplySafePower,
            "first dangerous sample after restore must request safe power");
}

void TestTestRunCounterUsesLifetimeBaseline() {
    Require(gtg::TestRunTriggerCount(12, 9) == 3,
            "test-run count must subtract its lifetime baseline");
    Require(gtg::TestRunTriggerCount(12, 12) == 0,
            "a newly reset test run must begin at zero");
    Require(gtg::TestRunTriggerCount(2, 9) == 2,
            "an invalid stale baseline must not underflow the displayed count");
    Require(gtg::HasNewTriggerCount(4, 5),
            "a Service counter increase must identify a new trip between polls");
    Require(!gtg::HasNewTriggerCount(5, 5),
            "an unchanged Service counter must not duplicate snapshots");
    Require(!gtg::HasNewTriggerCount(5, 0),
            "a manual counter reset must not look like a new trip");
}

void TestDeadlineAdvanceDoesNotDrift() {
    const auto on_time = gtg::timing::AdvanceDeadline(1'000, 1'050, 200);
    Require(on_time.next_deadline_ms == 1'200,
            "on-time work must advance from the prior deadline");
    Require(on_time.missed_periods == 0, "on-time work must not report a miss");

    const auto overrun = gtg::timing::AdvanceDeadline(1'000, 1'650, 200);
    Require(overrun.next_deadline_ms == 1'800,
            "overrun must select the next future cadence boundary");
    Require(overrun.missed_periods == 3,
            "overrun must count elapsed cadence boundaries without catch-up bursts");
}

void TestCadenceMissesSeparateWakeFromExecution() {
    const auto early_signal = gtg::timing::ClassifyCadence(1'000, 900, 920, 200);
    Require(early_signal.next_deadline_ticks == 1'000,
            "an early command wake must preserve the pending sample deadline");
    Require(early_signal.total_missed_periods == 0,
            "an early command wake must not report or create a cadence miss");

    const auto wake_late = gtg::timing::ClassifyCadence(1'000, 1'650, 1'660, 200);
    Require(wake_late.next_deadline_ticks == 1'800,
            "late wake must advance to the next future boundary");
    Require(wake_late.wake_missed_periods == 3,
            "scheduler delay must be classified as wake misses");
    Require(wake_late.execution_overrun_periods == 0,
            "short execution after a late wake must not be blamed for the miss");

    const auto execution_late = gtg::timing::ClassifyCadence(1'000, 1'050, 1'650, 200);
    Require(execution_late.wake_missed_periods == 0,
            "on-time wake must not report a scheduler miss");
    Require(execution_late.execution_overrun_periods == 3,
            "post-wake blocking must be classified as execution overrun");
    Require(execution_late.total_missed_periods == 3,
            "classified misses must preserve the total skipped cadence");

    const auto mixed = gtg::timing::ClassifyCadence(1'000, 1'450, 1'850, 200);
    Require(mixed.wake_missed_periods == 2 &&
                mixed.execution_overrun_periods == 2 &&
                mixed.total_missed_periods == 4,
            "mixed scheduler and execution delay must be accounted independently");
}

void TestDeferredLogQueueIsBoundedAndFifo() {
    gtg::logging::DeferredQueue<2, 16> queue;
    Require(queue.TryPush(gtg::logging::DeferredLevel::Info, L"first"),
            "first deferred log must fit");
    Require(queue.TryPush(gtg::logging::DeferredLevel::Warning, L"second"),
            "second deferred log must fill the bounded queue");
    Require(!queue.TryPush(gtg::logging::DeferredLevel::Error, L"overflow"),
            "full logging queue must drop instead of blocking");
    Require(queue.Dropped() == 1, "queue must expose dropped diagnostics");

    gtg::logging::DeferredRecord<16> record;
    Require(queue.TryPop(record) && record.level == gtg::logging::DeferredLevel::Info &&
                record.View() == L"first",
            "deferred queue must preserve FIFO order");
    Require(queue.TryPush(gtg::logging::DeferredLevel::Error, L"third"),
            "consumer progress must immediately free bounded capacity");
    Require(queue.TryPop(record) && record.View() == L"second",
            "second queued record must remain ordered");
    Require(queue.TryPop(record) && record.level == gtg::logging::DeferredLevel::Error &&
                record.View() == L"third",
            "new record must follow records that were already queued");
    Require(!queue.TryPop(record), "empty deferred queue must not block");
}

void TestDeferredLogConcurrentProducers() {
    gtg::logging::DeferredQueue<64, 1024> queue;
    std::atomic<bool> start{false};
    std::atomic<int> finished{0};
    std::atomic<int> accepted{0};
    auto produce = [&](wchar_t character) {
        const std::wstring payload(1000, character);
        while (!start.load()) std::this_thread::yield();
        for (int i = 0; i < 20'000; ++i) {
            if (queue.TryPush(gtg::logging::DeferredLevel::Info, payload)) ++accepted;
        }
        ++finished;
    };
    std::thread first(produce, L'A');
    std::thread second(produce, L'B');
    start = true;
    int consumed = 0;
    bool intact = true;
    gtg::logging::DeferredRecord<1024> record;
    while (finished.load() != 2 || !queue.Empty()) {
        if (queue.TryPop(record)) {
            ++consumed;
            intact = intact && record.length == 1000;
            for (std::size_t i = 0; i < std::min(record.length, record.text.size()); ++i) {
                intact = intact && record.text[i] == record.text[0];
            }
        } else {
            std::this_thread::yield();
        }
    }
    first.join();
    second.join();
    Require(intact, "concurrent producers must not tear log records");
    Require(consumed == accepted.load(), "every accepted record must be consumed once");
    Require(static_cast<std::uint64_t>(accepted.load()) + queue.Dropped() == 40'000,
            "all nonaccepted records must be counted as dropped");
}

void TestServiceReplyDoesNotWaitForClient() {
    const auto name = std::wstring(L"\\\\.\\pipe\\GTG-transport-test-") +
        std::to_wstring(GetCurrentProcessId());
    // Unique test-only endpoint. Production always uses SYSTEM/Admin ACL.
    gtg::service::PipeServer server(name.c_str(), L"D:P(A;;GA;;;WD)");
    (void)server.Poll();
    struct ClientHandle {
        HANDLE value{INVALID_HANDLE_VALUE};
        ~ClientHandle() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); }
    } client;
    client.value = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0,
        nullptr, OPEN_EXISTING, 0, nullptr);
    Require(client.value != INVALID_HANDLE_VALUE, "test pipe must accept a local client");
    DWORD mode = PIPE_READMODE_MESSAGE | PIPE_NOWAIT;
    Require(SetNamedPipeHandleState(client.value, &mode, nullptr, nullptr) != FALSE,
            "test client must use nonblocking reads");
    gtg::ipc::Request request{};
    DWORD written = 0;
    Require(WriteFile(client.value, &request, sizeof(request), &written, nullptr) &&
                written == sizeof(request), "client request must be written");
    Require(server.Poll().has_value(), "server must receive request");
    gtg::ipc::Response response{};
    response.trigger_count = 123;
    // A flushing implementation would hang here: no concurrent client reader.
    server.Reply(response);
    Require(!server.Poll().has_value(), "reply phase must not repeat command execution");
    gtg::ipc::Response received{};
    DWORD read = 0;
    Require(ReadFile(client.value, &received, sizeof(received), &read, nullptr) &&
                read == sizeof(received) && received.trigger_count == 123,
            "reply must remain available until client reads it");
    CloseHandle(client.value);
    client.value = INVALID_HANDLE_VALUE;
    (void)server.Poll();
    (void)server.Poll();
    client.value = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0,
        nullptr, OPEN_EXISTING, 0, nullptr);
    Require(client.value != INVALID_HANDLE_VALUE, "server must accept the next client");
    (void)server.Poll();
    Sleep(2'100);
    (void)server.Poll();
    DWORD available = 0;
    Require(!PeekNamedPipe(client.value, nullptr, 0, nullptr, &available, nullptr),
            "silent client must be disconnected after the bounded lease");
}

void TestLoggerRotatesDuringRuntime() {
    const auto directory = std::filesystem::temp_directory_path() /
        (L"GTG-log-test-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
         std::to_wstring(GetTickCount64()));
    Require(std::filesystem::create_directory(directory), "create isolated log test directory");
    struct Cleanup {
        std::filesystem::path directory;
        ~Cleanup() {
            std::error_code error;
            for (const auto* name : {L"test.exe", L"GpuThermalGuard-tray.log",
                                      L"GpuThermalGuard-tray.log.1"}) {
                std::filesystem::remove(directory / name, error);
            }
            std::filesystem::remove(directory, error);
        }
    } cleanup{directory};
    wchar_t executable[32'768]{};
    Require(GetModuleFileNameW(nullptr, executable, 32'768) != 0, "resolve test executable");
    const auto child = directory / L"test.exe";
    std::filesystem::copy_file(executable, child);
    std::wstring command = L"\"" + child.wstring() + L"\" --logger-rotation-child";
    STARTUPINFOW startup{sizeof(startup)};
    PROCESS_INFORMATION process{};
    Require(CreateProcessW(child.c_str(), command.data(), nullptr, nullptr, FALSE,
                          CREATE_NO_WINDOW, nullptr, directory.c_str(), &startup, &process),
            "start isolated logger child without touching application logs");
    CloseHandle(process.hThread);
    const auto waited = WaitForSingleObject(process.hProcess, 15'000);
    if (waited != WAIT_OBJECT_0) {
        TerminateProcess(process.hProcess, 1);  // Only the test child we just created.
        WaitForSingleObject(process.hProcess, 5'000);
    }
    DWORD code = 1;
    GetExitCodeProcess(process.hProcess, &code);
    CloseHandle(process.hProcess);
    Require(waited == WAIT_OBJECT_0 && code == 0, "logger child must finish successfully");
    const auto current = directory / L"GpuThermalGuard-tray.log";
    const auto previous = directory / L"GpuThermalGuard-tray.log.1";
    Require(std::filesystem::exists(previous), "runtime writes must rotate before restart");
    Require(std::filesystem::file_size(previous) >= 4 * 1024 * 1024 &&
                std::filesystem::file_size(previous) < 4 * 1024 * 1024 + 2'048,
            "rotated log must remain bounded to threshold plus one record");
    Require(std::filesystem::file_size(current) > 0 &&
                std::filesystem::file_size(current) < 4 * 1024 * 1024,
            "logging must continue in the new active file");
}

void TestLatencyWindowProducesTailSummaryWithoutGrowth() {
    gtg::timing::LatencyWindow<8> window;
    for (const std::int64_t value : {1, 2, 3, 4, 5, 6, 7, 8, 100}) {
        window.Add(value);
    }

    const auto summary = window.Summary();
    Require(summary.count == 8, "fixed latency window must retain its capacity");
    Require(summary.minimum_us == 2, "oldest value must be overwritten at capacity");
    Require(summary.p50_us == 5, "p50 must use the retained sorted distribution");
    Require(summary.p95_us == 100 && summary.p99_us == 100,
            "tail percentiles must expose the retained latency spike");
    Require(summary.maximum_us == 100, "maximum must expose the worst retained sample");
}

void TestGpuRecoveryReappliesSafePower() {
    gtg::ProtectionController controller({});
    (void)controller.ObserveTemperature(0, 85);
    const auto unavailable = controller.SensorUnavailable(100);
    Require(unavailable.state == gtg::ProtectionState::GpuUnavailable,
            "sensor loss should enter GpuUnavailable");

    const auto recovered = controller.SensorRecovered();
    Require(recovered.state == gtg::ProtectionState::SafeLatched,
            "latched GPU recovery must remain safe");
    Require(recovered.action == gtg::ProtectionAction::ApplySafePower,
            "latched GPU recovery must reapply safe power");
}

void TestTelemetryLossRequiresSustainedFailure() {
    gtg::ProtectionConfig config;
    config.sensor_unavailable_fail_safe_ms = 1'000;
    gtg::ProtectionController controller(config);

    const auto first = controller.SensorUnavailable(10'000);
    Require(first.state == gtg::ProtectionState::GpuUnavailable,
            "first failed read must report unavailable");
    Require(first.action == gtg::ProtectionAction::None,
            "one failed read must not change power");

    const auto early = controller.SensorUnavailable(10'999);
    Require(early.action == gtg::ProtectionAction::None,
            "telemetry loss must respect the timeout boundary");

    const auto timed_out = controller.SensorUnavailable(11'000);
    Require(timed_out.state == gtg::ProtectionState::SafeLatched,
            "sustained telemetry loss must request a safe latch");
    Require(timed_out.action == gtg::ProtectionAction::ApplySafePower,
            "sustained telemetry loss must request safe power");
    Require(timed_out.trip_reason == gtg::TripReason::TelemetryLossSafety,
            "telemetry loss must have a distinct diagnostic reason");
}

void TestHealthySampleResetsTelemetryLossTimer() {
    gtg::ProtectionConfig config;
    config.sensor_unavailable_fail_safe_ms = 1'000;
    gtg::ProtectionController controller(config);

    (void)controller.SensorUnavailable(1'000);
    (void)controller.SensorRecovered();
    (void)controller.ObserveTemperature(1'500, 50);
    const auto restarted = controller.SensorUnavailable(1'700);
    const auto still_early = controller.SensorUnavailable(2'499);

    Require(restarted.action == gtg::ProtectionAction::None,
            "a new loss episode must start a new timeout");
    Require(still_early.action == gtg::ProtectionAction::None,
            "a recovered stream must not retain the old loss deadline");
}

void TestPersistedLatchStartsSafe() {
    gtg::ProtectionController controller({});
    const auto restored = controller.RestorePersistedSafeLatch();
    Require(restored.state == gtg::ProtectionState::SafeLatched,
            "persisted latch must start in SafeLatched");
    Require(restored.safe_latched, "persisted latch must remain set");
    Require(restored.action == gtg::ProtectionAction::ApplySafePower,
            "persisted latch must request safe power immediately");
}

void TestWtlHeaders() {
    Require(WtlHeadersAvailable(), "WTL 10.01 headers should compile with local ATL");
}

void TestTelemetryHistoryRetainsOneHour() {
    gtg::telemetry::History history;
    history.AddSample({0, 40.0, 30.0, std::nullopt, std::nullopt, 1.0, 2.0});
    history.AddSample({3'600'000, 50.0, 40.0, std::nullopt, std::nullopt, 3.0, 4.0});
    history.AddSample({3'600'001, 60.0, 50.0, std::nullopt, std::nullopt, 5.0, 6.0});

    Require(history.Samples().size() == 2, "history must evict samples older than one hour");
    Require(history.Samples().front().monotonic_ms == 3'600'000,
            "one-hour boundary sample must be retained");
}

void TestTelemetryThirtySecondWindowRollsForward() {
    gtg::telemetry::History history;
    history.AddSample({1'000, 40.0, 30.0, std::nullopt, std::nullopt, 1.0, 2.0});
    history.AddSample({30'999, 50.0, 40.0, std::nullopt, std::nullopt, 3.0, 4.0});
    history.AddSample({31'001, 60.0, 50.0, std::nullopt, std::nullopt, 5.0, 6.0});

    Require(history.CountInRange(1'001, 31'001) == 2,
            "rolling view must exclude the sample just outside 30 seconds");
    Require(history.Latest() != nullptr && history.Latest()->temperature_c == 60.0,
            "latest sample must advance with the live window");
}

void TestTelemetryPreservesUnavailableMetrics() {
    gtg::telemetry::History history;
    history.AddSample({100, std::nullopt, 42.0, std::nullopt, std::nullopt,
                       std::nullopt, 7.0});

    const auto* sample = history.Latest();
    Require(sample != nullptr, "sample must be retained");
    Require(!sample->temperature_c.has_value(), "missing temperature must remain missing");
    Require(!sample->gpu_utilization_percent.has_value(),
            "missing GPU utilization must remain missing");
    Require(sample->power_w == 42.0, "available power must be retained independently");
}

void TestTelemetryPreservesVramCapacityUsage() {
    gtg::telemetry::History history;
    history.AddSample({100, 50.0, 100.0, 78.125, 75.0, 90.0, 20.0});

    const auto* sample = history.Latest();
    Require(sample != nullptr, "VRAM sample must be retained");
    Require(sample->vram_utilization_percent == 78.125,
            "VRAM capacity percentage must remain independent from GPU loading");
    Require(sample->vram_used_gib == 75.0, "VRAM used GiB must be retained for badges");
}

void TestPresentationFreshnessUsesLatestValidGpuSample() {
    std::deque<gtg::telemetry::Sample> samples;
    samples.push_back({1'000, 50.0, 100.0, 10.0, 9.5, 20.0, 5.0});
    samples.push_back({2'000, std::nullopt, 42.0, std::nullopt,
                       std::nullopt, std::nullopt, 10.0});

    const auto delayed = gtg::telemetry::EvaluateFreshness(samples, 2'100);
    Require(delayed.state == gtg::telemetry::FreshnessState::Delayed,
            "a sample without temperature must not make thermal telemetry fresh");
    Require(delayed.latest_valid_ms == 1'000 && delayed.age_ms == 1'100,
            "freshness must retain the latest valid GPU timestamp");

    const auto unavailable = gtg::telemetry::EvaluateFreshness(samples, 4'000);
    Require(unavailable.state == gtg::telemetry::FreshnessState::Unavailable,
            "old GPU presentation data must become unavailable");
}

void TestPresentationFreshnessRecoversWithNewGpuSample() {
    std::deque<gtg::telemetry::Sample> samples;
    samples.push_back({1'000, 50.0, 100.0, 10.0, 9.5, 20.0, 5.0});
    samples.push_back({4'000, std::nullopt, std::nullopt, std::nullopt,
                       std::nullopt, std::nullopt, 10.0});
    samples.push_back({4'200, 51.0, 105.0, 11.0, 10.5, 22.0, 6.0});

    const auto recovered = gtg::telemetry::EvaluateFreshness(samples, 4'250);
    Require(recovered.state == gtg::telemetry::FreshnessState::Fresh,
            "a new valid GPU sample must restore fresh presentation state");
    Require(recovered.latest_valid_ms == 4'200,
            "recovery must advance the valid GPU timestamp");
}

void TestPresentationGapsIdentifyMissingIntervals() {
    std::deque<gtg::telemetry::Sample> samples;
    samples.push_back({100, 50.0, 100.0, 10.0, 9.5, 20.0, 5.0});
    samples.push_back({300, 50.0, 100.0, 10.0, 9.5, 20.0, 5.0});
    samples.push_back({1'800, 51.0, 105.0, 11.0, 10.5, 22.0, 6.0});
    samples.push_back({2'500, std::nullopt, std::nullopt, std::nullopt,
                       std::nullopt, std::nullopt, 10.0});
    samples.push_back({3'200, 52.0, 110.0, 12.0, 11.5, 24.0, 7.0});

    const auto gaps = gtg::telemetry::FindPresentationGaps(samples, 500, 3'000);
    Require(gaps.size() == 2, "two missing presentation intervals must be retained");
    Require(gaps[0].start_ms == 500 && gaps[0].end_ms == 1'800 &&
                gaps[0].duration_ms == 1'500,
            "a gap crossing the viewport edge must be clipped but retain duration");
    Require(gaps[1].start_ms == 1'800 && gaps[1].end_ms == 3'000 &&
                gaps[1].duration_ms == 1'400,
            "a gap ending outside the viewport must be clipped but retain duration");
}

void TestStandaloneLocalizationSelection() {
    using gtg::localization::UiLanguage;
    Require(gtg::localization::Current() == UiLanguage::English,
            "English must be the process default language");
    gtg::localization::SetCurrent(UiLanguage::TraditionalChinese);
    Require(gtg::localization::Select(L"正體中文", L"English") == L"正體中文",
            "Traditional Chinese selection must use its embedded text");
    gtg::localization::SetCurrent(UiLanguage::English);
    Require(gtg::localization::Select(L"正體中文", L"English") == L"English",
            "English selection must use the embedded English text");
    Require(gtg::localization::FromRegistryValue(L"en-US") == UiLanguage::English,
            "en-US preference must round-trip");
    Require(gtg::localization::FromRegistryValue(L"zh-TW") ==
                UiLanguage::TraditionalChinese,
            "zh-TW preference must round-trip");
    Require(gtg::localization::FromRegistryValue(L"invalid") == UiLanguage::English,
            "invalid preference must fail safely to English");
    gtg::localization::SetCurrent(UiLanguage::English);
}

void TestRestorePromptOccursOncePerLatchEpisode() {
    gtg::RestorePromptGate gate;
    Require(gate.ShouldPrompt(true, true), "first ready state must offer restore");
    Require(!gate.ShouldPrompt(true, false), "temperature rebound must not reset the offer");
    Require(!gate.ShouldPrompt(true, true), "same latch must not create a nested prompt");
    Require(!gate.ShouldPrompt(false, false), "unlatched state resets without prompting");
    Require(gate.ShouldPrompt(true, true), "a new latch episode may offer restore once");
}

void TestSnapshotNamesPreserveWallTimeAndReason() {
    const gtg::snapshot::WallTime time{2026, 9, 2, 14, 3, 5, 17};
    Require(gtg::snapshot::FormatDisplayTime(time) == L"2026-09-02  14:03:05",
            "display wall time must be stable and human-readable");
    Require(gtg::snapshot::FormatFileName(gtg::snapshot::Reason::ThermalTrigger, time) ==
                L"GpuThermalGuard-thermal-trigger-20260902-140305-017.png",
            "thermal snapshot filename must retain incident time and reason");
}

std::size_t CountSnapshotInk(Gdiplus::Bitmap& bitmap) {
    std::size_t count = 0;
    const Gdiplus::Color background(255, GetRValue(GetSysColor(COLOR_BTNFACE)),
        GetGValue(GetSysColor(COLOR_BTNFACE)), GetBValue(GetSysColor(COLOR_BTNFACE)));
    for (UINT y = 0; y < bitmap.GetHeight(); y += 2) {
        for (UINT x = 0; x < bitmap.GetWidth(); x += 2) {
            Gdiplus::Color pixel;
            if (bitmap.GetPixel(x, y, &pixel) == Gdiplus::Ok &&
                (pixel.GetR() != background.GetR() || pixel.GetG() != background.GetG() ||
                 pixel.GetB() != background.GetB())) {
                ++count;
            }
        }
    }
    return count;
}

void TestHiddenWindowSnapshotCanBeEncoded() {
    Gdiplus::GdiplusStartupInput input;
    ULONG_PTR token = 0;
    Require(Gdiplus::GdiplusStartup(&token, &input, nullptr) == Gdiplus::Ok,
            "GDI+ must initialize for snapshot test");

    HWND window = CreateWindowExW(WS_EX_TOOLWINDOW, L"STATIC", L"Hidden incident window",
                                  WS_OVERLAPPED | WS_CAPTION, 0, 0, 320, 180,
                                  nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    Require(window != nullptr, "hidden snapshot test window must be created");
    HWND child = CreateWindowExW(0, L"STATIC", L"GPU 88 C / 300 W",
                                 WS_CHILD | WS_VISIBLE, 12, 16, 180, 24,
                                 window, nullptr, GetModuleHandleW(nullptr), nullptr);
    Require(child != nullptr, "hidden snapshot child must be created");
    Require(IsWindowVisible(window) == FALSE, "snapshot test window must remain hidden");

    const std::filesystem::path path = std::filesystem::temp_directory_path() /
        L"GpuThermalGuard-hidden-snapshot-test.png";
    std::error_code error;
    std::filesystem::remove(path, error);
    const auto result = gtg::snapshot::CaptureWindowToPng(window, path);
    Require(static_cast<bool>(result), "hidden window must render to PNG via WM_PRINT");
    Require(std::filesystem::exists(path) && std::filesystem::file_size(path) > 128,
            "hidden window snapshot PNG must contain encoded image data");
    {
        Gdiplus::Bitmap decoded(path.c_str());
        Require(decoded.GetLastStatus() == Gdiplus::Ok && decoded.GetWidth() > 0,
                "saved hidden snapshot must be a decodable PNG");
        Require(CountSnapshotInk(decoded) > 20,
                "hidden snapshot must contain child-control pixels, not only background");
    }

    ShowWindow(window, SW_SHOWMINNOACTIVE);
    ShowWindow(window, SW_HIDE);
    Require(IsIconic(window) != FALSE, "snapshot test window must retain minimized state");
    const std::filesystem::path minimized_path = std::filesystem::temp_directory_path() /
        L"GpuThermalGuard-minimized-snapshot-test.png";
    std::filesystem::remove(minimized_path, error);
    const auto minimized_result = gtg::snapshot::CaptureWindowToPng(window, minimized_path);
    Require(static_cast<bool>(minimized_result),
            "minimized window must render its restored client area without restore");
    {
        Gdiplus::Bitmap decoded(minimized_path.c_str());
        Require(decoded.GetLastStatus() == Gdiplus::Ok && decoded.GetWidth() > 250 &&
                    decoded.GetHeight() > 100,
                "minimized snapshot must use the restored client dimensions, not the icon rect");
        Require(CountSnapshotInk(decoded) > 20,
                "minimized snapshot must contain child-control pixels, not only background");
    }

    DestroyWindow(window);
    std::filesystem::remove(path, error);
    std::filesystem::remove(minimized_path, error);
    Gdiplus::GdiplusShutdown(token);
}

void TestOsdModePositions() {
    using namespace gtg::tray::placement;
    // An older install has only OsdX/OsdY; both shapes start from it.
    const auto upgraded = FromStored(POINT{3147, 291}, std::nullopt);
    Require(upgraded.expanded && upgraded.collapsed &&
                upgraded.collapsed->x == 3147 && upgraded.collapsed->y == 291,
            "compact without its own position starts from the shared one");
    const auto both = FromStored(POINT{100, 200}, POINT{3700, 300});
    Require(both.expanded->x == 100 && both.collapsed->x == 3700,
            "each shape keeps its own stored position");
    Require(!FromStored(std::nullopt, std::nullopt).collapsed,
            "nothing stored means nothing remembered");

    // The chevron stays put: a compact column (84 px) on a right edge
    // expands to 388 px leftwards and collapses back to exactly where it was,
    // however many times it is clicked (owner, 2026-10-05).
    POINT at{3756, 291};
    for (int click = 0; click < 6; ++click) {
        const bool collapsed = click % 2 == 0;
        const int before = collapsed ? 84 : 388;
        const int after = collapsed ? 388 : 84;
        const POINT next = ToggleKeepingButton(at, before, after);
        Require(next.x + after == at.x + before && next.y == at.y,
                "the right edge and the top do not move");
        at = next;
    }
    Require(at.x == 3756 && at.y == 291, "an even number of clicks ends where it began");
}

void TestUiThemePreference() {
    using namespace gtg::settings;
    Require(UiThemeFromRegistryValue(L"dark") == UiTheme::Dark, "dark round-trips");
    Require(UiThemeFromRegistryValue(L"light") == UiTheme::Light, "light round-trips");
    Require(UiThemeFromRegistryValue(L"auto") == UiTheme::Auto, "auto round-trips");
    Require(UiThemeFromRegistryValue(L"") == UiTheme::Auto, "empty is auto");
    Require(UiThemeFromRegistryValue(L"Dark") == UiTheme::Auto &&
                UiThemeFromRegistryValue(L"sepia") == UiTheme::Auto,
            "unrecognised is auto, never a guess");
    for (const auto theme : {UiTheme::Auto, UiTheme::Dark, UiTheme::Light}) {
        Require(UiThemeFromRegistryValue(RegistryValue(theme)) == theme,
                "every value survives the registry");
    }

    // Every combination of the four inputs.
    for (const auto preference : {UiTheme::Auto, UiTheme::Dark, UiTheme::Light}) {
        for (const bool system_dark : {false, true}) {
            for (const bool supported : {false, true}) {
                for (const bool contrast : {false, true}) {
                    const auto got = ResolveAppearance(preference, system_dark, supported, contrast);
                    const bool want_dark =
                        !contrast && supported &&
                        (preference == UiTheme::Dark ||
                         (preference == UiTheme::Auto && system_dark));
                    Require(got == (want_dark ? Appearance::Dark : Appearance::Classic),
                            "appearance follows the rule for every combination");
                }
            }
        }
    }
    Require(ResolveAppearance(UiTheme::Dark, true, true, true) == Appearance::Classic,
            "high contrast wins over an explicit dark");
    Require(ResolveAppearance(UiTheme::Dark, true, false, false) == Appearance::Classic,
            "unsupported dark falls back to classic, never half-dark");
    Require(ResolveAppearance(UiTheme::Light, true, true, false) == Appearance::Classic,
            "light ignores the system app mode");
}

void TestOverlayPreferences() {
    using namespace gtg::settings;
    Require(ResolveOverlayEnabled(false, 1), "absent overlay preference defaults on without automatic injection");
    Require(ResolveOverlayEnabled(true, 1) && !ResolveOverlayEnabled(true, 0),
            "stored overlay preference is honoured");

    Require(kDefaultOverlayHotkey.modifiers == MOD_ALT && kDefaultOverlayHotkey.key == VK_F10,
            "default hotkey is Alt+F10");
    Require(IsAcceptableOverlayHotkey(kDefaultOverlayHotkey), "the default is acceptable");
    Require(!IsAcceptableOverlayHotkey({MOD_ALT, VK_F12}), "F12 is reserved for debuggers");
    Require(!IsAcceptableOverlayHotkey({0, VK_F10}), "a bare key is refused");
    Require(!IsAcceptableOverlayHotkey({MOD_ALT | MOD_NOREPEAT, VK_F10}),
            "only the four modifiers are stored");
    Require(!IsAcceptableOverlayHotkey({MOD_CONTROL, VK_SHIFT}),
            "a modifier is not a key");
    Require(!IsAcceptableOverlayHotkey({MOD_ALT, 0}), "no key is refused");
    Require(IsAcceptableOverlayHotkey({MOD_CONTROL | MOD_SHIFT, 'O'}), "Ctrl+Shift+O is fine");

    const OverlayHotkey chosen{MOD_CONTROL | MOD_ALT, VK_F9};
    Require(ResolveOverlayHotkey(true, PackOverlayHotkey(chosen)) == chosen,
            "a hotkey survives the registry");
    Require(ResolveOverlayHotkey(false, PackOverlayHotkey(chosen)) == kDefaultOverlayHotkey,
            "absent is the default");
    Require(ResolveOverlayHotkey(true, PackOverlayHotkey({MOD_ALT, VK_F12})) ==
                kDefaultOverlayHotkey,
            "a stored F12 reads as the default, not as F12");
    Require(ResolveOverlayHotkey(true, 0xFFFFFFFFU) == kDefaultOverlayHotkey,
            "garbage reads as the default");
}

void TestHotkeyCapture() {
    using namespace gtg::tray::hotkey;
    using gtg::settings::OverlayHotkey;
    Capture capture;
    Require(capture.Judge() == Verdict::Empty, "nothing pressed is empty");

    capture.KeyDown(VK_LMENU);
    Require(capture.Judge() == Verdict::Incomplete &&
                capture.Shown() == OverlayHotkey{MOD_ALT, 0},
            "a held modifier shows on its own");
    capture.KeyDown(VK_LMENU);   // auto-repeat
    capture.KeyDown(VK_F10);
    Require(capture.Judge() == Verdict::Acceptable &&
                capture.Shown() == OverlayHotkey{MOD_ALT, VK_F10},
            "Alt then F10 records Alt+F10");
    capture.KeyUp(VK_F10);
    capture.KeyUp(VK_LMENU);
    Require(capture.Shown() == OverlayHotkey{MOD_ALT, VK_F10},
            "releasing keeps the finished combination on screen");

    capture.KeyDown(VK_RCONTROL);
    Require(capture.Judge() == Verdict::Incomplete &&
                capture.Shown() == OverlayHotkey{MOD_CONTROL, 0},
            "a new modifier after a finished combination starts over");
    capture.KeyDown(VK_LSHIFT);
    capture.KeyDown('O');
    Require(capture.Shown() == OverlayHotkey{MOD_CONTROL | MOD_SHIFT, 'O'},
            "two modifiers and a letter");
    capture.KeyUp(VK_LSHIFT);
    capture.KeyUp(VK_RCONTROL);

    capture.KeyDown(VK_F9);
    Require(capture.Judge() == Verdict::NeedsModifier, "a bare key needs a modifier");
    capture.KeyDown(VK_MENU);
    capture.KeyDown(VK_F12);
    Require(capture.Judge() == Verdict::Reserved, "F12 is reserved");
    capture.KeyUp(VK_MENU);

    capture.KeyDown(VK_LWIN);
    capture.KeyDown('G');
    Require(capture.Judge() == Verdict::Acceptable &&
                capture.Shown() == OverlayHotkey{MOD_WIN, 'G'},
            "Win counts as a modifier");
    capture.KeyUp(VK_LWIN);

    capture.Show(OverlayHotkey{MOD_ALT, VK_F10});
    Require(capture.Judge() == Verdict::Acceptable && capture.Held() == 0,
            "a stored combination is shown as finished");

    Require(PassesThrough(VK_TAB, 0) && PassesThrough(VK_TAB, MOD_SHIFT),
            "Tab and Shift+Tab keep moving focus");
    Require(PassesThrough(VK_ESCAPE, 0) && PassesThrough(VK_RETURN, 0),
            "Escape and Enter keep closing the dialog");
    Require(!PassesThrough(VK_RETURN, MOD_ALT) && !PassesThrough(VK_TAB, MOD_CONTROL) &&
                !PassesThrough(VK_F10, 0),
            "with a real modifier, or any other key, it is recorded");
}

void TestTripMarkers() {
    using gtg::tray::MarkerFraction;
    using gtg::tray::TripMarkers;
    Require(MarkerFraction(1'000, 1'000, 301'000) == 0.0, "view start is 0");
    Require(MarkerFraction(301'000, 1'000, 301'000) == 1.0, "view end is 1");
    Require(MarkerFraction(151'000, 1'000, 301'000) == 0.5, "middle is 0.5");
    Require(!MarkerFraction(999, 1'000, 301'000), "before the view is not drawn");
    Require(!MarkerFraction(301'001, 1'000, 301'000), "after the view is not drawn");
    Require(!MarkerFraction(5, 10, 10), "an empty view draws nothing");

    TripMarkers markers;
    markers.Add(1'000);
    markers.Add(2'000'000);
    markers.Prune(3'700'000, 3'600'000);
    Require(markers.Times().size() == 1 && markers.Times().front() == 2'000'000,
            "a trip older than the retained hour is dropped");
    for (std::uint64_t i = 0; i < TripMarkers::kCapacity + 5; ++i) markers.Add(3'000'000 + i);
    Require(markers.Times().size() == TripMarkers::kCapacity &&
                markers.Times().back() == 3'000'000 + TripMarkers::kCapacity + 4,
            "bounded, keeping the newest");
}

void TestHistoryTimelineTrack() {
    using gtg::tray::TimelineTrack;
    constexpr std::uint64_t kHour = 3'600'000;
    constexpr std::uint64_t kView = 300'000;
    const auto same = [](const double a, const double b) {
        return std::abs(a - b) < 1e-9;
    };

    // Long after boot: the track is the hour less one view, newest at the
    // right, and the thumb is one twelfth of it -- as it has always been.
    const auto up = TimelineTrack::For(18'000'000, kHour, kView);
    Require(up.earliest_end == 18'000'000 - (kHour - kView) && up.latest_end == 18'000'000,
            "the track spans 55 minutes ending now");
    Require(same(up.ThumbShare(), 1.0 / 12.0), "the thumb is one view against the hour");
    Require(up.FractionOf(18'000'000) == 1.0 && up.ViewEndAt(1.0) == 18'000'000,
            "the right end is live");
    Require(up.ViewEndAt(0.0) == 18'000'000 - (kHour - kView), "the left end is 55 minutes back");
    Require(same(up.FractionOf(up.ViewEndAt(0.25)), 0.25), "fraction and view end round-trip");
    Require(up.FractionOf(0) == 0.0 && up.FractionOf(99'000'000) == 1.0,
            "a view end outside the track is drawn at the nearer end");

    // Ten minutes after boot. The arithmetic this replaces divided by 55
    // minutes regardless, which put the live thumb at 600'000 / 3'300'000 =
    // 18 % of the track and pulled every drag to its right back to it.
    const auto booted = TimelineTrack::For(600'000, kHour, kView);
    Require(booted.FractionOf(600'000) == 1.0, "following live, the thumb is at the right end");
    Require(booted.ViewEndAt(1.0) == 600'000 && booted.ViewEndAt(0.0) == 0,
            "the whole track is draggable, from boot to now");
    Require(same(booted.FractionOf(300'000), 0.5), "halfway along the track is halfway to boot");
    Require(same(booted.ThumbShare(), up.ThumbShare()),
            "the thumb keeps its size early after boot");
    const auto just_booted = TimelineTrack::For(2'000, kHour, kView);
    Require(just_booted.FractionOf(2'000) == 1.0 && just_booted.ViewEndAt(0.0) == 0,
            "two seconds after boot the track still ends at now");
}

void TestOsdIdleClock() {
    using namespace gtg::tray::compact;
    Require(!HeaderIdle(true, false, 10'000, 0), "pointer over the window is never idle");
    Require(!HeaderIdle(false, false, 11'999, 10'000), "away under two seconds is not idle");
    Require(HeaderIdle(false, false, 12'000, 10'000), "away two seconds is idle");
    Require(!HeaderIdle(false, true, 60'000, 10'000),
            "unlocked is never idle: the amber lock stays visible");
    Require(!HeaderIdle(false, false, 5, 10), "a clock that went backwards is not idle");
    const auto text = [](const std::array<wchar_t, 9>& clock) { return std::wstring(clock.data()); };
    Require(text(FormatWallClock(0, 0, 0)) == L"00:00:00", "midnight");
    Require(text(FormatWallClock(9, 5, 7)) == L"09:05:07", "leading zeros");
    Require(text(FormatWallClock(23, 59, 59)) == L"23:59:59", "last second of the day");
}

void TestOsdSnap() {
    using gtg::tray::placement::Snap;
    // Two 1920 monitors side by side, a taskbar on the left one.
    const std::array<RECT, 2> areas{RECT{0, 0, 1920, 1040}, RECT{1920, 0, 3840, 1080}};
    const auto window = [](const int x, const int y) {
        return RECT{x, y, x + 300, y + 100};
    };
    const auto same = [](const RECT& a, const RECT& b) {
        return a.left == b.left && a.top == b.top && a.right == b.right &&
               a.bottom == b.bottom;
    };
    Require(same(Snap(window(500, 500), areas, 12), window(500, 500)),
            "far from every edge nothing moves");
    Require(same(Snap(window(8, 500), areas, 12), window(0, 500)),
            "left work-area edge pulls");
    Require(same(Snap(window(13, 500), areas, 12), window(13, 500)),
            "beyond the threshold it lets go");
    Require(same(Snap(window(3530, 500), areas, 12), window(3540, 500)),
            "right edge pulls the window's right side");
    Require(same(Snap(window(500, 935), areas, 12), window(500, 940)),
            "taskbar top is a work-area edge");
    Require(same(Snap(window(1615, 500), areas, 12), window(1620, 500)),
            "monitor seam pulls from the left side");
    Require(same(Snap(window(1926, 500), areas, 12), window(1920, 500)),
            "monitor seam pulls from the right side");
    Require(same(Snap(window(8, 4), areas, 12), window(0, 0)),
            "a corner snaps on both axes");
    // A line beside the window, not alongside it, is not a wall: the second
    // monitor's bottom (1080) is 5 px from this window's bottom but no edge
    // for a window on the first monitor.
    Require(same(Snap(window(500, 975), areas, 12), window(500, 975)),
            "only lines alongside the window count");
    Require(same(Snap(window(8, 500), areas, 0), window(8, 500)),
            "a zero threshold disables snapping");

    // Work areas and monitor bounds together: the taskbar's top and the
    // screen's bottom are both edges (owner, 2026-10-05).
    const std::array<RECT, 2> both{RECT{0, 0, 1920, 1040}, RECT{0, 0, 1920, 1080}};
    Require(same(Snap(window(500, 935), both, 12), window(500, 940)),
            "near the taskbar it lands on the taskbar's top");
    Require(same(Snap(window(500, 975), both, 12), window(500, 980)),
            "near the screen's bottom it lands on the screen's bottom");

    // The 2026-10-04 defect, reproduced. Windows' move loop hands WM_MOVING the
    // previous rectangle plus the mouse's movement since; snapping that
    // rectangle can never be dragged off an edge. Drag right in 4 px steps from
    // 30 px off the left edge, 60 steps: the cursor ends 210 px off the edge.
    using gtg::tray::placement::FromCursor;
    RECT incremental = window(30, 500);
    for (int step = 0; step < 60; ++step) {
        const int move = step < 10 ? -4 : 4;   // in to the edge, then away
        OffsetRect(&incremental, move, 0);
        incremental = Snap(incremental, areas, 12);
    }
    Require(incremental.left == 0,
            "incremental snapping is stuck at the edge (the defect this guards)");

    const POINT grab{40, 10};
    POINT cursor{30 + grab.x, 500 + grab.y};
    RECT anchored{};
    bool touched = false;
    for (int step = 0; step < 60; ++step) {
        cursor.x += step < 10 ? -4 : 4;
        anchored = Snap(FromCursor(cursor, grab, SIZE{300, 100}), areas, 12);
        touched = touched || anchored.left == 0;
    }
    Require(touched, "anchored snapping still pulls to the edge on the way in");
    Require(anchored.left == 30 - 40 + 200 && anchored.left == cursor.x - grab.x,
            "anchored snapping lets go and follows the cursor on the way out");
}

// --- top-app alert ----------------------------------------------------------

// The sampler's cost guard: charged with the sweep's own CPU time, doubling the
// interval over budget, pausing a minute after five overruns and trying again
// -- never stopping for as long as an alert lasts.
void TestTopAppSweepBudget() {
    using gtg::topapp::SweepBudget;
    SweepBudget budget;
    auto next = budget.Charge(20.0);
    Require(!next.paused && next.wait_ms == 1'000, "a sweep within budget keeps the cadence");
    Require(!budget.Charge(SweepBudget::kBudgetMs).paused, "the budget itself is within budget");

    const std::uint32_t backed_off[]{2'000, 4'000, 8'000, 8'000};
    for (const std::uint32_t wait : backed_off) {
        next = budget.Charge(60.0);
        Require(!next.paused && next.wait_ms == wait, "an overrun doubles the interval up to 8 s");
    }
    next = budget.Charge(60.0);
    Require(next.paused && next.first_pause && next.wait_ms == SweepBudget::kPauseMs,
            "the fifth overrun in a row pauses for a minute");

    next = budget.Charge(60.0);
    Require(!next.paused && next.wait_ms == 2'000 && !next.resumed,
            "after the pause it tries again from the start of the run");
    next = budget.Charge(20.0);
    Require(next.resumed && next.wait_ms == 1'000, "the first sweep within budget says so");
    Require(!budget.Charge(20.0).resumed, "and only the first");

    for (int i = 0; i < SweepBudget::kOverLimit - 1; ++i)
        (void)budget.Charge(60.0);
    next = budget.Charge(60.0);
    Require(next.paused && !next.first_pause,
            "a later pause in the same alert is not logged again");

    for (int i = 0; i < SweepBudget::kOverLimit - 1; ++i)
        (void)budget.Charge(60.0);
    (void)budget.Charge(20.0);
    for (int i = 0; i < SweepBudget::kOverLimit - 1; ++i)
        Require(!budget.Charge(60.0).paused, "a sweep within budget breaks the run");

    budget.Reset();
    for (int i = 0; i < SweepBudget::kOverLimit - 1; ++i)
        (void)budget.Charge(60.0);
    next = budget.Charge(60.0);
    Require(next.paused && next.first_pause, "a new alert logs its first pause again");
}

// Feeds `value` every 200 ms from `from` up to and including `to`; returns the
// rule's state after the last reading.
bool FeedTopApp(gtg::topapp::AlertRule& rule, const std::uint64_t from, const std::uint64_t to,
                const std::optional<double> value) {
    bool active = rule.Active();
    for (std::uint64_t t = from; t <= to; t += 200)
        active = rule.Observe(t, value);
    return active;
}

void TestTopAppAlertRule() {
    using gtg::topapp::AlertRule;
    // The rule, pinned with its own numbers: the tuned constants are
    // TestTopAppThresholds's business.
    const gtg::topapp::Thresholds kGpuThresholds{85.0, 70.0};
    {
        AlertRule rule(kGpuThresholds);
        Require(!FeedTopApp(rule, 1'000, 8'800, 95.0), "7.8 s of load is not yet an alert");
        Require(FeedTopApp(rule, 9'000, 9'000, 95.0), "8 s of sustained load enters");
    }
    {
        // Jitter that averages above the line and never dips below leave.
        AlertRule rule(kGpuThresholds);
        bool active = false;
        for (std::uint64_t t = 1'000; t <= 9'000; t += 200) {
            active = rule.Observe(t, (t / 200) % 2 == 0 ? 80.0 : 96.0);
        }
        Require(active, "jitter averaging 88 with no dip below 70 enters");
    }
    {
        // One dip below leave vetoes until it has aged out of the 8 s window.
        AlertRule rule(kGpuThresholds);
        FeedTopApp(rule, 1'000, 4'800, 95.0);
        rule.Observe(5'000, 60.0);
        Require(!FeedTopApp(rule, 5'200, 12'800, 95.0), "a dip below leave holds entry off");
        Require(FeedTopApp(rule, 13'000, 13'200, 95.0), "and entry follows once it has aged out");
    }
    {
        // A hard drop leaves exactly ten seconds later.
        AlertRule rule(kGpuThresholds);
        FeedTopApp(rule, 1'000, 9'000, 95.0);
        Require(rule.Active(), "precondition: in alert");
        Require(FeedTopApp(rule, 9'200, 19'000, 10.0), "still in alert 9.8 s after the drop");
        Require(!FeedTopApp(rule, 19'200, 19'200, 10.0), "left 10 s after the drop");
    }
    {
        // A spike back to enter restarts the ten seconds.
        AlertRule rule(kGpuThresholds);
        FeedTopApp(rule, 1'000, 9'000, 95.0);
        FeedTopApp(rule, 9'200, 14'000, 10.0);
        rule.Observe(14'200, 90.0);
        Require(FeedTopApp(rule, 14'400, 24'000, 10.0), "a spike to enter keeps the alert");
        Require(!FeedTopApp(rule, 24'200, 24'400, 10.0), "until 10 s after the spike");
    }
    {
        // Load between leave and enter keeps an alert up: it has not let go.
        AlertRule rule(kGpuThresholds);
        FeedTopApp(rule, 1'000, 9'000, 95.0);
        Require(FeedTopApp(rule, 9'200, 40'000, 75.0), "load above leave keeps the alert");
    }
    {
        // Missing readings: a short hole is survived, three seconds is not.
        AlertRule rule(kGpuThresholds);
        FeedTopApp(rule, 1'000, 9'000, 95.0);
        Require(FeedTopApp(rule, 9'200, 11'800, std::nullopt), "2.8 s without data keeps it");
        Require(!rule.Observe(12'000, std::nullopt), "3 s without data takes it down");
    }
    {
        // Before entry a missing reading restarts the run.
        AlertRule rule(kGpuThresholds);
        FeedTopApp(rule, 1'000, 6'000, 95.0);
        rule.Observe(6'200, std::nullopt);
        Require(!FeedTopApp(rule, 6'400, 14'200, 95.0), "the run restarts after a hole");
        Require(FeedTopApp(rule, 14'400, 14'400, 95.0), "8 s after the hole it enters");
    }
    {
        // A stalled caller is a gap, not a continuation.
        AlertRule rule(kGpuThresholds);
        FeedTopApp(rule, 1'000, 5'000, 95.0);
        Require(!FeedTopApp(rule, 7'000, 12'000, 95.0), "a 2 s silence restarts the run");
        Require(!rule.Observe(20'000, std::nullopt) && !rule.Active(), "stale and idle");
    }
}

void TestTopAppLeader() {
    using gtg::topapp::Candidate;
    using gtg::topapp::Dominance;
    using gtg::topapp::Leader;
    const auto apps = [](std::initializer_list<std::pair<const wchar_t*, double>> list) {
        std::vector<Candidate> out;
        for (const auto& [key, value] : list)
            out.push_back({key, value});
        return out;
    };
    {
        Leader leader(Dominance::ShareOfTotal);
        Require(!leader.Update(1'000, apps({{L"a", 60}, {L"b", 10}})), "one sweep names nobody");
        Require(leader.Update(2'000, apps({{L"a", 60}, {L"b", 10}})) == std::wstring(L"a"),
                "two agreeing sweeps name the leader");
    }
    {
        Leader leader(Dominance::ShareOfTotal);
        for (std::uint64_t t = 1'000; t <= 6'000; t += 1'000) {
            Require(!leader.Update(
                        t, apps({{L"a", 20}, {L"b", 20}, {L"c", 20}, {L"d", 21}, {L"e", 19}})),
                    "spread load names nobody");
        }
    }
    {
        // A challenger leading by more than a quarter for three sweeps, but
        // only once the shown name has been up five seconds.
        Leader leader(Dominance::ShareOfTotal);
        leader.Update(1'000, apps({{L"a", 50}, {L"b", 5}}));
        leader.Update(2'000, apps({{L"a", 50}, {L"b", 5}}));
        Require(leader.Shown() == std::wstring(L"a"), "precondition: a shown at 2 s");
        std::uint64_t t = 3'000;
        for (; t <= 6'000; t += 1'000)
            leader.Update(t, apps({{L"a", 10}, {L"b", 200}}));
        Require(leader.Shown() == std::wstring(L"a"), "not before five seconds shown");
        leader.Update(7'000, apps({{L"a", 10}, {L"b", 200}}));
        Require(leader.Shown() == std::wstring(L"b"), "switches once five seconds have passed");
    }
    {
        // Leading by less than a quarter never switches.
        Leader leader(Dominance::ShareOfTotal);
        leader.Update(1'000, apps({{L"a", 50}, {L"b", 45}}));
        leader.Update(2'000, apps({{L"a", 50}, {L"b", 45}}));
        for (std::uint64_t t = 3'000; t <= 20'000; t += 1'000) {
            leader.Update(t, apps({{L"a", 50}, {L"b", 60}}));
        }
        Require(leader.Shown() == std::wstring(L"a"), "a 20 % lead is not enough");
    }
    {
        // One busy sweep does not win: smoothing.
        Leader leader(Dominance::ShareOfTotal);
        for (std::uint64_t t = 1'000; t <= 8'000; t += 1'000) {
            leader.Update(t, apps({{L"a", 50}, {L"b", 10}}));
        }
        leader.Update(9'000, apps({{L"a", 50}, {L"b", 200}}));
        leader.Update(10'000, apps({{L"a", 50}, {L"b", 10}}));
        leader.Update(11'000, apps({{L"a", 50}, {L"b", 10}}));
        leader.Update(12'000, apps({{L"a", 50}, {L"b", 10}}));
        Require(leader.Shown() == std::wstring(L"a"), "one spike does not switch");
    }
    {
        // The shown app exits: gone at once.
        Leader leader(Dominance::ShareOfTotal);
        leader.Update(1'000, apps({{L"a", 50}, {L"b", 5}}));
        leader.Update(2'000, apps({{L"a", 50}, {L"b", 5}}));
        Require(leader.Update(3'000, apps({{L"b", 5}, {L"c", 4}})) != std::wstring(L"a"),
                "an exited app leaves at once");
    }
    {
        // Absolute dominance, as GPU uses it.
        Leader leader(Dominance::Absolute);
        leader.Update(1'000, apps({{L"a", 20}}));
        Require(!leader.Update(2'000, apps({{L"a", 20}})), "20 % busiest engine is not named");
        leader.Update(3'000, apps({{L"a", 90}}));
        Require(leader.Update(4'000, apps({{L"a", 90}})) == std::wstring(L"a"),
                "a busy engine is named");
    }
    {
        // The shown app stops being dominant: it goes, with the same patience.
        Leader leader(Dominance::ShareOfTotal);
        leader.Update(1'000, apps({{L"a", 50}, {L"b", 5}}));
        leader.Update(2'000, apps({{L"a", 50}, {L"b", 5}}));
        std::uint64_t t = 3'000;
        for (; t <= 30'000 && leader.Shown(); t += 1'000) {
            leader.Update(t,
                          apps({{L"a", 5}, {L"b", 5}, {L"c", 5}, {L"d", 5}, {L"e", 5}, {L"f", 5}}));
        }
        Require(!leader.Shown(), "spread load takes the name down");
    }
}

void TestTopAppNames() {
    using namespace gtg::topapp;
    Require(ExecutableStem(L"C:\\x\\vllm-server.exe") == L"vllm-server",
            "stem drops the extension");
    Require(ExecutableStem(L"C:/x/y/app") == L"app", "stem without an extension");
    Require(ExecutableStem(L"C:\\x\\.hidden") == L".hidden", "a leading dot is not an extension");
    Require(GroupKey(L"C:\\Program Files\\App\\App.EXE") ==
                GroupKey(L"c:\\program files\\app\\app.exe"),
            "the group key ignores case");
    Require(GroupKey(L"C:/a/b.exe") == GroupKey(L"C:\\a\\b.exe"), "and slash direction");
    Require(DisplayName(L"Firefox", L"C:\\x\\firefox.exe") == L"Firefox",
            "the description comes first");
    Require(DisplayName(L"", L"C:\\x\\vllm-server.exe") == L"vllm-server",
            "the executable name when there is no description");
    Require(DisplayName(L"  \t  ", L"C:\\x\\tool.exe") == L"tool", "a blank description is none");
    Require(DisplayName(L"  Visual\r\nStudio   Code ", L"") == L"Visual Studio Code",
            "control characters and runs of spaces collapse");
    Require(DisplayName(L"桌面視窗管理員", L"C:\\Windows\\dwm.exe") == L"桌面視窗管理員",
            "a localised description is kept as it is");
    // The short name (owner, 2026-10-16): the app's name, not its tagline.
    Require(DisplayName(L"qBittorrent - A Bittorrent Client", L"C:\\q\\qbittorrent.exe") ==
                L"qBittorrent",
            "a tagline after a spaced dash goes");
    Require(DisplayName(L"Foo Tool (x64)", L"") == L"Foo Tool", "a trailing bracket goes");
    Require(DisplayName(L"Foo\u00AE Studio\u2122", L"") == L"Foo Studio", "trademark marks go");
    Require(DisplayName(L"NVIDIA cicc, Version 12.4.131 ", L"C:\\c\\cicc.exe") == L"NVIDIA cicc",
            "a trailing version note goes");
    Require(DisplayName(L"Foo Tool version 2", L"") == L"Foo Tool",
            "in any case, with or without the comma");
    Require(DisplayName(L"Windows 11", L"") == L"Windows 11",
            "a bare number stays: it may be part of the name");
    Require(DisplayName(L"Foo, Version 1.2a", L"") == L"Foo, Version 1.2a",
            "only a plain number is a version");
    Require(DisplayName(L"Version 2", L"C:\\t\\tool.exe") == L"Version 2",
            "the version rule never leaves nothing");
    Require(DisplayName(L"Click-to-Run", L"") == L"Click-to-Run", "an unspaced hyphen stays");
    Require(DisplayName(L"(x64)", L"C:\\t\\tool.exe") == L"(x64)",
            "a rule that would leave nothing does not apply");
    Require(DisplayName(L"", L"C:\\g\\GameName-Win64-Shipping.exe") == L"GameName",
            "a game build's suffix goes from the executable name");
    Require(DisplayName(L"", L"C:\\t\\Tool_x64.exe") == L"Tool", "and an architecture suffix");
    Require(DisplayName(L"", L"C:\\t\\x64.exe") == L"x64", "but never the whole name");

    // The cap: 20 Latin letters' width, CJK counted 1.5, cut at a word.
    Require(NameWidth(L"abc") == 6 && NameWidth(L"\u684C\u9762") == 6, "CJK is 1.5 letters");
    Require(DisplayName(L"Google Chrome for Testing", L"") == L"Google Chrome for\u2026",
            "cut at a word, with an ellipsis");
    Require(DisplayName(L"Host Process for Windows Services", L"") == L"Host Process for\u2026",
            "a long description is cut the same way");
    Require(DisplayName(L"桌面視窗管理員", L"C:\\Windows\\dwm.exe") == L"桌面視窗管理員",
            "seven CJK characters fit");
    const std::wstring cjk = DisplayName(L"一二三四五六七八九十一二三四五", L"");
    Require(cjk.size() == 13 && cjk.back() == kEllipsis && NameWidth(cjk) <= kMaxNameWidth,
            "fifteen CJK characters are cut to twelve");
    const std::wstring long_name(80, L'x');
    const std::wstring cut = DisplayName(long_name, L"");
    Require(NameWidth(cut) <= kMaxNameWidth && cut.back() == kEllipsis,
            "a word longer than the cap is cut mid-word");
    std::wstring pair(17, L'y');
    pair += L"\U0001F600zzzz";
    const std::wstring kept = DisplayName(pair, L"");
    Require(kept.size() == 20 && kept[17] == 0xD83D && kept[18] == 0xDE00,
            "a surrogate pair that fits is kept whole");
    std::wstring straddle(18, L'y');
    straddle += L"\U0001F600zzzz";
    const std::wstring dropped = DisplayName(straddle, L"");
    Require(dropped.size() == 19 && dropped[17] == L'y', "one that does not is dropped whole");
}

void TestTopAppServiceHost() {
    using namespace gtg::topapp;
    Require(IsServiceHost(L"C:\\Windows\\System32\\svchost.exe"), "svchost is a service host");
    Require(IsServiceHost(L"C:\\Windows\\SysWOW64\\SvcHost.EXE"), "in any case");
    Require(!IsServiceHost(L"C:\\x\\svchostx.exe"), "but not a name that contains it");
    Require(ServiceHostName({L"Windows Update"}) == L"Windows Update", "one service: its name");
    Require(ServiceHostName({L"Remote Procedure Call (RPC)", L"DCOM Server Process Launcher",
                             L"RPC Endpoint Mapper"}) == L"Remote Procedure\u2026 +2",
            "several: the first, short, and how many more");
    Require(NameWidth(ServiceHostName({std::wstring(60, L'a'), L"b"})) <= kMaxNameWidth,
            "the count fits inside the cap");
    Require(ServiceHostName({}).empty(), "no services, no name");
}

void TestTopAppSystemProcesses() {
    using namespace gtg::topapp;
    const std::vector<ProcessEntry> list = {
        {8, 4, L"vmcompute.exe"},
        {100, 8, L"vmwp.exe"},
        {200, 100, L"vmmemWSL"},
        {300, 8, L"VMWP.EXE"},
        {400, 300, L"vmmem"},
        {500, 8, L"vmwp.exe"},
        {600, 999, L"vmmemCmZygote"},
        {700, 4, L"Memory Compression"},
        {4, 0, L"System"},
        {800, 4, L""},
        {900, 4, L"explorer.exe"},
        {950, 900, L"VMMEMWSL"},
    };
    const auto map = ClassifySystemProcesses(list);
    const auto at = [&](const std::uint32_t pid) {
        return map.at(pid);
    };
    Require(at(200).kind == SystemKind::Wsl && at(100).kind == SystemKind::Wsl,
            "vmmemWSL and its worker are WSL");
    Require(at(100).key == at(200).key, "one app: GPU from the worker, CPU and RAM from memory");
    Require(at(300).kind == SystemKind::VirtualMachine && at(300).key == at(400).key,
            "another VM is one app too, in any case");
    Require(at(500).kind == SystemKind::VirtualMachine && at(500).key != at(300).key &&
                at(300).key != at(100).key,
            "two VMs never merge, nor with WSL");
    Require(at(600).kind == SystemKind::VirtualMachine && at(600).key != at(500).key,
            "a memory process without a worker is a VM of its own");
    Require(at(700).kind == SystemKind::Kernel && at(700).name == L"Memory Compression",
            "a kernel process keeps its own name");
    Require(at(4).name == L"System" && at(4).key != at(700).key, "each under its own key");
    Require(!map.contains(800), "no name, not ranked");
    Require(at(950).kind == SystemKind::Wsl && at(900).kind == SystemKind::Kernel,
            "a parent that is no VM worker is not claimed");
    Require(KernelProcessName(L"foo.exe") == L"foo", "an executable's name loses .exe");
    Require(NameWidth(KernelProcessName(std::wstring(60, L'k'))) <= kMaxNameWidth, "and is cut");

    Require(IsVmWorker(L"C:\\Windows\\System32\\vmwp.exe"), "the system's worker");
    Require(IsVmWorker(L"c:\\windows\\SYSTEM32\\VMWP.EXE"), "in any case");
    Require(!IsVmWorker(L"C:\\Tools\\vmwp.exe"), "not a copy elsewhere");

    // The list is read only for processes newer than it, at most every 10
    // sweeps, and a reused pid never inherits the list's name.
    SystemProcessMap names;
    int reads = 0;
    std::uint64_t now = 1'000;
    const auto take = [&](std::vector<ProcessEntry>& entries) -> std::optional<std::uint64_t> {
        ++reads;
        entries = list;
        return now;
    };
    const SystemProcess* found = names.Find(200, 500, 1, take);
    Require(found != nullptr && found->kind == SystemKind::Wsl && reads == 1,
            "the first question reads the list");
    Require(names.Find(700, 900, 1, take) != nullptr && reads == 1, "an older process: no read");
    Require(names.Find(123, 900, 2, take) == nullptr && reads == 1,
            "a process the list does not name stays unnamed without a read");
    now = 3'000;
    Require(names.Find(200, 2'000, 5, take) == nullptr && reads == 1,
            "a newer process with a reused pid waits for the next read");
    Require(names.Find(200, 2'000, 11, take) != nullptr && reads == 2,
            "ten sweeps on, the list is read again");
    const auto fail = [&](std::vector<ProcessEntry>&) -> std::optional<std::uint64_t> {
        ++reads;
        return std::nullopt;
    };
    Require(names.Find(400, 5'000, 21, fail) == nullptr && reads == 3, "a failed read names none");
    Require(names.Find(700, 900, 22, fail) != nullptr && reads == 3,
            "and the list before it still holds");
}

void TestTopAppThresholds() {
    using namespace gtg::topapp;
    // Owner, 2026-10-16: GPU and VRAM name their app from 70 %.
    Require(kGpuThresholds.enter == 70.0 && kVramThresholds.enter == 70.0,
            "GPU and VRAM enter at 70 %");
    for (const Thresholds t : {kGpuThresholds, kVramThresholds, kCpuThresholds, kRamThresholds}) {
        Require(t.leave < t.enter, "every record leaves below where it enters");
    }
}

void TestTopAppCpuLedger() {
    using gtg::topapp::CpuLedger;
    using gtg::topapp::Identity;
    CpuLedger ledger;
    const Identity a{100, 1};
    ledger.BeginSweep(1'000);
    Require(!ledger.Update(a, 0, 4), "nothing on first sight");
    ledger.EndSweep();
    ledger.BeginSweep(2'000);
    // 2 s of busy time over 1 s on 4 processors is half the machine.
    const auto half = ledger.Update(a, 20'000'000, 4);
    Require(half && *half > 49.9 && *half < 50.1, "busy over wall over processors");
    ledger.EndSweep();
    ledger.BeginSweep(3'000);
    const Identity reused{100, 2};
    Require(!ledger.Update(reused, 50'000'000, 4), "a reused pid is a new process");
    ledger.EndSweep();
    ledger.BeginSweep(4'000);
    Require(!ledger.Update(a, 30'000'000, 4), "a process missed for a sweep starts over");
    ledger.EndSweep();
    ledger.BeginSweep(5'000);
    Require(!ledger.Update(a, 10'000'000, 4), "a counter going backwards is not a rate");
    ledger.EndSweep();
}

void TestTopAppMarquee() {
    using namespace gtg::topapp::marquee;
    Require(ScrollOffset(5'000, 50.0F, 60.0F, 24.0F, 24.0F, kPauseMs) == 0.0F,
            "a name that fits never moves");
    Require(ScrollOffset(500, 100.0F, 60.0F, 24.0F, 24.0F, 1'200) == 0.0F,
            "still during the pause");
    const float moved = ScrollOffset(2'200, 100.0F, 60.0F, 24.0F, 24.0F, 1'200);
    Require(moved > 23.9F && moved < 24.1F, "24 per second after the pause");
    // One cycle is the text and the gap: 124 at 24/s is 5166 ms, plus the pause.
    Require(ScrollOffset(1'200 + 5'166 + 1, 100.0F, 60.0F, 24.0F, 24.0F, 1'200) == 0.0F,
            "the loop starts over with a pause");
    Require(PageOffset(10'000, 50.0F, 60.0F, kPageMs) == 0.0F, "a fitting name does not page");
    Require(PageOffset(0, 100.0F, 60.0F, 2'000) == 0.0F, "first page at the start");
    Require(PageOffset(2'000, 100.0F, 60.0F, 2'000) == 40.0F,
            "the last page ends flush with the box");
    Require(PageOffset(4'000, 100.0F, 60.0F, 2'000) == 0.0F, "then it starts over");
    Require(PageOffset(2'000, 200.0F, 60.0F, 2'000) == 48.0F, "a page advances four fifths");
}

void TestTopAppPdhInstance() {
    using gtg::topapp::EqualsFolded;
    using gtg::topapp::InstanceId;
    using gtg::topapp::ParseInstance;
    InstanceId id;
    Require(ParseInstance(L"pid_1234_luid_0x00000000_0x01A121AC_phys_0_eng_0_engtype_3D", id) &&
                id.pid == 1234 && id.luid == L"0x00000000_0x01A121AC",
            "an engine instance");
    Require(ParseInstance(L"pid_8_luid_0x00000000_0x0001E692_phys_0", id) && id.pid == 8,
            "a memory instance");
    Require(EqualsFolded(id.luid, L"0x00000000_0x0001e692"), "LUIDs compare without case");
    Require(!EqualsFolded(id.luid, L"0x00000000_0x0001e693"), "but do compare");
    Require(!ParseInstance(L"_Total", id), "the total is not a process");
    Require(!ParseInstance(L"pid__luid_0x00000000_0x0001E692", id), "no digits");
    Require(!ParseInstance(L"pid_99999999999_luid_0x00000000_0x0001E692", id), "too long a pid");
    Require(!ParseInstance(L"pid_1_luid_0x0000", id), "a cut-off LUID");
    Require(!ParseInstance(L"pid_1_luid_zz00000000_0x0001E692_phys_0", id), "a malformed LUID");
}

void TestTopAppBoard() {
    using gtg::topapp::Carry;
    using gtg::topapp::Shown;
    std::optional<Shown> slot;
    Carry(slot, std::wstring(L"Firefox"), 1'000);
    Require(slot && slot->since_ms == 1'000, "a new name starts its clock");
    Carry(slot, std::wstring(L"Firefox"), 5'000);
    Require(slot && slot->since_ms == 1'000, "the same name keeps it");
    Carry(slot, std::wstring(L"vllm-server"), 6'000);
    Require(slot && slot->since_ms == 6'000 && slot->name == L"vllm-server",
            "a changed name starts over");
    Carry(slot, std::nullopt, 7'000);
    Require(!slot, "no name clears the slot");
    Require(gtg::settings::ResolveTopAppPreference(false, 0), "absent means on");
    Require(!gtg::settings::ResolveTopAppPreference(true, 0), "an explicit zero is off");
}

gtg::peer::PairingCode SamplePairingCode(const std::uint8_t seed) {
    gtg::peer::PairingCode code;
    code.address = {192, 168, 1, static_cast<std::uint8_t>(10 + seed)};
    code.port = static_cast<std::uint16_t>(47'800 + seed);
    for (std::size_t i = 0; i < code.secret.size(); ++i) {
        code.secret[i] = static_cast<std::uint8_t>(seed * 37 + i * 11);
    }
    return code;
}

void TestPeerPairingCode() {
    using namespace gtg::peer;
    const std::string_view qr_alphanumeric = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ $%*+-./:";
    for (std::uint8_t seed = 0; seed < 32; ++seed) {
        const PairingCode code = SamplePairingCode(seed);
        const std::string text = FormatCode(code);
        Require(text.size() == 51 && text.rfind("GTG-", 0) == 0, "the code's shape");
        for (std::size_t i = 3; i < text.size(); i += 6) {
            Require(text[i] == '-', "groups of five");
        }
        for (const char c : text) {
            Require(qr_alphanumeric.find(c) != std::string_view::npos,
                    "every character is in the QR alphanumeric set");
        }
        const auto parsed = ParseCode(std::string_view(text));
        Require(parsed.code && *parsed.code == code, "a code reads back as it was written");
    }

    const PairingCode code = SamplePairingCode(3);
    const std::string text = FormatCode(code);
    const auto reads = [&](const std::wstring& typed) {
        const auto parsed = ParseCode(std::wstring_view(typed));
        return parsed.code && *parsed.code == code;
    };
    const std::wstring wide(text.begin(), text.end());
    std::wstring lower = wide;
    for (wchar_t& c : lower)
        c = static_cast<wchar_t>(towlower(c));
    Require(reads(lower), "case is ignored");
    std::wstring bare;
    for (const wchar_t c : wide.substr(3)) {
        if (c != L'-') bare.push_back(c);
    }
    Require(reads(bare), "the prefix and the dashes are optional");
    std::wstring spaced = L"  " + wide.substr(0, 20) + L"\r\n" + wide.substr(20, 11) + L" \t" +
                          wide.substr(31) + L"\n";
    Require(reads(spaced), "spaces and line breaks are ignored");
    std::wstring full_width;
    for (const wchar_t c : wide) {
        full_width.push_back(c == L'-' ? L'\uFF0D' : static_cast<wchar_t>(c + 0xFEE0));
    }
    Require(reads(full_width), "full-width characters from an input method are read");
    std::wstring dashes = wide;
    for (wchar_t& c : dashes) {
        if (c == L'-') c = L'\u2013';
    }
    Require(reads(dashes), "a chat window's en dashes are read as dashes");
    std::wstring confusable = wide;
    for (std::size_t i = 4; i < confusable.size(); ++i) {
        if (confusable[i] == L'0') confusable[i] = L'O';
        if (confusable[i] == L'1') confusable[i] = (i % 2 == 0) ? L'I' : L'l';
    }
    Require(confusable != wide && reads(confusable), "O reads as 0, I and L as 1");

    // Any one symbol mistyped, and any two neighbours swapped, are caught by
    // the checksum before anything is sent.
    const std::string alphabet = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";
    for (std::size_t i = 4; i < text.size(); ++i) {
        if (text[i] == '-') continue;
        for (const char replacement : alphabet) {
            if (replacement == text[i]) continue;
            std::string typo = text;
            typo[i] = replacement;
            Require(ParseCode(std::string_view(typo)).error == CodeError::Checksum,
                    "a mistyped symbol is caught");
        }
    }
    std::string symbols;
    for (const char c : text.substr(4)) {
        if (c != '-') symbols.push_back(c);
    }
    for (std::size_t i = 0; i + 1 < symbols.size(); ++i) {
        if (symbols[i] == symbols[i + 1]) continue;
        std::string swapped = symbols;
        std::swap(swapped[i], swapped[i + 1]);
        Require(ParseCode(std::string_view(swapped)).error == CodeError::Checksum,
                "two swapped symbols are caught");
    }
    Require(ParseCode(std::string_view(symbols.substr(0, symbols.size() - 1))).error ==
                CodeError::Length,
            "a truncated code is incomplete");
    Require(ParseCode(std::string_view(symbols + "0")).error == CodeError::Length,
            "one symbol too many is not a code");
    Require(ParseCode(std::string_view(text + text)).error == CodeError::Length,
            "two codes are not one");
    std::string with_u = text;
    with_u[10] = 'U';
    Require(ParseCode(std::string_view(with_u)).error == CodeError::Character, "U is not a symbol");
    Require(ParseCode(std::string_view("GTG-@")).error == CodeError::Character,
            "punctuation is not a symbol");
    Require(ParseCode(std::string_view("")).error == CodeError::Empty &&
                ParseCode(std::wstring_view(L" - \r\n")).error == CodeError::Empty,
            "nothing pasted is empty");

    // Content that checksums but is not a code this version reads.
    std::array<std::uint8_t, kCodeBytes> bytes{};
    bytes[0] = 0x26;  // format 2, IPv6
    bytes[1] = 10;
    bytes[6] = 1;
    Require(ParseCode(std::string_view(code_detail::Encode(bytes))).error == CodeError::Format,
            "a later format is refused");
    for (const auto& address :
         {std::array<std::uint8_t, 4>{0, 0, 0, 0}, std::array<std::uint8_t, 4>{224, 0, 0, 1},
          std::array<std::uint8_t, 4>{255, 255, 255, 255}}) {
        PairingCode bad = code;
        bad.address = address;
        Require(ParseCode(std::string_view(FormatCode(bad))).error == CodeError::Address,
                "an address no host has is refused");
    }
    PairingCode no_port = code;
    no_port.port = 0;
    Require(ParseCode(std::string_view(FormatCode(no_port))).error == CodeError::Address,
            "port 0 is refused");
    PairingCode loopback = code;
    loopback.address = {127, 0, 0, 1};
    Require(ParseCode(std::string_view(FormatCode(loopback))).code.has_value(),
            "loopback is a host: two GTGs on one machine");
}

gtg::peer::HostState SampleHostState() {
    using namespace gtg::peer;
    HostState state;
    state.name = L"Render-01";
    state.version = {1, 2, 0};
    state.protection = Protection::Warning;
    state.stale = true;
    state.records[0] = {0.5F, 0.75F, true, false, 83.5F, std::nullopt};
    state.records[1] = {0.25F, 0.5F, false, false, 412.0F, std::nullopt};
    state.records[2] = {std::nullopt, 0.7F, false, false, std::nullopt, std::nullopt};
    state.records[6] = {0.6667F, 0.6667F, true, true, 59.9F, std::nullopt};
    state.records[7] = {0.125F, std::nullopt, false, false, 1.5e6F, 2.5e5F};
    return state;
}

void TestPeerProtocol() {
    using namespace gtg::peer;
    Require(DatagramBytes(Type::PairRequest) == 160 && DatagramBytes(Type::PairAccept) == 168 &&
                DatagramBytes(Type::State) == 244 && DatagramBytes(Type::Goodbye) == 60,
            "each type has one exact size");

    const HostState state = SampleHostState();
    const auto body = WriteStateBody(state);
    const auto read = ReadStateBody(body);
    Require(read && *read == state, "a state reads back as it was written");

    // Every check refuses the whole datagram.
    const auto refused = [&](const std::size_t offset, const std::uint8_t value, const char* what) {
        auto bad = body;
        bad[offset] = value;
        Require(!ReadStateBody(bad), what);
    };
    const std::size_t records = kNameBytes + 8;
    const auto record = [&](const std::size_t index) {
        return records + index * kRecordBytes;
    };
    refused(kNameBytes + 4, kProtectionCount, "a protection state that does not exist");
    refused(kNameBytes + 5, 0x02, "an unknown state flag");
    refused(kNameBytes + 6, 9, "another record count");
    refused(kNameBytes + 3, 1, "a reserved byte");
    refused(record(0), 0x40, "an unknown record flag");
    refused(record(0) + 1, 1, "a reserved record byte");
    refused(record(0) + 3, 0x28, "a level above 1");  // 10'240
    refused(record(2) + 2, 1, "a level without its flag");
    refused(
        record(1),
        record_flags::kLevel | record_flags::kTick | record_flags::kValue | record_flags::kAlarm,
        "an alarm on a record that has none");
    refused(record(0),
            record_flags::kLevel | record_flags::kTick | record_flags::kRaised |
                record_flags::kValue | record_flags::kValue2,
            "a second value on a record that has none");
    const auto set_float = [&](const std::size_t index, const std::size_t at, const float value,
                               const char* what) {
        auto bad = body;
        const auto raw = std::bit_cast<std::uint32_t>(value);
        for (std::size_t i = 0; i < 4; ++i) {
            bad[record(index) + at + i] = static_cast<std::uint8_t>(raw >> (8 * i));
        }
        Require(!ReadStateBody(bad), what);
    };
    set_float(0, 8, std::numeric_limits<float>::quiet_NaN(), "NaN");
    set_float(0, 8, std::numeric_limits<float>::infinity(), "infinity");
    set_float(3, 8, 5.0F, "a value without its flag must be zero");
    {
        auto bad = body;
        bad[record(3)] = record_flags::kValue;
        const auto raw = std::bit_cast<std::uint32_t>(100.5F);
        for (std::size_t i = 0; i < 4; ++i) {
            bad[record(3) + 8 + i] = static_cast<std::uint8_t>(raw >> (8 * i));
        }
        Require(!ReadStateBody(bad), "a percentage above 100");
    }
    Require(!ReadStateBody(std::span(body).first(body.size() - 1)), "a body one byte short");

    // Names: UTF-8 only, no controls, nothing after the padding.
    HostState named = state;
    named.name = L"\u5DE5\u4F5C\u7AD9 \u2014 GPU";
    Require(ReadStateBody(WriteStateBody(named))->name == named.name, "a CJK name round trips");
    named.name = std::wstring(60, L'\u5DE5');
    Require(ReadStateBody(WriteStateBody(named))->name == std::wstring(16, L'\u5DE5'),
            "a long name is cut at a character boundary");
    // Built from its code unit: a literal override trips the compiler's own
    // bidirectional-text warning.
    named.name = std::wstring(L"evil") + static_cast<wchar_t>(0x202E) + L"gnp.exe\r\n";
    Require(ReadStateBody(WriteStateBody(named))->name == L"evilgnp.exe",
            "direction overrides and controls are dropped");
    refused(0, 0xC0, "an overlong UTF-8 lead");
    refused(0, 0xFF, "a byte that is never UTF-8");
    {
        auto bad = body;
        bad[kNameBytes - 1] = 'x';
        Require(!ReadStateBody(bad), "bytes after the name's padding");
    }
    {
        auto bad = body;
        const std::uint8_t surrogate[3] = {0xED, 0xA0, 0x80};
        std::memcpy(bad.data(), surrogate, 3);
        Require(!ReadStateBody(bad), "an encoded surrogate");
    }

    // The sender clamps rather than sending what the receiver refuses.
    HostState wild = state;
    wild.records[3].value = 140.0F;
    wild.records[4].value = std::numeric_limits<float>::quiet_NaN();
    wild.records[0].level = 3.0F;
    wild.records[2].value2 = 7.0F;
    const auto clamped = ReadStateBody(WriteStateBody(wild));
    Require(clamped && clamped->records[3].value == 100.0F && !clamped->records[4].value &&
                clamped->records[0].level == 1.0F && !clamped->records[2].value2,
            "the sender clamps to what the receiver accepts");

    // Headers and prefixes.
    SealedHeader header;
    header.type = Type::State;
    header.sender = 0x0102030405060708ULL;
    header.salt.fill(0xA5);
    header.sequence = 77;
    header.sent_ms = 1'790'000'000'000;
    std::array<std::uint8_t, kStateBytes> datagram{};
    const auto header_bytes = WriteSealedHeader(header);
    std::memcpy(datagram.data(), header_bytes.data(), header_bytes.size());
    const auto back = ReadSealedHeader(datagram);
    Require(back && back->sender == header.sender && back->salt == header.salt &&
                back->sequence == 77 && back->sent_ms == header.sent_ms,
            "a sealed header reads back");
    Require(!ReadSealedHeader(std::span(datagram).first(kStateBytes - 1)),
            "a state datagram one byte short is dropped before anything else");
    Require(!ReadPrefix(std::span(datagram).first(kGoodbyeBytes)),
            "a state header on a goodbye's size is dropped");
    auto other = datagram;
    other[2] = kProtocolVersion + 1;
    Require(!ReadPrefix(other), "another protocol version");
    other = datagram;
    other[0] = 'X';
    Require(!ReadPrefix(other), "not ours");
    other = datagram;
    other[3] = 9;
    Require(!ReadPrefix(other), "a type that does not exist");
#ifndef GTG_FEATURE_UPDATE
    // A newer GTG may send types this one does not know: each is dropped
    // before anything else is read, so the state beside them carries on.
    for (int type = 5; type < 256; ++type) {
        other = datagram;
        other[3] = static_cast<std::uint8_t>(type);
        Require(!ReadPrefix(other), "a type from a newer GTG is dropped first");
    }
#endif

    PairRequest request;
    request.joiner = 42;
    request.code_id = {1, 2, 3, 4};
    request.public_key.fill(7);
    request.name = NameField(L"Joiner");
    request.mac.fill(9);
    const auto request_bytes = WritePairRequest(request);
    const auto request_back = ReadPairRequest(request_bytes);
    Require(request_back && request_back->joiner == 42 &&
                request_back->code_id == request.code_id &&
                request_back->public_key == request.public_key && request_back->mac == request.mac,
            "a pair request reads back");
    Require(MacInput(request_bytes).size() == kPairRequestBytes - kMacBytes,
            "the MAC covers everything before it");
    PairAccept accept;
    accept.issuer = 5;
    accept.status = AcceptStatus::Used;
    accept.joiner = 42;
    auto accept_bytes = WritePairAccept(accept);
    Require(
        ReadPairAccept(accept_bytes) && ReadPairAccept(accept_bytes)->status == AcceptStatus::Used,
        "a refusal reads back");
    accept_bytes[kPrefixBytes] = kAcceptStatusCount;
    Require(!ReadPairAccept(accept_bytes), "a status that does not exist");
    accept_bytes[kPrefixBytes] = 0;
    accept_bytes[kPrefixBytes + 1] = 1;
    Require(!ReadPairAccept(accept_bytes), "an accept's reserved byte");
}

void TestPeerHostsView() {
    using namespace gtg::peer;
    Require(PresenceAt({}, 50'000) == Presence::NotHeard, "never heard is not lost");
    Require(PresenceAt({1'000, std::nullopt}, 4'000) == Presence::Online, "3 s is still online");
    Require(PresenceAt({1'000, std::nullopt}, 4'001) == Presence::Dim, "after 3 s it dims");
    Require(PresenceAt({1'000, std::nullopt}, 11'000) == Presence::Dim, "10 s is still dim");
    Require(PresenceAt({1'000, std::nullopt}, 11'001) == Presence::Lost, "after 10 s it is lost");
    Require(PresenceAt({1'000, 1'500}, 60'000) == Presence::Offline, "a goodbye is offline, quiet");
    Require(PresenceAt({std::nullopt, 1'500}, 60'000) == Presence::Offline,
            "a goodbye before any state is offline");
    Require(PresenceAt({2'000, 1'500}, 2'500) == Presence::Online, "a state after goodbye is back");
    Require(PresenceAt({5'000, std::nullopt}, 1'000) == Presence::Online,
            "a clock read before the state is not silence");

    HostState calm;
    calm.protection = Protection::Normal;
    Require(InAlert(Presence::Lost, nullptr), "lost is an alert");
    Require(!InAlert(Presence::Online, &calm), "normal and nothing raised is quiet");
    Require(!InAlert(Presence::Offline, nullptr) && !InAlert(Presence::NotHeard, nullptr),
            "offline is quiet");
    for (const Protection protection :
         {Protection::Warning, Protection::SafePower, Protection::Fault}) {
        HostState protecting = calm;
        protecting.protection = protection;
        Require(InAlert(Presence::Online, &protecting) && InAlert(Presence::Dim, &protecting),
                "protection that is not normal is an alert");
        Require(!InAlert(Presence::Offline, &protecting), "an offline host's last state is past");
    }
    HostState unknown = calm;
    unknown.protection = Protection::Unknown;
    Require(!InAlert(Presence::Online, &unknown), "no protection running is not an alert");
    HostState raised = calm;
    raised.records[5].raised = true;
    Require(InAlert(Presence::Online, &raised), "a record raised by the host's rules");

    const auto shown = [](const std::vector<Candidate>& hosts) {
        const Shown result = ChooseShown(hosts);
        return std::vector<std::size_t>(
            result.index.begin(), result.index.begin() + static_cast<std::ptrdiff_t>(result.count));
    };
    const Candidate on{Presence::Online, false};
    const Candidate off{Presence::Offline, false};
    const Candidate alert{Presence::Online, true};
    const Candidate lost{Presence::Lost, true};
    Require(shown({}).empty(), "no hosts, nothing shown");
    Require(shown({off, on}) == std::vector<std::size_t>{0, 1}, "fewer than four: all, in order");
    Require(shown({on, on, on, on, on, on}) == std::vector<std::size_t>{0, 1, 2, 3},
            "the first four in the reader's order");
    Require(shown({on, on, on, on, alert, on}) == std::vector<std::size_t>{0, 1, 2, 4},
            "a host in alert takes a place");
    Require(shown({on, on, on, on, lost, lost, lost, lost, lost}) ==
                std::vector<std::size_t>{4, 5, 6, 7},
            "alerts take every place when there are four");
    Require(shown({off, off, on, off, on, on, on}) == std::vector<std::size_t>{2, 4, 5, 6},
            "present hosts before offline ones");
    Require(shown({off, off, off, off, off}) == std::vector<std::size_t>{0, 1, 2, 3},
            "all offline: the first four");
    Require(shown({alert, off, off, on}) == std::vector<std::size_t>{0, 1, 2, 3},
            "chosen hosts keep the reader's order");

    Require(SegmentFor(off) == Segment::Offline && SegmentFor(on) == Segment::Online &&
                SegmentFor(alert) == Segment::Alert && SegmentFor(lost) == Segment::Alert &&
                SegmentFor({Presence::Dim, false}) == Segment::Online &&
                SegmentFor({Presence::NotHeard, false}) == Segment::Offline,
            "Tiny's segments");
    Require(SilenceText(42'900) == L"0:42" && SilenceText(725'000) == L"12:05" &&
                SilenceText(3'723'000) == L"1:02:03",
            "how long a host has been silent");
}

void TestPeerReplay() {
    using namespace gtg::peer;
    const std::int64_t now = 1'790'000'000'000;
    SealedHeader first;
    first.salt.fill(1);
    first.sequence = 1;
    first.sent_ms = now;
    ReplayGuard guard;
    Require(guard.Classify(first, now) == Verdict::NewSession, "the first session");
    guard.Accept(first);
    Require(guard.Classify(first, now) == Verdict::Replayed, "the same datagram again");
    SealedHeader next = first;
    next.sequence = 2;
    next.sent_ms = now + 1'000;
    Require(guard.Classify(next, now + 1'000) == Verdict::Current, "the next number");
    guard.Accept(next);
    Require(guard.Classify(first, now + 1'000) == Verdict::Replayed, "an older number");
    // Nothing moves before the tag verifies: classifying is not accepting.
    SealedHeader skip = next;
    skip.sequence = 1'000;
    Require(guard.Classify(skip, now) == Verdict::Current, "a later number is current");
    Require(guard.Classify(next, now) == Verdict::Replayed, "classifying moved nothing");

    SealedHeader restart;
    restart.salt.fill(2);
    restart.sequence = 1;
    restart.sent_ms = now + 5'000;
    Require(guard.Classify(restart, now + 5'000) == Verdict::NewSession,
            "a restarted sender is a new session");
    restart.sent_ms = now + 500;
    Require(guard.Classify(restart, now + 5'000) == Verdict::Older,
            "a session from before the one in use is refused");
    restart.sent_ms = now + 5'000 + kSessionWindowMs + 1;
    Require(guard.Classify(restart, now + 5'000) == Verdict::OutOfWindow, "a clock too far ahead");
    restart.sent_ms = now + 5'000 - kSessionWindowMs - 1;
    Require(guard.Classify(restart, now + 5'000) == Verdict::OutOfWindow, "a clock too far behind");

    // A receiver that restarted knows no session: a recording older than the
    // window is refused, one inside it is the accepted two-minute exposure.
    ReplayGuard fresh;
    Require(fresh.Classify(first, now + kSessionWindowMs + 1) == Verdict::OutOfWindow,
            "a recording older than two minutes");
    Require(fresh.Classify(first, now + kSessionWindowMs) == Verdict::NewSession,
            "a recording inside the window");
}

std::vector<std::uint8_t> Hex(const std::string_view text) {
    std::vector<std::uint8_t> bytes;
    const auto nibble = [](const char c) {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        return c - 'A' + 10;
    };
    for (std::size_t i = 0; i + 1 < text.size(); i += 2) {
        bytes.push_back(static_cast<std::uint8_t>(nibble(text[i]) * 16 + nibble(text[i + 1])));
    }
    return bytes;
}

template <std::size_t N>
std::vector<std::uint8_t> AsVector(const std::array<std::uint8_t, N>& bytes) {
    return {bytes.begin(), bytes.end()};
}

void TestPeerCryptoVectors() {
    using namespace gtg::peer;
    Crypto crypto;
    Require(crypto.Load(), "CNG and DPAPI load from System32");

    // RFC 4231, test case 2.
    const std::string_view jefe = "Jefe";
    const std::string_view question = "what do ya want for nothing?";
    const auto hmac = crypto.Hmac(
        Bytes(reinterpret_cast<const std::uint8_t*>(jefe.data()), jefe.size()),
        {Bytes(reinterpret_cast<const std::uint8_t*>(question.data()), 10),
         Bytes(reinterpret_cast<const std::uint8_t*>(question.data()) + 10, question.size() - 10)});
    Require(hmac && AsVector(*hmac) ==
                        Hex("5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843"),
            "HMAC-SHA256 over parts as one message (RFC 4231)");

    // RFC 5869, test cases 1 and 3: the first 32 bytes of output.
    const auto ikm = Hex("0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b");
    const auto hkdf =
        crypto.Hkdf(ikm, Hex("000102030405060708090a0b0c"), Hex("f0f1f2f3f4f5f6f7f8f9"));
    Require(hkdf && AsVector(*hkdf) ==
                        Hex("3cb25f25faacd57a90434f64d0362f2a2d2d0a90cf1a5a4c5db02d56ecc4c5bf"),
            "HKDF-SHA256 (RFC 5869, case 1)");
    const auto unsalted = crypto.Hkdf(ikm, {}, {});
    Require(unsalted && AsVector(*unsalted) ==
                            Hex("8da4e775a563c18f715f802a063c5a31b8a11f5c5ee1879ec3454e5f3c738d2d"),
            "HKDF-SHA256 without salt or info (RFC 5869, case 3)");

    // The GCM specification's test case 16: AES-256, 96-bit IV, with AAD.
    Key key{};
    const auto key_bytes = Hex("feffe9928665731c6d6a8f9467308308feffe9928665731c6d6a8f9467308308");
    std::copy(key_bytes.begin(), key_bytes.end(), key.begin());
    std::array<std::uint8_t, kNonceBytes> iv{};
    const auto iv_bytes = Hex("cafebabefacedbaddecaf888");
    std::copy(iv_bytes.begin(), iv_bytes.end(), iv.begin());
    const auto plaintext = Hex(
        "d9313225f88406e5a55909c5aff5269a86a7a9531534f7da2e4c303d8a318a721c3c0c95956809532fcf0e2449a6"
        "b525b16aedf5aa0de657ba637b39");
    const auto aad = Hex("feedfacedeadbeeffeedfacedeadbeefabaddad2");
    const auto expected = Hex(
        "522dc1f099567d07f47f37a32a84427d643a8cdcbfe5c0c97598a2bd2555d1aa8cb08e48590dbb3da7b08b1056"
        "828838c5f61e6393ba7a0abcc9f662");
    const auto expected_tag = Hex("76fc6ece0f4e1768cddf8853bb2d551b");
    std::vector<std::uint8_t> ciphertext(plaintext.size());
    std::array<std::uint8_t, 16> tag{};
    Require(crypto.Seal(key, iv, aad, plaintext, ciphertext, tag) && ciphertext == expected &&
                AsVector(tag) == expected_tag,
            "AES-256-GCM (GCM test case 16)");
    std::vector<std::uint8_t> opened(ciphertext.size());
    Require(crypto.Open(key, iv, aad, ciphertext, tag, opened) && opened == plaintext,
            "AES-256-GCM opens what it sealed");
    for (std::size_t i = 0; i < ciphertext.size() + aad.size() + tag.size(); ++i) {
        auto c = ciphertext;
        auto a = aad;
        auto t = tag;
        if (i < c.size()) {
            c[i] ^= 0x01;
        } else if (i < c.size() + a.size()) {
            a[i - c.size()] ^= 0x80;
        } else {
            t[i - c.size() - a.size()] ^= 0x10;
        }
        std::vector<std::uint8_t> out(c.size(), 0xEE);
        Require(
            !crypto.Open(key, iv, a, c, t, out) &&
                std::all_of(out.begin(), out.end(), [](const std::uint8_t b) { return b == 0; }),
            "any bit flipped in ciphertext, data or tag is refused and nothing is released");
    }
    Key other_key = key;
    other_key[31] ^= 1;
    std::vector<std::uint8_t> wrong(ciphertext.size());
    Require(!crypto.Open(other_key, iv, aad, ciphertext, tag, wrong), "a wrong key is refused");

    // RFC 5903, 8.1: ECDH on P-256 from both sides.
    const auto i = Hex("C88F01F510D9AC3F70A292DAA2316DE544E9AAB8AFE84049C62A9C57862D1433");
    const auto gix = Hex("DAD0B65394221CF9B051E1FECA5787D098DFE637FC90B9EF945D0C3772581180");
    const auto giy = Hex("5271A0461CDB8252D61F1C456FA3E59AB1F45B33ACCF5F58389E0577B8990BB3");
    const auto r = Hex("C6EF9C5D78AE012A011164ACB397CE2088685D8F06BF9BE0B283AB46476BEE53");
    const auto grx = Hex("D12DFB5289C8D4F81208B70270398C342296970A0BCCB74C736FC7554494BF63");
    const auto gry = Hex("56FBF3CA366CC23E8157854C13C58D6AAC23F046ADA30F8353E74F33039872AB");
    const auto girx = Hex("D6840F6B42F6EDAFD13116E0E12565202FEF8E9ECE7DCE03812464D04B9442DE");
    const auto initiator = crypto.ImportEphemeral(i, gix, giy);
    const auto responder = crypto.ImportEphemeral(r, grx, gry);
    Require(initiator && responder, "known ECDH keys import");
    Require(
        AsVector(initiator->PublicKey()) ==
            [&] {
                auto both = gix;
                both.insert(both.end(), giy.begin(), giy.end());
                return both;
            }(),
        "a public key is X then Y, big-endian");
    const auto shared_i = crypto.Agree(*initiator, responder->PublicKey());
    const auto shared_r = crypto.Agree(*responder, initiator->PublicKey());
    Require(shared_i && shared_r && AsVector(*shared_i) == girx && AsVector(*shared_r) == girx,
            "ECDH P-256 agrees on the RFC 5903 secret from both sides");
    std::array<std::uint8_t, 64> off_curve{};
    off_curve[31] = 1;
    off_curve[63] = 1;
    Require(!crypto.Agree(*initiator, off_curve), "a point not on the curve is refused");

    const auto a = crypto.GenerateEphemeral();
    const auto b = crypto.GenerateEphemeral();
    Require(a && b && a->PublicKey() != b->PublicKey(), "fresh ephemeral keys differ");
    const auto ab = crypto.Agree(*a, b->PublicKey());
    const auto ba = crypto.Agree(*b, a->PublicKey());
    Require(ab && ba && *ab == *ba, "fresh keys agree");
    std::array<std::uint8_t, 32> first{};
    std::array<std::uint8_t, 32> second{};
    Require(crypto.Random(first) && crypto.Random(second) && first != second, "randomness");

    const auto blob = crypto.Protect(key);
    Require(blob && blob->size() > key.size() &&
                std::search(blob->begin(), blob->end(), key.begin(), key.end()) == blob->end(),
            "DPAPI protects a key without carrying it in the clear");
    const auto back = crypto.Unprotect(*blob);
    Require(back && *back == std::vector<std::uint8_t>(key.begin(), key.end()),
            "DPAPI gives the key back");
    auto damaged = *blob;
    damaged[damaged.size() / 2] ^= 0x01;
    Require(!crypto.Unprotect(damaged), "a damaged blob gives nothing back");
}

void TestPeerPairing() {
    using namespace gtg::peer;
    Crypto crypto;
    Require(crypto.Load(), "crypto loads");
    const Self issuer{0x1111, L"Issuer"};
    const Self joiner{0x2222, L"Joiner"};
    const Self late{0x3333, L"Late"};
    const std::uint64_t now = 5'000'000;

    auto invitation = Invitation::Create(crypto, {127, 0, 0, 1}, 47'810, now);
    Require(invitation && invitation->StatusAt(now) == Invitation::Status::Live, "a live code");
    const auto pasted = ParseCode(std::string_view(FormatCode(invitation->Code())));
    Require(pasted.code.has_value(), "the shown code reads");
    const auto joining = Joining::Start(crypto, joiner, *pasted.code);
    Require(joining.has_value(), "a joiner starts");
    const auto request = ReadPairRequest(joining->Request());
    Require(request.has_value(), "the request reads");

    const auto answer =
        invitation->Handle(crypto, issuer, *request, joining->Request(), now + 2'000);
    Require(answer.reply && answer.paired && answer.paired->peer == joiner.id &&
                answer.paired->name == L"Joiner",
            "the issuer accepts and learns the joiner");
    Require(invitation->StatusAt(now + 2'000) == Invitation::Status::Spent &&
                invitation->JoinedBy() == L"Joiner",
            "the code is spent");
    const auto accept = ReadPairAccept(*answer.reply);
    Require(accept.has_value(), "the accept reads");
    const auto joined = joining->Handle(crypto, joiner, *accept, *answer.reply);
    Require(joined.outcome == Joining::Outcome::Paired && joined.paired &&
                joined.paired->peer == issuer.id && joined.paired->name == L"Issuer",
            "the joiner learns the issuer");
    Require(joined.paired->key == answer.paired->key, "both sides derive one pair key");

    const auto again =
        invitation->Handle(crypto, issuer, *request, joining->Request(), now + 3'000);
    Require(again.reply && *again.reply == *answer.reply && !again.paired,
            "a repeated request gets the same accept and pairs nothing more");

    // Someone else who has the code, after it was spent, is told so.
    const auto second = Joining::Start(crypto, late, *pasted.code);
    const auto refused = invitation->Handle(crypto, issuer, *ReadPairRequest(second->Request()),
                                            second->Request(), now + 4'000);
    Require(refused.reply && !refused.paired, "a spent code answers without pairing");
    Require(second->Handle(crypto, late, *ReadPairAccept(*refused.reply), *refused.reply).outcome ==
                Joining::Outcome::Used,
            "the late joiner reads 'already used'");

    // Every byte of the accept is covered: a changed accept pairs nothing.
    for (std::size_t i = 0; i < kPairAcceptBytes; ++i) {
        auto bad = *answer.reply;
        bad[i] ^= 0x04;
        const auto parsed = ReadPairAccept(bad);
        if (!parsed) continue;
        Require(joining->Handle(crypto, joiner, *parsed, bad).outcome == Joining::Outcome::Ignored,
                "a changed accept is ignored");
    }
    Require(
        second->Handle(crypto, late, *accept, *answer.reply).outcome == Joining::Outcome::Ignored,
        "an accept for another joiner is ignored");

    // A request for another code is not counted against this one; a request
    // with this code's id and a bad MAC is, and the fifth spends it.
    auto fresh = Invitation::Create(crypto, {127, 0, 0, 1}, 47'811, now);
    const auto stranger = Joining::Start(crypto, late, invitation->Code());
    Require(!fresh->Handle(crypto, issuer, *ReadPairRequest(stranger->Request()),
                           stranger->Request(), now)
                    .reply &&
                fresh->BadMacs() == 0,
            "a request for another code is ignored and not counted");
    const auto honest = Joining::Start(crypto, joiner, fresh->Code());
    for (int attempt = 1; attempt <= kBadMacLimit; ++attempt) {
        auto forged = honest->Request();
        forged[kPrefixBytes + kCodeIdBytes + attempt] ^= 0x01;  // the public key
        const auto answer_forged =
            fresh->Handle(crypto, issuer, *ReadPairRequest(forged), forged, now + 1'000);
        Require(!answer_forged.reply && !answer_forged.paired, "a bad MAC gets no answer");
        Require(
            fresh->StatusAt(now + 1'000) ==
                (attempt < kBadMacLimit ? Invitation::Status::Live : Invitation::Status::Burned),
            "the fifth bad MAC spends the code");
    }
    const auto too_late = fresh->Handle(crypto, issuer, *ReadPairRequest(honest->Request()),
                                        honest->Request(), now + 2'000);
    Require(!too_late.paired &&
                honest->Handle(crypto, joiner, *ReadPairAccept(*too_late.reply), *too_late.reply)
                        .outcome == Joining::Outcome::Used,
            "after five bad MACs the holder is told the code is used");

    // Ten minutes.
    auto expiring = Invitation::Create(crypto, {127, 0, 0, 1}, 47'812, now);
    const auto slow = Joining::Start(crypto, joiner, expiring->Code());
    Require(expiring->StatusAt(now + kCodeLifetimeMs - 1) == Invitation::Status::Live,
            "live until ten minutes");
    const auto expired = expiring->Handle(crypto, issuer, *ReadPairRequest(slow->Request()),
                                          slow->Request(), now + kCodeLifetimeMs);
    Require(
        !expired.paired &&
            slow->Handle(crypto, joiner, *ReadPairAccept(*expired.reply), *expired.reply).outcome ==
                Joining::Outcome::Expired,
        "an expired code says so");

    // A GTG that pastes its own code pairs with nothing.
    auto own = Invitation::Create(crypto, {127, 0, 0, 1}, 47'813, now);
    const auto self_join = Joining::Start(crypto, issuer, own->Code());
    const auto self_answer = own->Handle(crypto, issuer, *ReadPairRequest(self_join->Request()),
                                         self_join->Request(), now);
    Require(
        !self_answer.reply && !self_answer.paired && own->StatusAt(now) == Invitation::Status::Live,
        "its own code is not spent on itself");

    // Sealed traffic under the pair key.
    const Key pair_key = answer.paired->key;
    SealedHeader header;
    header.sender = issuer.id;
    header.salt.fill(0x5A);
    header.sequence = 9;
    header.sent_ms = 1'790'000'000'000;
    const auto to_joiner = DirectionKey(crypto, pair_key, header.salt, issuer.id, joiner.id);
    const auto to_issuer = DirectionKey(crypto, pair_key, header.salt, joiner.id, issuer.id);
    Require(to_joiner && to_issuer && *to_joiner != *to_issuer, "each direction has its own key");
    const HostState state = SampleHostState();
    const auto sealed = SealState(crypto, *to_joiner, header, state);
    Require(sealed.has_value(), "a state seals");
    const auto read_header = ReadSealedHeader(*sealed);
    const auto opened = OpenSealed(crypto, *to_joiner, *read_header, *sealed);
    Require(opened.authentic && opened.state && *opened.state == state, "and opens");
    Require(!OpenSealed(crypto, *to_issuer, *read_header, *sealed).authentic,
            "the other direction's key does not open it");
    for (std::size_t at = 0; at < sealed->size(); ++at) {
        auto bad = *sealed;
        bad[at] ^= 0x20;
        const auto bad_header = ReadSealedHeader(bad);
        if (!bad_header) continue;
        const auto key_for = DirectionKey(crypto, pair_key, bad_header->salt, issuer.id, joiner.id);
        Require(!OpenSealed(crypto, *key_for, *bad_header, bad).authentic,
                "any byte changed in a state datagram is refused");
    }
    const auto goodbye = SealGoodbye(crypto, *to_joiner, header);
    const auto goodbye_header = ReadSealedHeader(*goodbye);
    const auto bye = OpenSealed(crypto, *to_joiner, *goodbye_header, *goodbye);
    Require(goodbye && bye.authentic && !bye.state, "a goodbye is sealed too");
    auto forged_bye = *goodbye;
    forged_bye[kGoodbyeBytes - 1] ^= 1;
    Require(!OpenSealed(crypto, *to_joiner, *goodbye_header, forged_bye).authentic,
            "a forged goodbye is refused");
}

// The link's table once `ready` holds, publishing each link's local state on
// every try as the dashboard does once a second.
template <typename Ready>
std::optional<gtg::peer::Table> AwaitTable(
    gtg::peer::Link& link, Ready ready,
    const std::vector<std::pair<gtg::peer::Link*, gtg::peer::HostState>>& feed,
    const std::uint64_t timeout_ms = 5'000) {
    const std::uint64_t deadline = GetTickCount64() + timeout_ms;
    while (GetTickCount64() < deadline) {
        for (const auto& [sender, state] : feed)
            sender->Publish(state);
        if (auto table = link.TryRead(); table && ready(*table)) return table;
        Sleep(20);
    }
    return std::nullopt;
}

void TestPeerLoopback() {
    using namespace gtg::peer;
    const std::array<std::uint8_t, 4> loopback{127, 0, 0, 1};
    // A port nothing holds right now: one Windows just handed out and took back.
    WSADATA wsa{};
    Require(WSAStartup(MAKEWORD(2, 2), &wsa) == 0, "the test's own socket");
    const auto free_port = [] {
        const SOCKET probe = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        sockaddr_in local{};
        local.sin_family = AF_INET;
        local.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        int size = sizeof(local);
        bind(probe, reinterpret_cast<const sockaddr*>(&local), sizeof(local));
        getsockname(probe, reinterpret_cast<sockaddr*>(&local), &size);
        closesocket(probe);
        return ntohs(local.sin_port);
    };
    const std::uint16_t port = free_port();
    std::mutex saved_mutex;
    std::vector<Pair> saved_b;
    Link a;
    Link b;
    Link::Options alpha;
    alpha.self = {0xA1, L"Alpha"};
    alpha.port = port;
    alpha.bind_address = loopback;
    Link::Options bravo;
    bravo.self = {0xB2, L"Bravo"};
    bravo.port = static_cast<std::uint16_t>(port + 1);
    bravo.bind_address = loopback;
    bravo.save = [&](const Pair& pair) {
        std::lock_guard lock(saved_mutex);
        const auto found = std::find_if(saved_b.begin(), saved_b.end(),
                                        [&](const Pair& p) { return p.peer == pair.peer; });
        if (found != saved_b.end()) {
            *found = pair;
        } else {
            saved_b.push_back(pair);
        }
    };
    Require(a.Start(alpha) && b.Start(bravo), "two links start");

    HostState from_a = SampleHostState();
    HostState from_b = SampleHostState();
    from_b.protection = Protection::Normal;
    from_b.records[0].raised = false;
    const std::vector<std::pair<Link*, HostState>> feed{{&a, from_a}, {&b, from_b}};

    const auto started =
        AwaitTable(a, [](const Table& t) { return t.status == LinkStatus::Running; }, feed);
    Require(started && !started->listening && started->port != port,
            "a GTG that has issued no code does not bind the fixed port");
    a.CreateCode();
    const auto issued =
        AwaitTable(a, [](const Table& t) { return t.invitation.has_value(); }, feed);
    Require(issued && issued->listening && issued->port == port,
            "issuing a code binds the fixed port");
    const auto code = ParseCode(std::string_view(issued->invitation->code));
    Require(code.code && code.code->address == loopback && code.code->port == port,
            "the code says where to go");

    b.Join(*code.code);
    const auto joined = AwaitTable(
        b, [](const Table& t) { return t.joining && t.joining->status == JoinStatus::Paired; },
        feed);
    Require(joined && joined->joining->issuer == L"Alpha" && joined->hosts.size() == 1 &&
                !joined->listening,
            "the joiner pairs, sending from an ephemeral port");
    const auto spent = AwaitTable(
        a,
        [](const Table& t) {
            return t.hosts.size() == 1 && t.invitation &&
                   t.invitation->status == Invitation::Status::Spent;
        },
        feed);
    Require(spent && spent->hosts[0].name == L"Bravo" && spent->invitation->joined_by == L"Bravo",
            "the issuer has the joiner and its code is spent");

    // State, both ways, with each sender's own name.
    const auto a_hears_b = AwaitTable(
        a,
        [](const Table& t) {
            return t.hosts.size() == 1 && t.hosts[0].state && t.hosts[0].state->name == L"Bravo";
        },
        feed);
    Require(a_hears_b && a_hears_b->hosts[0].state->protection == Protection::Normal &&
                a_hears_b->hosts[0].state->records == from_b.records,
            "the issuer receives the joiner's state");
    const auto b_hears_a = AwaitTable(
        b,
        [](const Table& t) {
            return t.hosts.size() == 1 && t.hosts[0].state && t.hosts[0].state->name == L"Alpha";
        },
        feed);
    Require(b_hears_a && b_hears_a->hosts[0].state->records == from_a.records &&
                b_hears_a->hosts[0].state->protection == Protection::Warning,
            "the joiner receives the issuer's state");
    Require(PresenceAt(a_hears_b->hosts[0].heard, GetTickCount64()) == Presence::Online,
            "both online");
    Require(a_hears_b->hosts[0].endpoint && a_hears_b->hosts[0].endpoint->port == joined->port,
            "the issuer learned the joiner's port from its datagrams");

    // A second code holder after it was spent, and a GTG's own code.
    Link c;
    Link::Options charlie;
    charlie.self = {0xC3, L"Charlie"};
    charlie.port = static_cast<std::uint16_t>(port + 2);
    charlie.bind_address = loopback;
    Require(c.Start(charlie), "a third link starts");
    c.Join(*code.code);
    const auto late = AwaitTable(
        c, [](const Table& t) { return t.joining && t.joining->status == JoinStatus::Used; }, feed);
    Require(late && late->hosts.empty(), "a spent code tells its holder it is used");
    c.Stop();
    a.CreateCode();
    const auto second_code = AwaitTable(
        a,
        [](const Table& t) {
            return t.invitation && t.invitation->status == Invitation::Status::Live;
        },
        feed);
    const auto own = ParseCode(std::string_view(second_code->invitation->code));
    a.Join(*own.code);
    const auto refused_own = AwaitTable(
        a, [](const Table& t) { return t.joining && t.joining->status == JoinStatus::Own; }, feed);
    Require(refused_own && refused_own->hosts.size() == 1, "a GTG does not pair with itself");
    a.CancelCode();

    // Traffic that is not a paired host's changes nothing.
    const SOCKET raw = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    sockaddr_in to{};
    to.sin_family = AF_INET;
    to.sin_port = htons(port);
    to.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    const auto send_raw = [&](const std::span<const std::uint8_t> bytes) {
        sendto(raw, reinterpret_cast<const char*>(bytes.data()), static_cast<int>(bytes.size()), 0,
               reinterpret_cast<const sockaddr*>(&to), sizeof(to));
    };
    std::array<std::uint8_t, 300> junk{};
    for (std::size_t i = 0; i < junk.size(); ++i)
        junk[i] = static_cast<std::uint8_t>(i * 7);
    send_raw(junk);
    SealedHeader forged;
    forged.sender = 0xB2;
    forged.salt.fill(0x33);
    forged.sequence = 1;
    forged.sent_ms = 1'790'000'000'000;
    std::array<std::uint8_t, kStateBytes> fake{};
    const auto fake_head = WriteSealedHeader(forged);
    std::memcpy(fake.data(), fake_head.data(), fake_head.size());
    send_raw(fake);
    for (int i = 0; i < 300; ++i)
        send_raw(fake);
    closesocket(raw);
    const std::uint64_t after_flood = GetTickCount64();
    const auto still = AwaitTable(
        a,
        [after_flood](const Table& t) {
            return t.hosts.size() == 1 && t.hosts[0].heard.state_ms &&
                   *t.hosts[0].heard.state_ms > after_flood + 1'000;
        },
        feed);
    Require(still && still->hosts[0].state->name == L"Bravo" &&
                still->hosts[0].state->records == from_b.records,
            "after junk, a forged state and a flood, the host is still heard as itself");

    // A goodbye is offline; the host comes back from its stored pair.
    b.Stop();
    const auto gone = AwaitTable(
        a, [](const Table& t) { return t.hosts.size() == 1 && t.hosts[0].heard.goodbye_ms; },
        {{&a, from_a}});
    Require(gone && PresenceAt(gone->hosts[0].heard, GetTickCount64()) == Presence::Offline,
            "a host that stops says goodbye: offline, not lost");
    {
        std::lock_guard lock(saved_mutex);
        Require(saved_b.size() == 1 && saved_b[0].peer == 0xA1 && !saved_b[0].issued &&
                    saved_b[0].endpoint && saved_b[0].endpoint->port == port,
                "the joiner saved the pair with the issuer's address");
        bravo.pairs = saved_b;
    }
    Link b2;
    Require(b2.Start(bravo), "the joiner starts again");
    const std::vector<std::pair<Link*, HostState>> feed2{{&a, from_a}, {&b2, from_b}};
    const auto back = AwaitTable(
        a,
        [](const Table& t) {
            return t.hosts.size() == 1 &&
                   PresenceAt(t.hosts[0].heard, GetTickCount64()) == Presence::Online;
        },
        feed2);
    Require(back.has_value(), "a host that restarts is back, in a new session");

    // Silence without a goodbye is what becomes lost.
    b2.Mute();
    Sleep(300);
    const auto before = a.TryRead();
    Sleep(1'500);
    const auto after = a.TryRead();
    // The goodbye heard is the first instance's, from before the restart.
    Require(before && after && after->hosts[0].heard.state_ms == before->hosts[0].heard.state_ms &&
                *after->hosts[0].heard.goodbye_ms < *after->hosts[0].heard.state_ms &&
                PresenceAt(after->hosts[0].heard,
                           *after->hosts[0].heard.state_ms + kLostAfterMs + 1) == Presence::Lost,
            "a host that goes silent without a goodbye becomes lost");

    // The receiving side's replay rules, through the link: datagrams sealed
    // with the real pair key, sent from somewhere else.
    Crypto crypto;
    Require(crypto.Load(), "crypto for the replay checks");
    Key pair_key{};
    {
        std::lock_guard lock(saved_mutex);
        pair_key = saved_b[0].key;
    }
    const SOCKET replayer = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    const auto seal_and_send = [&](const std::array<std::uint8_t, kSaltBytes>& salt,
                                   const std::uint64_t sequence, const std::int64_t sent_ms,
                                   const wchar_t* name) {
        SealedHeader header;
        header.sender = 0xB2;
        header.salt = salt;
        header.sequence = sequence;
        header.sent_ms = sent_ms;
        HostState state = from_b;
        state.name = name;
        const auto key = DirectionKey(crypto, pair_key, salt, 0xB2, 0xA1);
        const auto datagram = SealState(crypto, *key, header, state);
        sendto(replayer, reinterpret_cast<const char*>(datagram->data()),
               static_cast<int>(datagram->size()), 0, reinterpret_cast<const sockaddr*>(&to),
               sizeof(to));
        return *datagram;
    };
    const auto shows = [&](const wchar_t* name) {
        Sleep(250);
        const auto table =
            AwaitTable(a, [](const Table& t) { return t.hosts.size() == 1 && t.hosts[0].state; },
                       {{&a, from_a}});
        return table && table->hosts[0].state->name == name;
    };
    std::array<std::uint8_t, kSaltBytes> salt{};
    salt.fill(0x77);
    const std::int64_t wall = [] {
        FILETIME time{};
        GetSystemTimeAsFileTime(&time);
        return static_cast<std::int64_t>(
                   ((static_cast<std::uint64_t>(time.dwHighDateTime) << 32) | time.dwLowDateTime) /
                   10'000) -
               11'644'473'600'000LL;
    }();
    const auto ten = seal_and_send(salt, 10, wall, L"Session-10");
    Require(shows(L"Session-10"), "a new session sealed with the pair key is accepted");
    seal_and_send(salt, 9, wall, L"Session-9");
    Require(shows(L"Session-10"), "a lower number in the session is refused");
    seal_and_send(salt, 11, wall, L"Session-11");
    Require(shows(L"Session-11"), "a higher number is accepted");
    sendto(replayer, reinterpret_cast<const char*>(ten.data()), static_cast<int>(ten.size()), 0,
           reinterpret_cast<const sockaddr*>(&to), sizeof(to));
    Require(shows(L"Session-11"), "a datagram sent again is refused");
    std::array<std::uint8_t, kSaltBytes> earlier{};
    earlier.fill(0x66);
    seal_and_send(earlier, 50, wall - 5'000, L"Earlier");
    Require(shows(L"Session-11"), "a session from before the one in use is refused");
    std::array<std::uint8_t, kSaltBytes> skewed{};
    skewed.fill(0x55);
    seal_and_send(skewed, 1, wall + kSessionWindowMs + 60'000, L"Skewed");
    Require(shows(L"Session-11"), "a session whose clock is too far ahead is refused");
    const auto skew = AwaitTable(
        a, [](const Table& t) { return t.hosts.size() == 1 && t.hosts[0].clock_skew_ms; },
        {{&a, from_a}});
    Require(skew && *skew->hosts[0].clock_skew_ms > kSessionWindowMs,
            "and the dialog can say how far the clocks disagree");
    closesocket(replayer);
    WSACleanup();
    b2.Stop();

    a.Remove(0xB2);
    const auto removed =
        AwaitTable(a, [](const Table& t) { return t.hosts.empty(); }, {{&a, from_a}});
    Require(removed && !removed->listening, "a removed pair is gone, and so is the fixed port");
    a.Stop();
}

void TestPeerPortInUse() {
    using namespace gtg::peer;
    WSADATA wsa{};
    Require(WSAStartup(MAKEWORD(2, 2), &wsa) == 0, "the test's own socket");
    // Another program holds the fixed port.
    const SOCKET holder = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    BOOL exclusive = TRUE;
    setsockopt(holder, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&exclusive),
               sizeof(exclusive));
    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    int size = sizeof(local);
    Require(bind(holder, reinterpret_cast<const sockaddr*>(&local), sizeof(local)) == 0 &&
                getsockname(holder, reinterpret_cast<sockaddr*>(&local), &size) == 0,
            "a port is held");
    Link link;
    Link::Options options;
    options.self = {0xD4, L"Delta"};
    options.port = ntohs(local.sin_port);
    options.bind_address = std::array<std::uint8_t, 4>{127, 0, 0, 1};
    Require(link.Start(options), "the link starts");
    link.CreateCode();
    const auto taken = AwaitTable(
        link, [](const Table& t) { return t.invitation && t.status == LinkStatus::PortInUse; }, {});
    Require(taken && !taken->listening && taken->port != options.port,
            "a taken port is said, and the link sends from another");
    link.CancelCode();
    const auto released = AwaitTable(
        link, [](const Table& t) { return !t.invitation && t.status == LinkStatus::Running; }, {});
    Require(released && !released->listening,
            "without a code the fixed port is not wanted, and the warning goes");
    link.Stop();
    closesocket(holder);
    WSACleanup();
}

void TestTinyDescribeBar() {
    using namespace gtg::tray;
    tiny::BarInputs in;
    in.temperature_c = 85.0;
    in.power_w = 450.0;
    in.gpu_percent = 80.0;
    in.cpu_percent = 10.0;
    in.ram_percent = 50.0;
    in.network_bytes_per_second = 0.5;
    in.trigger_temperature_c = 85;
    in.safe_power_w = 300;
    const auto close_to = [](const std::optional<double> value, const double expected) {
        return value && std::abs(*value - expected) < 1e-9;
    };
    auto bar = tiny::DescribeBar(compact::Metric::Temperature, in);
    Require(close_to(bar.level, 55.0 / 75.0) && close_to(bar.tick, 55.0 / 75.0) && !bar.raised,
            "temperature on its lane's axis, the trigger as its tick");
    in.warning = true;
    Require(tiny::DescribeBar(compact::Metric::Temperature, in).raised &&
                !tiny::DescribeBar(compact::Metric::Power, in).raised,
            "a warning raises temperature, not power");
    in.warning = false;
    in.safe_power = true;
    bar = tiny::DescribeBar(compact::Metric::Power, in);
    Require(close_to(bar.level, 0.75) && close_to(bar.tick, 0.5) && bar.raised &&
                tiny::DescribeBar(compact::Metric::Temperature, in).raised,
            "safe power raises power and temperature; power stands on a 600 W axis");
    in.top_app_alerts = gtg::topapp::Bit(gtg::topapp::Record::Gpu);
    bar = tiny::DescribeBar(compact::Metric::Gpu, in);
    Require(close_to(bar.level, 0.8) && close_to(bar.tick, 0.7) && bar.raised &&
                !tiny::DescribeBar(compact::Metric::Cpu, in).raised,
            "a top-app alert raises its own record only");
    bar = tiny::DescribeBar(compact::Metric::Vram, in);
    Require(!bar.level && bar.tick && !bar.raised, "no reading: an empty track with its tick");
    in.host_memory_low = true;
    Require(tiny::DescribeBar(compact::Metric::Ram, in).raised, "Windows' low memory raises RAM");
    in.ram_percent.reset();
    Require(!tiny::DescribeBar(compact::Metric::Ram, in).raised, "but not without a reading");
    bar = tiny::DescribeBar(compact::Metric::Network, in);
    Require(close_to(bar.level, 0.5) && !bar.tick && !bar.raised, "network against a peak of 1");
    Require(!tiny::DescribeBar(compact::Metric::Fps, in).level, "no FPS, no bar");
    in.fps = tiny::FpsStability{30.0, 60.0, -0.5, tiny::FpsLevel(-0.5), tiny::Severity::Alarm};
    bar = tiny::DescribeBar(compact::Metric::Fps, in);
    Require(bar.raised && bar.alarm && close_to(bar.tick, tiny::kFpsReferenceLevel),
            "FPS far from steady raises and turns white");
}

void TestHostsBridge() {
    using namespace gtg::tray;
    hosts::LocalState local;
    local.bars.temperature_c = 70.0;
    local.bars.power_w = 300.0;
    local.bars.cpu_percent = 95.0;
    local.bars.top_app_alerts = gtg::topapp::Bit(gtg::topapp::Record::Cpu);
    local.bars.fps =
        tiny::FpsStability{60.0, 60.0, 0.0, tiny::FpsLevel(0.0), tiny::Severity::Steady};
    local.network_received = 1'000.0;
    local.network_sent = 250.0;
    local.bars.network_bytes_per_second = 1'000.0;
    local.bars.network_peak = 2'000.0;
    local.visual = hosts::Visual::Protected;
    local.version = {1, 2, 0};
    const auto state = hosts::Build(local);
    using gtg::peer::Record;
    const auto& cpu = state.records[static_cast<std::size_t>(Record::Cpu)];
    const auto& temperature = state.records[static_cast<std::size_t>(Record::Temperature)];
    const auto& network = state.records[static_cast<std::size_t>(Record::Network)];
    const auto& fps = state.records[static_cast<std::size_t>(Record::Fps)];
    Require(state.protection == gtg::peer::Protection::SafePower && state.version[1] == 2,
            "protection and version");
    Require(cpu.raised && cpu.value == 95.0F && temperature.raised && temperature.value == 70.0F,
            "each record as this machine's Tiny draws it, with its reading");
    Require(network.value == 1'000.0F && network.value2 == 250.0F && network.level == 0.5F,
            "network carries both directions");
    Require(fps.value == 60.0F && !fps.raised && fps.level, "a steady FPS");
    Require(!state.records[static_cast<std::size_t>(Record::Vram)].value &&
                !state.records[static_cast<std::size_t>(Record::Ram)].level,
            "what this machine does not read is not sent as zero");
    const auto read = gtg::peer::ReadStateBody(gtg::peer::WriteStateBody(state));
    Require(read && read->records[static_cast<std::size_t>(Record::Cpu)].raised &&
                read->protection == gtg::peer::Protection::SafePower,
            "what is built is what the wire carries");
    Require(hosts::ToWire(hosts::Visual::Neutral) == gtg::peer::Protection::Unknown &&
                hosts::ToWire(hosts::Visual::Armed) == gtg::peer::Protection::Normal,
            "no protection running is not normal");
}

void TestPeerAdmission() {
    using gtg::peer::Admission;
    using gtg::peer::Endpoint;
    Admission admission;
    const Endpoint a{{10, 0, 0, 1}, 5000};
    const Endpoint b{{10, 0, 0, 2}, 5000};
    for (int i = 0; i < Admission::kPerSourcePerSecond; ++i) {
        Require(admission.Admit(a, 10'000), "a host's datagrams within its share");
    }
    Require(!admission.Admit(a, 10'500), "beyond its share in one second");
    Require(admission.Admit(b, 10'500), "another source has its own share");
    Require(admission.Admit(a, 11'000), "the next second starts over");
    Admission flood;
    int admitted = 0;
    for (int i = 0; i < 1'000; ++i) {
        const Endpoint source{
            {10, 0, static_cast<std::uint8_t>(i / 250), static_cast<std::uint8_t>(i % 250)}, 4000};
        if (flood.Admit(source, 20'000)) ++admitted;
    }
    Require(admitted == Admission::kTotalPerSecond, "a flood from many sources is capped");
    Require(!flood.Admit(a, 20'999), "the cap holds for the whole second");
    Require(flood.Admit(a, 21'000), "and lifts after it");
}

}  // namespace

void TestFeatureNoticeSerial() {
    using namespace gtg::settings;
    Require(NeedsFeatureNotice(false, 0), "first use receives current feature notice");
    Require(NeedsFeatureNotice(true, 0), "old boolean false cannot acknowledge feature serial");
    Require(NeedsFeatureNotice(true, kFeatureNoticeSerial + 1), "different feature serial notifies");
    Require(!NeedsFeatureNotice(true, kFeatureNoticeSerial), "acknowledged feature serial does not repeat");
}

void TestSettingsTextBaseline() {
    Require(!gtg::timing::IsSlowUiPhase(1000, 1099) &&
            gtg::timing::IsSlowUiPhase(1000, 1100) &&
            gtg::timing::IsSlowUiPhase(1000, 11000) &&
            !gtg::timing::IsSlowUiPhase(1000, 999),
            "slow-stage timing handles threshold and backwards readings");
    Require(gtg::tray::ButtonCornerDiameter(96) == 8 &&
            gtg::tray::ButtonCornerDiameter(168) == 14 &&
            gtg::tray::ButtonCornerDiameter(192) == 16,
            "owner draw corner diameter scales with dialog DPI");
    struct Windows {
        HWND parent{};
        HFONT font{};
        ~Windows() { if (parent) DestroyWindow(parent); if (font) DeleteObject(font); }
    } owned;
    owned.parent = CreateWindowExW(0, L"STATIC", L"owned baseline oracle", WS_POPUP,
        0, 0, 600, 160, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    Require(owned.parent != nullptr, "owned baseline parent");
    const auto label = CreateWindowExW(0, L"STATIC", L"Working Limit (W)", WS_CHILD,
        10, 40, 200, 30, owned.parent, nullptr, GetModuleHandleW(nullptr), nullptr);
    const auto edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"350",
        WS_CHILD | ES_NUMBER | ES_AUTOHSCROLL, 220, 20, 100, 40,
        owned.parent, nullptr, GetModuleHandleW(nullptr), nullptr);
    Require(label && edit, "native single-line controls");
    for (int dpi : {96, 120, 144, 168, 192, 96}) {
        const auto replacement = CreateFontW(-MulDiv(9, dpi, 72), 0, 0, 0, FW_NORMAL, FALSE, FALSE,
            FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            DEFAULT_QUALITY, DEFAULT_PITCH, L"Segoe UI");
        Require(replacement != nullptr, "DPI test font");
        SendMessageW(label, WM_SETFONT, reinterpret_cast<WPARAM>(replacement), FALSE);
        SendMessageW(edit, WM_SETFONT, reinterpret_cast<WPARAM>(replacement), FALSE);
        if (owned.font) DeleteObject(owned.font);
        owned.font = replacement;
        SetWindowPos(label, nullptr, MulDiv(10, dpi, 96), MulDiv(40, dpi, 96),
            MulDiv(200, dpi, 96), MulDiv(20, dpi, 96), SWP_NOZORDER);
        SetWindowPos(edit, nullptr, MulDiv(220, dpi, 96), MulDiv(20, dpi, 96),
            MulDiv(100, dpi, 96), MulDiv(24, dpi, 96), SWP_NOZORDER);
        RECT original_label{};
        GetWindowRect(label, &original_label);
        Require(gtg::tray::AlignEditToLabel(owned.parent, label, edit), "baseline alignment succeeds");
        RECT unchanged_label{};
        GetWindowRect(label, &unchanged_label);
        Require(EqualRect(&original_label, &unchanged_label), "correct label position and size must never change");
        RECT text{}, label_rect{};
        SendMessageW(edit, EM_GETRECT, 0, reinterpret_cast<LPARAM>(&text));
        MapWindowPoints(edit, owned.parent, reinterpret_cast<POINT*>(&text), 2);
        GetWindowRect(label, &label_rect);
        MapWindowPoints(nullptr, owned.parent, reinterpret_cast<POINT*>(&label_rect), 2);
        Require(label_rect.top == text.top, "native edit and same-font label baseline aligned at every DPI");
        RECT edit_rect{}, edit_client{};
        GetWindowRect(edit, &edit_rect); GetClientRect(edit, &edit_client);
        TEXTMETRICW edit_metric{};
        Require(gtg::tray::ControlTextMetrics(edit, edit_metric), "native edit font metrics");
        Require(edit_client.bottom >= edit_metric.tmHeight &&
            edit_rect.bottom - edit_rect.top < MulDiv(24, dpi, 96),
            "textbox fits a complete font line without the oversized original height");
        Require(gtg::tray::AlignEditToLabel(owned.parent, label, edit), "alignment is repeatable");
        RECT repeated{}; GetWindowRect(label, &repeated);
        MapWindowPoints(nullptr, owned.parent, reinterpret_cast<POINT*>(&repeated), 2);
        Require(repeated.top == label_rect.top, "reapplication does not drift");
        RECT repeated_edit{}; GetWindowRect(edit, &repeated_edit);
        Require(EqualRect(&edit_rect, &repeated_edit), "textbox bounds do not drift on reapplication");
        wchar_t value[16]{}; GetWindowTextW(edit, value, 16);
        Require(std::wstring(value) == L"350" && !(GetWindowLongPtrW(edit, GWL_STYLE) & ES_MULTILINE),
            "numeric value and single-line semantics unchanged");
    }
}

namespace {

constexpr std::string_view kHostsLogRun = "11111111-1111-4111-8111-111111111111";

gtg::hostlog::Frame HostsLogFrame(const gtg::hostlog::Status status) {
    using namespace std::chrono;
    gtg::hostlog::Frame frame;
    frame.seq = 3;
    frame.captured_at = sys_days{year{2026} / 11 / 1} + hours{4} + milliseconds{5};
    frame.now_ms = 100'000;
    frame.observer_id = 1;
    frame.observer_name = L"Workstation";
    frame.version = {1, 1, 0};
    frame.enabled = status != gtg::hostlog::Status::Disabled;
    frame.status = status;
    frame.table_complete = status == gtg::hostlog::Status::Running;
    return frame;
}

gtg::hostlog::Host HostsLogHost(const std::uint64_t id, std::wstring name,
                                const std::optional<std::uint64_t> state_ms) {
    gtg::hostlog::Host host;
    host.id = id;
    host.name = std::move(name);
    host.heard.state_ms = state_ms;
    if (state_ms) {
        host.state.emplace();
        host.state->name = host.name;
        host.state->version = {1, 1, 0};
        host.state->protection = gtg::peer::Protection::Normal;
    }
    return host;
}

std::string HostsLogField(const std::string& line, const std::string& host_id,
                          const std::string& key) {
    const auto host = line.find("{\"id\":\"" + host_id + "\"");
    if (host == std::string::npos) return {};
    const auto at = line.find("\"" + key + "\":", host);
    if (at == std::string::npos) return {};
    const auto begin = at + key.size() + 3;
    return line.substr(begin, line.find_first_of(",}", begin) - begin);
}

}  // namespace

void TestHostsLogFrame() {
    using namespace gtg::hostlog;
    using gtg::peer::Protection;

    // Hosts off: what is known about this machine, and no table.
    Frame off = HostsLogFrame(Status::Disabled);
    off.hosts.push_back(HostsLogHost(10, L"Host-A", 99'000));
    Require(FormatLine(off, kHostsLogRun) ==
                R"({"v":1,"run_id":"11111111-1111-4111-8111-111111111111","seq":3,)"
                R"("captured_at":"2026-11-01T04:00:00.005Z","observer_id":"0000000000000001",)"
                R"("observer_name":"Workstation","gtg_version":"1.1.0","enabled":false,)"
                R"("status":"disabled","table_complete":false,"export_dropped_total":0,)"
                R"("hosts":[]})"
                "\n",
            "hosts log: Hosts off is the whole line, hosts unlisted");
    for (const auto status : {Status::Starting, Status::Unavailable}) {
        Frame frame = HostsLogFrame(status);
        frame.hosts.push_back(HostsLogHost(10, L"Host-A", 99'000));
        const std::string line = FormatLine(frame, kHostsLogRun);
        Require(line.find("\"enabled\":true") != std::string::npos &&
                    line.find("\"table_complete\":false") != std::string::npos &&
                    line.find("\"hosts\":[]") != std::string::npos,
                "hosts log: a table that is not complete lists no host");
    }
    const std::string starting = FormatLine(HostsLogFrame(Status::Starting), kHostsLogRun);
    const std::string unavailable = FormatLine(HostsLogFrame(Status::Unavailable), kHostsLogRun);
    Require(starting.find(R"("status":"starting")") != std::string::npos &&
                unavailable.find(R"("status":"unavailable")") != std::string::npos,
            "hosts log: status names");

    // Running with no pair: complete and empty.
    Frame empty = HostsLogFrame(Status::Running);
    empty.observer_id = 0;
    empty.observer_name = L" ";
    empty.dropped = 7;
    const std::string empty_line = FormatLine(empty, kHostsLogRun);
    Require(empty_line.find(R"("observer_id":null,"observer_name":"GTG")") != std::string::npos,
            "hosts log: no id yet is null, and a name is never empty");
    Require(empty_line.find(R"("status":"running","table_complete":true,)"
                            R"("export_dropped_total":7,"hosts":[]})") != std::string::npos,
            "hosts log: no pairs is a complete, empty table");

    // One host as the dashboard holds it, every record as received.
    Frame running = HostsLogFrame(Status::Running);
    Host a = HostsLogHost(10, L"Host-A", 99'650);
    a.state->protection = Protection::SafePower;
    auto& records = a.state->records;
    records[0] = {0.51F, 0.94F, false, false, 51.0F, std::nullopt};
    records[1].value = 120.5F;
    records[2] = {0.4F, 0.9F, true, false, 40.0F, std::nullopt};
    records[6].value = 0.0F;  // a real 0 fps, not a missing reading
    records[6].alarm = true;
    records[7] = {0.1F, std::nullopt, false, false, 12'500'000.0F, 500'000.0F};
    running.hosts.push_back(a);
    constexpr std::string_view kHostA =
        R"({"id":"000000000000000a","name":"Host-A","presence":"online","age_ms":350,)"
        R"("version":"1.1.0","protection":"safe_power","stale":false,"records":{)"
        R"("temperature":[51,null,0.51,0.94,false,false],)"
        R"("power":[120.5,null,null,null,false,false],)"
        R"("vram":[40,null,0.4,0.9,true,false],)"
        R"("gpu":[null,null,null,null,false,false],)"
        R"("cpu":[null,null,null,null,false,false],)"
        R"("ram":[null,null,null,null,false,false],)"
        R"("fps":[0,null,null,null,false,true],)"
        R"("network":[12500000,500000,0.1,null,false,false]}}]})";
    Require(FormatLine(running, kHostsLogRun).find(kHostA) != std::string::npos,
            "hosts log: a host and its eight records, exactly");

    // Presence is PresenceAt at the frame's instant, edges included.
    Frame edges = HostsLogFrame(Status::Running);
    edges.hosts.push_back(HostsLogHost(1, L"three", 100'000 - 3'000));
    edges.hosts.push_back(HostsLogHost(2, L"dims", 100'000 - 3'001));
    edges.hosts.push_back(HostsLogHost(3, L"ten", 100'000 - 10'000));
    edges.hosts.push_back(HostsLogHost(4, L"lost", 100'000 - 10'001));
    Host gone = HostsLogHost(5, L"gone", 90'000);
    gone.heard.goodbye_ms = 95'000;
    edges.hosts.push_back(gone);
    edges.hosts.push_back(HostsLogHost(6, L"never", std::nullopt));
    Host ahead = HostsLogHost(7, L"ahead", 100'200);
    edges.hosts.push_back(ahead);
    const std::string edge_line = FormatLine(edges, kHostsLogRun);
    const auto field = [&](const std::uint64_t id, const std::string& key) {
        return HostsLogField(edge_line, std::format("{:016x}", id), key);
    };
    Require(field(1, "presence") == "\"online\"" && field(2, "presence") == "\"dim\"" &&
                field(3, "presence") == "\"dim\"" && field(4, "presence") == "\"lost\"",
            "hosts log: 3 s is online, 10 s is dim, past it lost");
    Require(field(5, "presence") == "\"offline\"" && field(5, "age_ms") == "10000",
            "hosts log: a goodbye is offline, aged from the last state");
    Require(field(7, "age_ms") == "0", "hosts log: a state stamped after the frame is age 0");
    constexpr std::string_view kNever =
        R"({"id":"0000000000000006","name":"never","presence":"not_heard",)"
        R"("age_ms":null,"version":null,"protection":"unknown","stale":true,"records":{)"
        R"("temperature":[null,null,null,null,false,false],)";
    Require(edge_line.find(kNever) != std::string::npos,
            "hosts log: a host never heard has nothing, stale and unknown");
    Require(edge_line.find(R"("hosts":[{"id":"0000000000000001")") != std::string::npos &&
                edge_line.find("\"0000000000000006\"") < edge_line.find("\"0000000000000007\""),
            "hosts log: hosts in the reader's order");

    // Names: escaped, cleaned as the dashboard cleans them, cut at 48 bytes.
    Frame names = HostsLogFrame(Status::Running);
    names.hosts.push_back(HostsLogHost(1, L"a\"b\\c", 99'000));
    names.hosts.push_back(HostsLogHost(2, L"a\tb", 99'000));
    names.hosts.push_back(HostsLogHost(3, std::wstring(20, L'主'), 99'000));
    names.hosts.push_back(HostsLogHost(12, L" ", 99'000));
    const std::string name_line = FormatLine(names, kHostsLogRun);
    std::string sixteen;
    for (int i = 0; i < 16; ++i)
        sixteen += gtg::peer::wire::EncodeUtf8(L"主", 4);
    Require(name_line.find(R"("name":"a\"b\\c")") != std::string::npos,
            "hosts log: quotes and backslashes are escaped");
    Require(name_line.find(R"("name":"ab")") != std::string::npos,
            "hosts log: a control character is dropped, as on the dashboard");
    Require(name_line.find("\"name\":\"" + sixteen + "\"") != std::string::npos,
            "hosts log: a long CJK name is cut at a character within 48 bytes");
    Require(name_line.find(R"("name":"Host-000000000000000c")") != std::string::npos,
            "hosts log: an empty name is Host-<id>");
    Require(name_line.find('\n') == name_line.size() - 1, "hosts log: one line per frame");

    // A negative reading, numbers without exponents, and the run id's form.
    Frame cold = HostsLogFrame(Status::Running);
    Host chilled = HostsLogHost(9, L"cold", 99'000);
    chilled.state->records[0].value = -5.5F;
    chilled.state->records[7] = {0.0001F, std::nullopt, false, false, 1.0e12F, 500'000.0F};
    cold.hosts.push_back(chilled);
    const std::string cold_line = FormatLine(cold, kHostsLogRun);
    Require(cold_line.find(R"("temperature":[-5.5,)") != std::string::npos,
            "hosts log: a negative temperature is written as it is");
    Require(cold_line.find(R"("network":[999999995904,500000,0.0001,null,false,false])") !=
                std::string::npos,
            "hosts log: fixed-point numbers, never an exponent");
    std::array<std::uint8_t, 16> ones{};
    ones.fill(0xFF);
    Require(FormatRunId({}) == "00000000-0000-4000-8000-000000000000" &&
                FormatRunId(ones) == "ffffffff-ffff-4fff-bfff-ffffffffffff",
            "hosts log: a run id is a version 4 UUID");

    // Sixteen hosts at their longest stay one modest line.
    Frame full = HostsLogFrame(Status::Running);
    for (std::uint64_t id = 1; id <= gtg::peer::kMaxPairs; ++id) {
        Host host = HostsLogHost(0xFFFF'FFFF'FFFF'FF00ULL + id, std::wstring(48, L'"'), 99'000);
        host.state->stale = true;
        for (auto& record : host.state->records)
            record = {0.1235F, 0.9876F, false, false, -59.999996F, 123456790000.0F};
        full.hosts.push_back(host);
    }
    const std::string full_line = FormatLine(full, kHostsLogRun);
    Require(full_line.size() < 16 * 1024, "hosts log: sixteen hosts stay under 16 KiB");
}

void TestHostsLogMailbox() {
    using Box = gtg::hostlog::Mailbox<int>;
    Box box;
    Require(box.TryPut(1) == Box::Put::Taken && box.TryPut(2) == Box::Put::Replaced,
            "mailbox: a second value replaces one never taken");
    Require(box.Take() == 2 && !box.Take(), "mailbox: the taker gets the latest, once");

    // The giver never waits: a taker stopped inside Take, holding the slot,
    // makes TryPut refuse at once. The value's move blocks only while armed,
    // and Take is the only move made while it is.
    {
        static std::atomic<bool> armed{false};
        static HANDLE entered = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        static HANDLE release = CreateEventW(nullptr, TRUE, FALSE, nullptr);

        struct Slow {
            Slow() = default;

            Slow(Slow&&) noexcept {
                if (!armed.load()) return;
                SetEvent(entered);
                (void)WaitForSingleObject(release, 5'000);
            }

            Slow& operator=(Slow&&) noexcept { return *this; }
        };

        gtg::hostlog::Mailbox<Slow> slow;
        Require(slow.TryPut(Slow{}) == gtg::hostlog::Mailbox<Slow>::Put::Taken,
                "mailbox: a slow value goes in");
        armed.store(true);
        std::thread taker([&] { (void)slow.Take(); });
        Require(WaitForSingleObject(entered, 5'000) == WAIT_OBJECT_0,
                "mailbox: the taker is inside Take");
        armed.store(false);
        const auto begin = GetTickCount64();
        const auto put = slow.TryPut(Slow{});
        const auto spent = GetTickCount64() - begin;
        SetEvent(release);
        taker.join();
        Require(put == gtg::hostlog::Mailbox<Slow>::Put::Busy && spent < 1'000,
                "mailbox: a held slot refuses at once, without waiting");
    }

    // Against a taker on another thread: every value is taken, replaced or
    // refused, and nothing is lost without being counted.
    std::atomic<bool> done{false};
    int taken = 0;
    std::thread taker([&] {
        while (!done.load())
            taken += box.Take() ? 1 : 0;
        taken += box.Take() ? 1 : 0;
    });
    int fresh = 0;
    int replaced = 0;
    int busy = 0;
    constexpr int kPuts = 200'000;
    for (int i = 0; i < kPuts; ++i) {
        switch (box.TryPut(i)) {
            case Box::Put::Taken:
                ++fresh;
                break;
            case Box::Put::Replaced:
                ++replaced;
                break;
            case Box::Put::Busy:
                ++busy;
                break;
        }
    }
    done.store(true);
    taker.join();
    Require(fresh + replaced + busy == kPuts && taken == fresh,
            "mailbox: taken values are exactly the ones that found the slot empty");
}

void TestHostsLogWriter() {
    namespace fs = std::filesystem;
    using namespace gtg::hostlog;
    const fs::path dir =
        fs::temp_directory_path() / std::format(L"gtg-hosts-log-{}", GetCurrentProcessId());
    std::error_code ignored;
    fs::remove_all(dir, ignored);
    fs::create_directories(dir);
    const fs::path file = dir / kFileName;
    const auto wait_for = [](const auto& done) {
        for (int i = 0; i < 5'000 && !done(); ++i)
            Sleep(1);
        return done();
    };
    const auto read_lines = [](const fs::path& path) {
        std::ifstream in(path, std::ios::binary);
        const std::string text((std::istreambuf_iterator<char>(in)), {});
        std::vector<std::string> lines;
        std::size_t begin = 0;
        for (std::size_t end; (end = text.find('\n', begin)) != std::string::npos;
             begin = end + 1) {
            lines.push_back(text.substr(begin, end - begin));
        }
        // Anything after the last '\n' is a line cut short.
        if (begin != text.size()) lines.push_back("<cut>");
        return lines;
    };

    constexpr std::uint64_t kBound = 1'000;
    {
        Writer writer;
        Writer::Options options;
        options.file = file;
        options.rotate_at_bytes = kBound;
        Require(writer.Start(options) && writer.Running(), "hosts log writer: starts");
        Require(writer.RunId().size() == 36 && writer.RunId()[14] == '4',
                "hosts log writer: a fresh UUID per run");
        constexpr int kFrames = 20;
        for (int i = 0; i < kFrames; ++i) {
            Require(writer.Submit(HostsLogFrame(Status::Running)),
                    "hosts log writer: an idle slot takes the frame");
            const auto lines_so_far = static_cast<std::uint64_t>(i + 1);
            Require(wait_for([&] { return writer.Written() == lines_so_far; }),
                    "hosts log writer: each frame is written");
        }
        const auto older = read_lines(fs::path(file.wstring() + L".1"));
        const auto current = read_lines(file);
        Require(!older.empty() && !current.empty(), "hosts log writer: rotated to .1");
        Require(fs::file_size(file) <= kBound &&
                    fs::file_size(fs::path(file.wstring() + L".1")) <= kBound,
                "hosts log writer: neither file passes the bound");
        std::vector<std::string> lines = older;
        lines.insert(lines.end(), current.begin(), current.end());
        for (std::size_t i = 0; i < lines.size(); ++i) {
            const auto seq = kFrames - static_cast<int>(lines.size() - 1 - i);
            Require(lines[i].starts_with("{\"v\":1,\"run_id\":\"" + writer.RunId() +
                                         "\",\"seq\":" + std::to_string(seq) + ","),
                    "hosts log writer: whole lines, one run, seq rising without a gap");
        }
        Require(writer.Dropped() == 0, "hosts log writer: nothing dropped");

        // A reader that holds the file without delete sharing: the rename
        // fails, writing stops, and the bound holds.
        const HANDLE holder =
            CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        Require(holder != INVALID_HANDLE_VALUE, "hosts log writer: test holds the file");
        for (int i = 0; i < 6; ++i) {
            const std::uint64_t before = writer.Written() + writer.Dropped();
            (void)writer.Submit(HostsLogFrame(Status::Running));
            Require(wait_for([&] { return writer.Written() + writer.Dropped() > before; }),
                    "hosts log writer: each frame is written or counted");
        }
        CloseHandle(holder);
        Require(writer.Dropped() > 0 && fs::file_size(file) <= kBound,
                "hosts log writer: a failed rotation drops frames, never the bound");
        Require(read_lines(file).back() != "<cut>", "hosts log writer: no line left cut");

        const auto stop_begin = GetTickCount64();
        writer.Stop();
        Require(!writer.Running() && GetTickCount64() - stop_begin < 2'500,
                "hosts log writer: stop returns");
        Require(!writer.Submit(HostsLogFrame(Status::Running)),
                "hosts log writer: nothing is taken after stop");
    }
    fs::remove_all(dir, ignored);
}

int main(int argc, char** argv) {
    if (argc == 3 && std::string(argv[1]) == "--supervised") {
        const std::string raw(argv[2]);
        const std::wstring handle(raw.begin(), raw.end());
        if (!gtg::supervision::AttachChild(handle.c_str())) return 12;
        wchar_t mode[16]{};
        GetEnvironmentVariableW(L"GTG_SUPERVISION_TEST", mode, 16);
        if (wcscmp(mode, L"hang") == 0 && !gtg::supervision::TakeRecoveryLatch()) Sleep(INFINITE);
        if (wcscmp(mode, L"crash") == 0) {
            if (!gtg::supervision::TakeRecoveryLatch()) {
                (void)gtg::supervision::PinTargetUuid("GPU-test-identity");
                // Even exit code zero without explicit user intent is abnormal.
                return 0;
            }
            const bool identity = gtg::supervision::TargetUuid() == "GPU-test-identity";
            const bool rejects_other = !gtg::supervision::PinTargetUuid("GPU-another-device");
            gtg::supervision::MarkCleanExit();
            return identity && rejects_other ? 0 : 13;
        }
        gtg::supervision::MarkCleanExit();
        return GetModuleHandleW(L"nvml.dll") == nullptr ? 0 : 14;
    }
    if (argc == 2 && std::string(argv[1]) == "--logger-rotation-child") {
        if (!gtg::logging::Initialize(gtg::logging::Role::Tray)) return 1;
        const std::wstring line(1000, L'x');
        for (int i = 0; i < 6000; ++i) gtg::logging::Info(line);
        gtg::logging::Shutdown();
        return 0;
    }
    const std::vector<std::pair<std::string, std::function<void()>>> tests{
        {"OsdCompact", TestOsdCompact},
        {"OsdTiny", TestOsdTiny},
        {"TinyFpsStability", TestTinyFpsStability},
        {"OsdModePositions", TestOsdModePositions},
        {"OsdSnap", TestOsdSnap},
        {"OsdIdleClock", TestOsdIdleClock},
        {"TripMarkers", TestTripMarkers},
        {"HistoryTimelineTrack", TestHistoryTimelineTrack},
        {"HotkeyCapture", TestHotkeyCapture},
        {"UiThemePreference", TestUiThemePreference},
        {"OverlayPreferences", TestOverlayPreferences},
        {"FpsBackendResolution", TestFpsBackendResolution},
        {"FpsBackendFromRuntimeDevices", TestFpsBackendFromRuntimeDevices},
        {"FpsDeviceLedger", TestFpsDeviceLedger},
        {"FpsRateIsIndependentAndHonest", TestFpsRateIsIndependentAndHonest},
        {"FpsRateRejectsShortStartupBurst", TestFpsRateRejectsShortStartupBurst},
        {"FpsRateReclaimsOldSwapchains", TestFpsRateReclaimsOldSwapchains},
        {"FpsRateSparseDisplayedCannotOverridePresents",
         TestFpsRateSparseDisplayedCannotOverridePresents},
        {"FpsIndependentFlipFallsBackToPresented",
         TestFpsIndependentFlipFallsBackToPresented},
        {"FpsIndependentFlipKeepsCorrelatorHealthy",
         TestFpsIndependentFlipKeepsCorrelatorHealthy},
        {"FpsPlayClock", TestFpsPlayClock},
        {"TopAppAlertRule", TestTopAppAlertRule},
        {"TopAppSweepBudget", TestTopAppSweepBudget},
        {"TopAppLeader", TestTopAppLeader},
        {"TopAppNames", TestTopAppNames},
        {"TopAppServiceHost", TestTopAppServiceHost},
        {"TopAppSystemProcesses", TestTopAppSystemProcesses},
        {"TopAppThresholds", TestTopAppThresholds},
        {"TopAppCpuLedger", TestTopAppCpuLedger},
        {"TopAppMarquee", TestTopAppMarquee},
        {"TopAppPdhInstance", TestTopAppPdhInstance},
        {"TopAppBoard", TestTopAppBoard},
        {"PeerPairingCode", TestPeerPairingCode},
        {"PeerProtocol", TestPeerProtocol},
        {"PeerHostsView", TestPeerHostsView},
        {"HostsLogFrame", TestHostsLogFrame},
        {"HostsLogMailbox", TestHostsLogMailbox},
        {"HostsLogWriter", TestHostsLogWriter},
        {"PeerReplay", TestPeerReplay},
        {"PeerAdmission", TestPeerAdmission},
        {"PeerCryptoVectors", TestPeerCryptoVectors},
        {"PeerPairing", TestPeerPairing},
        {"PeerLoopback", TestPeerLoopback},
        {"PeerPortInUse", TestPeerPortInUse},
        {"TinyDescribeBar", TestTinyDescribeBar},
        {"HostsBridge", TestHostsBridge},
        {"AccessDeniedObserverProbation", TestAccessDeniedObserverProbation},
        {"FpsHistoryBreaksTargetAndSurfaceTransitions",
         TestFpsHistoryBreaksTargetAndSurfaceTransitions},
        {"FpsHistoryDrawsDarkGapsWithoutInventingSamples",
         TestFpsHistoryDrawsDarkGapsWithoutInventingSamples},
        {"FpsHistoryFollowsSampledForeground", TestFpsHistoryFollowsSampledForeground},
        {"FpsHistoryBoundsAndZeroSemantics", TestFpsHistoryBoundsAndZeroSemantics},
        {"DxgiPresentMatchingRejectsFailedAndTestCalls",
         TestDxgiPresentMatchingRejectsFailedAndTestCalls},
        {"ComposedFlipRequiresVerifiedScanout", TestComposedFlipRequiresVerifiedScanout},
        {"ComposedFlipDropsAndLossFailClosed", TestComposedFlipDropsAndLossFailClosed},
        {"ComposedFlipLongRunAndDwmThreadSwitch",
         TestComposedFlipLongRunAndDwmThreadSwitch},
        {"ComposedFlipAmbiguousSurfacesFailClosed",
         TestComposedFlipAmbiguousSurfacesFailClosed},
        {"FpsPreferenceDefaultsOnWithoutTouchingProtectionSettings",
         TestFpsPreferenceDefaultsOnWithoutTouchingProtectionSettings},
        {"HostMemoryComputeBoundaries", TestHostMemoryComputeBoundaries},
        {"HostMemoryHistoryRetentionAndClear",
         TestHostMemoryHistoryRetentionAndClear},
        {"PowerDefaultsComeFromTheCard", TestPowerDefaultsComeFromTheCard},
        {"MonitorOnlyWatchesAndNeverWrites", TestMonitorOnlyWatchesAndNeverWrites},
        {"RainAnimationIsDeterministicAndBounded", TestRainAnimationIsDeterministicAndBounded},
        {"OverlaySpinTurnsAndNeverFinishesEarly", TestOverlaySpinTurnsAndNeverFinishesEarly},
        {"BusyIndicatorCoversItsCellExactly", TestBusyIndicatorCoversItsCellExactly},
        {"OsdPreferenceDefaultsOn", TestOsdPreferenceDefaultsOn},
        {"EffectiveOsdScale", TestEffectiveOsdScale},
        {"DipPixelConversions", TestDipPixelConversions},
        {"RamPreferenceDefaultsOn", TestRamPreferenceDefaultsOn},
        {"LowMemorySignalIsAvailable", TestLowMemorySignalIsAvailable},
        {"ReclaimPolicySkipsWhatItMust", TestReclaimPolicySkipsWhatItMust},
        {"ReclaimShipsLive", TestReclaimShipsLive},
        {"ReclaimResultToneIsHonest", TestReclaimResultToneIsHonest},
        {"ReclaimIndicatorHasAFloor", TestReclaimIndicatorHasAFloor},
        {"ReclaimBudgetBoundsTheWorker", TestReclaimBudgetBoundsTheWorker},
        {"ReclaimOutcomeReportsHonestly", TestReclaimOutcomeReportsHonestly},
        {"CompactDefaultFootprintIsOneRow",
         TestCompactDefaultFootprintIsOneRow},
        {"CellHitTestingAcrossScales", TestCellHitTestingAcrossScales},
        {"CompactDropRules", TestCompactDropRules},
        {"LockAndChevronNeverOverlap", TestLockAndChevronNeverOverlap},
        {"CompactLayoutDrivesFootprint", TestCompactLayoutDrivesFootprint},
        {"CompactLockPreferenceDefaultsToLocked",
         TestCompactLockPreferenceDefaultsToLocked},
        {"CompactRowComposition", TestCompactRowComposition},
        {"NinthRecordMovesTheStore", TestNinthRecordMovesTheStore},
        {"ExpandedHeightRule", TestExpandedHeightRule},
        {"OsdCards", TestOsdCards},
        {"OsdHostsView", TestOsdHostsView},
        {"LaneValueTakesWhatTheAnnotationLeaves",
         TestLaneValueTakesWhatTheAnnotationLeaves},
        {"NetworkVerdictJudgesTheWorst", TestNetworkVerdictJudgesTheWorst},
        {"NetworkResultSaysWhatWasMeasured", TestNetworkResultSaysWhatWasMeasured},
        {"NetworkThroughputArithmetic", TestNetworkThroughputArithmetic},
        {"CompactSwapOnOverlap", TestCompactSwapOnOverlap},
        {"CompactWidestRowFloor", TestCompactWidestRowFloor},
        {"HeaderStatusTextThreshold", TestHeaderStatusTextThreshold},
        {"CompactLayoutMoveInsert", TestCompactLayoutMoveInsert},
        {"CompactLayoutStoredValueCannotHideARecord",
         TestCompactLayoutStoredValueCannotHideARecord},
        {"ConfigValidation", TestConfigValidation},
        {"WorkingPowerApply", TestWorkingPowerApply},
        {"WorkingPowerFailureAndContinuity", TestWorkingPowerFailureAndContinuity},
        {"RecoveryPolicy", TestRecoveryPolicy},
        {"NvmlAbsence", TestNvmlAbsence},
        {"GreyIconPixels", TestGreyIconPixels},
        {"HiddenWindowPosition", TestHiddenWindowPosition},
        {"SupervisorChildLifecycle", TestSupervisorChildLifecycle},
        {"CoolingRequiresContinuousObservations", TestCoolingRequiresContinuousObservations},
        {"RestoreRejectsStaleReadySample", TestRestoreRejectsStaleReadySample},
        {"VerifiedRestoreChecksBeforeWriting", TestVerifiedRestoreChecksBeforeWriting},
        {"RestoreFailureReassertsSafePower", TestRestoreFailureReassertsSafePower},
        {"RestoreRetryBudget", TestRestoreRetryBudget},
        {"PowerOperationRecheck", TestPowerOperationRecheck},
        {"HardLimitHasNoDebounce", TestHardLimitHasNoDebounce},
        {"PredictiveTripRequiresTwoConfirmations", TestPredictiveTripRequiresTwoConfirmations},
        {"SinglePredictionDoesNotTrip", TestSinglePredictionDoesNotTrip},
        {"SchedulerDelaySafetyTripsImmediatelyInsidePredictiveBand",
         TestSchedulerDelaySafetyTripsImmediatelyInsidePredictiveBand},
        {"SchedulerDelaySafetyRespectsDelayAndTemperatureGuards",
         TestSchedulerDelaySafetyRespectsDelayAndTemperatureGuards},
        {"CoolingAloneDoesNotRestoreWithoutPolicyAction",
         TestCoolingAloneDoesNotRestoreWithoutPolicyAction},
        {"RestoreRequiresStableCoolingAndExplicitRequest",
         TestRestoreRequiresStableCoolingAndExplicitRequest},
        {"AutomaticRestoreRequiresReadyState", TestAutomaticRestoreRequiresReadyState},
        {"AutomaticRestoreImmediatelyRearmsProtection",
         TestAutomaticRestoreImmediatelyRearmsProtection},
        {"TestRunCounterUsesLifetimeBaseline", TestTestRunCounterUsesLifetimeBaseline},
        {"DeadlineAdvanceDoesNotDrift", TestDeadlineAdvanceDoesNotDrift},
        {"CadenceMissesSeparateWakeFromExecution",
         TestCadenceMissesSeparateWakeFromExecution},
        {"DeferredLogQueueIsBoundedAndFifo", TestDeferredLogQueueIsBoundedAndFifo},
        {"DeferredLogConcurrentProducers", TestDeferredLogConcurrentProducers},
        {"ServiceReplyDoesNotWaitForClient", TestServiceReplyDoesNotWaitForClient},
        {"LoggerRotatesDuringRuntime", TestLoggerRotatesDuringRuntime},
        {"LatencyWindowProducesTailSummaryWithoutGrowth",
         TestLatencyWindowProducesTailSummaryWithoutGrowth},
        {"GpuRecoveryReappliesSafePower", TestGpuRecoveryReappliesSafePower},
        {"TelemetryLossRequiresSustainedFailure", TestTelemetryLossRequiresSustainedFailure},
        {"HealthySampleResetsTelemetryLossTimer", TestHealthySampleResetsTelemetryLossTimer},
        {"PersistedLatchStartsSafe", TestPersistedLatchStartsSafe},
        {"WtlHeaders", TestWtlHeaders},
        {"TelemetryHistoryRetainsOneHour", TestTelemetryHistoryRetainsOneHour},
        {"TelemetryThirtySecondWindowRollsForward", TestTelemetryThirtySecondWindowRollsForward},
        {"TelemetryPreservesUnavailableMetrics", TestTelemetryPreservesUnavailableMetrics},
        {"TelemetryPreservesVramCapacityUsage", TestTelemetryPreservesVramCapacityUsage},
        {"PresentationFreshnessUsesLatestValidGpuSample",
         TestPresentationFreshnessUsesLatestValidGpuSample},
        {"PresentationFreshnessRecoversWithNewGpuSample",
         TestPresentationFreshnessRecoversWithNewGpuSample},
        {"PresentationGapsIdentifyMissingIntervals",
         TestPresentationGapsIdentifyMissingIntervals},
        {"StandaloneLocalizationSelection", TestStandaloneLocalizationSelection},
        {"RestorePromptOccursOncePerLatchEpisode", TestRestorePromptOccursOncePerLatchEpisode},
        {"SnapshotNamesPreserveWallTimeAndReason",
         TestSnapshotNamesPreserveWallTimeAndReason},
        {"HiddenWindowSnapshotCanBeEncoded", TestHiddenWindowSnapshotCanBeEncoded},
        {"SettingsTextBaseline", TestSettingsTextBaseline},
        {"FeatureNoticeSerial", TestFeatureNoticeSerial},
    };

    int failures = 0;
    for (const auto& [name, test] : tests) {
        try {
            test();
            std::cout << "[PASS] " << name << '\n';
        } catch (const std::exception& error) {
            ++failures;
            std::cerr << "[FAIL] " << name << ": " << error.what() << '\n';
        }
    }

    std::cout << tests.size() - static_cast<std::size_t>(failures) << "/"
              << tests.size() << " tests passed\n";
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
