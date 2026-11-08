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
#ifdef GTG_FEATURE_UPDATE
// The supervisor's process id, as the supervisor wrote it into the mapping; 0
// outside a supervised Tray.
DWORD SupervisorPid() noexcept;
#endif
bool TakeRecoveryLatch() noexcept;
void MarkCleanExit() noexcept;
void ReportHealth(bool ready, bool valid_sample = true) noexcept;
// The heartbeat of a Tray on a machine with no NVIDIA GPU: monitoring only.
void ReportNoGpu() noexcept;
void RequestRecovery() noexcept;
// A GPU appeared while the Tray monitored only: replace it with a fresh Tray
// that protects it.
void RequestFreshStart() noexcept;
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
