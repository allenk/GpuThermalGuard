#include "supervision/supervisor.hpp"
#include "supervision/recovery_policy.hpp"
#include "logging/logger.hpp"

#include <sddl.h>
#include <array>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <format>
#include <vector>

namespace gtg::supervision {
namespace {
constexpr DWORD kSignature = 0x47544732;
constexpr DWORD kRecoveryExit = 0xE0470001;
constexpr DWORD kHangExit = 0xE0470002;
struct Shared {
    DWORD signature;
    DWORD child_pid;
    LONG recovery;
    volatile LONG clean_exit;
    volatile LONG restart_requested;
    volatile LONG ready;
    volatile LONG identity_ready;
    alignas(8) volatile LONG64 heartbeat;
    alignas(8) volatile LONG64 first_sample;
    char uuid[96];
};
Shared* shared_{};  // Attached before starting threads; mapping lives until exit.

std::uint64_t AwakeMs() noexcept {
    ULONGLONG ticks{};
    if (QueryUnbiasedInterruptTime(&ticks)) return ticks / 10000;
    return GetTickCount64();
}
struct Handle {
    HANDLE value{};
    explicit Handle(HANDLE h = nullptr) : value(h) {}
    ~Handle() { if (value) CloseHandle(value); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
};
struct View {
    Shared* value{};
    ~View() { if (value) UnmapViewOfFile(value); }
};
void ActivateExisting() noexcept {
    DWORD_PTR ignored{};
    (void)SendMessageTimeoutW(HWND_BROADCAST,
        RegisterWindowMessageW(L"GpuThermalGuard.Activate.v1"), 0, 0,
        SMTO_ABORTIFHUNG | SMTO_NORMAL, 1000, &ignored);
}
}

bool AttachChild(const wchar_t* text) noexcept {
    if (!text || !*text) return false;
    wchar_t* end{};
    errno = 0;
    const auto raw = wcstoull(text, &end, 10);
    if (errno || !end || *end || !raw) return false;
    Handle mapping(reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(raw)));
    auto* view = static_cast<Shared*>(MapViewOfFile(mapping.value,
        FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, sizeof(Shared)));
    if (!view) return false;
    if (view->signature != kSignature || view->child_pid != GetCurrentProcessId()) {
        UnmapViewOfFile(view);
        return false;
    }
    shared_ = view;
    // No inheritable supervision handles reach any future subprocess.
    return true;
}
bool IsChild() noexcept { return shared_ != nullptr; }
bool TakeRecoveryLatch() noexcept { return shared_ && InterlockedExchange(&shared_->recovery, 0) != 0; }
void MarkCleanExit() noexcept {
    if (shared_) InterlockedExchange(&shared_->clean_exit, 1);
}
void ReportHealth(bool ready, bool valid_sample) noexcept {
    if (!shared_) return;
    InterlockedExchange(&shared_->ready, ready ? 1 : 0);
    InterlockedExchange64(&shared_->heartbeat, static_cast<LONG64>(AwakeMs()));
    if (valid_sample) InterlockedCompareExchange64(&shared_->first_sample, static_cast<LONG64>(AwakeMs()), 0);
}
void RequestRecovery() noexcept {
    if (!shared_) return;
    InterlockedExchange(&shared_->ready, 0);
    InterlockedExchange(&shared_->restart_requested, 1);
}
std::string TargetUuid() {
    return shared_ && InterlockedCompareExchange(&shared_->identity_ready, 0, 0)
        ? std::string(shared_->uuid, strnlen_s(shared_->uuid, sizeof(shared_->uuid)))
                   : std::string{};
}
bool PinTargetUuid(const std::string& uuid) noexcept {
    if (uuid.empty() || uuid.size() >= sizeof(Shared::uuid)) return false;
    if (!shared_) return true;
    // Sole protection-owner writer publishes once; UI reads only after the barrier.
    if (InterlockedCompareExchange(&shared_->identity_ready, 0, 0)) return uuid == shared_->uuid;
    memcpy(shared_->uuid, uuid.c_str(), uuid.size() + 1);
    InterlockedExchange(&shared_->identity_ready, 1);
    return true;
}

WriterLease::WriterLease() noexcept {
    PSECURITY_DESCRIPTOR descriptor{};
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
            L"D:P(A;;GA;;;SY)(A;;GA;;;BA)", SDDL_REVISION_1, &descriptor, nullptr)) return;
    SECURITY_ATTRIBUTES attributes{sizeof(attributes), descriptor, FALSE};
    handle_ = CreateMutexW(&attributes, FALSE, L"Global\\GpuThermalGuard.Writer.v2");
    LocalFree(descriptor);
    if (handle_) {
        const DWORD wait = WaitForSingleObject(handle_, 0);
        acquired_ = wait == WAIT_OBJECT_0 || wait == WAIT_ABANDONED;
    }
}
WriterLease::~WriterLease() {
    if (acquired_) ReleaseMutex(handle_);
    if (handle_) CloseHandle(handle_);
}

int RunSupervisor(const std::wstring& executable, int show_command, const SupervisorOptions& options) {
    Handle singleton(CreateMutexW(nullptr, FALSE, options.singleton_name));
    if (!singleton.value) return static_cast<int>(GetLastError());
    if (GetLastError() == ERROR_ALREADY_EXISTS) { ActivateExisting(); return 0; }
    if (options.journal) (void)logging::Initialize(logging::Role::Supervisor);
    struct LogLifetime { ~LogLifetime() { logging::Shutdown(); } } log_lifetime;
    logging::Info(L"SUPERVISOR started; NVML-free; tray child isolation enabled");
    SECURITY_ATTRIBUTES inherit{sizeof(inherit), nullptr, TRUE};
    Handle mapping(CreateFileMappingW(INVALID_HANDLE_VALUE, &inherit,
        PAGE_READWRITE, 0, sizeof(Shared), nullptr));
    if (!mapping.value) return static_cast<int>(GetLastError());
    View view{static_cast<Shared*>(MapViewOfFile(mapping.value,
        FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, sizeof(Shared)))};
    if (!view.value) return static_cast<int>(GetLastError());
    Handle job(CreateJobObjectW(nullptr, nullptr));
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!job.value || !SetInformationJobObject(job.value, JobObjectExtendedLimitInformation,
            &limits, sizeof(limits))) return static_cast<int>(GetLastError());
    SIZE_T bytes{};
    (void)InitializeProcThreadAttributeList(nullptr, 1, 0, &bytes);
    std::vector<unsigned char> storage(bytes);
    auto* attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
    if (!InitializeProcThreadAttributeList(attributes, 1, 0, &bytes))
        return static_cast<int>(GetLastError());
    struct AttributesLifetime {
        LPPROC_THREAD_ATTRIBUTE_LIST value;
        ~AttributesLifetime() { DeleteProcThreadAttributeList(value); }
    } attribute_lifetime{attributes};
    if (!UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
            &mapping.value, sizeof(HANDLE), nullptr, nullptr)) return static_cast<int>(GetLastError());
    RecoveryPolicy policy;
    bool recovery = false;
    std::array<char, 96> uuid{};
    std::uint64_t outage_begin = AwakeMs();
    for (;;) {
        *view.value = {};
        view.value->signature = kSignature;
        view.value->recovery = recovery;
        memcpy(view.value->uuid, uuid.data(), uuid.size());
        view.value->identity_ready = uuid[0] != 0;
        std::wstring command = std::format(L"\"{}\" --supervised {}", executable,
            reinterpret_cast<std::uintptr_t>(mapping.value));
        STARTUPINFOEXW startup{};
        startup.StartupInfo.cb = sizeof(startup);
        startup.StartupInfo.dwFlags = STARTF_USESHOWWINDOW;
        startup.StartupInfo.wShowWindow = static_cast<WORD>(recovery ? SW_SHOWNOACTIVATE : show_command);
        startup.lpAttributeList = attributes;
        PROCESS_INFORMATION process{};
        if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, TRUE,
                CREATE_SUSPENDED | EXTENDED_STARTUPINFO_PRESENT, nullptr, nullptr,
                &startup.StartupInfo, &process)) {
            const DWORD error = GetLastError();
            logging::Error(std::format(L"SUPERVISOR launch failed error={}; no child running", error));
            // File/access failures are not driver readiness failures. Do not hide them in an infinite loop.
            return static_cast<int>(error);
        }
        Handle child(process.hProcess);
        Handle thread(process.hThread);
        view.value->child_pid = process.dwProcessId;
        if (!AssignProcessToJobObject(job.value, child.value)) {
            const DWORD error = GetLastError();
            (void)TerminateProcess(child.value, error);
            (void)WaitForSingleObject(child.value, INFINITE);
            logging::Error(std::format(L"SUPERVISOR job assignment failed error={}", error));
            return static_cast<int>(error);
        }
        if (ResumeThread(thread.value) == static_cast<DWORD>(-1)) return static_cast<int>(GetLastError());
        const auto launched = AwakeMs();
        auto last_healthy = launched;
        bool first_ready = false;
        bool first_sample_logged = false;
        bool termination_requested = false;
        logging::Info(std::format(L"SUPERVISOR child pid={} launched recovery={} outage_ms={}",
            process.dwProcessId, recovery, launched - outage_begin));
        for (;;) {
            const DWORD wait = WaitForSingleObject(child.value, 500);
            if (wait == WAIT_OBJECT_0) break;
            if (wait != WAIT_TIMEOUT) return static_cast<int>(GetLastError());
            const auto now = AwakeMs();
            const auto first_sample = InterlockedCompareExchange64(&view.value->first_sample, 0, 0);
            if (first_sample && !first_sample_logged) {
                logging::Info(std::format(L"SUPERVISOR child pid={} first valid sample outage_ms={}",
                    process.dwProcessId, static_cast<std::uint64_t>(first_sample) - outage_begin));
                first_sample_logged = true;
            }
            const auto heartbeat = static_cast<std::uint64_t>(
                InterlockedCompareExchange64(&view.value->heartbeat, 0, 0));
            const bool healthy = heartbeat && now >= heartbeat && now - heartbeat < 1500 &&
                InterlockedCompareExchange(&view.value->ready, 0, 0) != 0;
            policy.Observe(now, healthy);
            if (healthy) {
                last_healthy = now;
                if (!first_ready) {
                    logging::Info(std::format(L"SUPERVISOR child pid={} ready outage_ms={}",
                        process.dwProcessId, now - outage_begin));
                    first_ready = true;
                }
            }
            const bool clean = InterlockedCompareExchange(&view.value->clean_exit, 0, 0) != 0;
            const bool requested = InterlockedCompareExchange(&view.value->restart_requested, 0, 0) != 0;
            // Awake time excludes suspend. Low UI frame rate alone does not trip this watchdog.
            const bool stalled = now - last_healthy >= options.watchdog_ms;
            if (!termination_requested && (requested || stalled)) {
                logging::Error(std::format(
                    L"SUPERVISOR terminating child pid={} reason={} clean_exit={} ready_seen={} heartbeat_age_ms={}; protection unverified",
                    process.dwProcessId, requested ? L"recovery-request" : L"90s-no-valid-protection",
                    clean, first_ready, heartbeat && now >= heartbeat ? now - heartbeat : now - launched));
                termination_requested = TerminateProcess(child.value, requested ? kRecoveryExit : kHangExit) != FALSE;
                // Never create a replacement until the process handle is signaled, even if termination stalls.
                if (!termination_requested) return static_cast<int>(GetLastError());
            }
        }
        DWORD exit_code{};
        if (!GetExitCodeProcess(child.value, &exit_code)) return static_cast<int>(GetLastError());
        const bool clean = InterlockedCompareExchange(&view.value->clean_exit, 0, 0) != 0;
        logging::Warning(std::format(L"SUPERVISOR child pid={} exited code=0x{:08X} user_exit={}",
            process.dwProcessId, exit_code, clean));
        if (clean) return static_cast<int>(exit_code);
        if (InterlockedCompareExchange(&view.value->identity_ready, 0, 0))
            memcpy(uuid.data(), view.value->uuid, uuid.size());
        uuid.back() = 0;
        if (first_ready) outage_begin = AwakeMs();
        const unsigned delay = policy.NextDelayMs();
        logging::Warning(std::format(L"SUPERVISOR waiting {} ms before fresh NVML process; protection unavailable", delay));
        Sleep(delay);
        recovery = true;
    }
}
}  // namespace gtg::supervision
