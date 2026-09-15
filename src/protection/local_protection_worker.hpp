#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

#include <windows.h>

#include "core/protection.hpp"
#include "core/working_power_apply.hpp"
#include "timing/protection_timing.hpp"

namespace gtg::protection {

struct WorkerSnapshot {
    bool running{false};
    bool nvml_ready{false};
    ProtectionState state{ProtectionState::GpuUnavailable};
    bool safe_latched{false};
    std::optional<int> temperature_c;
    double rise_c_per_s{};
    double predicted_temperature_c{};
    TripReason last_trip_reason{TripReason::None};
    bool last_power_action_succeeded{false};
    bool auto_restore_exhausted{false};
    unsigned int trigger_count{};
    std::uint64_t trip_sequence{};
    std::uint64_t missed_deadlines{};
    std::uint64_t wake_missed_deadlines{};
    std::uint64_t execution_overruns{};
    std::uint64_t dropped_publications{};
    std::uint64_t deferred_logs_dropped{};
    timing::LatencySummary wake_lateness;
    timing::LatencySummary temperature_query;
    timing::LatencySummary decision;
    timing::LatencySummary intervention;
    timing::LatencySummary execution;
    timing::LatencySummary publication;
    std::string error;
};

class LocalProtectionWorker final {
public:
    struct ApplyCompletion {
        ApplyResult result;
        ProtectionConfig config;
        std::wstring detail;
    };
    LocalProtectionWorker() = default;
    ~LocalProtectionWorker();

    LocalProtectionWorker(const LocalProtectionWorker&) = delete;
    LocalProtectionWorker& operator=(const LocalProtectionWorker&) = delete;

    [[nodiscard]] bool Start(ProtectionConfig config, bool persisted_safe_latch);
    void Stop() noexcept;
    void RequestManualRestore() noexcept;
    // Single UI producer/consumer; owner thread never waits on a UI lock.
    [[nodiscard]] bool RequestApply(const ProtectionConfig& config);
    [[nodiscard]] std::optional<ApplyCompletion> TakeApplyCompletion();

    [[nodiscard]] bool IsRunning() const noexcept { return running_.load(); }
    [[nodiscard]] WorkerSnapshot Snapshot() const;

private:
    void Run(ProtectionConfig config, bool persisted_safe_latch) noexcept;
    void Publish(const WorkerSnapshot& snapshot);
    [[nodiscard]] bool TryPublish(const WorkerSnapshot& snapshot);

    std::atomic<bool> running_{false};
    std::atomic<bool> stop_requested_{false};
    std::atomic<bool> manual_restore_requested_{false};
    HANDLE wake_event_{nullptr};
    std::thread thread_;
    mutable std::mutex snapshot_mutex_;
    WorkerSnapshot snapshot_;
    std::atomic<int> apply_stage_{0}; // 0 idle, 1 request published, 2 result published
    ProtectionConfig apply_config_;
    ApplyCompletion apply_completion_;
};

}  // namespace gtg::protection
