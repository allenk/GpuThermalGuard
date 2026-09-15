#pragma once
#include <windows.h>
#include <string>

namespace gtg::supervision {
// Internal roles only. No NVML code or DLL is used by RunSupervisor.
struct SupervisorOptions {
    const wchar_t* singleton_name{L"Local\\GpuThermalGuard.Supervisor.v1"};
    unsigned watchdog_ms{90000};
    bool journal{true};
};
int RunSupervisor(const std::wstring& executable, int show_command,
                  const SupervisorOptions& options = {});
bool AttachChild(const wchar_t* mapping_handle) noexcept;
bool IsChild() noexcept;
bool TakeRecoveryLatch() noexcept;
void MarkCleanExit() noexcept;
void ReportHealth(bool ready, bool valid_sample = true) noexcept;
void RequestRecovery() noexcept;
std::string TargetUuid();
bool PinTargetUuid(const std::string& uuid) noexcept;
// Shared by Service and local protection; acquired on the writer thread.
class WriterLease {
public:
    WriterLease() noexcept;
    ~WriterLease();
    WriterLease(const WriterLease&) = delete;
    WriterLease& operator=(const WriterLease&) = delete;
    bool acquired() const noexcept { return acquired_; }
private:
    HANDLE handle_{};
    bool acquired_{};
};
}  // namespace gtg::supervision
