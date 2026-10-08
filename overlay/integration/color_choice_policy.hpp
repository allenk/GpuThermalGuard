#pragma once
#include "sdr_target_format.hpp"
#include <windows.h>

namespace gtg::overlay::integration {
enum class ColorPolicy : DWORD { Strict = 0, ResearchAssumeSdr = 1 };
enum class ColorProvenance : LONG {
    None,
    Legacy8,
    ObservedSdr,
    ResearchAssumedSdr,
    WaitingUnknown
};
constexpr LONG kColorFatalBit = 0x100;

inline bool ValidColorPolicy(DWORD value) noexcept {
    return value == static_cast<DWORD>(ColorPolicy::Strict) ||
           value == static_cast<DWORD>(ColorPolicy::ResearchAssumeSdr);
}

inline ColorProvenance DecideSdrAdmission(DXGI_FORMAT format, bool observed, ColorPolicy policy,
                                          bool fatal = false) noexcept {
    if (fatal || !ValidColorPolicy(static_cast<DWORD>(policy)) || !IsSupportedSdrTarget(format))
        return ColorProvenance::None;
    if (format != DXGI_FORMAT_R10G10B10A2_UNORM) return ColorProvenance::Legacy8;
    if (observed) return ColorProvenance::ObservedSdr;
    return policy == ColorPolicy::ResearchAssumeSdr ? ColorProvenance::ResearchAssumedSdr
                                                    : ColorProvenance::WaitingUnknown;
}

inline void RecordColorProvenance(volatile LONG& value, ColorProvenance source) noexcept {
    const LONG previous = InterlockedCompareExchange(&value, 0, 0);
    if (!(previous & kColorFatalBit))
        InterlockedCompareExchange(&value, static_cast<LONG>(source), previous);
}

inline void StopColorProvenance(volatile LONG& value) noexcept {
    InterlockedOr(&value, kColorFatalBit);
}
}
