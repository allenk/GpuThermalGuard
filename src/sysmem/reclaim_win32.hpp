#pragma once

// The Windows half of the host memory reclaim. Every decision it makes comes
// from `reclaim.hpp`, which is pure and tested without a desktop; this file
// only walks processes and issues the calls.
//
// Everything here is documented and every entry point is exported from
// kernel32.dll, so the single /MT EXE gains no sidecar dependency:
//
//   K32EnumProcesses, K32GetProcessMemoryInfo, K32EmptyWorkingSet
//       -- psapi.h declarations, kernel32 exports at PSAPI_VERSION >= 2
//   OpenProcess, GetForegroundWindow, GetWindowThreadProcessId, GetTickCount64
//   SetSystemFileCacheSize
//
// This must never run on the protection worker or on the UI thread. On the UI
// thread the message loop would stall, which would also stop the busy
// indicator animating -- the animation is not decoration, it is evidence that
// the work is off the message loop.

#define PSAPI_VERSION 2

#include <windows.h>
#include <psapi.h>

#include <cstdint>
#include <vector>

#include "sysmem/reclaim.hpp"

namespace gtg::sysmem::reclaim {

namespace detail {

// The process that owns the foreground window, or zero when there is none.
// Zero is the safe answer: it only means nothing is excluded on that ground.
[[nodiscard]] inline std::uint32_t ForegroundPid() noexcept {
    const HWND foreground = GetForegroundWindow();
    if (foreground == nullptr) return 0;
    DWORD pid = 0;
    (void)GetWindowThreadProcessId(foreground, &pid);
    return static_cast<std::uint32_t>(pid);
}

[[nodiscard]] inline std::uint64_t AvailablePhysicalBytes() noexcept {
    MEMORYSTATUSEX status{};
    status.dwLength = sizeof(status);
    if (GlobalMemoryStatusEx(&status) == 0) return 0;
    return status.ullAvailPhys;
}

}  // namespace detail

// Walks every process the caller may open, trims the ones the policy accepts,
// then flushes the system file cache working set.
//
// `now_ms` is injected rather than read here so the budget can be exercised in
// a test without waiting five seconds.
template <class Clock>
[[nodiscard]] inline Outcome RunWith(const Policy& policy, Clock&& now_ms) {
    Outcome outcome;
    const std::uint64_t started = now_ms();
    const auto self = static_cast<std::uint32_t>(GetCurrentProcessId());
    const std::uint32_t foreground = detail::ForegroundPid();
    outcome.available_before_bytes = detail::AvailablePhysicalBytes();

    std::vector<DWORD> pids(4096);
    DWORD bytes = 0;
    if (K32EnumProcesses(pids.data(),
                         static_cast<DWORD>(pids.size() * sizeof(DWORD)),
                         &bytes) == FALSE) {
        outcome.available_after_bytes = outcome.available_before_bytes;
        return outcome;
    }
    pids.resize(bytes / sizeof(DWORD));

    for (const DWORD raw_pid : pids) {
        if (BudgetSpent(started, now_ms())) {
            outcome.timed_out = true;
            break;
        }
        ++outcome.considered;
        const auto pid = static_cast<std::uint32_t>(raw_pid);

        Candidate candidate;
        candidate.pid = pid;
        candidate.is_self = pid == self;
        candidate.is_foreground = pid == foreground;
        // The idle process and System. Neither has a working set we may touch,
        // and asking is a wasted open.
        candidate.is_system_critical = pid == 0 || pid == 4;
        if (candidate.is_self || candidate.is_system_critical ||
            (policy.skip_foreground && candidate.is_foreground)) {
            ++outcome.skipped;
            continue;
        }

        // PROCESS_SET_QUOTA to trim, PROCESS_QUERY_LIMITED_INFORMATION to read
        // the working set first. A refusal here is the normal case for
        // protected and other-user processes; it is not an error.
        const HANDLE process = OpenProcess(
            PROCESS_SET_QUOTA | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, raw_pid);
        if (process == nullptr) {
            ++outcome.skipped;
            continue;
        }

        PROCESS_MEMORY_COUNTERS counters{};
        counters.cb = sizeof(counters);
        if (K32GetProcessMemoryInfo(process, &counters, sizeof(counters)) == FALSE) {
            ++outcome.skipped;
            CloseHandle(process);
            continue;
        }
        candidate.working_set_bytes = counters.WorkingSetSize;

        if (!ShouldTrim(candidate, policy)) {
            ++outcome.skipped;
            CloseHandle(process);
            continue;
        }
        if (policy.dry_run) {
            // Counted separately. `trimmed` means trimmed; a field that means
            // two different things depending on a flag is how a report starts
            // lying to the person reading it.
            ++outcome.would_trim;
            CloseHandle(process);
            continue;
        }
        if (K32EmptyWorkingSet(process) != FALSE) {
            ++outcome.trimmed;
        } else {
            // The process exited between the open and the call, or the system
            // refused. Expected often enough that it is counted, not logged.
            ++outcome.failed;
        }
        CloseHandle(process);
    }

    // The file cache is a separate pool from any process working set, so this
    // is not a second attempt at the same memory. SE_INCREASE_QUOTA_NAME is
    // required; without it the call simply fails and the rest still stands.
    outcome.cache_flushed =
        !policy.dry_run && SetSystemFileCacheSize(static_cast<SIZE_T>(-1),
                                                  static_cast<SIZE_T>(-1),
                                                  0) != FALSE;

    outcome.available_after_bytes = detail::AvailablePhysicalBytes();
    return outcome;
}

[[nodiscard]] inline Outcome Run(const Policy& policy) {
    return RunWith(policy, []() noexcept { return GetTickCount64(); });
}

}  // namespace gtg::sysmem::reclaim
