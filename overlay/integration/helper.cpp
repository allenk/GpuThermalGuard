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

// Which executable the target is. This log recorded the helper and target pids
// and never the image, so a refusal could not be attributed to a title once
// that process had gone -- which is exactly when a refusal gets read. The pid
// is checked against the creation time first, because a pid on its own names
// whatever holds it now rather than what the attach was aimed at.
void PublishTargetImage(Identity target) noexcept {
    try {
        Handle process;
        process.value = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, target.pid);
        wchar_t image[32768]{};
        DWORD length = 32768;
        if (!process.value || CreationTime(process.value) != target.created ||
            !QueryFullProcessImageNameW(process.value, 0, image, &length)) {
            AppendStartupLine("target-image unresolved");
            return;
        }
        AppendStartupLine("target-image path=\"{}\"",
                          RuntimeNarrowPath(std::filesystem::path(image)));
    } catch (...) {
        // A diagnostic line is never worth a failed attach.
    }
}

// Which Direct3D 12 runtime the target is on, and therefore which one this
// discovery loaded beside it. For a title shipping its own Agility SDK Core
// these are the two facts that separate a method address meaning something in
// the target from one meaning nothing, and neither was ever written down.
void PublishTargetRuntime(const ProductMethods& methods) noexcept {
    try {
        if (!methods.core_runtime) {
            AppendStartupLine("target-core none");
            return;
        }
        AppendStartupLine("target-core path=\"{}\" sdk={} system={}",
                          RuntimeNarrowPath(methods.core_runtime->Path()),
                          methods.core_runtime->SdkVersion(),
                          methods.core_runtime->IsSystem() ? 1 : 0);
    } catch (...) {
    }
}

// A refusal recorded while the target is on its way out is the ordinary end of
// a session rather than a defect, and the two were indistinguishable. The loop
// finds the counter set about a second before the process actually goes -- the
// product hook's per-frame path meets a swapchain that is being torn down -- so
// a single check at that instant reports the target as still running, the line
// read like a defect, and the user was told to restart a game they had just
// closed. Waiting that second out is what separates them. The wait is affordable
// only because the caller publishes the release and the failed state first, so
// nothing the overlay or the tray depends on is held up by a diagnostic.
inline constexpr DWORD kExitingTargetWaitMs = 2000;

const char* SectionFailureReason(HANDLE target) noexcept {
    return WaitForSingleObject(target, kExitingTargetWaitMs) == WAIT_TIMEOUT
               ? "section reported a failure"
               : "section reported a failure as the target exited";
}

// Why a session ended, with the counters that separate the reasons. Reading
// these after the fact is the only way to tell an install that started refusing
// from a game that exited, and both arrive at the tray as the same one-line
// warning.
//
// `ready` carries the reason itself. The product hook closes admissions through
// one function and records a code there, and a fatal code already published
// wins over any later one, so this is the first thing that went wrong rather
// than the last. Without it the line says that something refused and leaves
// which of the refusals to a second run of the title.
void PublishSessionEnd(const char* why, gtg::research::HookSection& section) noexcept {
    AppendStartupLine(
        "stage=session-ended elapsed_ms={} why=\"{}\" ready={} color={} backend={} "
        "txn_stage={} failures={} errors={} readback={} present_calls={} signal_calls={} "
        "nested_signal={} outside_signal={} wrong_thread={} queue_ambiguous={} state={}",
        GetTickCount64() - startup_started, why, static_cast<long>(section.product_ready),
        static_cast<long>(section.product_color_provenance),
        static_cast<long>(section.product_backend),
        static_cast<long>(section.transaction_stage), static_cast<long>(section.failures),
        static_cast<long>(section.control_errors), static_cast<long>(section.readback_errors),
        static_cast<long long>(section.hook_calls),
        static_cast<long long>(section.signal_calls),
        static_cast<long long>(section.nested_signal_calls),
        static_cast<long>(section.outside_signal_calls),
        static_cast<long>(section.wrong_thread), static_cast<long>(section.queue_ambiguous),
        static_cast<long>(section.header.state));
}
}  // namespace

void GtgFeatureStartupStage(const char* stage, ULONGLONG duration) noexcept {
    AppendStartupLine("stage={} elapsed_ms={} duration_ms={}", stage,
                      GetTickCount64() - startup_started, duration);
}

// The attach-side diagnostics, which used to go to std::cout. The helper is
// launched with CREATE_NO_WINDOW and no standard handles, so in a deployed
// build that stream has no reader at all: every entry-shape reading and every
// refusal breakdown was formatted and then dropped, and both were only ever
// seen when the helper was run from a console by hand. They travel through this
// log's one writer instead.
void GtgFeatureStartupDetail(const char* text) noexcept {
    AppendStartupLine("elapsed_ms={} {}", GetTickCount64() - startup_started, text);
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
    if (!GtgFeatureContinue()) {
        PublishSessionEnd("refused before the session began", section);
        return 1;
    }
    InterlockedExchange(&control->state, static_cast<LONG>(HelperState::Ready));
    GtgFeatureStartupStage("helper-ready", 0);
    bool first_draw{};
    while (GtgFeatureContinue() && WaitForSingleObject(target, 0) == WAIT_TIMEOUT) {
        if (section.failures || section.control_errors) {
            // Order matters: a failure raised while a target tears itself down
            // is the worst moment to delay the signal that stops drawing, and
            // the reason below spends up to two seconds establishing which
            // kind of ending this is.
            InterlockedExchange(&section.header.release, 1);
            InterlockedExchange(&control->state, static_cast<LONG>(HelperState::Failed));
            PublishSessionEnd(SectionFailureReason(target), section);
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
    // Re-read rather than inferred from which statement broke the loop: the
    // target going away and the tray going away are the two ordinary ends and
    // they are told apart by asking, not by where control arrived from.
    const char* why = "stop requested or the tray went away";
    if (WaitForSingleObject(target, 0) != WAIT_TIMEOUT) why = "target exited";
    PublishSessionEnd(why, section);
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
        // Named before discovery runs, so a discovery that refuses is still
        // attributable to a title.
        PublishTargetImage(initial.target);
        ProductMethods methods;
        const auto discovery_started = GetTickCount64();
        // The target's own identity: a method address is only evidence about
        // the runtime that target has loaded.
        Check(methods.Discover(temp_file, initial.target), "product graphics discovery");
        PublishTargetRuntime(methods);
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
