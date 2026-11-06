#include "update/handoff.hpp"

#include <cwchar>
#include <format>
#include <vector>

#include "localization/localization.hpp"
#include "update/files.hpp"
#include "update/json.hpp"
#include "update/update_resource.h"

namespace gtg::update {
namespace {

class Handle {
public:
    explicit Handle(const HANDLE value = nullptr) noexcept : value_(value) {}

    ~Handle() {
        if (value_ != nullptr && value_ != INVALID_HANDLE_VALUE) CloseHandle(value_);
    }

    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;

    [[nodiscard]] HANDLE Get() const noexcept { return value_; }

private:
    HANDLE value_;
};

// Written beside the target and renamed over it, so the script never reads a
// half-written plan.
bool WriteWhole(const std::filesystem::path& path, const std::string_view bytes, DWORD& error) {
    const std::wstring temporary = path.wstring() + L".tmp";
    {
        Handle file(CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                FILE_ATTRIBUTE_NORMAL, nullptr));
        if (file.Get() == INVALID_HANDLE_VALUE) {
            error = GetLastError();
            return false;
        }
        DWORD written = 0;
        if (!WriteFile(file.Get(), bytes.data(), static_cast<DWORD>(bytes.size()), &written,
                       nullptr) ||
            written != bytes.size() || !FlushFileBuffers(file.Get())) {
            error = GetLastError();
            return false;
        }
    }
    if (!MoveFileExW(temporary.c_str(), path.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        error = GetLastError();
        return false;
    }
    return true;
}

// This process's environment without PSModulePath. A PowerShell 7 shell leaves
// its own module path in the environment of whatever it starts, and Windows
// PowerShell then fails to load the commands its own modules define.
std::vector<wchar_t> ScriptEnvironment() {
    std::vector<wchar_t> block;
    wchar_t* strings = GetEnvironmentStringsW();
    if (strings == nullptr) return block;
    for (const wchar_t* entry = strings; *entry != L'\0'; entry += std::wcslen(entry) + 1) {
        if (_wcsnicmp(entry, L"PSModulePath=", 13) == 0) continue;
        block.insert(block.end(), entry, entry + std::wcslen(entry) + 1);
    }
    FreeEnvironmentStringsW(strings);
    block.push_back(L'\0');
    if (block.size() == 1) block.push_back(L'\0');
    return block;
}

// loaded.json names the script's process once it has read the plan.
bool Loaded(const std::filesystem::path& file, const DWORD pid) {
    Handle handle(CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (handle.Get() == INVALID_HANDLE_VALUE) return false;
    char text[256]{};
    DWORD read = 0;
    if (!ReadFile(handle.Get(), text, sizeof(text) - 1, &read, nullptr)) return false;
    const json::ParseResult parsed = json::Parse(std::string_view(text, read));
    if (!parsed.value) return false;
    const json::Value* value = parsed.value->Find("pid");
    return value != nullptr && value->integer == static_cast<std::int64_t>(pid);
}

HandOffResult Fail(const std::filesystem::path& plan, std::wstring problem) {
    // A script that starts after this finds no plan and stops.
    (void)DeleteFileW(plan.c_str());
    HandOffResult result;
    result.problem = std::move(problem);
    return result;
}

}  // namespace

std::optional<std::string_view> EmbeddedScript(const HMODULE module) noexcept {
    const HRSRC found = FindResourceW(module, MAKEINTRESOURCEW(IDR_APPLY_UPDATE), RT_RCDATA);
    if (found == nullptr) return std::nullopt;
    const HGLOBAL loaded = LoadResource(module, found);
    const DWORD size = SizeofResource(module, found);
    const void* data = loaded != nullptr ? LockResource(loaded) : nullptr;
    if (data == nullptr || size == 0) return std::nullopt;
    return std::string_view(static_cast<const char*>(data), size);
}

HandOffResult HandOff(const std::filesystem::path& work, const Plan& plan,
                      const std::string_view script, const DWORD parent, const DWORD loaded_ms) {
    const std::filesystem::path plan_file = work / kPlanFileName;
    const std::filesystem::path script_file = work / kScriptFileName;
    const std::filesystem::path loaded_file = work / kLoadedFileName;
    for (const wchar_t* stale : {kLoadedFileName, kReadyFileName, kOutcomeFileName}) {
        (void)DeleteFileW((work / stale).c_str());
    }
    DWORD error = 0;
    if (!WriteWhole(script_file, script, error) ||
        !WriteWhole(plan_file, FormatPlan(plan), error)) {
        return Fail(plan_file,
                    localization::Format(L"無法寫入更新腳本（Win32 {}）",
                                         L"The update script cannot be written (Win32 {})", error));
    }

    Handle supervisor(OpenProcess(
        PROCESS_CREATE_PROCESS | PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, parent));
    if (supervisor.Get() == nullptr) {
        return Fail(plan_file, localization::Format(L"無法開啟 supervisor（Win32 {}）",
                                                    L"The supervisor cannot be opened (Win32 {})",
                                                    GetLastError()));
    }
    SIZE_T bytes = 0;
    (void)InitializeProcThreadAttributeList(nullptr, 1, 0, &bytes);
    std::vector<unsigned char> storage(bytes);
    auto* attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
    if (!InitializeProcThreadAttributeList(attributes, 1, 0, &bytes)) {
        return Fail(plan_file,
                    localization::Format(L"無法啟動更新腳本（Win32 {}）",
                                         L"The update script cannot be started (Win32 {})",
                                         GetLastError()));
    }
    HANDLE parent_handle = supervisor.Get();
    const bool attributed =
        UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_PARENT_PROCESS,
                                  &parent_handle, sizeof(parent_handle), nullptr, nullptr) != FALSE;
    wchar_t system[MAX_PATH]{};
    GetSystemDirectoryW(system, MAX_PATH);
    const std::wstring powershell =
        std::wstring(system) + L"\\WindowsPowerShell\\v1.0\\powershell.exe";
    std::wstring command = std::format(
        L"\"{}\" -NoProfile -NonInteractive -ExecutionPolicy Bypass -File \"{}\" -Plan \"{}\"",
        powershell, script_file.wstring(), plan_file.wstring());
    std::vector<wchar_t> environment = ScriptEnvironment();
    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.lpAttributeList = attributes;
    PROCESS_INFORMATION process{};
    const BOOL started =
        attributed &&
        CreateProcessW(powershell.c_str(), command.data(), nullptr, nullptr, FALSE,
                       EXTENDED_STARTUPINFO_PRESENT | CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT,
                       environment.empty() ? nullptr : environment.data(), work.c_str(),
                       &startup.StartupInfo, &process);
    const DWORD start_error = GetLastError();
    DeleteProcThreadAttributeList(attributes);
    if (!started) {
        return Fail(plan_file, localization::Format(
                                   L"無法啟動更新腳本（Win32 {}）",
                                   L"The update script cannot be started (Win32 {})", start_error));
    }
    Handle script_process(process.hProcess);
    Handle script_thread(process.hThread);

    HandOffResult result;
    result.script_pid = process.dwProcessId;
    BOOL in_job = FALSE;
    result.in_job = IsProcessInJob(script_process.Get(), nullptr, &in_job) && in_job;
    const ULONGLONG deadline = GetTickCount64() + loaded_ms;
    for (;;) {
        if (Loaded(loaded_file, process.dwProcessId)) {
            result.ok = true;
            return result;
        }
        if (WaitForSingleObject(script_process.Get(), 0) == WAIT_OBJECT_0) {
            DWORD code = 0;
            GetExitCodeProcess(script_process.Get(), &code);
            HandOffResult failed = Fail(
                plan_file, localization::Format(L"更新腳本提早結束（代碼 {}）",
                                                L"The update script ended early (code {})", code));
            failed.script_pid = process.dwProcessId;
            return failed;
        }
        if (GetTickCount64() >= deadline) break;
        Sleep(100);
    }
    HandOffResult late =
        Fail(plan_file, localization::Format(L"更新腳本在 {} 秒內沒有回應",
                                             L"The update script did not answer within {} s",
                                             loaded_ms / 1000));
    late.script_pid = process.dwProcessId;
    return late;
}

}  // namespace gtg::update
