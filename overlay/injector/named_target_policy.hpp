#pragma once
#include <cstdint>
#include <string_view>

namespace gtg::research {
struct NamedTargetProfile {
    std::wstring_view image, family, counter_dll, draw_dll, association_dll;
};
#ifdef GTG_FOREGROUND_OVERLAY
inline constexpr NamedTargetProfile kNamedProfile{L"foreground target", L"", L"gtg_overlay.dll",
                                                  L"gtg_overlay.dll", L"gtg_overlay.dll"};
#endif
inline bool NamedIdentity(const NamedTargetProfile& profile, std::wstring_view image,
                          std::wstring_view family) noexcept {
    return image == profile.image && family == profile.family;
}

inline bool UnsignedTrialAllowed(bool signature_ok, std::uint32_t signature, bool dynamic_ok,
                                 std::uint32_t dynamic) noexcept {
    // Only known audit/opt-out fields may be set. Never exercise opt-out or
    // remote downgrade, and refuse unknown policy bits as well as enforcement.
    return signature_ok && dynamic_ok && !(signature & ~24U) && !(dynamic & ~14U);
}
}
#ifndef GTG_FOREGROUND_OVERLAY
// Research builds target a named game; the product never includes this.
#include "named_target_research.hpp"
#endif
