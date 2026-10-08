#pragma once
#include "color_choice_policy.hpp"
#include "../injector/hook_protocol.hpp"

namespace gtg::overlay::integration {
inline bool ValidProductColorContract(const research::HookSection& s, DWORD pid,
                                      std::uint64_t created) noexcept {
    return s.header.magic == research::kHookMagic && s.header.version == research::kVersion &&
           s.header.size == sizeof(s) && s.header.target_pid == pid && pid && created &&
           s.product_version == research::kProductVersion && s.product_target_created == created &&
           ValidColorPolicy(s.product_color_policy) && s.product_color_provenance == 0;
}
}
