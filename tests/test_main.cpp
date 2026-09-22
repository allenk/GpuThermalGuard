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
    // A sixth record used to widen the overlay to 464. It now wraps to a
    // second row so the width never changes.
    const auto six = ChooseFootprint(true, false, true, 1366, 768);
    Require(six.width == 388 && six.height == 142 && six.rows == 2,
        "a sixth record wraps downward instead of widening the overlay");
    const auto compact_grid = ChooseFootprint(true, false, true, 683, 384);
    Require(compact_grid.width == 388 && compact_grid.height == 142,
        "a high-DPI work area still fits the constant-width compact OSD");
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
    Require(gtg::fps::FirstStageD3D11Candidate(true, false),
        "known D3D11-only process is eligible for first-stage FPS");
    Require(!gtg::fps::FirstStageD3D11Candidate(false, true) &&
            !gtg::fps::FirstStageD3D11Candidate(true, true) &&
            !gtg::fps::FirstStageD3D11Candidate(false, false),
        "D3D12, mixed API and unknown targets stay unavailable");
    Require(gtg::fps::ValidatedDxgiCandidate(false, true),
        "trace-validated D3D12-only target is eligible");
    Require(gtg::fps::ValidatedDxgiCandidate(true, false) &&
            gtg::fps::ValidatedDxgiCandidate(true, true) &&
            !gtg::fps::ValidatedDxgiCandidate(false, false),
        "either DXGI-capable module is a hint; event evidence decides FPS");
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

void TestRamPreferenceDefaultsOn() {
    Require(gtg::settings::ResolveRamPreference(false, 0),
            "absent preference enables the RAM record");
    Require(gtg::settings::ResolveRamPreference(true, 1),
            "an explicit non-zero enables it");
    Require(!gtg::settings::ResolveRamPreference(true, 0),
            "an explicit zero disables it");
}

void TestCompactFootprintKeepsWidthAndFpsLast() {
    using namespace gtg::tray::compact;
    // Five cells per row; the overlay grows downward, never sideways.
    const auto five = ChooseFootprint(true, false, false, 2560, 1440);
    const auto six_fps = ChooseFootprint(true, false, true, 2560, 1440);
    const auto six_ram = ChooseFootprint(true, true, false, 2560, 1440);
    const auto seven = ChooseFootprint(true, true, true, 2560, 1440);

    Require(five.width == 388 && five.height == 88 && five.rows == 1,
            "five cells stay one row");
    Require(six_fps.width == 388 && six_fps.height == 142 && six_fps.rows == 2,
            "six cells wrap to a second row");
    Require(six_ram.width == six_fps.width && six_ram.height == six_fps.height,
            "either sixth record gives the same footprint");
    Require(seven.width == 388 && seven.height == 142 && seven.rows == 2,
            "seven cells fit the same two rows");
    Require(five.width == six_fps.width && five.width == seven.width,
            "the compact overlay never changes width");

    // Record order, and the invariant the default arrangement preserves.
    Require(CellIndexOf(Metric::Fps, true, true) == 6, "FPS is the last cell");
    Require(CellIndexOf(Metric::Ram, true, true) == 5, "RAM precedes FPS");
    Require(CellIndexOf(Metric::Fps, false, true) == 5,
            "FPS stays last without RAM");
    Require(CellIndexOf(Metric::Ram, true, false) == 5,
            "RAM is last when FPS is off");
}

void TestConfigValidation() {
    gtg::ProtectionConfig valid;
    Require(!gtg::ValidateConfig(valid).has_value(), "default config should be valid");

    valid.safe_power_w = valid.normal_power_w;
    Require(gtg::ValidateConfig(valid).has_value(), "safe power must be below normal power");
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
        {"RamPreferenceDefaultsOn", TestRamPreferenceDefaultsOn},
        {"CompactFootprintKeepsWidthAndFpsLast",
         TestCompactFootprintKeepsWidthAndFpsLast},
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
