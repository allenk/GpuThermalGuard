#include "runtime.hpp"
#include "control.hpp"
#include "product_methods.hpp"
#include "color_choice.hpp"
#include "startup_failure.hpp"
#include "../injector/hook_protocol.hpp"
#include "../offsets/method_report.hpp"
#include <d3d11.h>
#include <wrl/client.h>
#include <filesystem>
#include <stdexcept>
#include <cwchar>
#include <format>
#include <string_view>
int GtgFeatureAttach(gtg::overlay::integration::Identity, const wchar_t*, const wchar_t*);

namespace {
using namespace gtg::overlay::integration;
Control* control{};
HANDLE parent{};
ULONGLONG startup_started{};

void Check(bool b, const char* why) {
    if (!b) throw std::runtime_error(why);
}

struct Handle {
    HANDLE value{};

    ~Handle() {
        if (value) CloseHandle(value);
    }
};

struct View {
    void* value{};

    ~View() {
        if (value) UnmapViewOfFile(value);
    }
};

std::filesystem::path OwnPath() {
    wchar_t path[32768]{};
    const auto n = GetModuleFileNameW(nullptr, path, 32768);
    Check(n && n < 32768, "executable path");
    return path;
}

// The one writer for the startup log: a timestamp, the two identities, and the
// fields the caller is reporting. Diagnostic I/O must never affect attachment
// or Win32 error reporting, so the caller's error is restored and every failure
// here, including formatting, is swallowed.
template <typename... Args>
void AppendStartupLine(std::format_string<Args...> fields, Args&&... args) noexcept {
    const auto saved_error = GetLastError();
    try {
        const auto path = OwnPath().parent_path() / L"GpuThermalGuard-overlay-startup.log";
        Handle file;
        const auto opened = CreateFileW(path.c_str(), FILE_APPEND_DATA,
                                        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                        nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (opened != INVALID_HANDLE_VALUE) {
            file.value = opened;
            SYSTEMTIME time{};
            GetLocalTime(&time);
            const auto line = std::format(
                "{:04}-{:02}-{:02}T{:02}:{:02}:{:02}.{:03} helper={} target={} {}\r\n", time.wYear,
                time.wMonth, time.wDay, time.wHour, time.wMinute, time.wSecond, time.wMilliseconds,
                GetCurrentProcessId(), control ? control->target.pid : 0,
                std::format(fields, std::forward<Args>(args)...));
            DWORD written{};
            (void)WriteFile(file.value, line.data(), static_cast<DWORD>(line.size()), &written,
                            nullptr);
        }
    } catch (...) {
        // A diagnostic line is never worth a failed attach.
    }
    SetLastError(saved_error);
}

// A refusal has to say what it refused. Every distinct refusal used to arrive
// as one generic failure, so the reason travels as its own quoted field. The
// Win32 number stays the field it always was and no longer stands in for the
// reason: it is whatever was last set when the refusal surfaced, which is not
// necessarily the call that failed.
void PublishStartupFailure(DWORD error, std::string_view reason) noexcept {
    AppendStartupLine("stage=attach-failed elapsed_ms={} duration_ms=0 error={} reason=\"{}\"",
                      GetTickCount64() - startup_started, error, reason);
    if (control && ValidControl(*control)) {
        InterlockedExchange(&control->error, static_cast<LONG>(error));
        InterlockedExchange(&control->state, static_cast<LONG>(HelperState::Failed));
    }
}
}  // namespace

void GtgFeatureStartupStage(const char* stage, ULONGLONG duration) noexcept {
    AppendStartupLine("stage={} elapsed_ms={} duration_ms={}", stage,
                      GetTickCount64() - startup_started, duration);
}

bool GtgFeatureContinue() noexcept {
    return control && parent && WaitForSingleObject(parent, 0) == WAIT_TIMEOUT &&
           !InterlockedCompareExchange(&control->stop, 0, 0);
}

gtg::overlay::integration::ChoiceRead GtgFeatureColorChoice(
    DWORD pid, std::uint64_t created, gtg::overlay::integration::ColorPolicy& policy) noexcept {
    if (!control || !ValidControl(*control)) return ChoiceRead::Invalid;
    return ReadColorChoice(pid, created, policy, control->parent_pid, control->parent_created);
}

int GtgFeatureSession(gtg::research::HookSection& section, HANDLE target) noexcept {
    if (!GtgFeatureContinue()) return 1;
    InterlockedExchange(&control->state, static_cast<LONG>(HelperState::Ready));
    GtgFeatureStartupStage("helper-ready", 0);
    bool first_draw{};
    while (GtgFeatureContinue() && WaitForSingleObject(target, 0) == WAIT_TIMEOUT) {
        if (section.failures || section.control_errors) {
            InterlockedExchange(&section.header.release, 1);
            InterlockedExchange(&control->state, static_cast<LONG>(HelperState::Failed));
            return 1;
        }
        InterlockedExchange(&section.header.release,
                            InterlockedCompareExchange(&control->enabled, 0, 0) ? 0 : 1);
        if (!first_draw && InterlockedCompareExchange(&section.control_samples[0], 0, 0) > 0) {
            first_draw = true;
            GtgFeatureStartupStage("first-draw-submitted", 0);
        }
        if (WaitForSingleObject(parent, 33) != WAIT_TIMEOUT) break;
    }
    InterlockedExchange(&section.header.release, 1);
    InterlockedExchange(&control->state, static_cast<LONG>(HelperState::Closed));
    return 0;
}

int gtg::overlay::integration::HelperMain(const wchar_t* argument) noexcept {
    startup_started = GetTickCount64();
    Handle mapping;
    View mapped;
    Handle owner;
    wchar_t temp_file[MAX_PATH]{};

    struct Temp {
        wchar_t* path;

        ~Temp() {
            if (*path) DeleteFileW(path);
        }
    } temp{temp_file};

    int result = 1;
    try {
        wchar_t* end{};
        const auto raw = std::wcstoull(argument, &end, 10);
        Check(raw && end && !*end, "inherited control handle");
        mapping.value = reinterpret_cast<HANDLE>(raw);
        mapped.value = MapViewOfFile(mapping.value, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Control));
        control = static_cast<Control*>(mapped.value);
        Check(control && ValidControl(*control), "control identity");
        const Control initial = *control;
        owner.value =
            OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, initial.parent_pid);
        parent = owner.value;
        Check(parent && CreationTime(parent) == initial.parent_created && GtgFeatureContinue(),
              "parent identity");
        wchar_t parent_path[32768]{};
        DWORD length = 32768;
        Check(QueryFullProcessImageNameW(parent, 0, parent_path, &length) &&
                  _wcsicmp(parent_path, OwnPath().c_str()) == 0,
              "same executable parent");
        wchar_t directory[MAX_PATH]{};
        const auto directory_length = GetTempPathW(MAX_PATH, directory);
        Check(directory_length && directory_length < MAX_PATH &&
                  GetTempFileNameW(directory, L"gtg", 0, temp_file),
              "temporary report");
        ProductMethods methods;
        const auto discovery_started = GetTickCount64();
        // The target's own identity: a method address is only evidence about
        // the runtime that target has loaded.
        Check(methods.Discover(temp_file, initial.target), "product graphics discovery");
        GtgFeatureStartupStage("discovery-complete", GetTickCount64() - discovery_started);
        Check(GtgFeatureContinue(), "canceled discovery");
        const auto dll = OwnPath().parent_path() / L"gtg_overlay.dll";
        result = GtgFeatureAttach(initial.target, dll.c_str(), temp_file);
    } catch (const std::exception& failure) {
        // Capture the Win32 error before anything else runs.
        const auto saved_error = GetLastError();
        const auto reason = StartupFailureReason(failure.what());
        PublishStartupFailure(saved_error, reason.data());
    } catch (...) {
        const auto saved_error = GetLastError();
        PublishStartupFailure(saved_error, "unknown attach failure");
    }
    control = nullptr;
    parent = nullptr;
    return result;
}
