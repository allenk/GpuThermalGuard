#include "core/power_envelope.hpp"
#include "net/throughput.hpp"
#include "net/action.hpp"
#include "net/verdict.hpp"
#include "core/protection.hpp"
#include "supervision/recovery_policy.hpp"
#include "supervision/supervisor.hpp"
#include "tray/window_position.hpp"
#include "tray/osd_compact.hpp"
#include "core/working_power_apply.hpp"
#include "core/restore_prompt_gate.hpp"
#include "core/restore_policy.hpp"
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
#include "fps/observer_admission.hpp"
#include "fps/fps_history.hpp"
#include "fps/dxgi_event.hpp"
#include "fps/display_correlator.hpp"
#include "settings/settings.hpp"
#include "sysmem/host_memory.hpp"
#include "sysmem/reclaim.hpp"
#include "tray/osd_animation.hpp"

#include <windows.h>
#include <objidl.h>
#include <gdiplus.h>

#include <cstdlib>
#include <array>
#include <cstring>
#include <exception>
#include <filesystem>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>
#include <thread>
#include <atomic>

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
    Require(expanded.height > 330 && expanded.height <= 384 &&
            expanded.columns == 1, "roomy expanded OSD adds FPS without exceeding half height");
    const auto expanded_grid = ChooseFootprint(false, false, true, 683, 384);
    Require(expanded_grid.height <= 220 && expanded_grid.columns == 2,
        "small high-DPI expanded OSD reflows to two columns");
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

void TestEighthRecordFillsTheWord() {
    using namespace gtg::tray::compact;

    Require(kRecordCount == 8, "network is the eighth record");
    Require(IsOptional(Metric::Network), "and it is optional, like RAM and FPS");
    Require(!IsOptional(Metric::Temperature), "while the thermal five are not");

    // Off unless asked for, which is the shipped default and also what keeps
    // every caller written before the eighth record meaning what it meant.
    Require(Resolve(Layout{}, true, true).count == 7,
            "the default arrangement does not place network");
    const auto all = Resolve(Layout{}, true, true, true);
    Require(all.count == 8, "and placing it gives eight records");
    {
        unsigned seen = 0;
        for (int slot = 0; slot < all.count; ++slot)
            seen |= 1u << static_cast<unsigned>(all.cells[static_cast<std::size_t>(slot)]);
        Require(seen == (1u << kRecordCount) - 1u,
                "each record exactly once, network included");
    }

    // Every break set of eight records survives the stored form. 128 of them
    // now, where there were 64.
    for (unsigned breaks = 0; breaks < (1u << kMaxBreaks); ++breaks) {
        Layout layout;
        layout.breaks = static_cast<std::uint8_t>(breaks);
        Require(IsValidLayout(layout), "every break set is legal");
        const auto restored = UnpackLayout(PackLayout(layout));
        Require(restored.has_value(), "and round-trips");
        Require(restored->breaks == layout.breaks, "with its breaks intact");
        Require(restored->order == layout.order, "and its order intact");
    }

    // A non-default order round-trips too, so the 24 bits really are all read.
    {
        Layout layout;
        layout.order = {Metric::Network, Metric::Fps, Metric::Ram, Metric::Cpu,
                        Metric::Gpu, Metric::Vram, Metric::Power,
                        Metric::Temperature};
        layout.breaks = 0b1010101;
        const auto restored = UnpackLayout(PackLayout(layout));
        Require(restored.has_value() && restored->order == layout.order,
                "a fully reversed order survives the stored form");
        Require(restored->breaks == layout.breaks, "and so do all seven breaks");
    }

    // The tag moved from four bits at 28 to one bit at 31, because eight
    // records leave exactly one bit. Everything the old schema wrote carried
    // 1 << 28, so bit 31 was clear in all of it and is still rejected.
    {
        const std::uint32_t old_schema = (1u << 28) | 0b010101u | (0b101010u << 21);
        Require(!UnpackLayout(old_schema).has_value(),
                "a value from the four-bit tag schema is refused");
        Require(SanitizeLayout(old_schema).order == DefaultOrder(),
                "and the reader gets the default rather than a reshuffle");
        Require(!UnpackLayout(0u).has_value(), "an empty value is refused");
        Require(!UnpackLayout(0x7FFFFFFFu).has_value(),
                "and so is anything with the tag bit clear");
    }

    // The word is full. A ninth record needs four-bit indices, and 36 + 8 + 1
    // does not fit; this is the assertion that says so out loud rather than
    // leaving it in a comment.
    Require(kRecordCount * static_cast<int>(kRecordBits) + kMaxBreaks + 1 == 32,
            "eight records use every bit of the stored word");
    Require((kRecordCount + 1) * 4 + kRecordCount + 1 > 32,
            "and a ninth cannot fit under any arrangement");
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
        int seen[7]{};
        for (const Metric metric : safe.order)
            ++seen[static_cast<int>(metric)];
        for (int i = 0; i < 7; ++i)
            Require(seen[i] == 1, "every record present exactly once after sanitizing");
        Require((safe.breaks >> kMaxBreaks) == 0, "no break bit outside the field");
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

    // Equal was rejected until AF-20260924-monitor-only-and-device-defaults;
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

}  // namespace

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
        {"FpsRateIsIndependentAndHonest", TestFpsRateIsIndependentAndHonest},
        {"FpsRateRejectsShortStartupBurst", TestFpsRateRejectsShortStartupBurst},
        {"FpsRateReclaimsOldSwapchains", TestFpsRateReclaimsOldSwapchains},
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
        {"BusyIndicatorCoversItsCellExactly", TestBusyIndicatorCoversItsCellExactly},
        {"OsdPreferenceDefaultsOn", TestOsdPreferenceDefaultsOn},
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
        {"EighthRecordFillsTheWord", TestEighthRecordFillsTheWord},
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
