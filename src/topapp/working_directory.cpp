#include "topapp/working_directory.hpp"

#include <windows.h>
#include <winternl.h>

#include <cstddef>
#include <cstdint>
#include <format>
#include <vector>

#include "logging/logger.hpp"

namespace gtg::topapp {
namespace {

// No documented call gives another process's working directory. It is where
// the process keeps it: PEB.ProcessParameters (documented in winternl.h), and
// in the parameters the CURDIR whose first member is the DOS path, at 0x38 on
// x64 -- inside what winternl.h declares as Reserved2, unchanged since the
// first x64 Windows. The documented neighbours are checked, so a header that
// lays the structure out differently fails the build instead of a read
// landing on another field.
constexpr std::size_t kCurrentDirectoryOffset = 0x38;
static_assert(sizeof(void*) == 8, "the parameters' layout here is the 64-bit one");
static_assert(offsetof(RTL_USER_PROCESS_PARAMETERS, ImagePathName) == 0x60,
              "ImagePathName follows CurrentDirectory (0x38) and DllPath (0x50)");
static_assert(offsetof(PEB, ProcessParameters) == 0x20, "the PEB's documented layout");

using QueryProcess = NTSTATUS(NTAPI*)(HANDLE, PROCESSINFOCLASS, PVOID, ULONG, PULONG);

// ntdll is in every process, so resolving it adds nothing to the import
// table.
QueryProcess QueryProcessFunction() noexcept {
    static const QueryProcess function = [] {
        const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
        return ntdll == nullptr ? nullptr
                                : reinterpret_cast<QueryProcess>(
                                      GetProcAddress(ntdll, "NtQueryInformationProcess"));
    }();
    return function;
}

// Exactly `size` bytes at `address` in `process`, or false.
bool ReadExactly(const HANDLE process, const std::uintptr_t address, void* into,
                 const std::size_t size) noexcept {
    SIZE_T read = 0;
    return address != 0 &&
           ReadProcessMemory(process, reinterpret_cast<LPCVOID>(address), into, size, &read) &&
           read == size;
}

std::uint64_t Join(const FILETIME& time) noexcept {
    return (static_cast<std::uint64_t>(time.dwHighDateTime) << 32) | time.dwLowDateTime;
}

}  // namespace

std::optional<std::wstring> ReadWorkingDirectory(const Identity& identity) noexcept {
    const QueryProcess query = QueryProcessFunction();
    if (query == nullptr || identity.pid == 0) return std::nullopt;
    const HANDLE process =
        OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ, FALSE, identity.pid);
    if (process == nullptr) return std::nullopt;
    std::optional<std::wstring> directory;
    try {
        FILETIME creation{};
        FILETIME exit{};
        FILETIME kernel{};
        FILETIME user{};
        BOOL wow64 = TRUE;
        PROCESS_BASIC_INFORMATION basic{};
        ULONG returned = 0;
        std::uintptr_t parameters = 0;
        UNICODE_STRING path{};
        // The process it was asked about, still running, 64-bit; its PEB, its
        // parameters, and the directory's string, each read whole.
        if (GetProcessTimes(process, &creation, &exit, &kernel, &user) &&
            Join(creation) == identity.created && Join(exit) == 0 &&
            IsWow64Process(process, &wow64) && wow64 == FALSE &&
            query(process, ProcessBasicInformation, &basic, sizeof(basic), &returned) >= 0 &&
            basic.PebBaseAddress != nullptr &&
            ReadExactly(process,
                        reinterpret_cast<std::uintptr_t>(basic.PebBaseAddress) +
                            offsetof(PEB, ProcessParameters),
                        &parameters, sizeof(parameters)) &&
            ReadExactly(process, parameters + kCurrentDirectoryOffset, &path, sizeof(path)) &&
            path.Length != 0 && path.Length % 2 == 0 && path.Length / 2 <= kMaxDirectoryChars &&
            path.Buffer != nullptr) {
            std::vector<wchar_t> text(path.Length / 2);
            if (ReadExactly(process, reinterpret_cast<std::uintptr_t>(path.Buffer), text.data(),
                            path.Length)) {
                directory = NormalDirectory(std::wstring_view(text.data(), text.size()));
            }
        }
    } catch (...) {
        directory.reset();
    }
    CloseHandle(process);
    return directory;
}

ProcessTokenScope::ProcessTokenScope() noexcept {
    HANDLE token = nullptr;
    if (OpenThreadToken(GetCurrentThread(), TOKEN_IMPERSONATE, TRUE, &token)) {
        ready_ = RevertToSelf() != FALSE;
        if (ready_) {
            saved_ = token;
        } else {
            CloseHandle(token);
        }
    } else {
        ready_ = GetLastError() == ERROR_NO_TOKEN;
    }
}

ProcessTokenScope::~ProcessTokenScope() {
    if (saved_ == nullptr) return;
    if (!SetThreadToken(nullptr, saved_)) {
        const DWORD error = GetLastError();
        try {
            (void)logging::TryWarning(
                std::format(L"top app sampler could not take back its thread token: error {}; "
                            L"virtual machines are not named",
                            error));
        } catch (...) {
        }
    }
    CloseHandle(saved_);
}

}  // namespace gtg::topapp
