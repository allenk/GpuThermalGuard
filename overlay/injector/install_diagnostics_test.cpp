#include "install_diagnostics.hpp"
#include "install_thread_ops.hpp"
#include "native_thread_snapshot.hpp"
#include <cstdio>
#include <stdexcept>
using namespace gtg::research;

void Check(bool ok, const char* why) {
    if (!ok) throw std::runtime_error(why);
}

struct FakeSites {
    InstallReport& report;
    int failure;
    InstallOperation operation;
    bool recovery{};
    DWORD legacy_stage{};

    bool Prepare(std::size_t) { return true; }

    SiteResult Install(std::size_t i) {
        if (static_cast<int>(i) == failure) {
            RecordInstallError(report.sites[i].primary, operation, 0, ERROR_ACCESS_DENIED, true);
            return recovery ? SiteResult::kRecoveryRequired : SiteResult::kRefused;
        }
        legacy_stage = 10;
        return SiteResult::kInstalled;
    }
};

struct FakeNative {
    DWORD resume_value{MAXDWORD};

    DWORD suspend(HANDLE) {
        SetLastError(ERROR_ACCESS_DENIED);
        return MAXDWORD;
    }

    DWORD resume(HANDLE) {
        SetLastError(ERROR_INVALID_HANDLE);
        return resume_value;
    }

    bool get(HANDLE, CONTEXT&) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return false;
    }

    bool set(HANDLE, const CONTEXT&) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return false;
    }
};

int main() {
    try {
        static_assert(sizeof(HookSection) == 712);
        // Guard the read-only PowerShell runtime decoder against layout drift.
        static_assert(offsetof(HookSection, product_version) == 556);
        static_assert(offsetof(HookSection, product_backend) == 560);
        static_assert(offsetof(HookSection, product_ready) == 564);
        static_assert(offsetof(HookSection, control_samples) == 220);
        static_assert(offsetof(HookSection, control_errors) == 232);
        static_assert(offsetof(HookSection, product_color_policy) == 696);
        static_assert(offsetof(HookSection, product_color_provenance) == 700);
        static_assert(offsetof(HookSection, product_target_created) == 704);
        for (auto operation : {InstallOperation::Collect, InstallOperation::Protect}) {
            for (int failure = -1; failure < 7; ++failure) {
                for (bool recovery : {false, true}) {
                    std::atomic<bool> gate{false};
                    InstallReport report{};
                    report.site_count = 7;
                    FakeSites native{report, failure, operation, recovery};
                    RecordedInstallSites sites{native, report};
                    const auto result = AttachStaged(7, gate, sites);
                    FinishInstallReport(report, result);
                    Check(gate.load() == (failure == -1), "only complete installation activates");
                    Check(report.installed == (failure == -1 ? 7U : static_cast<DWORD>(failure)),
                          "exact confirmed prefix");
                    Check(report.failed_site ==
                              (failure == -1 ? MAXDWORD : static_cast<DWORD>(failure)),
                          "exact failed site");
                    if (failure > 0)
                        Check(native.legacy_stage == 10 && !gate.load(),
                              "legacy complete stage hides later collection refusal");
                    if (failure >= 0)
                        Check(result.result == (recovery  ? AttachResult::kRecoveryRequired
                                                : failure ? AttachResult::kPartialResident
                                                          : AttachResult::kRefused),
                              "unchanged residency policy");
                }
            }
        }
        InstallSite native_site{};
        splice::os::win32::ThreadSlot slot{};
        slot.handle = reinterpret_cast<HANDLE>(1);
        slot.id = 123;
        InstallThreadOps<FakeNative> ops{native_site, std::span(&slot, 1)};
        Check(ops.resume(slot.handle) == MAXDWORD, "native resume failure forwarded");
        Check(native_site.primary.operation == InstallOperation::Resume &&
                  native_site.primary.error == ERROR_INVALID_HANDLE,
              "first resume failure retains native primary error");
        native_site = {};
        Check(ops.suspend(slot.handle) == MAXDWORD, "native suspend forwarded");
        SetLastError(0);
        CONTEXT context{};
        Check(!ops.get(slot.handle, context), "native context forwarded");
        Check(native_site.primary.operation == InstallOperation::Suspend &&
                  native_site.primary.error == ERROR_ACCESS_DENIED &&
                  native_site.primary.tid == 123,
              "adapter preserves first native error and thread");
        native_site = {};
        std::atomic<void*> unpublished{};
        InstallThreadOps<FakeNative> abort_ops{native_site, std::span(&slot, 1)};
        abort_ops.publication = &unpublished;
        abort_ops.resume(slot.handle);
        Check(native_site.primary.operation == InstallOperation::None &&
                  native_site.cleanup.operation == InstallOperation::Resume,
              "abort cleanup is not fabricated as initiating cause");
        native_site = {};
        abort_ops.native.resume_value = 0;
        Check(abort_ops.resume(slot.handle) == 0 &&
                  native_site.cleanup.operation == InstallOperation::Resume &&
                  native_site.cleanup.reason == 1 && !native_site.cleanup.error_valid &&
                  native_site.cleanup.tid == 123,
              "successful resume count mismatch retains semantic thread evidence");
        Check(native_site.cleanup.expected_count == 1 && native_site.cleanup.observed_count == 0,
              "resume count values retained");
        native_site = {};
        context.Rip = slot.saved.Rip;
        Check(!abort_ops.set(slot.handle, context) &&
                  native_site.primary.operation == InstallOperation::None &&
                  native_site.cleanup.operation == InstallOperation::RestoreContext &&
                  native_site.cleanup.tid == 123,
              "rollback native error stays cleanup, not migration cause");
        std::vector<splice::os::win32::ThreadSlot> indexed_slots(4096);
        for (std::size_t i = 0; i < indexed_slots.size(); ++i) {
            indexed_slots[i].handle = reinterpret_cast<HANDLE>(2 * (4096 - i));
            indexed_slots[i].id = static_cast<DWORD>(i + 1);
        }
        InstallThreadOps<FakeNative> indexed_ops{native_site, indexed_slots};
        for (const auto& indexed_slot : indexed_slots)
            Check(indexed_ops.Id(indexed_slot.handle) == indexed_slot.id,
                  "full capacity sorted handle index preserves slot identity");
        Check(indexed_ops.Id(nullptr) == 0, "unknown handle lookup");
        for (DWORD site = 0; site < 7; ++site) {
            for (auto operation : {InstallOperation::Collect, InstallOperation::Protect}) {
                InstallDiagnostics data{kInstallMagic, 1, sizeof(InstallDiagnostics), 42, 99};
                InstallReport report{};
                report.site_count = 7;
                report.failed_site = site;
                report.installed = site;
                report.outcome = static_cast<DWORD>(site ? AttachResult::kPartialResident
                                                         : AttachResult::kRefused);
                RecordInstallError(report.sites[site].primary, operation, 0, ERROR_ACCESS_DENIED,
                                   true, 123);
                SetLastError(ERROR_SUCCESS);
                RecordInstallError(report.sites[site].primary, InstallOperation::Close, 0,
                                   ERROR_INVALID_HANDLE, true);
                RecordInstallError(report.sites[site].cleanup, InstallOperation::Close, 0,
                                   ERROR_INVALID_HANDLE, true);
                Check(PublishInstallReport(data, report), "terminal publication");
                InstallReport read{};
                Check(ReadInstallReport(data, 42, 99, read), "matching terminal reader");
                Check(read.failed_site == site && read.installed == site &&
                          read.sites[site].primary.operation == operation &&
                          read.sites[site].primary.error == ERROR_ACCESS_DENIED &&
                          read.sites[site].primary.tid == 123 &&
                          read.sites[site].cleanup.error == ERROR_INVALID_HANDLE,
                      "first failure survives cleanup");
                Check(!PublishInstallReport(data, {}), "terminal immutable");
                Check(!ReadInstallReport(data, 43, 99, read) &&
                          !ReadInstallReport(data, 42, 100, read),
                      "identity mismatch");
                data.version = 2;
                Check(!ReadInstallReport(data, 42, 99, read), "mixed version refused");
            }
        }
        InstallDiagnostics pending{kInstallMagic, 1, sizeof(InstallDiagnostics), 42, 99};
        InstallReport read{};
        Check(!ReadInstallReport(pending, 42, 99, read), "pending reader");
        pending.complete = -1;
        Check(!ReadInstallReport(pending, 42, 99, read), "writer-in-progress reader");
        pending.complete = 1;
        pending.magic = 0;
        Check(!ReadInstallReport(pending, 42, 99, read), "invalid magic");
        pending.magic = kInstallMagic;
        pending.size = 1;
        Check(!ReadInstallReport(pending, 42, 99, read), "invalid size");
        pending.size = sizeof(pending);
        pending.report.site_count = 8;
        Check(!ReadInstallReport(pending, 42, 99, read), "invalid report bounds");
        InstallError semantic{};
        RecordInstallError(semantic, InstallOperation::Liveness, WAIT_OBJECT_0);
        Check(semantic.operation == InstallOperation::Liveness && !semantic.error_valid,
              "semantic refusal not stale Win32 error");
        NativeThreadSnapshot snapshot;
        InstallSite capacity{};
        SetLastError(ERROR_ACCESS_DENIED);
        Check(!snapshot.Collect(0, &capacity) &&
                  capacity.primary.operation == InstallOperation::Capacity &&
                  !capacity.primary.error_valid && snapshot.Handles().empty(),
              "real collection semantic refusal has no stale error");
        InstallDiagnosticMapping mapping;
        const DWORD pid = GetCurrentProcessId();
        const auto created = InstallCreationTime(GetCurrentProcess());
        Check(mapping.Create(pid, created), "native fresh mapping");
        Check(PublishInstallReport(*mapping.Data(), {}), "native terminal publication");
        wchar_t name[64]{};
        InstallSectionName(pid, name);
        HANDLE handle = OpenFileMappingW(FILE_MAP_READ, FALSE, name);
        Check(handle != nullptr, "read-only reader handle");
        const auto* view = static_cast<const InstallDiagnostics*>(
            MapViewOfFile(handle, FILE_MAP_READ, 0, 0, sizeof(InstallDiagnostics)));
        Check(view && ReadInstallReport(*view, pid, created, read),
              "read-only mapping never writes atomic marker");
        Check(UnmapViewOfFile(view) && CloseHandle(handle), "native reader cleanup");
        std::puts("PASS install ledger identity, publication, first failure and cleanup");
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "FAIL %s\n", e.what());
        return 1;
    }
}
