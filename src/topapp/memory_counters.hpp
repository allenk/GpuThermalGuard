#pragma once

#include <windows.h>
#include <psapi.h>

#include <cstdint>
#include <optional>

namespace gtg::topapp {

// PROCESS_MEMORY_COUNTERS_EX2 is declared only for NTDDI_WIN10_CU and later;
// this build targets plain Windows 10. Same layout, documented.
struct MemoryCountersEx2 {
    PROCESS_MEMORY_COUNTERS_EX base;
    SIZE_T private_working_set;
    ULONG64 shared_commit;
};

// What the private working set holds before the call. No process has that
// many bytes, so a value still equal to it was not written.
inline constexpr SIZE_T kUnwrittenPrivateWorkingSet = ~SIZE_T{0};

// The structure to hand K32GetProcessMemoryInfo.
[[nodiscard]] inline MemoryCountersEx2 MemoryCountersRequest() noexcept {
    MemoryCountersEx2 counters{};
    counters.base.cb = sizeof(counters);
    counters.private_working_set = kUnwrittenPrivateWorkingSet;
    return counters;
}

// A process's private bytes from a call that succeeded: its private working
// set where this Windows reported one, else its commit charge.
//
// A Windows older than the larger structure need not refuse it: handed a size
// larger than it knows, Windows succeeds, fills what it knows, reports that in
// `cb` and leaves the rest as it was. Read without these checks, every process
// on such a Windows holds nothing, and a RAM alert never names one.
[[nodiscard]] inline std::optional<std::uint64_t> PrivateBytes(
    const MemoryCountersEx2& counters) noexcept {
    if (counters.base.cb >= sizeof(MemoryCountersEx2) &&
        counters.private_working_set != kUnwrittenPrivateWorkingSet) {
        return counters.private_working_set;
    }
    if (counters.base.cb >= sizeof(PROCESS_MEMORY_COUNTERS_EX)) return counters.base.PrivateUsage;
    return std::nullopt;
}

}  // namespace gtg::topapp
