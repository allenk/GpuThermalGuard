#pragma once
#include "native_thread_snapshot.hpp"
#include "os/win32/thread_transaction.h"
#include <span>

namespace gtg::research {
// Owned counter-only research. Collection is not proof against new callers.
class SnapshotSlots {
public:
    bool Collect(DWORD required_tid, InstallSite* diagnostic = nullptr) noexcept {
        if (!snapshot_.Collect(splice::os::win32::max_enlisted_threads, diagnostic)) return false;
        try {
            slots_.assign(snapshot_.Handles().size(), {});
        } catch (const std::bad_alloc&) {
            if (diagnostic) RecordInstallError(diagnostic->primary, InstallOperation::Allocation);
            return false;
        }
        bool found = required_tid == 0; // Named-game research has no host-provided caller TID.
        for (std::size_t i = 0; i < slots_.size(); ++i) {
            slots_[i].handle = snapshot_.Handles()[i];
            found |= GetThreadId(slots_[i].handle) == required_tid;
        }
        if (!found && diagnostic)
            RecordInstallError(diagnostic->primary, InstallOperation::ThreadIdentity, 2, 0, false,
                               required_tid);
        if (diagnostic) diagnostic->threads = static_cast<DWORD>(slots_.size());
        return found;
    }

    std::span<splice::os::win32::ThreadSlot> Slots() noexcept { return slots_; }

    bool Close(InstallSite* diagnostic = nullptr) noexcept { return snapshot_.Close(diagnostic); }

private:
    NativeThreadSnapshot snapshot_;
    std::vector<splice::os::win32::ThreadSlot> slots_;
};
}  // namespace gtg::research
