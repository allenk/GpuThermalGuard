#include "service/service_main.hpp"
#include "supervision/supervisor.hpp"
#include "service/pipe_server.hpp"

#include <windows.h>
#include <sddl.h>

#include <cstdint>
#include <cstring>
#include <algorithm>
#include <format>
#include <optional>

#include "core/protection.hpp"
#include "core/restore_policy.hpp"
#include "ipc/protocol.hpp"
#include "nvml/nvml_loader.hpp"
#include "settings/settings.hpp"
#include "logging/logger.hpp"
#include "timing/protection_timing.hpp"
#include "timing/protection_thread_policy.hpp"

namespace gtg::service {
namespace {

constexpr wchar_t kServiceName[] = L"GpuThermalGuard";
constexpr DWORD kPollIntervalMs = 200;

SERVICE_STATUS_HANDLE g_status_handle = nullptr;
SERVICE_STATUS g_status{};
HANDLE g_stop_event = nullptr;

std::wstring AsciiToWide(const char* text) {
    if (text == nullptr) return {};
    return std::wstring(text, text + std::strlen(text));
}

std::int64_t QpcNow() noexcept {
    LARGE_INTEGER value{};
    QueryPerformanceCounter(&value);
    return value.QuadPart;
}

std::int64_t QpcFrequency() noexcept {
    LARGE_INTEGER value{};
    QueryPerformanceFrequency(&value);
    return value.QuadPart;
}

std::int64_t QpcToMicroseconds(const std::int64_t ticks,
                               const std::int64_t frequency) noexcept {
    return ticks <= 0 || frequency <= 0 ? 0 : ticks * 1'000'000 / frequency;
}

std::int64_t QpcToMilliseconds(const std::int64_t ticks,
                               const std::int64_t frequency) noexcept {
    return frequency <= 0 ? 0 : ticks * 1'000 / frequency;
}

HANDLE CreatePollTimer() noexcept {
    constexpr DWORD kAccess = TIMER_MODIFY_STATE | SYNCHRONIZE;
    HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr,
        CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, kAccess);
    return timer != nullptr ? timer : CreateWaitableTimerExW(nullptr, nullptr, 0, kAccess);
}

bool ArmPollTimer(const HANDLE timer, const std::int64_t deadline,
                  const std::int64_t frequency) noexcept {
    const std::int64_t remaining = std::max<std::int64_t>(0, deadline - QpcNow());
    LARGE_INTEGER due{};
    due.QuadPart = -std::max<std::int64_t>(1, (remaining * 10'000'000) / frequency);
    return SetWaitableTimerEx(timer, &due, 0, nullptr, nullptr, nullptr, 0) != FALSE;
}


void DebugLog(const std::wstring& message) {
    (void)logging::TryInfo(message);
}

void ReportStatus(DWORD state, DWORD win32_exit_code = NO_ERROR, DWORD wait_hint = 0) {
    g_status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    g_status.dwCurrentState = state;
    g_status.dwWin32ExitCode = win32_exit_code;
    g_status.dwWaitHint = wait_hint;
    g_status.dwControlsAccepted = state == SERVICE_START_PENDING
        ? 0 : SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN;
    g_status.dwCheckPoint = (state == SERVICE_RUNNING || state == SERVICE_STOPPED)
        ? 0 : g_status.dwCheckPoint + 1;
    if (g_status_handle != nullptr) SetServiceStatus(g_status_handle, &g_status);
}

DWORD WINAPI ControlHandler(DWORD control, DWORD, void*, void*) {
    if (control == SERVICE_CONTROL_STOP || control == SERVICE_CONTROL_SHUTDOWN) {
        ReportStatus(SERVICE_STOP_PENDING, NO_ERROR, 3'000);
        if (g_stop_event != nullptr) SetEvent(g_stop_event);
        return NO_ERROR;
    }
    if (control == SERVICE_CONTROL_INTERROGATE) {
        ReportStatus(g_status.dwCurrentState);
        return NO_ERROR;
    }
    return ERROR_CALL_NOT_IMPLEMENTED;
}

void WINAPI ServiceMain(DWORD, wchar_t**) {
    g_status_handle = RegisterServiceCtrlHandlerExW(kServiceName, ControlHandler, nullptr);
    if (g_status_handle == nullptr) return;
    logging::Info(L"SCM entered ServiceMain");
    ReportStatus(SERVICE_START_PENDING, NO_ERROR, 3'000);

    g_stop_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (g_stop_event == nullptr) {
        ReportStatus(SERVICE_STOPPED, GetLastError());
        return;
    }

    nvml::Library nvml;
    supervision::WriterLease writer_lease;
    if (!writer_lease.acquired()) {
        logging::Error(L"GPU writer lease unavailable; refusing concurrent writer");
        CloseHandle(g_stop_event);
        g_stop_event = nullptr;
        ReportStatus(SERVICE_STOPPED, ERROR_BUSY);
        return;
    }
    if (!nvml.Initialize()) {
        logging::Error(std::format(L"NVML initialization failed: {}",
                                   std::wstring(nvml.last_error().begin(), nvml.last_error().end())));
        CloseHandle(g_stop_event);
        g_stop_event = nullptr;
        ReportStatus(SERVICE_STOPPED, ERROR_DEVICE_NOT_AVAILABLE);
        return;
    }
    if (!nvml.BindDevice(0)) {
        logging::Error(std::format(L"NVML target-device binding failed: {}",
            std::wstring(nvml.last_error().begin(), nvml.last_error().end())));
        CloseHandle(g_stop_event);
        g_stop_event = nullptr;
        ReportStatus(SERVICE_STOPPED, ERROR_DEVICE_NOT_AVAILABLE);
        return;
    }

    const ProtectionConfig config = settings::Load().config;
    logging::Info(std::format(
        L"service config normal={} W safe={} W trigger={} C auto_restore={} "
        L"sensor_loss_fail_safe_ms={}",
        config.normal_power_w, config.safe_power_w, config.trigger_temperature_c,
        config.auto_restore ? L"true" : L"false",
        config.sensor_unavailable_fail_safe_ms));
    ProtectionController controller(config);
    bool safe_limit_verified = false;
    if (settings::LoadSafeLatch()) {
        (void)controller.RestorePersistedSafeLatch();
        safe_limit_verified = nvml.SetBoundPowerLimitWatts(
            static_cast<unsigned int>(config.safe_power_w));
        if (safe_limit_verified) {
            logging::Warning(L"service restored and verified persisted safe-power latch");
        } else {
            logging::Error(std::format(
                L"service could not verify persisted safe-power latch: {}",
                std::wstring(nvml.last_error().begin(), nvml.last_error().end())));
        }
    }
    PipeServer pipe_server;
    std::int64_t last_retry_ms = 0;
    std::optional<int> current_temperature;
    std::optional<ProtectionState> last_logged_state;
    std::int64_t last_snapshot_log_ms = 0;
    RestoreRetryBudget restore_budget;
    std::uint32_t trigger_count = settings::LoadTriggerCount();
    bool unavailable_logged = false;
    bool safe_episode_counted = controller.safe_latched();
    ReportStatus(SERVICE_RUNNING);
    logging::Info(L"service protection loop running; dedicated deadline cadence=200 ms");

    HANDLE poll_timer = CreatePollTimer();
    if (poll_timer == nullptr) {
        CloseHandle(g_stop_event);
        g_stop_event = nullptr;
        ReportStatus(SERVICE_STOPPED, GetLastError());
        return;
    }
    const std::int64_t qpc_frequency = QpcFrequency();
    const std::int64_t period_ticks = qpc_frequency * kPollIntervalMs / 1'000;
    std::int64_t next_deadline = QpcNow();
    std::uint64_t missed_deadlines = 0;
    std::uint64_t wake_missed_deadlines = 0;
    std::uint64_t execution_overruns = 0;
    std::int64_t last_timing_log_ms = QpcToMilliseconds(next_deadline, qpc_frequency);
    timing::LatencyWindow<512> wake_latency;
    timing::LatencyWindow<512> temperature_latency;
    timing::LatencyWindow<512> decision_latency;
    timing::LatencyWindow<128> intervention_latency;
    timing::LatencyWindow<512> execution_latency;
    const std::uint64_t initial_log_drops = logging::DeferredDroppedCount();
    const auto thread_policy = timing::ConfigureProtectionThread(
        L"GpuThermalGuard Service Protection");
    (void)logging::TryInfo(std::format(
        L"service protection thread policy priority=HIGHEST effective={} applied={} error={} "
        L"high_qos={} qos_error={}",
        thread_policy.effective_priority,
        thread_policy.priority_set ? L"true" : L"false",
        thread_policy.priority_error,
        thread_policy.high_qos_set ? L"true" : L"false",
        thread_policy.high_qos_error));

    while (ArmPollTimer(poll_timer, next_deadline, qpc_frequency)) {
        HANDLE handles[]{g_stop_event, poll_timer};
        const DWORD wait = WaitForMultipleObjects(2, handles, FALSE, INFINITE);
        if (wait == WAIT_OBJECT_0) break;
        if (wait != WAIT_OBJECT_0 + 1) break;

        const std::int64_t wake_qpc = QpcNow();
        const std::int64_t wake_lateness_ticks =
            std::max<std::int64_t>(0, wake_qpc - next_deadline);
        wake_latency.Add(QpcToMicroseconds(wake_lateness_ticks, qpc_frequency));
        const std::int64_t temperature_begin = QpcNow();
        auto sampled_temperature = nvml.ReadBoundTemperature();
        if (!sampled_temperature.has_value()) {
            if (nvml.BindDevice(0)) sampled_temperature = nvml.ReadBoundTemperature();
        }
        const auto temperature_end = QpcNow();
        const auto now = static_cast<std::int64_t>(GetTickCount64());
        const auto scheduler_delay_ms = QpcToMilliseconds(
            wake_lateness_ticks + temperature_end - temperature_begin, qpc_frequency);
        temperature_latency.Add(QpcToMicroseconds(
            temperature_end - temperature_begin, qpc_frequency));
        if (!sampled_temperature.has_value()) {
            const std::int64_t decision_begin = QpcNow();
            const auto decision = controller.SensorUnavailable(now);
            decision_latency.Add(QpcToMicroseconds(
                QpcNow() - decision_begin, qpc_frequency));
            current_temperature.reset();
            if (!unavailable_logged) {
                (void)logging::TryError(std::format(L"service GPU snapshot unavailable: {}",
                    std::wstring(nvml.last_error().begin(), nvml.last_error().end())));
                unavailable_logged = true;
            }
            if (decision.action == ProtectionAction::ApplySafePower) {
                safe_episode_counted = false;
                const bool applied = nvml.SetBoundPowerLimitWatts(
                    static_cast<unsigned int>(config.safe_power_w));
                safe_limit_verified = applied;
                intervention_latency.Add(QpcToMicroseconds(
                    QpcNow() - wake_qpc, qpc_frequency));
                if (applied) {
                    std::wstring latch_error;
                    (void)settings::SaveSafeLatch(true, latch_error);
                    if (!safe_episode_counted) {
                        ++trigger_count;
                        safe_episode_counted = true;
                        std::wstring counter_error;
                        if (!settings::SaveTriggerCount(trigger_count, counter_error)) {
                            (void)logging::TryWarning(counter_error);
                        }
                    }
                }
                (void)logging::TryWarning(std::format(
                    L"TRIGGER reason={} temp=unavailable; requested={} W result={} "
                    L"wake_delay_ms={}",
                    AsciiToWide(ToString(decision.trip_reason)), config.safe_power_w,
                    applied ? L"verified" : L"failed", scheduler_delay_ms));
                if (!applied) {
                    (void)logging::TryError(std::format(
                        L"telemetry-loss safe-power action failed: {}",
                        std::wstring(nvml.last_error().begin(), nvml.last_error().end())));
                }
                last_retry_ms = now;
            }
            const ProtectionState effective_state =
                decision.safe_latched && !safe_limit_verified
                    ? ProtectionState::Fault : decision.state;
            if (!last_logged_state || *last_logged_state != effective_state) {
                (void)logging::TryInfo(std::format(
                    L"service protection state -> {}; temp=unavailable",
                    AsciiToWide(ToString(effective_state))));
                last_logged_state = effective_state;
            }
            if (decision.safe_latched && now - last_retry_ms >= 2'000) {
                const auto current_limit = nvml.ReadBoundPowerLimitMillwatts();
                const bool safe_is_effective = current_limit.has_value() &&
                    *current_limit <=
                        static_cast<unsigned int>(config.safe_power_w * 1000);
                if (safe_is_effective) {
                    safe_limit_verified = true;
                    if (!safe_episode_counted) {
                        std::wstring latch_error;
                        (void)settings::SaveSafeLatch(true, latch_error);
                        ++trigger_count;
                        safe_episode_counted = true;
                        std::wstring counter_error;
                        (void)settings::SaveTriggerCount(trigger_count, counter_error);
                        (void)logging::TryWarning(std::format(
                            L"service deferred safe-power read-back succeeded: {} W; trips={}",
                            config.safe_power_w, trigger_count));
                    }
                } else {
                    const bool applied = nvml.SetBoundPowerLimitWatts(
                        static_cast<unsigned int>(config.safe_power_w));
                    safe_limit_verified = applied;
                    if (applied && !safe_episode_counted) {
                        std::wstring latch_error;
                        (void)settings::SaveSafeLatch(true, latch_error);
                        ++trigger_count;
                        safe_episode_counted = true;
                        std::wstring counter_error;
                        (void)settings::SaveTriggerCount(trigger_count, counter_error);
                        (void)logging::TryWarning(std::format(
                            L"service deferred safe-power verification succeeded: {} W; "
                            L"trips={}", config.safe_power_w, trigger_count));
                    } else if (!applied) {
                        (void)logging::TryError(std::format(
                            L"service telemetry-loss safe-power retry failed: {}",
                            std::wstring(nvml.last_error().begin(), nvml.last_error().end())));
                    }
                }
                last_retry_ms = now;
            }
        } else {
            bool reapply_safe = false;
            if (unavailable_logged) {
                (void)logging::TryInfo(L"service GPU snapshot stream recovered");
                reapply_safe = controller.SensorRecovered().action ==
                    ProtectionAction::ApplySafePower;
            }
            unavailable_logged = false;
            current_temperature = *sampled_temperature;
            const std::int64_t decision_begin = QpcNow();
            auto decision = controller.ObserveTemperature(
                now, *sampled_temperature, scheduler_delay_ms);
            if (reapply_safe) decision.action = ProtectionAction::ApplySafePower;
            decision_latency.Add(QpcToMicroseconds(
                QpcNow() - decision_begin, qpc_frequency));
            if (scheduler_delay_ms >= config.scheduler_delay_fail_safe_ms &&
                decision.action != ProtectionAction::ApplySafePower) {
                (void)logging::TryWarning(std::format(
                    L"service scheduler delay observed delay_ms={} temp={} C; outside active "
                    L"fail-safe condition, no power action",
                    scheduler_delay_ms, *sampled_temperature));
            }
            if (decision.action == ProtectionAction::ApplySafePower) {
                const bool applied = nvml.SetBoundPowerLimitWatts(
                    static_cast<unsigned int>(config.safe_power_w));
                safe_limit_verified = applied;
                intervention_latency.Add(QpcToMicroseconds(
                    QpcNow() - wake_qpc, qpc_frequency));
                (void)logging::TryWarning(std::format(L"TRIGGER reason={} temp={} C rise={:.2f} C/s predicted={:.1f} C; requested={} W result={} wake_delay_ms={}",
                    AsciiToWide(ToString(decision.trip_reason)),
                    *sampled_temperature, decision.rise_c_per_s,
                    decision.predicted_temperature_c, config.safe_power_w,
                    applied ? L"verified" : L"failed", scheduler_delay_ms));
                if (!applied) {
                    (void)logging::TryError(std::format(L"failed to apply safe power limit: {}",
                        std::wstring(nvml.last_error().begin(), nvml.last_error().end())));
                } else {
                    std::wstring latch_error;
                    (void)settings::SaveSafeLatch(true, latch_error);
                    if (!safe_episode_counted) {
                        ++trigger_count;
                        safe_episode_counted = true;
                        std::wstring counter_error;
                        if (!settings::SaveTriggerCount(trigger_count, counter_error)) {
                            (void)logging::TryWarning(counter_error);
                        }
                    }
                    DebugLog(std::format(L"safe power {} W applied at {} C", config.safe_power_w,
                                         *sampled_temperature));
                }
                last_retry_ms = now;
            }
            // Keep all diagnostic file I/O after the first power-limit setter.
            const ProtectionState effective_state =
                decision.safe_latched && !safe_limit_verified
                    ? ProtectionState::Fault : decision.state;
            if (!last_logged_state || *last_logged_state != effective_state) {
                (void)logging::TryInfo(std::format(L"service protection state -> {}; temp={} C rise={:.2f} C/s predicted={:.1f} C",
                    AsciiToWide(ToString(effective_state)),
                    *sampled_temperature, decision.rise_c_per_s,
                    decision.predicted_temperature_c));
                last_logged_state = effective_state;
            }
            restore_budget.ObserveLatch(controller.safe_latched());
            if (ShouldAutomaticallyRestore(config.auto_restore, decision.state) &&
                restore_budget.CanAttempt(now)) {
                (void)logging::TryInfo(std::format(
                    L"service automatic restore eligible; temp={} C target={} W trips={}",
                    *sampled_temperature, config.normal_power_w, trigger_count));
                const auto restore = RestoreWithSafeFallback(controller,
                    static_cast<std::int64_t>(GetTickCount64()), *sampled_temperature, safe_limit_verified, [&] {
                        const bool verified = nvml.SetBoundPowerLimitWatts(
                            static_cast<unsigned int>(config.normal_power_w), L"service-auto-normal");
                        restore_budget.BeginAttempt(static_cast<std::int64_t>(GetTickCount64()));
                        if (!verified && !nvml.last_power_result().retryable) restore_budget.StopRetrying();
                        (void)logging::TryInfo(std::format(
                            L"RESTORE service auto episode={} attempt={}/3 operation={} temp_c={} verified={} stopped={}",
                            restore_budget.episode(), restore_budget.attempts(),
                            nvml.last_power_result().operation_id, *sampled_temperature, verified, restore_budget.exhausted()));
                        return verified;
                    }, [&] {
                        last_retry_ms = static_cast<std::int64_t>(GetTickCount64());
                        return nvml.SetBoundPowerLimitWatts(
                            static_cast<unsigned int>(config.safe_power_w), L"service-auto-safe-fallback");
                    });
                decision = restore.decision;
                if (restore.verified) {
                    safe_episode_counted = false;
                    safe_limit_verified = false;
                    std::wstring latch_error;
                    (void)settings::SaveSafeLatch(false, latch_error);
                    (void)logging::TryInfo(std::format(
                        L"service automatic normal-power restore verified: {} W; trips retained={}; protection rearmed",
                        config.normal_power_w, trigger_count));
                } else if (restore.attempted) {
                    (void)logging::TryWarning(safe_limit_verified
                        ? L"service auto restore failed; safe fallback verified; latch retained"
                        : L"service auto restore failed; safe fallback UNVERIFIED; latch retained");
                    (void)logging::TryWarning(restore_budget.exhausted()
                        ? L"RESTORE service automatic retries STOPPED; manual restore required; see POWER_OP evidence"
                        : L"RESTORE service retry eligible after at least 5 s and fresh stable cooling");
                }
            }
            if (now - last_snapshot_log_ms >= 5'000) {
                last_snapshot_log_ms = now;
                const auto current_power = nvml.ReadBoundPowerUsageMillwatts();
                const auto current_limit = nvml.ReadBoundPowerLimitMillwatts();
                (void)logging::TryInfo(std::format(L"snapshot temp_c={} power_w={} limit_w={} state={}",
                    *sampled_temperature,
                    current_power ? std::format(L"{:.1f}", *current_power / 1000.0) : L"null",
                    current_limit ? std::format(L"{:.0f}", *current_limit / 1000.0) : L"null",
                    AsciiToWide(ToString(decision.state))));
            }
            if (decision.safe_latched && now - last_retry_ms >= 2'000) {
                const auto current_limit = nvml.ReadBoundPowerLimitMillwatts();
                const bool safe_is_effective = current_limit.has_value() &&
                    *current_limit <=
                        static_cast<unsigned int>(config.safe_power_w * 1000);
                if (safe_is_effective) {
                    safe_limit_verified = true;
                    if (!safe_episode_counted) {
                        std::wstring latch_error;
                        (void)settings::SaveSafeLatch(true, latch_error);
                        ++trigger_count;
                        safe_episode_counted = true;
                        std::wstring counter_error;
                        (void)settings::SaveTriggerCount(trigger_count, counter_error);
                    }
                } else {
                    if (nvml.SetBoundPowerLimitWatts(
                            static_cast<unsigned int>(config.safe_power_w))) {
                        safe_limit_verified = true;
                        (void)logging::TryWarning(std::format(L"service re-applied safe power after drift: {} W",
                                                     config.safe_power_w));
                        if (!safe_episode_counted) {
                            std::wstring latch_error;
                            (void)settings::SaveSafeLatch(true, latch_error);
                            ++trigger_count;
                            safe_episode_counted = true;
                            std::wstring counter_error;
                            (void)settings::SaveTriggerCount(trigger_count, counter_error);
                        }
                    } else {
                        safe_limit_verified = false;
                        (void)logging::TryError(std::format(L"service safe-power retry failed: {}",
                            std::wstring(nvml.last_error().begin(), nvml.last_error().end())));
                    }
                }
                last_retry_ms = now;
            }
        }

        restore_budget.ObserveLatch(controller.safe_latched());
        const auto request = pipe_server.Poll();
        if (request.has_value()) {
            ipc::Response response{};
            response.protection_state = static_cast<std::uint32_t>(
                controller.safe_latched() && !safe_limit_verified
                    ? ProtectionState::Fault : controller.state());
            response.temperature_c = current_temperature.value_or(-1);
            response.normal_power_w = config.normal_power_w;
            response.safe_power_w = config.safe_power_w;
            response.trigger_temperature_c = config.trigger_temperature_c;
            response.trigger_count = trigger_count;
            if (request->command == ipc::Command::Query && controller.safe_latched() && restore_budget.exhausted())
                response.win32_error = ERROR_RETRY;  // Automatic retries stopped; manual request remains available.
            if (controller.safe_latched() && safe_limit_verified)
                response.flags |= ipc::SafeLatched;
            if (controller.state() == ProtectionState::ReadyToRestore && safe_limit_verified)
                response.flags |= ipc::ReadyToRestore;

            if (request->command == ipc::Command::RestoreNormalPower) {
                restore_budget.ResetForManualRequest();
                (void)logging::TryInfo(L"tray requested normal-power restore");
                VerifiedRestoreResult restore{};
                if (current_temperature) {
                    restore = RestoreWithSafeFallback(controller,
                        static_cast<std::int64_t>(GetTickCount64()), *current_temperature, safe_limit_verified, [&] {
                            const bool verified = nvml.SetBoundPowerLimitWatts(
                                static_cast<unsigned int>(config.normal_power_w), L"service-manual-normal");
                            restore_budget.BeginAttempt(static_cast<std::int64_t>(GetTickCount64()));
                            if (!verified && !nvml.last_power_result().retryable) restore_budget.StopRetrying();
                            (void)logging::TryInfo(std::format(
                                L"RESTORE service manual episode={} attempt={} operation={} temp_c={} verified={}",
                                restore_budget.episode(), restore_budget.attempts(),
                                nvml.last_power_result().operation_id, *current_temperature, verified));
                            return verified;
                        }, [&] {
                            last_retry_ms = static_cast<std::int64_t>(GetTickCount64());
                            return nvml.SetBoundPowerLimitWatts(
                                static_cast<unsigned int>(config.safe_power_w), L"service-manual-safe-fallback");
                        });
                }
                if (!restore.attempted) {
                    response.win32_error = ERROR_NOT_READY;
                    response.flags &= ~ipc::ReadyToRestore;
                    response.protection_state = static_cast<std::uint32_t>(
                        controller.safe_latched() && !safe_limit_verified
                            ? ProtectionState::Fault : controller.state());
                } else if (!restore.verified) {
                    response.win32_error = ERROR_WRITE_FAULT;
                    response.flags = safe_limit_verified ? ipc::SafeLatched : 0;
                    response.protection_state = static_cast<std::uint32_t>(
                        safe_limit_verified ? controller.state() : ProtectionState::Fault);
                    (void)logging::TryWarning(safe_limit_verified
                        ? L"service manual restore failed; safe fallback verified; latch retained"
                        : L"service manual restore failed; safe fallback UNVERIFIED; latch retained");
                    (void)logging::TryError(L"service normal-power restore failed; see original POWER_OP evidence");
                } else {
                    safe_episode_counted = false;
                    safe_limit_verified = false;
                    std::wstring latch_error;
                    (void)settings::SaveSafeLatch(false, latch_error);
                    response.flags = ipc::LastCommandSucceeded;
                    response.trigger_count = trigger_count;
                    response.protection_state = static_cast<std::uint32_t>(controller.state());
                    (void)logging::TryInfo(std::format(
                        L"service restored and verified normal power: {} W; trips retained={}",
                        config.normal_power_w, trigger_count));
                }
            }
            restore_budget.ObserveLatch(controller.safe_latched());
            pipe_server.Reply(response);
        }

        const std::int64_t timing_now_ms = QpcToMilliseconds(QpcNow(), qpc_frequency);
        if (timing_now_ms - last_timing_log_ms >= 60'000) {
            const auto wake = wake_latency.Summary();
            const auto read = temperature_latency.Summary();
            const auto decision = decision_latency.Summary();
            const auto intervention = intervention_latency.Summary();
            const auto execution = execution_latency.Summary();
            (void)logging::TryInfo(std::format(
                L"service timing samples={} missed={} wake_missed={} execution_overrun={} "
                L"wake_us[p50={} p95={} p99={} max={}] "
                L"temp_us[p50={} p95={} p99={} max={}] decision_us[p50={} p95={} p99={} max={}] "
                L"intervention_us[p50={} p95={} p99={} max={}] "
                L"execution_us[p50={} p95={} p99={} max={}] log_dropped={}",
                wake.count, missed_deadlines, wake_missed_deadlines, execution_overruns,
                wake.p50_us, wake.p95_us, wake.p99_us, wake.maximum_us,
                read.p50_us, read.p95_us, read.p99_us, read.maximum_us,
                decision.p50_us, decision.p95_us, decision.p99_us, decision.maximum_us,
                intervention.p50_us, intervention.p95_us, intervention.p99_us,
                intervention.maximum_us,
                execution.p50_us, execution.p95_us, execution.p99_us,
                execution.maximum_us,
                logging::DeferredDroppedCount() - initial_log_drops));
            last_timing_log_ms = timing_now_ms;
        }

        const std::int64_t completed_qpc = QpcNow();
        execution_latency.Add(QpcToMicroseconds(
            completed_qpc - wake_qpc, qpc_frequency));
        const auto cadence = timing::ClassifyCadence(
            next_deadline, wake_qpc, completed_qpc, period_ticks);
        next_deadline = cadence.next_deadline_ticks;
        wake_missed_deadlines += cadence.wake_missed_periods;
        execution_overruns += cadence.execution_overrun_periods;
        missed_deadlines += cadence.total_missed_periods;
    }

    logging::Info(std::format(
        L"service protection loop missed_deadlines={} wake_missed={} execution_overrun={} "
        L"log_dropped={}", missed_deadlines, wake_missed_deadlines, execution_overruns,
        logging::DeferredDroppedCount() - initial_log_drops));
    CancelWaitableTimer(poll_timer);
    CloseHandle(poll_timer);
    CloseHandle(g_stop_event);
    g_stop_event = nullptr;
    ReportStatus(SERVICE_STOPPED);
    logging::Info(L"service protection loop stopped");
}

}  // namespace

int Run() {
    SERVICE_TABLE_ENTRYW table[] = {
        {const_cast<wchar_t*>(kServiceName), ServiceMain},
        {nullptr, nullptr},
    };
    if (StartServiceCtrlDispatcherW(table) != FALSE) return 0;
    return static_cast<int>(GetLastError());
}

}  // namespace gtg::service
