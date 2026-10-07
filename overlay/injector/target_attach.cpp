// Separate named game profiles; default invocation is read-only preflight.
#include "hook_protocol.hpp"
#include "queue_discovery.hpp"
#include "device_diagnostics.hpp"
#include "draw_diagnostics.hpp"
#include "install_diagnostics.hpp"
#include "named_target_policy.hpp"
#include "drawing_gate_guard.hpp"
#include <appmodel.h>
#include <tlhelp32.h>
#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>
#ifdef GTG_FEATURE_ATTACH
#include "../integration/control.hpp"
#include "../integration/target_admission.hpp"
#include "../integration/product_methods.hpp"
#include "../integration/color_choice.hpp"
gtg::overlay::integration::ChoiceRead GtgFeatureColorChoice(DWORD, std::uint64_t,
    gtg::overlay::integration::ColorPolicy&) noexcept;
#include "../integration/startup_readiness.hpp"
bool GtgFeatureContinue() noexcept;
int GtgFeatureSession(gtg::research::HookSection&, HANDLE) noexcept;
void GtgFeatureStartupStage(const char*, ULONGLONG) noexcept;
namespace { gtg::overlay::integration::Identity feature_identity; }
#endif

namespace {
using namespace gtg::research;
struct Handle {
    HANDLE value{};
    explicit Handle(HANDLE h) : value(h) {}
    ~Handle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
};
struct Module {
    HMODULE value{};
    ~Module() { if (value) FreeLibrary(value); }
};
struct View {
    HookSection* value{};
    ~View() { if (value) UnmapViewOfFile(value); }
};
void Check(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
std::vector<MODULEENTRY32W> Modules(DWORD pid) {
    Handle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, pid));
    Check(snapshot.value != INVALID_HANDLE_VALUE, "module snapshot failed");
    MODULEENTRY32W entry{}; entry.dwSize = sizeof(entry);
    Check(Module32FirstW(snapshot.value, &entry), "module enumeration failed");
    std::vector<MODULEENTRY32W> result;
    do { result.push_back(entry); } while (Module32NextW(snapshot.value, &entry));
    Check(GetLastError() == ERROR_NO_MORE_FILES, "incomplete module list");
    return result;
}
DWORD FindNamedGame() {
    Handle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
    Check(snapshot.value != INVALID_HANDLE_VALUE, "process snapshot failed");
    PROCESSENTRY32W entry{}; entry.dwSize = sizeof(entry);
    Check(Process32FirstW(snapshot.value, &entry), "process enumeration failed");
    DWORD pid{};
    do {
        if (_wcsicmp(entry.szExeFile, kNamedProfile.image.data()) == 0) {
            Check(pid == 0, "multiple named game processes; refuse ambiguity"); pid = entry.th32ProcessID;
        }
    } while (Process32NextW(snapshot.value, &entry));
    Check(GetLastError() == ERROR_NO_MORE_FILES && pid != 0, "one running named game required");
    return pid;
}
void CheckNamedProcess(HANDLE process) {
#ifdef GTG_FEATURE_ATTACH
    Check(GetProcessId(process) == feature_identity.pid &&
        gtg::overlay::integration::CreationTime(process) == feature_identity.created &&
        gtg::overlay::integration::TargetAdmitted(process), "foreground target admission changed");
#else
    wchar_t family[128]{}; UINT32 count = 128;
    wchar_t image[32768]{}; DWORD size = 32768;
    Check(GetPackageFamilyName(process, &count, family) == ERROR_SUCCESS &&
        QueryFullProcessImageNameW(process, 0, image, &size) &&
        NamedIdentity(kNamedProfile, std::filesystem::path(image).filename().native(), family),
        "named game package/image identity mismatch");
#endif
    USHORT machine{}, native{};
    Check(IsWow64Process2(process, &machine, &native) && machine == IMAGE_FILE_MACHINE_UNKNOWN &&
        native == IMAGE_FILE_MACHINE_AMD64, "native AMD64 target required");
}
void CheckMitigations(HANDLE process) {
    PROCESS_MITIGATION_BINARY_SIGNATURE_POLICY signature{};
    PROCESS_MITIGATION_DYNAMIC_CODE_POLICY dynamic{};
    const bool signature_ok = GetProcessMitigationPolicy(process, ProcessSignaturePolicy, &signature, sizeof(signature)) != FALSE;
    const bool dynamic_ok = GetProcessMitigationPolicy(process, ProcessDynamicCodePolicy, &dynamic, sizeof(dynamic)) != FALSE;
    Check(UnsignedTrialAllowed(signature_ok, signature.Flags, dynamic_ok, dynamic.Flags),
        "mitigation query failed or unsigned/patch trial prohibited; no downgrade attempted");
}
std::uintptr_t RemoteAddress(HANDLE process, const std::vector<MODULEENTRY32W>& modules, void* local) {
    HMODULE owner{};
    Check(GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(local), &owner), "local address owner");
    wchar_t path[32768]{};
    const auto length = GetModuleFileNameW(owner, path, 32768);
    Check(length && length < 32768, "local owner path");
    const auto base = reinterpret_cast<std::uintptr_t>(owner);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    const auto rva = reinterpret_cast<std::uintptr_t>(local) - base;
    for (const auto& module : modules) {
        if (_wcsicmp(module.szExePath, path) != 0) continue;
        Check(module.modBaseSize == nt->OptionalHeader.SizeOfImage && rva + 16 <= module.modBaseSize,
            "remote image size/RVA mismatch");
        IMAGE_DOS_HEADER rd{}; IMAGE_NT_HEADERS64 rn{}; SIZE_T read{};
        Check(ReadProcessMemory(process, module.modBaseAddr, &rd, sizeof(rd), &read) && read == sizeof(rd) &&
            rd.e_magic == IMAGE_DOS_SIGNATURE && rd.e_lfanew > 0 &&
            static_cast<DWORD>(rd.e_lfanew) <= module.modBaseSize - sizeof(rn), "remote DOS header");
        Check(ReadProcessMemory(process, module.modBaseAddr + rd.e_lfanew, &rn, sizeof(rn), &read) && read == sizeof(rn) &&
            rn.Signature == IMAGE_NT_SIGNATURE && rn.FileHeader.Machine == IMAGE_FILE_MACHINE_AMD64 &&
            rn.FileHeader.TimeDateStamp == nt->FileHeader.TimeDateStamp &&
            rn.OptionalHeader.SizeOfImage == nt->OptionalHeader.SizeOfImage, "remote image identity mismatch");
        const auto remote = reinterpret_cast<std::uintptr_t>(module.modBaseAddr) + rva;
        MEMORY_BASIC_INFORMATION info{};
        Check(VirtualQueryEx(process, reinterpret_cast<void*>(remote), &info, sizeof(info)) == sizeof(info) &&
            info.State == MEM_COMMIT && info.Type == MEM_IMAGE && info.Protect == PAGE_EXECUTE_READ &&
            remote >= reinterpret_cast<std::uintptr_t>(info.BaseAddress) &&
            remote - reinterpret_cast<std::uintptr_t>(info.BaseAddress) + 16 <= info.RegionSize,
            "remote executable range");
        return remote;
    }
    throw std::runtime_error("matching system module absent in named game");
}
int Observe(DWORD pid, HANDLE process, std::uintptr_t present,
    const std::array<unsigned char, 16>& expected, const std::vector<MODULEENTRY32W>& modules) {
    wchar_t name[64]{}; SectionName(pid, name);
    Handle mapping(OpenFileMappingW(FILE_MAP_READ, FALSE, name));
    Check(mapping.value != nullptr, "resident counter mapping required");
    View view{static_cast<HookSection*>(MapViewOfFile(mapping.value, FILE_MAP_READ, 0, 0, sizeof(HookSection)))};
    const auto* section = view.value;
    Check(section && section->header.magic == kHookMagic && section->header.version == kVersion &&
        section->header.size == sizeof(HookSection) && section->header.target_pid == pid &&
        section->present_address == present && std::equal(expected.begin(), expected.end(), section->expected),
        "resident counter identity mismatch");
    Check(std::any_of(modules.begin(), modules.end(), [section](const auto& module) {
        return reinterpret_cast<std::uintptr_t>(module.modBaseAddr) == section->header.module_base &&
            (_wcsicmp(module.szModule, kNamedProfile.counter_dll.data()) == 0 ||
             _wcsicmp(module.szModule, kNamedProfile.association_dll.data()) == 0 ||
             _wcsicmp(module.szModule, kNamedProfile.draw_dll.data()) == 0);
    }), "resident counter module mismatch");
    // Read-only mapped volatile aligned scalar loads on the Windows x64 target.
    // Do not issue Interlocked read/modify/write operations on FILE_MAP_READ.
    const auto before = section->hook_calls;
    wchar_t draw_name[64]{}; DrawSectionName(pid, draw_name);
    Handle draw_mapping(OpenFileMappingW(FILE_MAP_READ, FALSE, draw_name));
    View draw_view{draw_mapping.value ? reinterpret_cast<HookSection*>(MapViewOfFile(
        draw_mapping.value, FILE_MAP_READ, 0, 0, sizeof(DrawDiagnostics))) : nullptr};
    Check(!draw_mapping.value || draw_view.value,
        "resident draw diagnostics unreadable or incompatible; use its matching observer or restart for the new DLL");
    const auto* draw_data = reinterpret_cast<const DrawDiagnostics*>(draw_view.value);
    if (draw_data) Check(draw_data->magic == kDrawDiagnosticMagic && draw_data->version == kDrawDiagnosticVersion &&
        draw_data->size == sizeof(DrawDiagnostics) && draw_data->pid == pid, "draw diagnostic identity mismatch");
    for (unsigned sample = 0; sample < 20; ++sample) {
        Sleep(500);
        Check(WaitForSingleObject(process, 0) == WAIT_TIMEOUT, "named game exited while observing");
        std::cout << "observe state=" << section->header.state << " presents=" << section->hook_calls
            << " failures=" << section->failures << " nested=" << section->nested_signal_calls
            << " queue=" << section->queue_identity << " ambiguous=" << section->queue_ambiguous << '\n';
        std::cout << "drawn=" << section->control_samples[0] << " placement=" << section->motion_sampled_placement
            << " rect=" << section->motion_last_left << ',' << section->leading_small_samples
            << ' ' << section->observed_width << 'x' << section->observed_height
            << " copies=" << section->bitmap_copies << " uploads=" << section->bitmap_uploads << '\n';
        if (draw_data) PrintDrawDiagnostics(std::cout, *draw_data);
    }
    Check(section->header.state == static_cast<LONG>(HookState::kInstalled) &&
        section->hook_calls > before && section->failures == 0, "no healthy counter progress during observation");
    std::cout << "PASS named resident counter progressed; observer made no target writes\n";
    return 0;
}
int Run(bool attach, bool load_only, bool observe, bool discovery, bool association, bool drawing,
    const std::filesystem::path& dll, const std::filesystem::path& report) {
    const DWORD pid =
#ifdef GTG_FEATURE_ATTACH
        feature_identity.pid;
#else
        FindNamedGame();
#endif
    const DWORD access = PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_QUERY_INFORMATION | PROCESS_VM_READ | SYNCHRONIZE |
        (attach ? PROCESS_CREATE_THREAD | PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_QUERY_INFORMATION : 0);
    Handle process(OpenProcess(access, FALSE, pid));
    Check(process.value != nullptr, "open named game failed");
    CheckNamedProcess(process.value);
    if (!observe) CheckMitigations(process.value);
    FILETIME created{}, exited{}, kernel{}, user{};
    Check(GetProcessTimes(process.value, &created, &exited, &kernel, &user), "creation identity");
#ifdef GTG_FEATURE_ATTACH
    Check(gtg::overlay::integration::CreationTime(process.value)==feature_identity.created &&
        GtgFeatureContinue(), "feature target lifetime or parent changed");
#endif
    const auto modules = Modules(pid);
    const bool counter_resident = std::any_of(modules.begin(), modules.end(), [](const auto& module) {
        return _wcsicmp(module.szModule, kNamedProfile.counter_dll.data()) == 0 ||
            _wcsicmp(module.szModule, kNamedProfile.association_dll.data()) == 0 ||
            _wcsicmp(module.szModule, kNamedProfile.draw_dll.data()) == 0;
    });
    for (const auto& module : modules) if (!observe)
        Check((discovery || _wcsicmp(module.szModule, kNamedProfile.counter_dll.data()) != 0) &&
            (discovery || _wcsicmp(module.szModule, kNamedProfile.association_dll.data()) != 0) &&
            (discovery || _wcsicmp(module.szModule, kNamedProfile.draw_dll.data()) != 0) &&
            _wcsicmp(module.szModule, L"gtg_overlay_load_probe.dll") != 0 &&
            _wcsicmp(module.szModule, L"gtg_overlay_queue_probe.dll") != 0,
            "research DLL already resident; do not attach twice");
    // Read the first Present object emitted by our hardware offset helper.
    std::ifstream input(report);
    Check(input.good(), "current hardware method report required");
    const std::string text{std::istreambuf_iterator<char>(input), {}};
    Check(text.size() < 65536, "bounded method report");
    const auto first = text.find("{\"role\":\"Present\"");
    const auto last = text.find('}', first);
    Check(first != std::string::npos && last != std::string::npos, "Present report object");
    auto object = text.substr(first, last - first);
    const auto number = [&](const char* key) {
        const auto at = object.find(key);
        Check(at != std::string::npos, "method report field");
        const auto begin = at + std::strlen(key);
        const auto end = object.find_first_not_of("0123456789", begin);
        Check(end != begin && end != std::string::npos && (object[end] == ',' || object[end] == '}'), "numeric field syntax");
        return std::stoull(object.substr(begin, end - begin));
    };
    Module dxgi{LoadLibraryExW(L"dxgi.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32)};
    Check(dxgi.value != nullptr, "load local system DXGI");
    const auto local_base = reinterpret_cast<std::uintptr_t>(dxgi.value);
    const auto* local_dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(local_base);
    const auto* local_nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(local_base + local_dos->e_lfanew);
    const auto rva = number("\"rva\":");
    Check(number("\"image_size\":") == local_nt->OptionalHeader.SizeOfImage &&
        number("\"timestamp\":") == local_nt->FileHeader.TimeDateStamp &&
        local_nt->OptionalHeader.SizeOfImage >= 16 && rva <= local_nt->OptionalHeader.SizeOfImage - 16,
        "report/local DXGI identity mismatch");
    const auto present_local = reinterpret_cast<void*>(local_base + rva);
    const auto present = RemoteAddress(process.value, modules, present_local);
    std::array<unsigned char, 16> expected{};
    const auto hex_at = object.find("\"bytes\":\"");
    Check(hex_at != std::string::npos && object.size() >= hex_at + 9 + 33 && object[hex_at + 9 + 32] == '"',
        "method report bytes");
    for (std::size_t i = 0; i < 32; ++i)
        Check(std::isxdigit(static_cast<unsigned char>(object[hex_at + 9 + i])) != 0, "hex byte syntax");
    for (std::size_t i = 0; i < expected.size(); ++i)
        expected[i] = static_cast<unsigned char>(std::stoul(object.substr(hex_at + 9 + i * 2, 2), nullptr, 16));
    if (observe) return Observe(pid, process.value, present, expected, modules);
    if (discovery && counter_resident) Observe(pid, process.value, present, expected, modules);
    std::array<unsigned char, 16> bytes{}; SIZE_T read{};
    Check(ReadProcessMemory(process.value, reinterpret_cast<void*>(present), bytes.data(), bytes.size(), &read) &&
        read == bytes.size() && (bytes == expected || (discovery && counter_resident)) && (present & 7) == 0,
        "Present bytes/alignment mismatch");
    const auto before_bytes = bytes; // Queue-only trial must preserve the existing counter patch.
    std::wcout << kNamedProfile.image << L" pid=" << pid << L'\n';
    std::cout << "creation=" << created.dwHighDateTime << ':' << created.dwLowDateTime
        << " present=" << present << " preflight=pass\n";
    if (!attach) return 0;
    Check(dll.filename() == (drawing ? kNamedProfile.draw_dll : association ? kNamedProfile.association_dll : discovery ? L"gtg_overlay_queue_probe.dll" :
        load_only ? L"gtg_overlay_load_probe.dll" : kNamedProfile.counter_dll) &&
        std::filesystem::is_regular_file(dll), "named DLL required");
    wchar_t name[64]{}; SectionName(pid, name);
    if (discovery) QueueSectionName(pid, name);
    Handle mapping(CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(HookSection), name));
    Check(mapping.value && GetLastError() != ERROR_ALREADY_EXISTS, "fresh isolated mapping required");
    View view{static_cast<HookSection*>(MapViewOfFile(mapping.value, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(HookSection)))};
    auto* section = view.value;
    Check(section != nullptr, "map section");
    DrawingGateGuard drawing_guard(drawing ? &section->header.release : nullptr);
    *section = {};
    section->header = {discovery ? kQueueMagic : load_only ? kMagic : kHookMagic, kVersion,
        load_only && !discovery ? sizeof(LoadSection) : sizeof(HookSection), pid, 0, 0, 0};
#if defined(GTG_NAMED_TARGET_RESEARCH) || defined(GTG_FOREGROUND_OVERLAY)
    if (drawing) section->header.release = 1; // Counter/COM inspection only until admitted.
#endif
    section->present_address = present;
    std::copy(expected.begin(), expected.end(), section->expected);
#ifdef GTG_FEATURE_ATTACH
    section->product_version = kProductVersion;
    section->product_target_created = feature_identity.created;
    gtg::overlay::integration::ColorPolicy color_policy{};
    Check(GtgFeatureColorChoice(pid, feature_identity.created, color_policy) !=
        gtg::overlay::integration::ChoiceRead::Invalid, "color choice identity/version/publication invalid");
    section->product_color_policy = static_cast<DWORD>(color_policy);
    const auto method = [&](const char* role, std::uint64_t& address, unsigned char (&entry)[16], bool optional) {
        const std::string marker = std::string("{\"role\":\"") + role + "\"";
        const auto start = text.find(marker), end = text.find('}', start);
        if (start == std::string::npos && optional) return;
        Check(start != std::string::npos && end != std::string::npos, "required product method report");
        object = text.substr(start, end-start);
        HMODULE owner = dxgi.value;
        if (optional) {
            // Signal may belong to D3D12Core, not the DXGI image. Never trust
            // a report-provided arbitrary path or image outside these runtimes.
            const auto at = object.find("\"module\":\"");
            Check(at != std::string::npos, "Signal module field");
            const auto stop = object.find('"', at+10);
            Check(stop != std::string::npos, "Signal module syntax");
            const auto filename = std::filesystem::path(object.substr(at+10, stop-at-10)).filename().wstring();
            Check(_wcsicmp(filename.c_str(), L"d3d12.dll") == 0 || _wcsicmp(filename.c_str(), L"d3d12core.dll") == 0, "Signal runtime owner");
            owner = GetModuleHandleW(filename.c_str());
            Check(owner != nullptr, "Signal local runtime retained by discovery");
            const bool loaded = std::any_of(modules.begin(), modules.end(), [&](const auto& module) { return _wcsicmp(module.szModule, filename.c_str()) == 0; });
            if (!loaded) return; // Actual D12 chains will refuse missing support.
        }
        const auto base = reinterpret_cast<std::uintptr_t>(owner);
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
        const auto offset = number("\"rva\":");
        Check(number("\"image_size\":") == nt->OptionalHeader.SizeOfImage && number("\"timestamp\":") == nt->FileHeader.TimeDateStamp &&
            nt->OptionalHeader.SizeOfImage >= 16 && offset <= nt->OptionalHeader.SizeOfImage-16, "product method image identity");
        address = RemoteAddress(process.value, modules, reinterpret_cast<void*>(base+offset));
        const auto hex = object.find("\"bytes\":\"");
        Check(hex != std::string::npos && object.size() >= hex+42 && object[hex+41] == '"', "product expected bytes");
        for (unsigned i = 0; i < 32; ++i) Check(std::isxdigit(static_cast<unsigned char>(object[hex+9+i])) != 0, "product hex syntax");
        for (unsigned i = 0; i < 16; ++i) entry[i] = static_cast<unsigned char>(std::stoul(object.substr(hex+9+i*2,2),nullptr,16));
        unsigned char actual[16]{}; SIZE_T bytes_read{};
        Check(!(address&7) && ReadProcessMemory(process.value, reinterpret_cast<void*>(address), actual, 16, &bytes_read) &&
            bytes_read == 16 && std::equal(std::begin(actual), std::end(actual), std::begin(entry)), "product remote entry bytes/alignment");
    };
    for (unsigned i = 0; i < 5; ++i) method(gtg::overlay::integration::kProductRoles[i], section->product_methods[i], section->product_expected[i], false);
    method("Signal", section->signal_address, section->signal_expected, true);
#endif
    DeviceDiagnosticMapping diagnostic;
    DrawDiagnosticMapping draw_diagnostic;
#ifdef GTG_FEATURE_ATTACH
    InstallDiagnosticMapping install_diagnostic;
    const bool install_diagnostic_available = install_diagnostic.Create(pid, feature_identity.created);
#endif
    if (drawing) Check(draw_diagnostic.Create(pid), "fresh draw diagnostic mapping required");
    if (association) Check(diagnostic.Create(pid), "fresh device diagnostic mapping required");
    const auto local_loader = GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW");
    const auto loader = RemoteAddress(process.value, modules, reinterpret_cast<void*>(local_loader));
    CheckNamedProcess(process.value);
    CheckMitigations(process.value);
#ifndef GTG_FEATURE_ATTACH
    Check(FindNamedGame() == pid && WaitForSingleObject(process.value, 0) == WAIT_TIMEOUT,
        "retained named target changed before remote allocation");
#endif
#ifdef GTG_FEATURE_ATTACH
    Check(GtgFeatureContinue(), "feature canceled before remote allocation");
#endif
    const SIZE_T size = (dll.native().size() + 1) * sizeof(wchar_t);
    void* remote = VirtualAllocEx(process.value, nullptr, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    Check(remote != nullptr, "remote path allocation");
    SIZE_T written{};
    if (!WriteProcessMemory(process.value, remote, dll.c_str(), size, &written) || written != size) {
        VirtualFreeEx(process.value, remote, 0, MEM_RELEASE); throw std::runtime_error("remote path write");
    }
    Check(WaitForSingleObject(process.value, 0) == WAIT_TIMEOUT, "target exited before loading");
#ifdef GTG_FEATURE_ATTACH
    const auto loader_started = GetTickCount64();
#endif
    Handle thread(CreateRemoteThread(process.value, nullptr, 0, reinterpret_cast<LPTHREAD_START_ROUTINE>(loader), remote, 0, nullptr));
    if (!thread.value) { VirtualFreeEx(process.value, remote, 0, MEM_RELEASE); throw std::runtime_error("remote loader refused"); }
    if (WaitForSingleObject(thread.value, 5000) != WAIT_OBJECT_0)
        throw std::runtime_error("loader pending; path retained, target untouched by controller cleanup");
    Check(VirtualFreeEx(process.value, remote, 0, MEM_RELEASE), "free completed loader arguments");
#ifdef GTG_FEATURE_ATTACH
    GtgFeatureStartupStage("loader-complete", GetTickCount64() - loader_started);
#endif
    if (load_only) {
        const auto ready_deadline = GetTickCount64() + (discovery ? 3000 : 1000);
        while (InterlockedCompareExchange(&section->header.state, 0, 0) == static_cast<LONG>(LoadState::kWaiting) &&
               GetTickCount64() < ready_deadline) Sleep(10);
        Check(InterlockedCompareExchange(&section->header.state, 0, 0) == static_cast<LONG>(LoadState::kReady),
            "load worker did not acknowledge IPC");
        const auto base = section->header.module_base;
        Check(base != 0, "load worker module identity missing");
        std::cout << "load=ready module=" << base << '\n';
        InterlockedExchange(&section->header.release, 1);
        if (discovery) {
            const auto current = Modules(pid);
            const auto owner = std::find_if(current.begin(), current.end(), [section](const auto& module) {
                return reinterpret_cast<std::uintptr_t>(module.modBaseAddr) == static_cast<std::uintptr_t>(section->queue_identity);
            });
            Check(owner != current.end() && section->signal_address >= static_cast<std::uintptr_t>(section->queue_identity) &&
                owner->modBaseSize >= 16 && section->signal_address - static_cast<std::uintptr_t>(section->queue_identity) <= owner->modBaseSize - 16,
                "queue candidate owner/RVA");
            IMAGE_DOS_HEADER dos{}; IMAGE_NT_HEADERS64 nt{};
            Check(ReadProcessMemory(process.value, owner->modBaseAddr, &dos, sizeof(dos), &read) && read == sizeof(dos) &&
                dos.e_magic == IMAGE_DOS_SIGNATURE && dos.e_lfanew > 0 &&
                static_cast<DWORD>(dos.e_lfanew) <= owner->modBaseSize - sizeof(nt), "queue owner DOS");
            Check(ReadProcessMemory(process.value, owner->modBaseAddr + dos.e_lfanew, &nt, sizeof(nt), &read) && read == sizeof(nt) &&
                nt.Signature == IMAGE_NT_SIGNATURE && nt.FileHeader.Machine == IMAGE_FILE_MACHINE_AMD64 &&
                nt.FileHeader.TimeDateStamp == section->reserved &&
                nt.OptionalHeader.SizeOfImage == static_cast<DWORD>(section->transaction_stage) &&
                nt.OptionalHeader.SizeOfImage == owner->modBaseSize, "queue owner PE identity");
            std::array<unsigned char, 16> signal_bytes{};
            Check(ReadProcessMemory(process.value, reinterpret_cast<void*>(section->signal_address), signal_bytes.data(), signal_bytes.size(), &read) &&
                read == signal_bytes.size() && std::equal(signal_bytes.begin(), signal_bytes.end(), section->signal_expected),
                "queue candidate bytes");
            std::wcout << L"candidate owner=" << owner->szExePath << L" rva=" <<
                (section->signal_address - static_cast<std::uintptr_t>(section->queue_identity)) << L" timestamp=" <<
                section->reserved << L" image_size=" << section->transaction_stage << L'\n';
        }
        bool absent = false;
        const auto unload_deadline = GetTickCount64() + 3000;
        while (GetTickCount64() < unload_deadline) {
            Sleep(20);
            const auto current = Modules(pid);
            absent = std::none_of(current.begin(), current.end(), [base, &dll](const auto& module) {
                return reinterpret_cast<std::uintptr_t>(module.modBaseAddr) == base ||
                    _wcsicmp(module.szModule, dll.filename().c_str()) == 0;
            });
            if (absent) break;
        }
        Check(absent && InterlockedCompareExchange(&section->header.state, 0, 0) == static_cast<LONG>(LoadState::kReleased),
            "load probe did not acknowledge release and disappear");
        Check(WaitForSingleObject(process.value, 0) == WAIT_TIMEOUT &&
            ReadProcessMemory(process.value, reinterpret_cast<void*>(present), bytes.data(), bytes.size(), &read) &&
            read == bytes.size() && bytes == before_bytes, "target health/unchanged Present verification failed");
        std::cout << "PASS named game " << (discovery ? "queue candidate discovery" : "remote load IPC") <<
            " release unloaded=1 alive=1 present_unchanged=1\n";
        return 0;
    }
#ifdef GTG_FEATURE_ATTACH
    struct ReadyClock {
        ULONGLONG Now() noexcept { return GetTickCount64(); }
        void Pause(DWORD ms) noexcept { Sleep(ms); }
    } ready_clock;
    struct ReadyOps {
        HookSection* section;
        const DrawDiagnostics* data;
        HANDLE process;
        DWORD pid;
        bool Continue() noexcept { return GtgFeatureContinue() && WaitForSingleObject(process, 0) == WAIT_TIMEOUT; }
        gtg::overlay::integration::InspectionState Inspect() noexcept {
            gtg::overlay::integration::InspectionEvidence e;
            e.hook_state = InterlockedCompareExchange(&section->header.state, 0, 0);
            e.backend = InterlockedCompareExchange(&section->product_backend, 0, 0);
            e.ready = InterlockedCompareExchange(&section->product_ready, 0, 0);
            e.presents = InterlockedCompareExchange64(&section->hook_calls, 0, 0) > 0;
            e.errors = section->failures != 0 || section->control_errors != 0;
            e.ambiguous = section->queue_ambiguous != 0;
            e.signal = section->signal_address != 0;
            e.nested = section->nested_signal_calls > 0;
            e.queue = section->queue_identity != 0;
            if (data && InterlockedCompareExchange(const_cast<volatile LONG*>(&data->identity_state), 0, 0) == 2) {
                e.identity_ready = true;
                e.metadata_valid = data->chain && data->device && data->width && data->height &&
                    SUCCEEDED(data->desc_result) && data->on12_result == E_NOINTERFACE;
                DWORD actual_pid{};
                e.window_owned = GetWindowThreadProcessId(reinterpret_cast<HWND>(data->output_window), &actual_pid) && actual_pid == pid;
                e.mismatch = data->mismatch != 0;
                e.staged_draw = data->draw_state != 0;
            }
            e.staged_draw |= section->control_samples[0] != 0 || section->bitmap_uploads != 0;
            return gtg::overlay::integration::ClassifyInspection(e);
        }
    } ready_ops{section, draw_diagnostic.Data(), process.value, pid};
    const auto inspection_started = GetTickCount64();
    const bool inspection_ready = gtg::overlay::integration::WaitForInspection(ready_clock, ready_ops);
    GtgFeatureStartupStage(inspection_ready ? "inspection-ready" : "inspection-refused-or-timeout", GetTickCount64() - inspection_started);
    Check(inspection_ready, "native inspection not ready, refused, canceled or timed out; drawing remains disabled");
#else
    for (unsigned sample = 0; sample < 20; ++sample) {
        Sleep(500);
        std::cout << "state=" << InterlockedCompareExchange(&section->header.state, 0, 0)
            << " stage=" << section->transaction_stage << " threads=" << section->reserved
            << " presents=" << InterlockedCompareExchange64(&section->hook_calls, 0, 0)
            << " failures=" << InterlockedCompareExchange(&section->failures, 0, 0)
            << " signals=" << InterlockedCompareExchange64(&section->signal_calls, 0, 0)
            << " nested=" << InterlockedCompareExchange64(&section->nested_signal_calls, 0, 0)
            << " queue=" << InterlockedCompareExchange64(&section->queue_identity, 0, 0)
            << " ambiguous=" << InterlockedCompareExchange(&section->queue_ambiguous, 0, 0)
            << " product_ready=" << section->product_ready << " copy_size=" << section->product_patch_copy_size
            << " boundary_mask=" << section->product_patch_boundaries << '\n';
        if (drawing) std::cout << "drawn=" << section->control_samples[0]
            << " placement=" << section->motion_sampled_placement
            << " rect=" << section->motion_last_left << ',' << section->leading_small_samples
            << ' ' << section->observed_width << 'x' << section->observed_height
            << " copies=" << section->bitmap_copies << " uploads=" << section->bitmap_uploads << '\n';
    }
#endif
    const bool installed = InterlockedCompareExchange(&section->header.state, 0, 0) == static_cast<LONG>(HookState::kInstalled);
#ifdef GTG_FEATURE_ATTACH
    InstallReport install_report{};
    if (install_diagnostic_available && ReadInstallReport(*install_diagnostic.Data(), pid, feature_identity.created, install_report))
        PrintInstallReport(std::cout, install_report);
    else std::cout << "install diagnostics unavailable or pending\n";
#endif
    if (association) {
        const auto* data = diagnostic.Data();
        std::cout << "device dx11=" << data->dx11_calls << " dx12=" << data->dx12_calls << " neither=" << data->neither_calls << '\n';
        if (InterlockedCompareExchange(&diagnostic.Data()->first_state, 0, 0) == 2)
            std::cout << "device first_step=" << data->first_step << " HRESULT=0x" << std::hex <<
                static_cast<DWORD>(data->first_hresult) << std::dec << " depth=" << data->first_depth <<
                " chain=" << data->first_chain << '\n';
    }
    const bool seen = InterlockedCompareExchange64(&section->hook_calls, 0, 0) > 0;
    const bool healthy = InterlockedCompareExchange(&section->failures, 0, 0) == 0 &&
        WaitForSingleObject(process.value, 0) == WAIT_TIMEOUT;
    Check(installed && seen && healthy, "counter did not acknowledge successful Presents in a live target");
#if defined(GTG_NAMED_TARGET_RESEARCH) || defined(GTG_FOREGROUND_OVERLAY)
    if (drawing) {
        const auto* data = draw_diagnostic.Data();
        PrintDrawDiagnostics(std::cout, *data);
        DWORD window_pid{};
        bool backend_ok = true;
#ifdef GTG_FEATURE_ATTACH
        backend_ok = section->product_ready == 1 && (section->product_backend == 1 ||
            (section->product_backend == 2 && section->signal_address && section->nested_signal_calls > 0 &&
                section->queue_identity && !section->queue_ambiguous));
#endif
        Check(backend_ok && InterlockedCompareExchange(&draw_diagnostic.Data()->identity_state, 0, 0) == 2 &&
            data->chain && data->device && data->width && data->height && SUCCEEDED(data->desc_result) &&
            data->on12_result == E_NOINTERFACE &&
            GetWindowThreadProcessId(reinterpret_cast<HWND>(data->output_window), &window_pid) && window_pid == pid &&
            data->mismatch == 0 && section->control_samples[0] == 0 && section->bitmap_uploads == 0 && data->draw_state == 0,
            "native backend/target-window inspection not established; drawing remains disabled, DLL resident");
        CheckNamedProcess(process.value);
        Check(WaitForSingleObject(process.value, 0) == WAIT_TIMEOUT, "target exited before drawing activation");
        std::cout << "PASS counter/native backend inspection; no uploads or drawing; enabling IPC OSD\n";
#ifdef GTG_FEATURE_ATTACH
        return GtgFeatureSession(*section,process.value);
#else
        InterlockedExchange(&section->header.release, 0);
        Sleep(3000);
        if (section->failures || section->control_errors || WaitForSingleObject(process.value, 0) != WAIT_TIMEOUT) {
            InterlockedExchange(&section->header.release, 1);
            throw std::runtime_error("OSD health failure; drawing disabled, hook retained");
        }
#endif
    }
#endif
    if (drawing) {
        PrintDrawDiagnostics(std::cout, *draw_diagnostic.Data());
        Check(section->control_samples[0] > 5 && section->control_errors == 0 &&
            section->bitmap_uploads > 0 && section->pixel_samples == 0,
            "OSD draw submissions not established; hook remains resident, no readback attempted");
        std::cout << "PASS named game draw submissions; visible OSD and movement require owner observation\n";
    }
    if (association) Check(section->nested_signal_calls > 0 && section->queue_identity != 0 &&
        section->queue_ambiguous == 0, "actual queue association not proven; resident hooks remain counter-only");
    std::cout << "PASS named game counter; DLL resident until game exits\n";
    drawing_guard.Complete();
    return 0;
}
}
#ifdef GTG_FEATURE_ATTACH
int GtgFeatureAttach(gtg::overlay::integration::Identity target, const wchar_t* dll, const wchar_t* report) {
    feature_identity=target;
    return Run(true,false,false,false,false,true,std::filesystem::canonical(dll),std::filesystem::canonical(report));
}
#else
int wmain(int argc, wchar_t** argv) {
    try {
        const bool discovery = argc == 4 && std::wstring(argv[2]) == L"--queue-probe";
        const bool association = argc == 4 && std::wstring(argv[2]) == L"--attach-association";
        const bool drawing = argc == 4 && std::wstring(argv[2]) == L"--attach-osd";
        const bool load_only = discovery || (argc == 4 && std::wstring(argv[2]) == L"--load-probe");
        const bool observe = argc == 3 && std::wstring(argv[2]) == L"--observe";
#ifdef GTG_NAMED_TARGET_RESEARCH
        Check(argc == 2 || observe || (argc == 4 && (drawing || (load_only && !discovery))),
            "this profile supports only preflight, load-probe, staged attach-osd and observe");
#endif
        Check(argc == 2 || observe || (argc == 4 && (drawing || association || load_only || std::wstring(argv[2]) == L"--attach-counter")),
            "usage: gtg_target_attach HARDWARE_REPORT [--observe | --queue-probe|--load-probe|--attach-counter|--attach-association|--attach-osd PATH_TO_NAMED_DLL]");
        return Run(argc == 4, load_only, observe, discovery, association, drawing, argc == 4 ? std::filesystem::canonical(argv[3]) : std::filesystem::path{},
            std::filesystem::canonical(argv[1]));
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << " Win32=" << GetLastError() << '\n'; return 1;
    }
}
#endif
