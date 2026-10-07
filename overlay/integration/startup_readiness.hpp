#pragma once
#include "../injector/hook_protocol.hpp"
namespace gtg::overlay::integration {
enum class InspectionState { Waiting, Ready, Refused };
struct InspectionEvidence {
    LONG hook_state{}, backend{}, ready{};
    bool presents{}, errors{}, ambiguous{}, signal{}, nested{}, queue{};
    bool identity_ready{}, metadata_valid{}, window_owned{}, mismatch{}, staged_draw{};
};
inline InspectionState ClassifyInspection(const InspectionEvidence& e) noexcept {
    if (e.hook_state == static_cast<LONG>(research::HookState::kWaiting)) return InspectionState::Waiting;
    if (e.hook_state != static_cast<LONG>(research::HookState::kInstalled) || e.errors || e.ambiguous ||
        (e.ready < 0 && e.ready != -203)) return InspectionState::Refused;
    if (!e.presents || e.ready != 1 || !e.backend || !e.identity_ready) return InspectionState::Waiting;
    if (e.backend != 1 && e.backend != 2) return InspectionState::Refused;
    if (e.backend == 2 && !e.signal) return InspectionState::Refused;
    if (e.backend == 2 && (!e.nested || !e.queue)) return InspectionState::Waiting;
    if (!e.metadata_valid || !e.window_owned || e.mismatch || e.staged_draw) return InspectionState::Refused;
    return InspectionState::Ready;
}
template<class Clock, class Ops>
bool WaitForInspection(Clock& clock, Ops& ops) noexcept {
    const auto started = clock.Now();
    while (ops.Continue()) {
        if (clock.Now() - started >= 10000) return false;
        const auto state = ops.Inspect();
        const auto elapsed = clock.Now() - started;
        if (!ops.Continue() || elapsed >= 10000) return false;
        if (state == InspectionState::Ready) return true;
        if (state == InspectionState::Refused) return false;
        const auto remaining = 10000 - elapsed;
        clock.Pause(static_cast<DWORD>(remaining < 25 ? remaining : 25));
    }
    return false;
}
}
