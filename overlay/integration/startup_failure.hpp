// A refusal reason, bounded and fit for one log field. The failure path cannot
// allocate or throw, and a Win32 error number is not a reason: many distinct
// refusals used to arrive as one generic helper failure.
#pragma once
#include <array>
#include <cstddef>
#include <string_view>

namespace gtg::overlay::integration {
inline constexpr std::size_t kStartupFailureCapacity = 256;

// Control characters, quotes and backslashes become spaces: the reason is
// written as one quoted field on one line, and nothing in it may end the field
// early or escape it.
inline std::array<char, kStartupFailureCapacity> StartupFailureReason(
    std::string_view reason) noexcept {
    if (reason.empty()) reason = "unknown attach failure";
    std::array<char, kStartupFailureCapacity> result{};
    for (std::size_t i = 0; i < reason.size() && i + 1 < result.size(); ++i) {
        const auto byte = static_cast<unsigned char>(reason[i]);
        const bool printable = byte >= 32 && byte != 127;
        const bool quoting = byte == '"' || byte == '\\';
        result[i] = printable && !quoting ? reason[i] : ' ';
    }
    return result;
}
}  // namespace gtg::overlay::integration
