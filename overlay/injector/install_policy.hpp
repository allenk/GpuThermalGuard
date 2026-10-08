#pragma once
#include "os/win32/thread_transaction.h"

namespace gtg::research {
template <class Plan>
bool OneInstructionEntry(const Plan& plan) noexcept {
    if (plan.copy_size() != 5) return false;
    const auto address = reinterpret_cast<std::uintptr_t>(plan.target());
    if ((address & 7) != 0) return false;
    for (std::uintptr_t offset = 1; offset < 5; ++offset) {
        std::uintptr_t mapped{};
        if (plan.map_ip(address + offset, mapped)) return false;
    }
    return true;
}

// Recovery uses retained handles and saved contexts. No allocation, logging,
// loader or application lock. Failure preserves state for the next attempt.
template <class Ops>
bool RecoverEnlisted(std::span<splice::os::win32::ThreadSlot> slots,
                     splice::os::win32::TransactionStage stage, bool published, void* target,
                     Ops& ops, bool& counts_uncertain) noexcept {
    using splice::os::win32::TransactionStage;
    if (stage == TransactionStage::rollback) {
        if (published) return false;
        bool restored = true;
        for (auto& slot : slots) {
            if (!slot.context_attempted) continue;
            if (!ops.set(slot.handle, slot.saved))
                restored = false;
            else
                slot.context_attempted = false;
        }
        if (!restored) return false; // Do not resume a partially restored set.
    } else if (stage != TransactionStage::flush && stage != TransactionStage::resume)
        return false;
    if (published && !ops.flush(target)) return false;
    bool resumed = true;
    for (std::size_t i = slots.size(); i != 0; --i) {
        auto& slot = slots[i - 1];
        if (!slot.suspended) continue;
        slot.resume_result = ops.resume(slot.handle);
        if (slot.resume_result == MAXDWORD) {
            resumed = false;
            continue;
        }
        slot.suspended = false;
        if (slot.resume_result != slot.prior_suspend + 1) counts_uncertain = true;
    }
    return resumed && !counts_uncertain;
}
} // namespace gtg::research
