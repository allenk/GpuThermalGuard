// Only the child created here is injectable. No arbitrary PID/game admission.
#include "product_methods.hpp"
#include "control.hpp"
#include "color_choice.hpp"
#include "../injector/hook_protocol.hpp"
#include "../injector/install_diagnostics.hpp"
#include "../injector/renderer_fixture.hpp"
#include <cstdio>
#include <algorithm>
#include <string>
using namespace gtg::overlay::integration;

namespace {
gtg::research::FixturePublisher* fixture{};
bool inspect_only{};
bool work_resize{};
bool shared_worker{};
bool d3d11{};
bool sdr10{};
bool unknown_color{};
bool assume_sdr{};
bool assume_resize{}, assume_observed{}, assume_pq{}, pre_pq{};
bool preselection_pq{};
bool solid_rect{}, solid_resize{};
bool solid_strict{};

void Check(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

struct Handle {
    HANDLE value{};

    ~Handle() {
        if (value) CloseHandle(value);
    }
};
}

ULONGLONG fixture_started{};

void GtgFeatureStartupStage(const char* stage, ULONGLONG duration) noexcept {
    std::fprintf(stdout, "startup stage=%s duration_ms=%llu\n", stage, duration);
}

bool GtgFeatureContinue() noexcept {
    return true;
}

ChoiceRead GtgFeatureColorChoice(DWORD pid, std::uint64_t created, ColorPolicy& policy) noexcept {
    return ReadColorChoice(pid, created, policy);
}

int GtgFeatureSession(gtg::research::HookSection& section, HANDLE child) noexcept {
    using namespace gtg::research;
    try {
        InstallDiagnosticMapping diagnostic;
        InstallReport report{};
        const auto pid = GetProcessId(child);
        const auto created = CreationTime(child);
        Check(diagnostic.OpenReader(pid, created) &&
                  ReadInstallReport(*diagnostic.Data(), pid, created, report),
              "owned product terminal diagnostic read-only mapping");
        Check(report.outcome == static_cast<DWORD>(AttachResult::kActive) &&
                  report.failed_site == MAXDWORD && report.installed == report.site_count &&
                  report.site_count == (section.signal_address ? 7U : 1U),
              "owned product complete installation ledger");
        for (DWORD i = 0; i < report.site_count; ++i)
            Check(report.sites[i].state == 3 && report.sites[i].transaction_entered &&
                      report.sites[i].transaction_stage == 10 &&
                      report.sites[i].primary.operation == InstallOperation::None &&
                      report.sites[i].cleanup.operation == InstallOperation::None,
                  "owned native successful site evidence");
        Check(section.product_backend == (d3d11 ? 1 : 2) && section.product_ready == 1 &&
                  (d3d11 || section.queue_identity) && !section.queue_ambiguous,
              "actual native backend/queue qualification before activation");
        Check(section.bitmap_uploads == 0 && section.control_samples[0] == 0, "no staged drawing");
        if (inspect_only) {
            Check(!section.failures && !section.control_errors,
                  "inspection preserves original-call health");
            std::puts("PASS staged interface inspection only; no overlay Execute or activation");
            return 0;
        }
        InterlockedExchange(&section.header.release, 0);
        const auto until = (std::max)(GetTickCount64() + 5500, fixture_started + 16500);
        bool republished{};
        bool saw_assumption{}, saw_draw{};
        while (GetTickCount64() < until && WaitForSingleObject(child, 0) == WAIT_TIMEOUT) {
            if (work_resize && !republished &&
                fixture->RequestReceived(GetProcessId(child), 320, 180)) {
                fixture->PublishControlFrame(2, 16, 320, 180);
                republished = true;
            }
            saw_assumption |= section.product_color_provenance ==
                              static_cast<LONG>(ColorProvenance::ResearchAssumedSdr);
            saw_draw |= section.control_samples[0] > 0;
            fixture->Beat();
            fixture->TickMotion(false, republished ? 320 : 640, republished ? 180 : 360);
            Sleep(50);
        }
        Check(!work_resize || republished, "resized reverse request reaches publisher");
        std::printf(
            "product ready=%ld backend=%ld draws=%ld uploads=%lld copies=%lld placement=%ld errors=%ld original_failures=%ld\n",
            section.product_ready, section.product_backend, section.control_samples[0],
            section.bitmap_uploads, section.bitmap_copies, section.motion_sampled_placement,
            section.control_errors, section.failures);
        const LONG64 expected_uploads = work_resize ? 2 : 1;
        if (unknown_color && !assume_sdr) {
            const bool metadata = fixture->RequestReceived(GetProcessId(child), 640, 360);
            Check(solid_rect ? !metadata : metadata,
                  "unknown SDR10 metadata follows bitmap versus metadata-free rectangle contract");
            Check(section.product_ready == -203, "unknown SDR10 reports recoverable color wait");
            Check(section.control_samples[0] == 0 && section.bitmap_uploads == 0 &&
                      !section.control_errors && !section.failures && !section.queue_ambiguous,
                  "unknown 10-bit color space cannot draw or upload");
            InterlockedExchange(&section.header.release, 1);
            std::puts("PASS unknown SDR10 color evidence preserves no-draw and original health");
            return 0;
        }
        if (solid_rect) {
            Check(section.product_ready == 1 && section.control_samples[0] > 30 &&
                      section.bitmap_uploads == 0 && section.bitmap_copies == 0 &&
                      section.motion_sampled_placement == 0 && !section.control_errors &&
                      !section.failures && !section.queue_ambiguous &&
                      section.product_color_provenance ==
                          static_cast<LONG>(ColorProvenance::ResearchAssumedSdr),
                  "solid rectangle submits without disabled IPC bitmap/placement");
            Check(!fixture->RequestReceived(GetProcessId(child), 640, 360) &&
                      !fixture->RequestReceived(GetProcessId(child), 320, 180),
                  "rectangle never requests a publisher image");
            InterlockedExchange(&section.header.release, 1);
            Sleep(200);
            const auto stopped = section.control_samples[0];
            Sleep(200);
            Check(section.control_samples[0] == stopped, "rectangle hide stops submissions");
            std::puts("PASS native rectangle/cache-independent draw, original health and hide");
            return 0;
        }
        if (assume_pq) {
            Check(saw_assumption && saw_draw && section.product_ready == -202 &&
                      section.control_errors &&
                      (section.product_color_provenance & kColorFatalBit) && !section.failures,
                  "observed PQ transition permanently stops even explicit SDR drawing");
            InterlockedExchange(&section.header.release, 1);
            const auto stopped = section.control_samples[0];
            Sleep(300);
            Check(stopped == section.control_samples[0],
                  "fatal transition cannot resume assumed drawing");
            std::puts(
                "PASS observed non-SDR disables explicit assumption; original calls remain healthy");
            return 0;
        }
        if (assume_sdr)
            Check(saw_assumption &&
                      section.product_color_provenance ==
                          static_cast<LONG>(assume_observed ? ColorProvenance::ObservedSdr
                                                            : ColorProvenance::ResearchAssumedSdr),
                  "native explicit SDR remains assumed, not fabricated observation");
        Check(section.product_ready == 1, "qualified SDR drawing clears pending reason");
        Check(section.control_samples[0] > 30 && section.bitmap_uploads == expected_uploads &&
                  section.bitmap_copies == expected_uploads &&
                  section.motion_sampled_placement >= 30 && !section.control_errors &&
                  !section.failures && !section.queue_ambiguous,
              "product D12 draw/cache/movement/original-call health");
        InterlockedExchange(&section.header.release, 1);
        Sleep(200);
        const auto stopped = section.control_samples[0];
        Sleep(200);
        Check(section.control_samples[0] == stopped, "hide stops submissions without DLL unload");
        std::printf(
            "PASS product native backend=%ld hooks: presents=%lld draws=%ld uploads=%lld placement=%ld\n",
            section.product_backend, section.hook_calls, stopped, section.bitmap_uploads,
            section.motion_sampled_placement);
        return 0;
    } catch (const std::exception& error) {
        InterlockedExchange(&section.header.release, 1);
        std::fprintf(stderr, "FAIL %s\n", error.what());
        return 1;
    }
}

int GtgFeatureAttach(Identity, const wchar_t*, const wchar_t*);

int wmain(int argc, wchar_t** argv) {
    const std::wstring mode = argc == 4 ? argv[3] : L"";
    if (argc != 3 &&
        !(argc == 4 &&
          (mode == L"--inspect-only" || mode == L"--work-resize" || mode == L"--shared-worker" ||
           mode == L"--d3d11" || mode == L"--sdr10" || mode == L"--sdr10-unknown" ||
           mode == L"--assume-sdr" || mode == L"--assume-resize" || mode == L"--assume-observed" ||
           mode == L"--assume-pq" || mode == L"--pre-pq-assume" || mode == L"--preselection-pq" ||
           mode == L"--solid-rect" || mode == L"--solid-rect-resize" ||
           mode == L"--solid-rect-unknown"))) {
        std::fputs(
            "usage: owned-product-hook-test generator.exe gtg_overlay.dll [--inspect-only|--work-resize|--shared-worker|--d3d11|--sdr10|--sdr10-unknown]\n",
            stderr);
        return 2;
    }
    inspect_only = mode == L"--inspect-only";
    assume_resize = mode == L"--assume-resize";
    assume_observed = mode == L"--assume-observed";
    assume_pq = mode == L"--assume-pq";
    pre_pq = mode == L"--pre-pq-assume";
    preselection_pq = mode == L"--preselection-pq";
    solid_resize = mode == L"--solid-rect-resize";
    solid_strict = mode == L"--solid-rect-unknown";
    solid_rect = mode == L"--solid-rect" || solid_resize || solid_strict;
    work_resize = mode == L"--work-resize" || assume_resize;
    shared_worker = mode == L"--shared-worker";
    d3d11 = mode == L"--d3d11";
    assume_sdr = mode == L"--assume-sdr" || assume_resize || assume_observed || assume_pq ||
                 pre_pq || preselection_pq || (solid_rect && !solid_strict);
    sdr10 = mode == L"--sdr10" || mode == L"--sdr10-unknown" || assume_sdr || solid_strict;
    unknown_color = mode == L"--sdr10-unknown" || assume_sdr || solid_strict;
    Handle job, process, thread, output, input;
    wchar_t report[MAX_PATH]{}, temporary[MAX_PATH]{};
    try {
        Check(GetTempPathW(MAX_PATH, temporary) && GetTempFileNameW(temporary, L"gtg", 0, report),
              "temporary report");
        ProductMethods methods;
        Check(methods.Discover(report), "helper product discovery");
        job.value = CreateJobObjectW(nullptr, nullptr);
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        Check(job.value && SetInformationJobObject(job.value, JobObjectExtendedLimitInformation,
                                                   &limits, sizeof(limits)),
              "owned child job");
        std::wstring command = L"\"" + std::wstring(argv[1]) +
                               L"\" --hardware --seconds 25 --start-delay-ms 1000 --size 640x360";
        if (!d3d11) command += L" --debug-layer --load-d3d11-module";
        if (sdr10) command += L" --sdr10";
        if (sdr10 && !unknown_color) command += L" --color-at 4";
        if (assume_observed || assume_pq) command += L" --color-at 14";
        if (assume_pq) command += L" --color-pq";
        if (pre_pq) command += L" --initial-pq";
        if (preselection_pq) command += L" --preselection-pq";
        if (work_resize) command += L" --decoy after --resize-at 14 --signal-worker decoy";
        if (solid_resize) command += L" --decoy after --resize-at 14 --signal-worker decoy";
        if (shared_worker) command += L" --decoy after --signal-worker shared";
        const auto child_log =
            std::filesystem::path(argv[2]).parent_path() / L"dx12-product-child.log";
        SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
        output.value =
            CreateFileW(child_log.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                        &security, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        input.value = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                  &security, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        Check(output.value != INVALID_HANDLE_VALUE && input.value != INVALID_HANDLE_VALUE,
              "owned child diagnostics");
        STARTUPINFOEXW startup{};
        startup.StartupInfo.cb = sizeof(startup);
        startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
        startup.StartupInfo.hStdOutput = startup.StartupInfo.hStdError = output.value;
        startup.StartupInfo.hStdInput = input.value;
        SIZE_T attribute_bytes{};
        InitializeProcThreadAttributeList(nullptr, 1, 0, &attribute_bytes);
        std::vector<unsigned char> attribute_storage(attribute_bytes);
        startup.lpAttributeList =
            reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attribute_storage.data());
        Check(InitializeProcThreadAttributeList(startup.lpAttributeList, 1, 0, &attribute_bytes),
              "child attribute list");

        struct Attributes {
            LPPROC_THREAD_ATTRIBUTE_LIST list;

            ~Attributes() { DeleteProcThreadAttributeList(list); }
        } attributes{startup.lpAttributeList};

        HANDLE inherited[] = {output.value, input.value};
        Check(
            UpdateProcThreadAttribute(startup.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                      inherited, sizeof(inherited), nullptr, nullptr),
            "only diagnostic handles inherited");
        PROCESS_INFORMATION child{};
        Check(CreateProcessW(argv[1], command.data(), nullptr, nullptr, TRUE,
                             CREATE_SUSPENDED | CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT,
                             nullptr, nullptr, &startup.StartupInfo, &child),
              "owned generator child");
        process.value = child.hProcess;
        thread.value = child.hThread;
        fixture_started = GetTickCount64();
        Check(AssignProcessToJobObject(job.value, process.value), "only owned child assigned");
        gtg::research::FixturePublisher publisher;
        publisher.Start(child.dwProcessId, !solid_rect, true);
        fixture = &publisher;
        const Identity target{child.dwProcessId, CreationTime(process.value)};
        ColorChoiceMapping choice;
        Check(!assume_sdr || choice.Create(target.pid, target.created),
              "owned explicit color choice before attach");
        Check(target.created && ResumeThread(thread.value) != MAXDWORD, "owned child start");
        Sleep(1500);
        int result{};
        bool refused{};
        try {
            result = GtgFeatureAttach(target, argv[2], report);
        } catch (const std::exception&) {
            if (!preselection_pq) throw;
            refused = true;
        }
        if (preselection_pq) {
            Check(refused, "observed preselection PQ must refuse before any drawing");
            wchar_t name[64]{};
            gtg::research::SectionName(target.pid, name);
            Handle mapping;
            mapping.value = OpenFileMappingW(FILE_MAP_READ, FALSE, name);
            const auto* peer =
                mapping.value
                    ? static_cast<const gtg::research::HookSection*>(MapViewOfFile(
                          mapping.value, FILE_MAP_READ, 0, 0, sizeof(gtg::research::HookSection)))
                    : nullptr;
            Check(peer != nullptr, "resident preselection evidence");
            const bool healthy =
                peer->header.state == static_cast<LONG>(gtg::research::HookState::kInstalled) &&
                peer->product_ready == -202 && peer->control_errors && peer->failures == 0 &&
                peer->control_samples[0] == 0 && peer->bitmap_uploads == 0 &&
                (peer->product_color_provenance & kColorFatalBit);
            UnmapViewOfFile(peer);
            Check(healthy, "preselection observed PQ is sticky no-draw with healthy originals");
            std::puts("PASS post-install preselection PQ refused without any drawing");
        }
        if (result == 0) {
            // Early refusal no longer spends ten seconds in the research observer.
            // Preserve the 25s fixture's full lifetime and bounded normal-exit oracle.
            const auto now = GetTickCount64();
            const auto exit_deadline = fixture_started + 31000;
            const auto remaining = now < exit_deadline ? exit_deadline - now : 0;
            Check(
                WaitForSingleObject(process.value, static_cast<DWORD>(remaining)) == WAIT_OBJECT_0,
                "owned generator exits normally");
            DWORD exit{};
            Check(GetExitCodeProcess(process.value, &exit) && exit == 0,
                  "native debug oracle exits green");
        }
        DeleteFileW(report);
        return result;
    } catch (const std::exception& error) {
        if (*report) DeleteFileW(report);
        std::fprintf(stderr, "FAIL %s Win32=%lu\n", error.what(), GetLastError());
        return 1;
    }
}
