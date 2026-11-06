#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "topapp/app_name.hpp"

namespace gtg::topapp {

// Processes named by what they are rather than by an executable. A virtual
// machine's load is charged to two of them: its worker (`vmwp.exe`) holds the
// GPU work and video memory, and a minimal process with no executable at all
// (`vmmemWSL`, `vmmem`) holds the CPU time and memory. Linux programs in WSL
// are invisible to Windows, so the honest name is the VM's (owner, 2026-10-16).
// Other minimal processes -- Memory Compression, System, Registry -- have only
// their kernel name.
enum class SystemKind {
    Wsl,
    VirtualMachine,
    Kernel,
};

struct SystemProcess {
    SystemKind kind{};
    // The group key. A VM's worker and memory process share one, so the VM is
    // one app; every kernel process has its own, so two never merge.
    std::wstring key;
    // The kernel name, for SystemKind::Kernel; the VM kinds are named by the
    // caller in the reader's language.
    std::wstring name;
};

// One row of the system process list.
struct ProcessEntry {
    std::uint32_t pid{};
    std::uint32_t parent{};
    std::wstring name;
};

[[nodiscard]] inline bool IsVmWorkerName(const std::wstring_view name) noexcept {
    return EqualsIgnoringAsciiCase(name, L"vmwp.exe");
}

[[nodiscard]] inline bool IsVmMemoryName(const std::wstring_view name) noexcept {
    return name.size() >= 5 && EqualsIgnoringAsciiCase(name.substr(0, 5), L"vmmem");
}

// A VM worker is asked about by its image path; only the system's own copy
// counts.
[[nodiscard]] inline bool IsVmWorker(const std::wstring_view image_path) {
    constexpr std::wstring_view kTail = L"\\system32\\vmwp.exe";
    const std::wstring key = GroupKey(image_path);
    return key.size() >= kTail.size() &&
           key.compare(key.size() - kTail.size(), kTail.size(), kTail.data(), kTail.size()) == 0;
}

// The name a kernel process is shown with: its own, cleaned and cut like any
// other. Empty when it has none.
[[nodiscard]] inline std::wstring KernelProcessName(const std::wstring_view name) {
    return CutToWidth(CleanName(ExecutableName(name)), kMaxNameWidth);
}

// Every process in `entries`, classified. A memory process belongs to the VM
// whose worker is its parent; WSL is the VM whose memory process is
// `vmmemWSL`. A worker with no memory process is a VM of its own. A process
// with no name is left out, so it is never ranked.
[[nodiscard]] inline std::unordered_map<std::uint32_t, SystemProcess> ClassifySystemProcesses(
    const std::vector<ProcessEntry>& entries) {
    std::unordered_map<std::uint32_t, const ProcessEntry*> by_pid;
    for (const ProcessEntry& entry : entries)
        by_pid.emplace(entry.pid, &entry);

    std::unordered_map<std::uint32_t, SystemProcess> out;
    for (const ProcessEntry& entry : entries) {
        if (IsVmWorkerName(entry.name)) {
            out.emplace(
                entry.pid,
                SystemProcess{SystemKind::VirtualMachine, L"vm|" + std::to_wstring(entry.pid), {}});
        }
    }
    for (const ProcessEntry& entry : entries) {
        if (!IsVmMemoryName(entry.name)) continue;
        const bool wsl = EqualsIgnoringAsciiCase(entry.name, L"vmmemWSL");
        const auto parent = by_pid.find(entry.parent);
        const bool has_worker = entry.parent != entry.pid && parent != by_pid.end() &&
                                IsVmWorkerName(parent->second->name);
        const SystemProcess vm{
            wsl ? SystemKind::Wsl : SystemKind::VirtualMachine,
            wsl ? std::wstring(L"vm|wsl")
                : L"vm|" + std::to_wstring(has_worker ? entry.parent : entry.pid),
            {}};
        out.insert_or_assign(entry.pid, vm);
        if (has_worker) out.insert_or_assign(entry.parent, vm);
    }
    for (const ProcessEntry& entry : entries) {
        if (out.contains(entry.pid)) continue;
        std::wstring name = KernelProcessName(entry.name);
        if (name.empty()) continue;
        out.emplace(entry.pid,
                    SystemProcess{SystemKind::Kernel, L"sys|" + std::to_wstring(entry.pid),
                                  std::move(name)});
    }
    return out;
}

// The latest process list, read again only when a process newer than it asks.
// The list is 40-50 ms to read (it carries every thread), so it is never read
// per sweep; the processes it names live as long as their VM or as Windows.
class SystemProcessMap {
public:
    static constexpr std::uint64_t kRefreshSweeps = 10;

    // The process `pid` created at `created` (FILETIME units), as the list
    // names it, or nothing. A process created before the list was read was in
    // it, so the list's row for its pid is that process and never an earlier
    // owner of the pid. A newer process asks for a new list, at most once
    // every kRefreshSweeps sweeps, and is not named until one covers it.
    // `take` fills the entries it is given and returns when it began reading,
    // or nothing when it could not read.
    template <typename Take>
    const SystemProcess* Find(const std::uint32_t pid, const std::uint64_t created,
                              const std::uint64_t generation, Take&& take) {
        if (!Covers(created)) {
            if (refreshed_ && generation < *refreshed_ + kRefreshSweeps) return nullptr;
            refreshed_ = generation;
            std::vector<ProcessEntry> entries;
            const std::optional<std::uint64_t> taken = take(entries);
            if (!taken) return nullptr;
            by_pid_ = ClassifySystemProcesses(entries);
            taken_ = taken;
            if (!Covers(created)) return nullptr;
        }
        const auto found = by_pid_.find(pid);
        return found == by_pid_.end() ? nullptr : &found->second;
    }

private:
    [[nodiscard]] bool Covers(const std::uint64_t created) const noexcept {
        return taken_ && created <= *taken_;
    }

    std::unordered_map<std::uint32_t, SystemProcess> by_pid_;
    std::optional<std::uint64_t> taken_;
    std::optional<std::uint64_t> refreshed_;
};

}  // namespace gtg::topapp
