#pragma once
#include <windows.h>
#include <cstdint>
#include "session_policy.hpp"
namespace gtg::overlay::integration {
inline constexpr DWORD kControlMagic = 0x49475447;
enum class HelperState : LONG { Starting, Ready, Failed, Closed };
struct alignas(8) Control {
    DWORD magic{kControlMagic}, version{1}, size{sizeof(Control)}, parent_pid{};
    Identity target{};
    std::uint64_t parent_created{};
    volatile LONG enabled{1}, stop{}, state{}, error{};
};
inline std::uint64_t CreationTime(HANDLE process) noexcept {
    FILETIME c{}, e{}, k{}, u{};
    return GetProcessTimes(process, &c, &e, &k, &u)
               ? (static_cast<std::uint64_t>(c.dwHighDateTime) << 32) | c.dwLowDateTime
               : 0;
}
inline bool ValidControl(const Control& c) noexcept {
    return c.magic == kControlMagic && c.version == 1 && c.size == sizeof(Control) &&
           c.parent_pid && c.parent_created && c.target.pid && c.target.created;
}
}  // namespace gtg::overlay::integration
