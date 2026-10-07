#pragma once
#include <windows.h>
namespace gtg::overlay::integration {
// Internal reason codes use the existing runtime field, not a new IPC layout.
template<class Ops>
LONG DiagnoseSdrOutput(Ops& ops) noexcept {
    if (!ops.Parent()) return -260;
    if (!ops.Current()) return -261;
    if (!ops.Containing()) return -262;
    if (!ops.Output6()) return -263;
    if (!ops.Description()) return -264;
    if (!ops.Sdr()) return -265;
    return 1;
}
template<class Probe, class Rebuild>
LONG DiagnoseOutputGeneration(bool drained, bool initialized, bool original_ok, bool blocked,
    Probe&& output, Rebuild&& rebuild) noexcept {
    const LONG reason = !drained ? -251 : !original_ok ? -270 : blocked ? -271 : output();
    const bool rebuilt = !initialized || (drained && rebuild(reason == 1));
    return reason != 1 ? reason : rebuilt ? 1 : -272;
}
}
