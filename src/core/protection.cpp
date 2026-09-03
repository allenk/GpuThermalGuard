#include "core/protection.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

namespace gtg {

std::optional<std::string> ValidateConfig(const ProtectionConfig& config) {
    if (config.normal_power_w <= 0) {
        return "normal power must be positive";
    }
    if (config.safe_power_w <= 0) {
        return "safe power must be positive";
    }
    if (config.safe_power_w >= config.normal_power_w) {
        return "safe power must be lower than normal power";
    }
    if (config.trigger_temperature_c < 40 || config.trigger_temperature_c > 100) {
        return "trigger temperature must be between 40 and 100 C";
    }
    if (config.predictive_band_c < 1 || config.predictive_band_c > 10) {
        return "predictive band must be between 1 and 10 C";
    }
    if (config.prediction_horizon_s <= 0.0 || config.prediction_horizon_s > 5.0) {
        return "prediction horizon must be in (0, 5] seconds";
    }
    if (config.minimum_rise_c_per_s <= 0.0) {
        return "minimum rise rate must be positive";
    }
    if (config.recovery_stable_ms < 1'000) {
        return "recovery stable duration must be at least one second";
    }
    if (config.recovery_delta_c < 1 ||
        config.recovery_delta_c >= config.trigger_temperature_c) {
        return "recovery delta is invalid";
    }
    return std::nullopt;
}

ProtectionController::ProtectionController(ProtectionConfig config)
    : config_(std::move(config)) {
    if (const auto error = ValidateConfig(config_); error.has_value()) {
        state_ = ProtectionState::Fault;
    }
}

int ProtectionController::recovery_temperature_c() const noexcept {
    return config_.trigger_temperature_c - config_.recovery_delta_c;
}

void ProtectionController::PushSample(const Sample sample) noexcept {
    const auto index = (sample_start_ + sample_count_) % kSampleCapacity;
    if (sample_count_ < kSampleCapacity) {
        samples_[index] = sample;
        ++sample_count_;
        return;
    }

    samples_[sample_start_] = sample;
    sample_start_ = (sample_start_ + 1) % kSampleCapacity;
}

double ProtectionController::CalculateRiseRate(const std::int64_t now_ms) const noexcept {
    double sum_x = 0.0;
    double sum_y = 0.0;
    double sum_xx = 0.0;
    double sum_xy = 0.0;
    std::size_t count = 0;

    for (std::size_t i = 0; i < sample_count_; ++i) {
        const auto& sample = samples_[(sample_start_ + i) % kSampleCapacity];
        if (sample.monotonic_ms < now_ms - kSlopeWindowMs) {
            continue;
        }

        const double x = static_cast<double>(sample.monotonic_ms - now_ms) / 1000.0;
        const double y = static_cast<double>(sample.temperature_c);
        sum_x += x;
        sum_y += y;
        sum_xx += x * x;
        sum_xy += x * y;
        ++count;
    }

    if (count < 4) {
        return 0.0;
    }

    const double n = static_cast<double>(count);
    const double denominator = n * sum_xx - sum_x * sum_x;
    if (std::abs(denominator) < 1e-9) {
        return 0.0;
    }
    return (n * sum_xy - sum_x * sum_y) / denominator;
}

ProtectionDecision ProtectionController::CurrentDecision() const noexcept {
    ProtectionDecision decision;
    decision.state = state_;
    decision.safe_latched = safe_latched_;
    return decision;
}

ProtectionDecision ProtectionController::Trip(
    const TripReason reason,
    const double slope,
    const double predicted) {
    safe_latched_ = true;
    state_ = ProtectionState::SafeLatched;
    recovery_since_ms_.reset();
    predictive_hits_ = 0;

    auto decision = CurrentDecision();
    decision.action = ProtectionAction::ApplySafePower;
    decision.trip_reason = reason;
    decision.rise_c_per_s = slope;
    decision.predicted_temperature_c = predicted;
    return decision;
}

void ProtectionController::ResetPredictor() noexcept {
    predictive_hits_ = 0;
    sample_start_ = 0;
    sample_count_ = 0;
}

ProtectionDecision ProtectionController::ObserveTemperature(
    const std::int64_t monotonic_ms,
    const int temperature_c) {
    if (state_ == ProtectionState::Fault) {
        return CurrentDecision();
    }

    PushSample({monotonic_ms, temperature_c});

    if (!safe_latched_) {
        if (temperature_c >= config_.trigger_temperature_c) {
            return Trip(
                TripReason::HardLimit,
                CalculateRiseRate(monotonic_ms),
                static_cast<double>(temperature_c));
        }

        const double slope = CalculateRiseRate(monotonic_ms);
        const double predicted = static_cast<double>(temperature_c) +
                                 std::max(0.0, slope) * config_.prediction_horizon_s;
        const bool near_limit =
            temperature_c >= config_.trigger_temperature_c - config_.predictive_band_c;
        const bool predictive_condition =
            near_limit && slope >= config_.minimum_rise_c_per_s &&
            predicted >= static_cast<double>(config_.trigger_temperature_c);

        if (predictive_condition) {
            ++predictive_hits_;
            if (predictive_hits_ >= 2) {
                return Trip(TripReason::PredictedCrossing, slope, predicted);
            }
            state_ = ProtectionState::PreTrip;
        } else {
            predictive_hits_ = 0;
            state_ = ProtectionState::Armed;
        }

        auto decision = CurrentDecision();
        decision.rise_c_per_s = slope;
        decision.predicted_temperature_c = predicted;
        return decision;
    }

    if (temperature_c <= recovery_temperature_c()) {
        if (!recovery_since_ms_.has_value()) {
            recovery_since_ms_ = monotonic_ms;
        }
        if (monotonic_ms - *recovery_since_ms_ >= config_.recovery_stable_ms) {
            state_ = ProtectionState::ReadyToRestore;
        } else {
            state_ = ProtectionState::SafeLatched;
        }
    } else {
        recovery_since_ms_.reset();
        state_ = ProtectionState::SafeLatched;
    }

    return CurrentDecision();
}

ProtectionDecision ProtectionController::SensorUnavailable() {
    state_ = ProtectionState::GpuUnavailable;
    return CurrentDecision();
}

ProtectionDecision ProtectionController::SensorRecovered() {
    ResetPredictor();
    recovery_since_ms_.reset();

    auto decision = CurrentDecision();
    if (safe_latched_) {
        state_ = ProtectionState::SafeLatched;
        decision = CurrentDecision();
        decision.action = ProtectionAction::ApplySafePower;
    } else {
        state_ = ProtectionState::Armed;
        decision = CurrentDecision();
        decision.action = ProtectionAction::ApplyNormalPower;
    }
    return decision;
}

ProtectionDecision ProtectionController::RestorePersistedSafeLatch() {
    safe_latched_ = true;
    state_ = ProtectionState::SafeLatched;
    recovery_since_ms_.reset();
    ResetPredictor();
    auto decision = CurrentDecision();
    decision.action = ProtectionAction::ApplySafePower;
    return decision;
}

ProtectionDecision ProtectionController::RequestRestore(
    const std::int64_t monotonic_ms,
    const int current_temperature_c) {
    (void)monotonic_ms;
    if (state_ != ProtectionState::ReadyToRestore ||
        current_temperature_c > recovery_temperature_c()) {
        return CurrentDecision();
    }

    safe_latched_ = false;
    state_ = ProtectionState::Armed;
    recovery_since_ms_.reset();
    ResetPredictor();

    auto decision = CurrentDecision();
    decision.action = ProtectionAction::ApplyNormalPower;
    return decision;
}

const char* ToString(const ProtectionState state) noexcept {
    switch (state) {
        case ProtectionState::Armed: return "Armed";
        case ProtectionState::PreTrip: return "PreTrip";
        case ProtectionState::SafeLatched: return "SafeLatched";
        case ProtectionState::ReadyToRestore: return "ReadyToRestore";
        case ProtectionState::GpuUnavailable: return "GpuUnavailable";
        case ProtectionState::Fault: return "Fault";
    }
    return "Unknown";
}

const char* ToString(const ProtectionAction action) noexcept {
    switch (action) {
        case ProtectionAction::None: return "None";
        case ProtectionAction::ApplySafePower: return "ApplySafePower";
        case ProtectionAction::ApplyNormalPower: return "ApplyNormalPower";
    }
    return "Unknown";
}

const char* ToString(const TripReason reason) noexcept {
    switch (reason) {
        case TripReason::None: return "None";
        case TripReason::HardLimit: return "HardLimit";
        case TripReason::PredictedCrossing: return "PredictedCrossing";
    }
    return "Unknown";
}

}  // namespace gtg
