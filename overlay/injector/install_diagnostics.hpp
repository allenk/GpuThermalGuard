#pragma once
#include "hook_protocol.hpp"
#include "staged_attach.hpp"
#include <cstring>
#include <ostream>

namespace gtg::research {
enum class InstallOperation : DWORD {
    None,
    Prepare,
    Collect,
    Snapshot,
    FirstThread,
    NextThread,
    OpenThread,
    ThreadIdentity,
    ProcessIdentity,
    Liveness,
    Capacity,
    Allocation,
    Duplicate,
    Protect,
    Suspend,
    Context,
    Migrate,
    Resume,
    Flush,
    Transaction,
    RestoreProtect,
    Close,
    Exception,
    RestoreContext
};

struct InstallError {
    InstallOperation operation{};
    DWORD reason{}, error{}, error_valid{}, tid{};
    DWORD expected_count{}, observed_count{};
};

struct InstallSite {
    DWORD state{}, threads{}, transaction_entered{}, transaction_stage{}, transaction_outcome{};
    InstallError primary{}, cleanup{};
};

struct InstallReport {
    DWORD outcome{static_cast<DWORD>(AttachResult::kRefused)}, installed{}, failed_site{MAXDWORD},
        site_count{};
    InstallSite sites[7]{};
};

struct alignas(8) InstallDiagnostics {
    DWORD magic{}, version{}, size{}, pid{};
    UINT64 created{};
    volatile LONG complete{};
    DWORD reserved{};
    InstallReport report{};
};

static_assert(sizeof(InstallError) == 28 && sizeof(InstallSite) == 76);
static_assert(offsetof(InstallDiagnostics, report) == 32 && sizeof(InstallDiagnostics) == 584);
constexpr DWORD kInstallMagic = 0x49475447;
constexpr DWORD kInstallCauseUnavailable = 0x80000000U;

inline UINT64 InstallCreationTime(HANDLE process) noexcept {
    FILETIME created{}, exit{}, kernel{}, user{};
    if (!GetProcessTimes(process, &created, &exit, &kernel, &user)) return 0;
    return (static_cast<UINT64>(created.dwHighDateTime) << 32) | created.dwLowDateTime;
}

inline bool ValidInstallDiagnostics(const InstallDiagnostics& data, DWORD pid,
                                    UINT64 created) noexcept {
    return data.magic == kInstallMagic && data.version == 1 && data.size == sizeof(data) &&
           data.pid == pid && created != 0 && data.created == created;
}

inline void InstallSectionName(DWORD pid, wchar_t (&name)[64]) noexcept {
    SectionName(pid, name);
    wcscat_s(name, L".Install");
}

inline void RecordInstallError(InstallError& destination, InstallOperation operation,
                               DWORD reason = 0, DWORD error = 0, bool valid = false,
                               DWORD tid = 0) noexcept {
    if (destination.operation == InstallOperation::None)
        destination = {operation, reason, valid ? error : 0, valid ? 1U : 0U, tid};
}

inline bool PublishInstallReport(InstallDiagnostics& data, const InstallReport& report) noexcept {
    if (InterlockedCompareExchange(&data.complete, -1, 0) != 0) return false;
    data.report = report;
    InterlockedExchange(&data.complete, 1);
    return true;
}

inline bool ReadInstallReport(const InstallDiagnostics& data, DWORD pid, UINT64 created,
                              InstallReport& report) noexcept {
    // A load, not an interlocked read-modify-write: readers can map FILE_MAP_READ.
    if (!ValidInstallDiagnostics(data, pid, created) ||
        std::atomic_ref<LONG>(const_cast<LONG&>(data.complete)).load(std::memory_order_acquire) !=
            1)
        return false;
    report = data.report;
    return report.site_count <= 7 && report.installed <= report.site_count &&
           (report.failed_site == MAXDWORD || report.failed_site < report.site_count);
}

inline void FinishInstallReport(InstallReport& report, AttachOutcome result) noexcept {
    report.outcome = static_cast<DWORD>(result.result);
    report.installed = static_cast<DWORD>(result.installed);
}

template <class Ops>
struct RecordedInstallSites {
    Ops& ops;
    InstallReport& report;

    bool Prepare(std::size_t i) {
        report.failed_site = static_cast<DWORD>(i);
        const bool result = ops.Prepare(i);
        report.sites[i].state = result ? 1 : 4;
        if (!result)
            RecordInstallError(report.sites[i].primary, InstallOperation::Prepare);
        else
            report.failed_site = MAXDWORD;
        return result;
    }

    SiteResult Install(std::size_t i) {
        report.failed_site = static_cast<DWORD>(i);
        auto& site = report.sites[i];
        site.state = 2;
        const auto result = ops.Install(i);
        site.state = result == SiteResult::kInstalled ? 3 : result == SiteResult::kRefused ? 4 : 5;
        if (result == SiteResult::kInstalled) report.failed_site = MAXDWORD;
        return result;
    }
};

class InstallDiagnosticMapping {
public:
    InstallDiagnosticMapping() = default;
    InstallDiagnosticMapping(const InstallDiagnosticMapping&) = delete;
    InstallDiagnosticMapping& operator=(const InstallDiagnosticMapping&) = delete;

    ~InstallDiagnosticMapping() {
        if (data_) UnmapViewOfFile(data_);
        if (handle_) CloseHandle(handle_);
    }

    bool Create(DWORD pid, UINT64 created) noexcept {
        wchar_t name[64]{};
        InstallSectionName(pid, name);
        handle_ = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                                     sizeof(InstallDiagnostics), name);
        if (!handle_ || GetLastError() == ERROR_ALREADY_EXISTS) return false;
        data_ = static_cast<InstallDiagnostics*>(
            MapViewOfFile(handle_, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(InstallDiagnostics)));
        if (!data_ || !created) return false;
        *data_ = {kInstallMagic, 1, sizeof(InstallDiagnostics), pid, created};
        return true;
    }

    bool OpenWriter(DWORD pid, UINT64 created) noexcept {
        wchar_t name[64]{};
        InstallSectionName(pid, name);
        handle_ = OpenFileMappingW(FILE_MAP_WRITE | FILE_MAP_READ, FALSE, name);
        if (!handle_) return false;
        data_ = static_cast<InstallDiagnostics*>(MapViewOfFile(
            handle_, FILE_MAP_WRITE | FILE_MAP_READ, 0, 0, sizeof(InstallDiagnostics)));
        return data_ && ValidInstallDiagnostics(*data_, pid, created);
    }

    bool OpenReader(DWORD pid, UINT64 created) noexcept {
        wchar_t name[64]{};
        InstallSectionName(pid, name);
        handle_ = OpenFileMappingW(FILE_MAP_READ, FALSE, name);
        if (!handle_) return false;
        data_ = static_cast<InstallDiagnostics*>(
            MapViewOfFile(handle_, FILE_MAP_READ, 0, 0, sizeof(InstallDiagnostics)));
        return data_ && ValidInstallDiagnostics(*data_, pid, created);
    }

    InstallDiagnostics* Data() const noexcept { return data_; }

    void RetainUntilProcessExit() noexcept {
        // A resident hook keeps one bounded diagnostic mapping alive after the
        // helper exits. OS process teardown reclaims it; hot unload is forbidden.
        handle_ = nullptr;
        data_ = nullptr;
    }

private:
    HANDLE handle_{};
    InstallDiagnostics* data_{};
};

inline const char* InstallOperationName(InstallOperation operation) noexcept {
    constexpr const char* names[] = {"none",        "prepare",         "collect",
                                     "snapshot",    "first-thread",    "next-thread",
                                     "open-thread", "thread-identity", "process-identity",
                                     "liveness",    "capacity",        "allocation",
                                     "duplicate",   "protect",         "suspend",
                                     "context",     "migrate",         "resume",
                                     "flush",       "transaction",     "restore-protect",
                                     "close",       "exception",       "restore-context"};
    const auto index = static_cast<DWORD>(operation);
    return index < std::size(names) ? names[index] : "invalid";
}

inline void PrintInstallReport(std::ostream& output, const InstallReport& report) {
    output << "install outcome=" << report.outcome << " installed=" << report.installed << '/'
           << report.site_count << " failed_site=" << report.failed_site << '\n';
    for (DWORD i = 0; i < report.site_count; ++i) {
        constexpr const char* roles[] = {
            "Present",  "Signal",        "ResizeBuffers", "ResizeBuffers1", "SetFullscreenState",
            "Present1", "SetColorSpace1"};
        const auto& site = report.sites[i];
        output << "install site=" << i << " role=" << roles[i] << " state=" << site.state
               << " threads=" << site.threads << " transaction=" << site.transaction_entered << ':'
               << site.transaction_outcome << ':' << site.transaction_stage;
        for (const auto* error : {&site.primary, &site.cleanup})
            output << (error == &site.primary ? " primary=" : " cleanup=")
                   << InstallOperationName(error->operation) << " reason=" << error->reason
                   << " win32_valid=" << error->error_valid << " win32=" << error->error
                   << " tid=" << error->tid << " count=" << error->observed_count << '/'
                   << error->expected_count;
        output << '\n';
    }
}
}  // namespace gtg::research
