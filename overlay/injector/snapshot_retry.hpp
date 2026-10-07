#pragma once
#include "install_diagnostics.hpp"
#include <tlhelp32.h>
#include <cstddef>
namespace gtg::research {
constexpr unsigned kSnapshotCollectionAttempts = 3;
template<class Ops>
bool CollectStableSnapshot(Ops& ops, InstallSite* diagnostic = nullptr) noexcept {
    if (diagnostic && diagnostic->transaction_entered) return false;
    for (unsigned index = 0; index < kSnapshotCollectionAttempts; ++index) {
        InstallSite attempt{};
        const auto publish = [&]() noexcept {
            if (diagnostic) {
                diagnostic->threads = attempt.threads;
                diagnostic->primary = attempt.primary;
                diagnostic->cleanup = attempt.cleanup;
            }
        };
        if (ops.Collect(attempt)) { publish(); return true; }
        const bool closed = ops.Close(attempt);
        const auto& error = attempt.primary;
        if (!closed || attempt.transaction_entered || attempt.cleanup.operation != InstallOperation::None ||
            index + 1 == kSnapshotCollectionAttempts || error.operation != InstallOperation::OpenThread ||
            !error.error_valid || error.error != ERROR_INVALID_PARAMETER || !error.tid ||
            !ops.Absent(error.tid, attempt)) { publish(); return false; }
    }
    return false;
}
inline bool ThreadAbsentFromFreshSnapshot(DWORD tid, InstallSite& diagnostic) noexcept {
    if (!tid) return false;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return false;
    THREADENTRY32 entry{}; entry.dwSize = sizeof(entry);
    bool valid = Thread32First(snapshot, &entry) != FALSE;
    bool present = false;
    unsigned scanned = 0;
    if (valid) {
        do {
            if (++scanned > 65536 || entry.dwSize < offsetof(THREADENTRY32, th32ThreadID) + sizeof(DWORD)) {
                valid = false; break;
            }
            if (entry.th32ThreadID == tid) { present = true; break; }
            entry.dwSize = sizeof(entry);
        } while (Thread32Next(snapshot, &entry));
        if (!present && valid && GetLastError() != ERROR_NO_MORE_FILES) valid = false;
    }
    if (!CloseHandle(snapshot)) {
        RecordInstallError(diagnostic.cleanup, InstallOperation::Close, 0, GetLastError(), true);
        return false;
    }
    return valid && !present;
}
}
