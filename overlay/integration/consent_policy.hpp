#pragma once
#include "session_policy.hpp"

namespace gtg::overlay::integration {
enum class ColorConsent { Cancel, Strict, AssumeSdr };
inline constexpr int kConsentStrict = 100, kConsentSdr = 101;

template <typename Ask>
ColorConsent SelectColorConsent(bool auto_sdr, Ask&& ask) {
    return auto_sdr ? ColorConsent::AssumeSdr : ask();
}

inline ColorConsent DecodeConsent(int button) noexcept {
    if (button == kConsentStrict) return ColorConsent::Strict;
    if (button == kConsentSdr) return ColorConsent::AssumeSdr;
    return ColorConsent::Cancel;
}

inline bool AcceptConsent(ColorConsent choice, Identity captured, Identity current, bool alive,
                          bool enabled) noexcept {
    return (choice == ColorConsent::Strict || choice == ColorConsent::AssumeSdr) && captured.pid &&
           captured.created && captured == current && alive && enabled;
}
}
