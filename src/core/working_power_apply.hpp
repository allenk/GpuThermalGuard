#pragma once

#include "core/protection.hpp"

namespace gtg {
enum class ApplyStatus { Rejected, WriteFailed, Applied, AppliedNotSaved };
struct ApplyResult {
    ApplyStatus status{ApplyStatus::Rejected};
    bool safe_verified{};
};

// Called only by the protection owner, after its current safety decision.
// Registry and hardware cannot be committed atomically: verified hardware wins
// for the active configuration; persistence failure is reported separately.
template <typename Writer, typename SafeWriter, typename Save>
ApplyResult ApplyWorkingPower(ProtectionController& controller,
    ProtectionConfig& active, const ProtectionConfig& requested,
    std::optional<int> temperature, std::int64_t sample_age_ms,
    Writer&& writer, SafeWriter&& safe_writer, Save&& save) {
    // Armed and MonitorOnly are both states a reader may configure from.
    // MonitorOnly is where every fresh install begins, so excluding it here
    // made the very first configuration the one setting that could never be
    // applied -- observed on an RTX 4080, where Apply was refused every time
    // while the same build worked on a machine that already had stored
    // settings and therefore never entered the state.
    //
    // This rule also lives in ProtectionController::UpdateWorkingConfig, and
    // the copy here is why relaxing it there was not enough.
    const bool configurable = controller.state() == ProtectionState::Armed ||
                              controller.state() == ProtectionState::MonitorOnly;
    if (ValidateConfig(requested) || controller.safe_latched() ||
        !configurable || !temperature ||
        sample_age_ms < 0 || sample_age_ms >= 1000 ||
        *temperature >= active.trigger_temperature_c - active.predictive_band_c ||
        *temperature >= requested.trigger_temperature_c - requested.predictive_band_c)
        return {};
    auto candidate = controller;
    if (!candidate.UpdateWorkingConfig(requested)) return {};
    if (!writer()) {
        (void)controller.RestorePersistedSafeLatch();
        return {ApplyStatus::WriteFailed, safe_writer()};
    }
    controller = candidate;
    active = requested;
    return {save() ? ApplyStatus::Applied : ApplyStatus::AppliedNotSaved, false};
}
}  // namespace gtg
