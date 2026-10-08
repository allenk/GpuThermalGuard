#pragma once
#include <windows.h>
#include <tlhelp32.h>
#include <algorithm>
#include <cstddef>
#include <new>
#include <vector>
#include "install_diagnostics.hpp"

namespace gtg::research {
// Pre-suspension, current-process snapshot only. Not a caller-closure proof.
class NativeThreadSnapshot {
public:
    NativeThreadSnapshot() = default;
    NativeThreadSnapshot(const NativeThreadSnapshot&) = delete;
    NativeThreadSnapshot& operator=(const NativeThreadSnapshot&) = delete;

    ~NativeThreadSnapshot() { Close(); }

    bool Collect(std::size_t capacity, InstallSite* diagnostic = nullptr) noexcept {
        const auto fail = [&](InstallOperation operation, DWORD reason = 0, DWORD error = 0,
                              bool valid = false, DWORD tid = 0) noexcept {
            if (diagnostic) {
                diagnostic->threads = static_cast<DWORD>(handles_.size());
                RecordInstallError(diagnostic->primary, operation, reason, error, valid, tid);
            }
            Close(diagnostic);
            return false;
        };
        if (!Close(diagnostic)) return fail(InstallOperation::Close);
        if (capacity == 0 || capacity > 4096) return fail(InstallOperation::Capacity);
        std::vector<DWORD> ids;
        try {
            handles_.reserve(capacity);
            ids.reserve(capacity);
        } catch (const std::bad_alloc&) {
            return fail(InstallOperation::Allocation);
        }
        snapshot_ = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        if (snapshot_ == INVALID_HANDLE_VALUE) {
            const DWORD error = GetLastError();
            snapshot_ = nullptr;
            return fail(InstallOperation::Snapshot, 0, error, true);
        }
        const DWORD pid = GetCurrentProcessId(), self = GetCurrentThreadId();
        THREADENTRY32 entry{};
        entry.dwSize = sizeof(entry);
        if (!Thread32First(snapshot_, &entry))
            return fail(InstallOperation::FirstThread, 0, GetLastError(), true);
        std::size_t scanned = 0;
        do {
            if (++scanned > 65536 ||
                entry.dwSize < offsetof(THREADENTRY32, th32OwnerProcessID) + sizeof(DWORD))
                return fail(InstallOperation::Capacity, 1);
            if (entry.th32OwnerProcessID == pid && entry.th32ThreadID != self) {
                if (handles_.size() == capacity) return fail(InstallOperation::Capacity, 2);
                HANDLE handle =
                    OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_SET_CONTEXT |
                                   THREAD_QUERY_INFORMATION | SYNCHRONIZE,
                               FALSE, entry.th32ThreadID);
                if (!handle)
                    return fail(InstallOperation::OpenThread, 0, GetLastError(), true,
                                entry.th32ThreadID);
                handles_.push_back(handle); // Capacity was reserved before acquiring resources.
                const DWORD actual_tid = GetThreadId(handle);
                if (!actual_tid)
                    return fail(InstallOperation::ThreadIdentity, 0, GetLastError(), true,
                                entry.th32ThreadID);
                if (actual_tid != entry.th32ThreadID)
                    return fail(InstallOperation::ThreadIdentity, 1, 0, false, entry.th32ThreadID);
                const DWORD actual_pid = GetProcessIdOfThread(handle);
                if (!actual_pid)
                    return fail(InstallOperation::ProcessIdentity, 0, GetLastError(), true,
                                entry.th32ThreadID);
                if (actual_pid != pid)
                    return fail(InstallOperation::ProcessIdentity, 1, 0, false, entry.th32ThreadID);
                const DWORD wait = WaitForSingleObject(handle, 0);
                if (wait == WAIT_FAILED)
                    return fail(InstallOperation::Liveness, wait, GetLastError(), true,
                                entry.th32ThreadID);
                if (wait != WAIT_TIMEOUT)
                    return fail(InstallOperation::Liveness, wait, 0, false, entry.th32ThreadID);
                ids.push_back(entry.th32ThreadID);
            }
            entry.dwSize = sizeof(entry);
        } while (Thread32Next(snapshot_, &entry));
        const DWORD enumeration_error = GetLastError();
        if (enumeration_error != ERROR_NO_MORE_FILES)
            return fail(InstallOperation::NextThread, 0, enumeration_error, true);
        if (handles_.empty()) return fail(InstallOperation::Collect, 1);
        std::sort(ids.begin(), ids.end());
        if (std::adjacent_find(ids.begin(), ids.end()) != ids.end())
            return fail(InstallOperation::Duplicate);
        if (!CloseHandle(snapshot_)) return fail(InstallOperation::Close, 0, GetLastError(), true);
        snapshot_ = nullptr;
        return true;
    }

    bool Close(InstallSite* diagnostic = nullptr) noexcept {
        bool ok = true;
        if (snapshot_) {
            if (!CloseHandle(snapshot_)) {
                const DWORD error = GetLastError();
                ok = false;
                if (diagnostic)
                    RecordInstallError(diagnostic->cleanup, InstallOperation::Close, 0, error,
                                       true);
            } else
                snapshot_ = nullptr;
        }
        for (auto& handle : handles_) {
            if (handle && !CloseHandle(handle)) {
                const DWORD error = GetLastError();
                ok = false;
                if (diagnostic)
                    RecordInstallError(diagnostic->cleanup, InstallOperation::Close, 0, error,
                                       true);
            } else
                handle = nullptr;
        }
        if (ok) handles_.clear();
        return ok;
    }

    const std::vector<HANDLE>& Handles() const noexcept { return handles_; }

private:
    HANDLE snapshot_{};
    std::vector<HANDLE> handles_;
};
}  // namespace gtg::research
