#pragma once
#include <dxgi.h>

namespace gtg::overlay::integration {
constexpr UINT kOutputAdapterLimit = 8;
constexpr UINT kOutputPerAdapterLimit = 16;

template <class Ops>
LONG FindFreshMonitorOutput(Ops& ops) noexcept {
    for (UINT adapter = 0; adapter < kOutputAdapterLimit; ++adapter) {
        const HRESULT found_adapter = ops.Adapter(adapter);
        if (found_adapter == DXGI_ERROR_NOT_FOUND) return -283;
        if (FAILED(found_adapter)) return -282;
        for (UINT output = 0; output < kOutputPerAdapterLimit; ++output) {
            const HRESULT found_output = ops.Output(output);
            if (found_output == DXGI_ERROR_NOT_FOUND) break;
            if (FAILED(found_output)) return -282;
            if (!ops.OutputDescription()) return -264;
            if (ops.MatchesMonitor()) return 1;
            if (output + 1 == kOutputPerAdapterLimit) return -285;
        }
    }
    return -285;
}

template <class Ops>
LONG QualifyFreshOutput(Ops& ops) noexcept {
    if (!ops.Window()) return -281;
    if (!ops.Create()) return -280;
    if (!ops.Current()) return -261;
    const LONG found = ops.Find();
    if (found != 1) return found;
    if (!ops.Output6()) return -263;
    if (!ops.Description()) return -264;
    if (!ops.Sdr()) return -265;
    if (!ops.Current()) return -261;
    if (!ops.SameMonitor()) return -284;
    return 1;
}
}
