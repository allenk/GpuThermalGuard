#pragma once

// The one undocumented call in this project: empty every working set on the
// system in a single operation.
//
// Everything about it is deliberately conservative, because none of it is
// documented:
//
//  - Resolved at run time from ntdll, which is already loaded in every Windows
//    process before kernel32, so this adds no binary dependency. If the export
//    is absent the feature reports itself unsupported and does nothing.
//  - Never reached from the overlay. It trims the foreground process too,
//    which during a game is exactly the stutter the reader is trying to avoid.
//    Its only route is a tray entry whose confirmation dialog is the consent.
//  - Fails closed. A missing export, a privilege that cannot be enabled, or a
//    non-success status all mean nothing happened, and the caller is told so.
//    It never falls back to the documented tier: a reader who asked for the
//    blunt tool must not be told they got it when they did not.
//
// The enum below has been stable since Vista and is depended on by RAMMap,
// Process Explorer, System Informer and a long tail of memory tools, so a
// renumbering would break all of them at once. That is the reason to believe
// it, not a guarantee. If it ever did shift, every command in the enum is
// still a memory reclaim, so the operation performed would remain a deep
// memory clean -- which is what the reader confirmed.

#include <windows.h>

#include <cstdint>

namespace gtg::sysmem::reclaim::nt {

// SYSTEM_INFORMATION_CLASS value for the memory-list commands.
inline constexpr ULONG kSystemMemoryListInformation = 80;

// SYSTEM_MEMORY_LIST_COMMAND. Only the first is issued here; the rest are
// named so a future reader knows what neighbours the command has, and why a
// shift in the numbering would still land on a memory reclaim.
enum MemoryListCommand : ULONG {
    kMemoryCaptureAccessedBits = 0,
    kMemoryCaptureAndResetAccessedBits = 1,
    kMemoryEmptyWorkingSets = 2,
    kMemoryFlushModifiedList = 3,
    kMemoryPurgeStandbyList = 4,
    kMemoryPurgeLowPriorityStandbyList = 5,
};

enum class Status {
    Succeeded,
    Unsupported,     // ntdll has no such export on this Windows
    NoPrivilege,     // SeProfileSingleProcessPrivilege could not be enabled
    Refused,         // the call returned a failure status
};

namespace detail {

using NtSetSystemInformationFn = LONG(NTAPI*)(ULONG, PVOID, ULONG);

[[nodiscard]] inline NtSetSystemInformationFn Resolve() noexcept {
    // GetModuleHandle, not LoadLibrary: ntdll is already mapped into every
    // process, so there is nothing to load and nothing to unload.
    const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (ntdll == nullptr) return nullptr;
    return reinterpret_cast<NtSetSystemInformationFn>(
        reinterpret_cast<void*>(
            GetProcAddress(ntdll, "NtSetSystemInformation")));
}

// Enables one privilege for this process, restoring nothing: the process is
// already running elevated to write power limits, and the privilege is
// harmless while merely enabled.
[[nodiscard]] inline bool EnablePrivilege(const wchar_t* name) noexcept {
    HANDLE token = nullptr;
    if (OpenProcessToken(GetCurrentProcess(),
                         TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY,
                         &token) == FALSE) {
        return false;
    }
    TOKEN_PRIVILEGES privileges{};
    privileges.PrivilegeCount = 1;
    privileges.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    bool ok = LookupPrivilegeValueW(nullptr, name,
                                    &privileges.Privileges[0].Luid) != FALSE;
    if (ok) {
        ok = AdjustTokenPrivileges(token, FALSE, &privileges, 0, nullptr,
                                   nullptr) != FALSE;
        // AdjustTokenPrivileges reports success even when it granted nothing,
        // so the error code is the only way to know.
        if (ok && GetLastError() == ERROR_NOT_ALL_ASSIGNED) ok = false;
    }
    CloseHandle(token);
    return ok;
}

}  // namespace detail

// Is the entry point present at all? Used to grey the menu rather than offer
// something that cannot work.
[[nodiscard]] inline bool Available() noexcept {
    return detail::Resolve() != nullptr;
}

[[nodiscard]] inline Status EmptyAllWorkingSets() noexcept {
    const auto set_system_information = detail::Resolve();
    if (set_system_information == nullptr) return Status::Unsupported;
    if (!detail::EnablePrivilege(SE_PROF_SINGLE_PROCESS_NAME)) {
        return Status::NoPrivilege;
    }
    ULONG command = kMemoryEmptyWorkingSets;
    const LONG status = set_system_information(
        kSystemMemoryListInformation, &command, sizeof(command));
    return status >= 0 ? Status::Succeeded : Status::Refused;
}

[[nodiscard]] constexpr const wchar_t* Describe(const Status status) noexcept {
    switch (status) {
        case Status::Succeeded: return L"succeeded";
        case Status::Unsupported: return L"unsupported";
        case Status::NoPrivilege: return L"no privilege";
        case Status::Refused: return L"refused";
    }
    return L"unknown";
}

}  // namespace gtg::sysmem::reclaim::nt
