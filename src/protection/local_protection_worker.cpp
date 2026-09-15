#include "protection/local_protection_worker.hpp"
#include "supervision/supervisor.hpp"
#include "supervision/recovery_policy.hpp"

#include <algorithm>
#include <chrono>
#include <format>
#include <utility>

#include "core/restore_policy.hpp"
#include "logging/logger.hpp"
#include "nvml/nvml_loader.hpp"
#include "settings/settings.hpp"
#include "timing/protection_thread_policy.hpp"

namespace gtg::protection {
namespace {

constexpr std::int64_t kPollIntervalMs = 200;
constexpr std::int64_t kSafeRetryMs = 2'000;
constexpr std::int64_t kTimingLogIntervalMs = 60'000;

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

std::int64_t ToMicroseconds(const std::int64_t ticks,
                            const std::int64_t frequency) noexcept {
    if (ticks <= 0 || frequency <= 0) return 0;
    return (ticks * 1'000'000) / frequency;
}

std::int64_t ToMilliseconds(const std::int64_t ticks,
                            const std::int64_t frequency) noexcept {
    if (frequency <= 0) return 0;
    return (ticks * 1'000) / frequency;
}

HANDLE CreateDeadlineTimer() noexcept {
    constexpr DWORD kTimerAccess = TIMER_MODIFY_STATE | SYNCHRONIZE;
    HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr,
        CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, kTimerAccess);
    if (timer == nullptr) {
        timer = CreateWaitableTimerExW(nullptr, nullptr, 0, kTimerAccess);
    }
    return timer;
}

bool ArmDeadline(const HANDLE timer,
                 const std::int64_t deadline_qpc,
                 const std::int64_t frequency) noexcept {
    const std::int64_t remaining = std::max<std::int64_t>(0, deadline_qpc - QpcNow());
    LARGE_INTEGER due{};
    due.QuadPart = -std::max<std::int64_t>(1, (remaining * 10'000'000) / frequency);
    return SetWaitableTimerEx(timer, &due, 0, nullptr, nullptr, nullptr, 0) != FALSE;
}

std::wstring Widen(const std::string& value) {
    return {value.begin(), value.end()};
}

void LogTimingSummary(const WorkerSnapshot& snapshot) {
    const auto& wake = snapshot.wake_lateness;
    const auto& read = snapshot.temperature_query;
    const auto& decision = snapshot.decision;
    const auto& intervention = snapshot.intervention;
    const auto& execution = snapshot.execution;
    const auto& publication = snapshot.publication;
    (void)logging::TryInfo(std::format(
        L"protection timing samples={} missed={} wake_missed={} execution_overrun={} "
        L"wake_us[p50={} p95={} p99={} max={}] "
        L"temp_us[p50={} p95={} p99={} max={}] decision_us[p50={} p95={} p99={} max={}] "
        L"intervention_us[p50={} p95={} p99={} max={}] "
        L"execution_us[p50={} p95={} p99={} max={}] "
        L"publish_us[p50={} p95={} p99={} max={}] publish_dropped={} log_dropped={}",
        wake.count, snapshot.missed_deadlines, snapshot.wake_missed_deadlines,
        snapshot.execution_overruns,
        wake.p50_us, wake.p95_us, wake.p99_us, wake.maximum_us,
        read.p50_us, read.p95_us, read.p99_us, read.maximum_us,
        decision.p50_us, decision.p95_us, decision.p99_us, decision.maximum_us,
        intervention.p50_us, intervention.p95_us, intervention.p99_us,
        intervention.maximum_us,
        execution.p50_us, execution.p95_us, execution.p99_us, execution.maximum_us,
        publication.p50_us, publication.p95_us, publication.p99_us,
        publication.maximum_us, snapshot.dropped_publications,
        snapshot.deferred_logs_dropped));
}

}  // namespace

LocalProtectionWorker::~LocalProtectionWorker() {
    Stop();
}

bool LocalProtectionWorker::Start(ProtectionConfig config,
                                  const bool persisted_safe_latch) {
    Stop();
    {
        std::scoped_lock lock(snapshot_mutex_);
        snapshot_ = {};
    }
    apply_stage_.store(0);
    wake_event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (wake_event_ == nullptr) return false;
    {
        std::scoped_lock lock(snapshot_mutex_);
        snapshot_.running = true;
    }
    stop_requested_.store(false);
    manual_restore_requested_.store(false);
    running_.store(true);
    try {
        thread_ = std::thread(&LocalProtectionWorker::Run, this,
                              std::move(config), persisted_safe_latch);
    } catch (...) {
        running_.store(false);
        {
            std::scoped_lock lock(snapshot_mutex_);
            snapshot_.running = false;
        }
        CloseHandle(wake_event_);
        wake_event_ = nullptr;
        return false;
    }
    return true;
}

void LocalProtectionWorker::Stop() noexcept {
    stop_requested_.store(true);
    if (wake_event_ != nullptr) SetEvent(wake_event_);
    if (thread_.joinable()) thread_.join();
    if (wake_event_ != nullptr) CloseHandle(wake_event_);
    wake_event_ = nullptr;
    running_.store(false);
}

void LocalProtectionWorker::RequestManualRestore() noexcept {
    manual_restore_requested_.store(true);
    if (wake_event_ != nullptr) SetEvent(wake_event_);
}

bool LocalProtectionWorker::RequestApply(const ProtectionConfig& config) {
    if (!running_.load() || apply_stage_.load(std::memory_order_acquire) != 0) return false;
    apply_config_ = config;
    apply_stage_.store(1, std::memory_order_release);
    return true; // next ordinary 200 ms tick; never alter the deadline cadence
}

std::optional<LocalProtectionWorker::ApplyCompletion> LocalProtectionWorker::TakeApplyCompletion() {
    if (apply_stage_.load(std::memory_order_acquire) != 2) return std::nullopt;
    auto result = apply_completion_;
    apply_stage_.store(0, std::memory_order_release);
    return result;
}

WorkerSnapshot LocalProtectionWorker::Snapshot() const {
    std::scoped_lock lock(snapshot_mutex_);
    return snapshot_;
}

void LocalProtectionWorker::Publish(const WorkerSnapshot& snapshot) {
    std::scoped_lock lock(snapshot_mutex_);
    snapshot_ = snapshot;
}

bool LocalProtectionWorker::TryPublish(const WorkerSnapshot& snapshot) {
    std::unique_lock lock(snapshot_mutex_, std::try_to_lock);
    if (!lock.owns_lock()) return false;
    snapshot_ = snapshot;
    return true;
}

void LocalProtectionWorker::Run(ProtectionConfig config,
                                const bool persisted_safe_latch) noexcept {
    const auto thread_policy = timing::ConfigureProtectionThread(
        L"GpuThermalGuard Protection");
    (void)logging::TryInfo(std::format(
        L"protection thread policy priority=HIGHEST effective={} applied={} error={} "
        L"high_qos={} qos_error={}",
        thread_policy.effective_priority,
        thread_policy.priority_set ? L"true" : L"false",
        thread_policy.priority_error,
        thread_policy.high_qos_set ? L"true" : L"false",
        thread_policy.high_qos_error));

    WorkerSnapshot published;
    published.running = true;
    published.trigger_count = settings::LoadTriggerCount();

    supervision::WriterLease writer_lease;
    if (!writer_lease.acquired()) {
        published.running = false;
        published.error = "GPU protection writer is already owned or access denied";
        Publish(published);
        running_.store(false);
        return;
    }

    nvml::Library nvml;
    if (!nvml.Initialize() || !nvml.BindDeviceByUuid(supervision::TargetUuid()) ||
        !supervision::PinTargetUuid(nvml.bound_uuid())) {
        published.running = false;
        published.error = nvml.last_error();
        Publish(published);
        running_.store(false);
        (void)logging::TryError(std::format(L"RECOVERY startup NVML failure: {}", Widen(published.error)));
        supervision::RequestRecovery();
        return;
    }
    published.nvml_ready = true;

    ProtectionController controller(config);
    bool safe_limit_verified = false;
    const bool restarting_after_fault = supervision::TakeRecoveryLatch();
    const bool recovery_latch = persisted_safe_latch || restarting_after_fault;
    if (recovery_latch) {
        (void)controller.RestorePersistedSafeLatch();
        published.last_power_action_succeeded =
            nvml.SetBoundPowerLimitWatts(static_cast<unsigned int>(config.safe_power_w));
        safe_limit_verified = published.last_power_action_succeeded;
        if (safe_limit_verified) {
            std::wstring ignored;
            (void)settings::SaveSafeLatch(true, ignored);
        }
        if (!published.last_power_action_succeeded) {
            published.error = nvml.last_error();
            (void)logging::TryError(std::format(
                L"worker failed to restore persisted safe power: {}", Widen(published.error)));
        }
    }

    HANDLE timer = CreateDeadlineTimer();
    if (timer == nullptr) {
        published.error = "CreateWaitableTimerExW failed";
        Publish(published);
        running_.store(false);
        return;
    }

    const std::int64_t frequency = QpcFrequency();
    const std::int64_t period_ticks = (frequency * kPollIntervalMs) / 1'000;
    std::int64_t next_deadline = QpcNow();
    std::int64_t last_safe_retry_ms = 0;
    RestoreRetryBudget restore_budget;
    std::int64_t last_timing_log_ms = ToMilliseconds(next_deadline, frequency);
    bool sensor_unavailable = false;
    bool safe_episode_counted = recovery_latch;
    supervision::Readiness readiness;
    std::optional<unsigned int> health_power_limit;
    std::int64_t last_health_power_ms = 0;
    std::optional<std::int64_t> missing_since;
    timing::LatencyWindow<512> wake_window;
    timing::LatencyWindow<512> temperature_window;
    timing::LatencyWindow<512> decision_window;
    timing::LatencyWindow<128> intervention_window;
    timing::LatencyWindow<512> execution_window;
    timing::LatencyWindow<512> publication_window;
    const std::uint64_t initial_log_drops = logging::DeferredDroppedCount();

    while (!stop_requested_.load()) {
        if (!ArmDeadline(timer, next_deadline, frequency)) {
            published.error = "SetWaitableTimerEx failed";
            break;
        }
        HANDLE handles[]{wake_event_, timer};
        const DWORD wait = WaitForMultipleObjects(2, handles, FALSE, INFINITE);
        if (stop_requested_.load()) break;
        if (wait != WAIT_OBJECT_0 && wait != WAIT_OBJECT_0 + 1) {
            published.error = "WaitForMultipleObjects failed";
            break;
        }

        const std::int64_t wake_qpc = QpcNow();
        const std::int64_t wake_lateness_ticks =
            std::max<std::int64_t>(0, wake_qpc - next_deadline);
        wake_window.Add(ToMicroseconds(wake_lateness_ticks, frequency));
        const std::int64_t read_begin = QpcNow();
        auto temperature = nvml.ReadBoundTemperature();
        const std::int64_t read_end = QpcNow();
        const std::int64_t now_ms = ToMilliseconds(read_end, frequency);
        if (temperature) missing_since.reset();
        else if (!missing_since) missing_since = now_ms;
        const std::int64_t scheduler_delay_ms = ToMilliseconds(
            wake_lateness_ticks + read_end - read_begin, frequency);
        temperature_window.Add(ToMicroseconds(read_end - read_begin, frequency));

        ProtectionDecision decision;
        const std::int64_t decision_begin = QpcNow();
        if (!temperature.has_value()) {
            decision = controller.SensorUnavailable(now_ms);
            sensor_unavailable = true;
            published.error = nvml.last_error();
        } else {
            bool reapply_safe = false;
            if (sensor_unavailable) {
                reapply_safe = controller.SensorRecovered().action ==
                    ProtectionAction::ApplySafePower;
                sensor_unavailable = false;
            }
            decision = controller.ObserveTemperature(
                now_ms, *temperature, scheduler_delay_ms);
            if (reapply_safe) decision.action = ProtectionAction::ApplySafePower;
            published.error.clear();
        }
        const std::int64_t decision_end = QpcNow();
        decision_window.Add(ToMicroseconds(decision_end - decision_begin, frequency));

        if (temperature.has_value() &&
            scheduler_delay_ms >= config.scheduler_delay_fail_safe_ms &&
            decision.action != ProtectionAction::ApplySafePower) {
            (void)logging::TryWarning(std::format(
                L"scheduler delay observed delay_ms={} temp={} C; outside active fail-safe "
                L"condition, no power action",
                scheduler_delay_ms, *temperature));
        }

        if (decision.action == ProtectionAction::ApplySafePower) {
            if (!published.safe_latched) safe_episode_counted = false;
            const std::int64_t action_begin = QpcNow();
            const bool applied = nvml.SetBoundPowerLimitWatts(
                static_cast<unsigned int>(config.safe_power_w));
            const std::int64_t action_end = QpcNow();
            intervention_window.Add(ToMicroseconds(action_end - wake_qpc, frequency));
            published.last_power_action_succeeded = applied;
            safe_limit_verified = applied;
            published.last_trip_reason = decision.trip_reason;
            if (applied) {
                std::wstring ignored;
                (void)settings::SaveSafeLatch(true, ignored);
                if (!safe_episode_counted) {
                    ++published.trigger_count;
                    ++published.trip_sequence;
                    safe_episode_counted = true;
                    (void)settings::SaveTriggerCount(published.trigger_count, ignored);
                }
            } else {
                published.error = nvml.last_error();
            }
            last_safe_retry_ms = now_ms;
            (void)logging::TryWarning(std::format(
                L"TRIGGER worker reason={} temp={} C rise={:.2f} C/s predicted={:.1f} C; "
                L"requested={} W result={} action_us={} wake_delay_ms={}",
                Widen(ToString(decision.trip_reason)),
                temperature ? std::to_wstring(*temperature) : L"unavailable",
                decision.rise_c_per_s, decision.predicted_temperature_c,
                config.safe_power_w, applied ? L"verified" : L"failed",
                ToMicroseconds(action_end - action_begin, frequency), scheduler_delay_ms));
        }

        if (apply_stage_.load(std::memory_order_acquire) == 1) {
            ApplyCompletion completion;
            completion.config = apply_config_;
            const auto age = ToMilliseconds(QpcNow() - read_begin, frequency);
            completion.result = ApplyWorkingPower(controller, config, completion.config,
                temperature, age, [&] {
                    const bool ok = nvml.SetBoundPowerLimitWatts(
                        static_cast<unsigned int>(completion.config.normal_power_w), L"local-apply-working");
                    // Diagnostics must not throw between uncertain write and safe fallback.
                    if (!ok) { try { completion.detail = Widen(nvml.last_error()); } catch (...) {} }
                    return ok;
                }, [&] {
                    const bool ok = nvml.SetBoundPowerLimitWatts(
                        static_cast<unsigned int>(config.safe_power_w), L"local-apply-safe-fallback");
                    if (!ok) { try { completion.detail += L"; safe fallback: " + Widen(nvml.last_error()); } catch (...) {} }
                    return ok;
                }, [&] {
                    try { return settings::Save(config, completion.detail); }
                    catch (...) { return false; } // hardware already verified: active, not saved
                });
            if (completion.result.status == ApplyStatus::WriteFailed) {
                safe_limit_verified = completion.result.safe_verified;
                published.last_power_action_succeeded = safe_limit_verified;
                published.error = "Working power Apply failed; safe latch retained";
                std::wstring latch_error;
                if (!settings::SaveSafeLatch(true, latch_error)) completion.detail += L"; " + latch_error;
                decision = controller.RestorePersistedSafeLatch();
                last_safe_retry_ms = 0; // existing verification/counting path handles the new latch
            }
            (void)logging::TryInfo(std::format(
                L"APPLY working={} W result={} safe_fallback_verified={} detail={}",
                completion.config.normal_power_w, static_cast<int>(completion.result.status),
                completion.result.safe_verified, completion.detail));
            apply_completion_ = std::move(completion);
            apply_stage_.store(2, std::memory_order_release);
        }

        restore_budget.ObserveLatch(controller.safe_latched());
        if (temperature.has_value() &&
            ShouldAutomaticallyRestore(config.auto_restore, decision.state) &&
            restore_budget.CanAttempt(now_ms)) {
            const auto restore = RestoreWithSafeFallback(controller,
                ToMilliseconds(QpcNow(), frequency), *temperature, safe_limit_verified, [&] {
                    const bool verified = nvml.SetBoundPowerLimitWatts(
                        static_cast<unsigned int>(config.normal_power_w), L"local-auto-normal");
                    restore_budget.BeginAttempt(ToMilliseconds(QpcNow(), frequency));
                    if (!verified && !nvml.last_power_result().retryable) restore_budget.StopRetrying();
                    (void)logging::TryInfo(std::format(
                        L"RESTORE local auto episode={} attempt={}/3 operation={} temp_c={} verified={} stopped={}",
                        restore_budget.episode(), restore_budget.attempts(),
                        nvml.last_power_result().operation_id, *temperature, verified, restore_budget.exhausted()));
                    return verified;
                }, [&] {
                    published.error = nvml.last_error();
                    last_safe_retry_ms = ToMilliseconds(QpcNow(), frequency);
                    return nvml.SetBoundPowerLimitWatts(
                        static_cast<unsigned int>(config.safe_power_w), L"local-auto-safe-fallback");
                });
            decision = restore.decision;
            if (restore.attempted) published.last_power_action_succeeded = restore.verified;
            if (restore.verified) {
                safe_episode_counted = false;
                safe_limit_verified = false;
                std::wstring ignored;
                (void)settings::SaveSafeLatch(false, ignored);
                (void)logging::TryInfo(std::format(
                    L"worker automatic restore verified: {} W; trips retained={}; rearmed",
                    config.normal_power_w, published.trigger_count));
            } else if (restore.attempted) {
                if (!safe_limit_verified) published.error = nvml.last_error();
                published.last_power_action_succeeded = safe_limit_verified;
                (void)logging::TryWarning(safe_limit_verified
                    ? L"worker auto restore failed; safe fallback verified; latch retained"
                    : L"worker auto restore failed; safe fallback UNVERIFIED; latch retained");
            }
        }

        if (temperature.has_value() && manual_restore_requested_.exchange(false)) {
            restore_budget.ResetForManualRequest();
            const auto restore = RestoreWithSafeFallback(controller,
                ToMilliseconds(QpcNow(), frequency), *temperature, safe_limit_verified, [&] {
                    const bool verified = nvml.SetBoundPowerLimitWatts(
                        static_cast<unsigned int>(config.normal_power_w), L"local-manual-normal");
                    restore_budget.BeginAttempt(ToMilliseconds(QpcNow(), frequency));
                    if (!verified && !nvml.last_power_result().retryable) restore_budget.StopRetrying();
                    (void)logging::TryInfo(std::format(
                        L"RESTORE local manual episode={} attempt={} operation={} temp_c={} verified={}",
                        restore_budget.episode(), restore_budget.attempts(),
                        nvml.last_power_result().operation_id, *temperature, verified));
                    return verified;
                }, [&] {
                    published.error = nvml.last_error();
                    last_safe_retry_ms = ToMilliseconds(QpcNow(), frequency);
                    return nvml.SetBoundPowerLimitWatts(
                        static_cast<unsigned int>(config.safe_power_w), L"local-manual-safe-fallback");
                });
            decision = restore.decision;
            if (restore.verified) {
                safe_episode_counted = false;
                safe_limit_verified = false;
                std::wstring ignored;
                (void)settings::SaveSafeLatch(false, ignored);
                published.last_power_action_succeeded = true;
                (void)logging::TryInfo(std::format(
                    L"worker manual restore verified: {} W; trips retained={}; rearmed",
                    config.normal_power_w, published.trigger_count));
            } else if (restore.attempted) {
                if (!safe_limit_verified) published.error = nvml.last_error();
                published.last_power_action_succeeded = safe_limit_verified;
                (void)logging::TryWarning(safe_limit_verified
                    ? L"worker manual restore failed; safe fallback verified; latch retained"
                    : L"worker manual restore failed; safe fallback UNVERIFIED; latch retained");
            }
        }

        if (decision.safe_latched &&
            now_ms - last_safe_retry_ms >= kSafeRetryMs) {
            const auto current_limit = nvml.ReadBoundPowerLimitMillwatts();
            const bool safe_effective = current_limit.has_value() &&
                *current_limit <= static_cast<unsigned int>(config.safe_power_w * 1000);
            if (safe_effective) {
                published.last_power_action_succeeded = true;
                safe_limit_verified = true;
                if (!safe_episode_counted) {
                    std::wstring ignored;
                    (void)settings::SaveSafeLatch(true, ignored);
                    ++published.trigger_count;
                    ++published.trip_sequence;
                    safe_episode_counted = true;
                    (void)settings::SaveTriggerCount(published.trigger_count, ignored);
                    (void)logging::TryWarning(std::format(
                        L"worker deferred safe-power read-back succeeded: {} W; trips={}",
                        config.safe_power_w, published.trigger_count));
                }
            } else {
                published.last_power_action_succeeded = nvml.SetBoundPowerLimitWatts(
                    static_cast<unsigned int>(config.safe_power_w));
                safe_limit_verified = published.last_power_action_succeeded;
                if (!published.last_power_action_succeeded) {
                    published.error = nvml.last_error();
                } else if (!safe_episode_counted) {
                    std::wstring ignored;
                    (void)settings::SaveSafeLatch(true, ignored);
                    ++published.trigger_count;
                    ++published.trip_sequence;
                    safe_episode_counted = true;
                    (void)settings::SaveTriggerCount(published.trigger_count, ignored);
                    (void)logging::TryWarning(std::format(
                        L"worker deferred safe-power verification succeeded: {} W; trips={}",
                        config.safe_power_w, published.trigger_count));
                }
            }
            last_safe_retry_ms = now_ms;
        }

        restore_budget.ObserveLatch(controller.safe_latched());
        published.auto_restore_exhausted = controller.safe_latched() && restore_budget.exhausted();
        published.state = decision.safe_latched && !safe_limit_verified
            ? ProtectionState::Fault : decision.state;
        published.safe_latched = decision.safe_latched && safe_limit_verified;
        published.temperature_c = temperature;
        published.rise_c_per_s = decision.rise_c_per_s;
        published.predicted_temperature_c = decision.predicted_temperature_c;
        // Readiness is an observation, never a prerequisite for hard-trip protection.
        // Run after all immediate protection actions; no extra reads before a trip.
        if (now_ms - last_health_power_ms >= 2000) {
            health_power_limit = nvml.ReadBoundPowerLimitMillwatts();
            last_health_power_ms = now_ms;
        }
        const auto health_now_ms = ToMilliseconds(QpcNow(), frequency);
        const bool fresh_temperature = temperature.has_value() &&
            supervision::IsFreshSample(health_now_ms, now_ms);
        supervision::ReportHealth(readiness.Observe(health_now_ms,
            fresh_temperature && health_power_limit.has_value() && *health_power_limit > 0 &&
            (!controller.safe_latched() || (safe_limit_verified &&
                *health_power_limit <= static_cast<unsigned int>(config.safe_power_w * 1000)))),
            fresh_temperature);
        if (missing_since && now_ms - *missing_since >= 5000) {
            (void)logging::TryError(std::format(
                L"RECOVERY sustained temperature failure; fresh process requested: {}", Widen(published.error)));
            supervision::RequestRecovery();
        }
        if (now_ms - last_timing_log_ms >= kTimingLogIntervalMs) {
            published.wake_lateness = wake_window.Summary();
            published.temperature_query = temperature_window.Summary();
            published.decision = decision_window.Summary();
            published.intervention = intervention_window.Summary();
            published.execution = execution_window.Summary();
            published.publication = publication_window.Summary();
            published.deferred_logs_dropped =
                logging::DeferredDroppedCount() - initial_log_drops;
            LogTimingSummary(published);
            last_timing_log_ms = now_ms;
        }
        const std::int64_t publication_begin = QpcNow();
        if (!TryPublish(published)) ++published.dropped_publications;
        publication_window.Add(ToMicroseconds(QpcNow() - publication_begin, frequency));

        const std::int64_t completed = QpcNow();
        execution_window.Add(ToMicroseconds(completed - wake_qpc, frequency));
        const auto cadence = timing::ClassifyCadence(
            next_deadline, wake_qpc, completed, period_ticks);
        published.wake_missed_deadlines += cadence.wake_missed_periods;
        published.execution_overruns += cadence.execution_overrun_periods;
        published.missed_deadlines += cadence.total_missed_periods;
        next_deadline = cadence.next_deadline_ticks;
    }

    CancelWaitableTimer(timer);
    CloseHandle(timer);
    published.running = false;
    Publish(published);
    running_.store(false);
}

}  // namespace gtg::protection
