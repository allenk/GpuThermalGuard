#pragma once
#include <cstdint>
#include <windows.h>
#include "output_diagnostics.hpp"
namespace gtg::overlay::integration {
template<class Call, class Fail>
HRESULT ForwardNestedColor(bool sdr, Call&& call, Fail&& fail) noexcept {
    const HRESULT result = call();
    if (!sdr || FAILED(result)) fail();
    return result;
}
inline bool TrackColorSetter(std::uintptr_t selected, std::uintptr_t actual) noexcept {
    return actual && (!selected || selected == actual);
}
template<class Probe, class Rebuild>
bool RequalifyOutputGeneration(bool drained, bool initialized, bool original_ok, bool blocked,
    Probe&& output, Rebuild&& rebuild) noexcept {
    return DiagnoseOutputGeneration(drained, initialized, original_ok, blocked,
        [&]() noexcept { return output() ? 1L : -252L; }, rebuild) == 1;
}
}
