#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace gtg::topapp {

// The GPU counters' instance names carry the process and the adapter:
// `pid_1234_luid_0x00000000_0x0000C5A3_phys_0_eng_0_engtype_3D` for an engine,
// `pid_1234_luid_0x00000000_0x0000C5A3_phys_0` for memory. Parsed by hand,
// without allocating: a sweep reads some 2600 of them.
struct InstanceId {
    std::uint32_t pid{};
    std::wstring_view luid;   // `0x00000000_0x0000C5A3`, as written
};

[[nodiscard]] constexpr bool ParseInstance(const std::wstring_view name, InstanceId& out) noexcept {
    constexpr std::wstring_view kPid = L"pid_";
    constexpr std::wstring_view kLuid = L"_luid_";
    // Two hexadecimal halves joined by an underscore.
    constexpr std::size_t kLuidLength = 21;
    if (name.substr(0, kPid.size()) != kPid) return false;
    std::size_t i = kPid.size();
    const std::size_t digits = i;
    std::uint64_t pid = 0;
    while (i < name.size() && name[i] >= L'0' && name[i] <= L'9' && i - digits < 10) {
        pid = pid * 10 + static_cast<std::uint64_t>(name[i] - L'0');
        ++i;
    }
    if (i == digits || pid > 0xFFFFFFFFULL) return false;
    if (name.substr(i, kLuid.size()) != kLuid) return false;
    i += kLuid.size();
    if (name.size() < i + kLuidLength) return false;
    const std::wstring_view luid = name.substr(i, kLuidLength);
    if (luid[0] != L'0' || (luid[1] != L'x' && luid[1] != L'X') || luid[10] != L'_' ||
        luid[11] != L'0' || (luid[12] != L'x' && luid[12] != L'X')) {
        return false;
    }
    out.pid = static_cast<std::uint32_t>(pid);
    out.luid = luid;
    return true;
}

// Equal ignoring ASCII case; `folded` is already lower case.
[[nodiscard]] constexpr bool EqualsFolded(const std::wstring_view text,
                                          const std::wstring_view folded) noexcept {
    if (text.size() != folded.size()) return false;
    for (std::size_t i = 0; i < text.size(); ++i) {
        wchar_t c = text[i];
        if (c >= L'A' && c <= L'Z') c = static_cast<wchar_t>(c - L'A' + L'a');
        if (c != folded[i]) return false;
    }
    return true;
}

}  // namespace gtg::topapp
