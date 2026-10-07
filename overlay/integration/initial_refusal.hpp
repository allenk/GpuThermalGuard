#pragma once
#include <windows.h>

namespace gtg::overlay::integration {
constexpr LONG kWaitingSdrEvidence = -203;
enum class InitialRefusal : LONG {
    Backend = -210, Renderer = -211, Signal = -212,
    Chain3 = -220, ChainIdentity = -221, Device = -222,
    DeviceIdentity = -223, Description = -224, Window = -225,
    WindowPid = -226, NodeCount = -227, Format = -228,
    PresentMethod = -240, ResizeMethod = -241, Resize1Method = -242,
    FullscreenMethod = -243, Present1Method = -244, ColorSpaceMethod = -245
};
inline void RecordInitialRefusal(volatile LONG& ready, InitialRefusal reason) noexcept {
    // Fixed-size atomic diagnostic only; no formatting, locks or I/O in Present.
    InterlockedCompareExchange(&ready, static_cast<LONG>(reason), 0);
}
inline void RecordInitialReady(volatile LONG& ready) noexcept {
    InterlockedCompareExchange(&ready, 1, 0);
}
template<class Compare>
inline void PublishRuntimeRefusal(LONG reason, Compare&& compare) noexcept {
    const auto mutable_state = [](LONG value) noexcept {
        return value == 0 || value == 1 || value == kWaitingSdrEvidence;
    };
    // Caller owns admission or first closes it with Failure(). Only the one
    // already admitted Present can still publish initial ready plus one status
    // update. Three fixed CAS attempts cover those two changes, without waiting
    // for a thread or spinning. Any previously published fatal reason wins.
    const LONG first = compare(reason, 0);
    if (first == 0 || !mutable_state(first)) return;
    const LONG second = compare(reason, first);
    if (second == first || !mutable_state(second)) return;
    (void)compare(reason, second);
}
inline void RecordRuntimeRefusal(volatile LONG& ready, LONG reason) noexcept {
    PublishRuntimeRefusal(reason, [&](LONG value, LONG expected) noexcept {
        return InterlockedCompareExchange(&ready, value, expected);
    });
}
inline void RecordColorWait(volatile LONG& ready) noexcept {
    InterlockedCompareExchange(&ready, kWaitingSdrEvidence, 1);
}
inline void ClearColorWait(volatile LONG& ready) noexcept {
    // Only a pending state can recover. A fatal refusal must never be erased
    // by an already admitted Present that finishes later.
    InterlockedCompareExchange(&ready, 1, kWaitingSdrEvidence);
}
}
