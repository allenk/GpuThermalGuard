#pragma once
#include "install_diagnostics.hpp"
#include "os/win32/thread_transaction.h"
namespace gtg::research {
// This adapter writes preallocated local POD only. Lowercase methods implement
// the existing Splice compile-time Ops interface; no engine API is modified.
template<class Native = splice::os::win32::NativeThreadOps>
struct InstallThreadOps {
    InstallThreadOps(InstallSite& evidence, std::span<splice::os::win32::ThreadSlot> enlisted) noexcept
        : site(evidence), slots(enlisted) {
        lookup_count_ = std::min(slots.size(), lookup_.size());
        for (std::size_t i = 0; i < lookup_count_; ++i)
            lookup_[i] = {reinterpret_cast<std::uintptr_t>(slots[i].handle), i};
        std::sort(lookup_.begin(), lookup_.begin() + lookup_count_,
            [](const Lookup& a, const Lookup& b) { return a.handle < b.handle; });
    }
    InstallSite& site;
    std::span<splice::os::win32::ThreadSlot> slots;
    Native native{};
    std::atomic<void*>* publication{};
    const splice::os::win32::ThreadSlot* Find(HANDLE h) const noexcept {
        const auto handle = reinterpret_cast<std::uintptr_t>(h);
        const auto end = lookup_.begin() + lookup_count_;
        const auto found = std::lower_bound(lookup_.begin(), end, handle,
            [](const Lookup& item, std::uintptr_t key) { return item.handle < key; });
        return found != end && found->handle == handle ? &slots[found->index] : nullptr;
    }
    DWORD Id(HANDLE h) const noexcept {
        const auto* slot = Find(h); return slot ? slot->id : 0;
    }
    bool Aborting() const noexcept { return publication && publication->load(std::memory_order_acquire) == nullptr; }
    void Error(InstallOperation operation, DWORD error, HANDLE h = nullptr, bool cleanup = false) noexcept {
        if (!cleanup) RecordInstallError(site.primary, operation, 0, error, true, Id(h));
        if (cleanup) RecordInstallError(site.cleanup, operation, 0, error, true, Id(h));
    }
    DWORD thread_id(HANDLE h) noexcept {
        const DWORD result = native.thread_id(h);
        if (!result) Error(InstallOperation::ThreadIdentity, GetLastError(), h);
        return result;
    }
    DWORD process_id(HANDLE h) noexcept {
        const DWORD result = native.process_id(h);
        if (!result) Error(InstallOperation::ProcessIdentity, GetLastError(), h);
        return result;
    }
    DWORD current_thread() noexcept { return native.current_thread(); }
    DWORD current_process() noexcept { return native.current_process(); }
    DWORD suspend(HANDLE h) noexcept {
        const DWORD result = native.suspend(h);
        if (result == MAXDWORD) Error(InstallOperation::Suspend, GetLastError(), h);
        return result;
    }
    DWORD resume(HANDLE h) noexcept {
        const DWORD result = native.resume(h);
        if (result == MAXDWORD) {
            const DWORD error = GetLastError();
            if (!Aborting()) Error(InstallOperation::Resume, error, h);
            Error(InstallOperation::Resume, error, h, true);
        } else {
            const auto* found = Find(h);
            if (found && result != found->prior_suspend + 1) {
                const auto& slot = *found;
                const auto record = [&](InstallError& evidence) noexcept {
                    if (evidence.operation != InstallOperation::None) return;
                    RecordInstallError(evidence, InstallOperation::Resume, 1, 0, false, slot.id);
                    evidence.expected_count = slot.prior_suspend + 1;
                    evidence.observed_count = result;
                };
                if (!Aborting()) record(site.primary);
                record(site.cleanup);
            }
        }
        return result;
    }
    bool get(HANDLE h, CONTEXT& thread_context) noexcept {
        const bool result = native.get(h, thread_context);
        if (!result) Error(InstallOperation::Context, GetLastError(), h);
        return result;
    }
    bool set(HANDLE h, const CONTEXT& thread_context) noexcept {
        const bool result = native.set(h, thread_context);
        if (!result) {
            const DWORD error = GetLastError();
            const auto* slot = Find(h);
            const bool restoring = slot && thread_context.Rip == slot->saved.Rip;
            Error(restoring ? InstallOperation::RestoreContext : InstallOperation::Migrate, error, h, restoring);
        }
        return result;
    }
    bool flush(void* target) noexcept {
        const bool result = native.flush(target);
        if (!result) Error(InstallOperation::Flush, GetLastError());
        return result;
    }
private:
    struct Lookup { std::uintptr_t handle{}; std::size_t index{}; };
    std::array<Lookup, splice::os::win32::max_enlisted_threads> lookup_;
    std::size_t lookup_count_{};
};
}  // namespace gtg::research
