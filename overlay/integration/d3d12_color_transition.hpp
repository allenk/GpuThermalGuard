#pragma once
#include "d3d12_lifecycle_gate.hpp"

namespace gtg::overlay::integration {
struct ColorTransitionResult { HRESULT original_result; bool qualified; };
template<class Ops>
void RecordColorTransitionRefusal(Ops& ops, LONG reason) noexcept {
    if constexpr (requires { ops.RefuseColor(reason); }) ops.RefuseColor(reason);
}
template<class Clock, class Ops, class Call>
ColorTransitionResult ForwardColorTransition(D3d12LifecycleGate& gate, Clock& clock,
    Ops& ops, Call&& call) noexcept {
    const bool owned = gate.BeginTransition(clock);
    const bool drained = owned && ops.BeforeColor(gate.Remaining(clock));
    if (!drained) RecordColorTransitionRefusal(ops, -251);
    const HRESULT result = call(); // Preserve even failure/timeout HRESULT.
    if (FAILED(result)) RecordColorTransitionRefusal(ops, -270);
    bool qualified = false;
    if (owned) {
        if (drained && SUCCEEDED(result)) {
            const bool blocked = gate.Disabled();
            if (blocked) RecordColorTransitionRefusal(ops, -271);
            qualified = !blocked && ops.AfterColor(result);
        }
        if (!qualified) ops.AbortColor();
        gate.EndTransition(qualified);
    }
    return {result, qualified};
}
}
