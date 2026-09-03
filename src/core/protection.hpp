#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

namespace gtg {

struct ProtectionConfig {
    int normal_power_w{350};
    int safe_power_w{300};
    int trigger_temperature_c{85};
    bool auto_restore{false};
    int predictive_band_c{3};
    double prediction_horizon_s{1.5};
    double minimum_rise_c_per_s{0.75};
    std::int64_t recovery_stable_ms{30'000};
    int recovery_delta_c{10};
};

[[nodiscard]] std::optional<std::string> ValidateConfig(const ProtectionConfig& config);

enum class ProtectionState {
    Armed,
    PreTrip,
    SafeLatched,
    ReadyToRestore,
    GpuUnavailable,
    Fault,
};

enum class ProtectionAction {
    None,
    ApplySafePower,
    ApplyNormalPower,
};

enum class TripReason {
    None,
    HardLimit,
    PredictedCrossing,
};

struct ProtectionDecision {
    ProtectionState state{ProtectionState::Armed};
    ProtectionAction action{ProtectionAction::None};
    TripReason trip_reason{TripReason::None};
    double rise_c_per_s{0.0};
    double predicted_temperature_c{0.0};
    bool safe_latched{false};
};

class ProtectionController final {
public:
    explicit ProtectionController(ProtectionConfig config);

    [[nodiscard]] ProtectionDecision ObserveTemperature(
        std::int64_t monotonic_ms,
        int temperature_c);
    [[nodiscard]] ProtectionDecision SensorUnavailable();
    [[nodiscard]] ProtectionDecision SensorRecovered();
    [[nodiscard]] ProtectionDecision RestorePersistedSafeLatch();
    [[nodiscard]] ProtectionDecision RequestRestore(
        std::int64_t monotonic_ms,
        int current_temperature_c);

    [[nodiscard]] ProtectionState state() const noexcept { return state_; }
    [[nodiscard]] bool safe_latched() const noexcept { return safe_latched_; }
    [[nodiscard]] int recovery_temperature_c() const noexcept;

private:
    struct Sample {
        std::int64_t monotonic_ms{};
        int temperature_c{};
    };

    static constexpr std::size_t kSampleCapacity = 16;
    static constexpr std::int64_t kSlopeWindowMs = 1'600;

    void PushSample(Sample sample) noexcept;
    [[nodiscard]] double CalculateRiseRate(std::int64_t now_ms) const noexcept;
    [[nodiscard]] ProtectionDecision CurrentDecision() const noexcept;
    [[nodiscard]] ProtectionDecision Trip(TripReason reason, double slope, double predicted);
    void ResetPredictor() noexcept;

    ProtectionConfig config_;
    ProtectionState state_{ProtectionState::Armed};
    bool safe_latched_{false};
    int predictive_hits_{0};
    std::optional<std::int64_t> recovery_since_ms_;
    std::array<Sample, kSampleCapacity> samples_{};
    std::size_t sample_start_{0};
    std::size_t sample_count_{0};
};

[[nodiscard]] const char* ToString(ProtectionState state) noexcept;
[[nodiscard]] const char* ToString(ProtectionAction action) noexcept;
[[nodiscard]] const char* ToString(TripReason reason) noexcept;

}  // namespace gtg
