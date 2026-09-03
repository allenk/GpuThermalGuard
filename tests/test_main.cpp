#include "core/protection.hpp"
#include "core/restore_prompt_gate.hpp"
#include "core/restore_policy.hpp"
#include "telemetry/telemetry_history.hpp"
#include "snapshot/ui_snapshot.hpp"
#include "localization/localization.hpp"

#include <windows.h>
#include <objidl.h>
#include <gdiplus.h>

#include <cstdlib>
#include <exception>
#include <filesystem>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

bool WtlHeadersAvailable();

namespace {

void Require(const bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void TestConfigValidation() {
    gtg::ProtectionConfig valid;
    Require(!gtg::ValidateConfig(valid).has_value(), "default config should be valid");

    valid.safe_power_w = valid.normal_power_w;
    Require(gtg::ValidateConfig(valid).has_value(), "safe power must be below normal power");
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

void TestCoolingAloneDoesNotRestoreWithoutPolicyAction() {
    gtg::ProtectionController controller({});
    (void)controller.ObserveTemperature(0, 85);
    const auto cooled = controller.ObserveTemperature(1'000, 60);
    Require(cooled.state == gtg::ProtectionState::SafeLatched,
            "cooling alone must keep safe latch");
    Require(cooled.action == gtg::ProtectionAction::None,
            "cooling must not automatically request normal power");
}

void TestRestoreRequiresStableCoolingAndExplicitRequest() {
    gtg::ProtectionController controller({});
    (void)controller.ObserveTemperature(0, 85);
    (void)controller.ObserveTemperature(1'000, 75);

    const auto too_early = controller.RequestRestore(20'000, 75);
    Require(too_early.action == gtg::ProtectionAction::None,
            "restore must be rejected before stable duration");

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

    (void)controller.ObserveTemperature(31'000, 75);
    const auto restored = controller.RequestRestore(31'001, 75);
    Require(restored.action == gtg::ProtectionAction::ApplyNormalPower,
            "automatic restore may request normal power only when ready");
}

void TestAutomaticRestoreImmediatelyRearmsProtection() {
    gtg::ProtectionController controller({});
    (void)controller.ObserveTemperature(0, 85);
    (void)controller.ObserveTemperature(1'000, 75);
    (void)controller.ObserveTemperature(31'000, 75);
    const auto restored = controller.RequestRestore(31'001, 75);
    Require(restored.state == gtg::ProtectionState::Armed,
            "successful restore must rearm protection");

    const auto retrip = controller.ObserveTemperature(31'201, 90);
    Require(retrip.state == gtg::ProtectionState::SafeLatched,
            "first dangerous sample after restore must relatch");
    Require(retrip.action == gtg::ProtectionAction::ApplySafePower,
            "first dangerous sample after restore must request safe power");
}

void TestAutomaticRestoreRetainsTripCounter() {
    Require(!gtg::ShouldResetTriggerCount(gtg::RestoreInitiator::Automatic),
            "automatic restore must retain the trip counter");
    Require(gtg::ShouldResetTriggerCount(gtg::RestoreInitiator::Manual),
            "manual restore must reset the trip counter");
    Require(gtg::HasNewTriggerCount(4, 5),
            "a Service counter increase must identify a new trip between polls");
    Require(!gtg::HasNewTriggerCount(5, 5),
            "an unchanged Service counter must not duplicate snapshots");
    Require(!gtg::HasNewTriggerCount(5, 0),
            "a manual counter reset must not look like a new trip");
}

void TestGpuRecoveryReappliesSafePower() {
    gtg::ProtectionController controller({});
    (void)controller.ObserveTemperature(0, 85);
    const auto unavailable = controller.SensorUnavailable();
    Require(unavailable.state == gtg::ProtectionState::GpuUnavailable,
            "sensor loss should enter GpuUnavailable");

    const auto recovered = controller.SensorRecovered();
    Require(recovered.state == gtg::ProtectionState::SafeLatched,
            "latched GPU recovery must remain safe");
    Require(recovered.action == gtg::ProtectionAction::ApplySafePower,
            "latched GPU recovery must reapply safe power");
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

int main() {
    const std::vector<std::pair<std::string, std::function<void()>>> tests{
        {"ConfigValidation", TestConfigValidation},
        {"HardLimitHasNoDebounce", TestHardLimitHasNoDebounce},
        {"PredictiveTripRequiresTwoConfirmations", TestPredictiveTripRequiresTwoConfirmations},
        {"SinglePredictionDoesNotTrip", TestSinglePredictionDoesNotTrip},
        {"CoolingAloneDoesNotRestoreWithoutPolicyAction",
         TestCoolingAloneDoesNotRestoreWithoutPolicyAction},
        {"RestoreRequiresStableCoolingAndExplicitRequest",
         TestRestoreRequiresStableCoolingAndExplicitRequest},
        {"AutomaticRestoreRequiresReadyState", TestAutomaticRestoreRequiresReadyState},
        {"AutomaticRestoreImmediatelyRearmsProtection",
         TestAutomaticRestoreImmediatelyRearmsProtection},
        {"AutomaticRestoreRetainsTripCounter", TestAutomaticRestoreRetainsTripCounter},
        {"GpuRecoveryReappliesSafePower", TestGpuRecoveryReappliesSafePower},
        {"PersistedLatchStartsSafe", TestPersistedLatchStartsSafe},
        {"WtlHeaders", TestWtlHeaders},
        {"TelemetryHistoryRetainsOneHour", TestTelemetryHistoryRetainsOneHour},
        {"TelemetryThirtySecondWindowRollsForward", TestTelemetryThirtySecondWindowRollsForward},
        {"TelemetryPreservesUnavailableMetrics", TestTelemetryPreservesUnavailableMetrics},
        {"TelemetryPreservesVramCapacityUsage", TestTelemetryPreservesVramCapacityUsage},
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
