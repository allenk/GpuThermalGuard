#include "color_choice.hpp"
#include "../injector/load_protocol.hpp"
#include <cstdio>
#include <limits>
#include <string_view>
using namespace gtg::overlay::integration;
namespace {
bool Number(std::wstring_view text, std::uint64_t& value) noexcept {
    value = 0; if (text.empty()) return false;
    for (const wchar_t c : text) {
        if (c < L'0' || c > L'9') return false;
        const auto digit = static_cast<unsigned>(c - L'0');
        if (value > (std::numeric_limits<std::uint64_t>::max() - digit) / 10) return false;
        value = value * 10 + digit;
    }
    return value != 0;
}
struct Process { HANDLE value{}; ~Process() { if (value) CloseHandle(value); } };
}
int wmain(int argc, wchar_t** argv) {
    std::uint64_t pid{}, created{};
    if (argc != 6 || std::wstring_view(argv[1]) != L"--pid" || std::wstring_view(argv[3]) != L"--created" ||
        std::wstring_view(argv[5]) != L"--assume-sdr" || !Number(argv[2], pid) || pid > MAXDWORD || !Number(argv[4], created)) {
        std::fputs("usage: gtg_overlay_color_choice --pid PID --created FILETIME --assume-sdr\n"
            "Explicit research SDR interpretation; NOT color detection. Wrong choice may produce wrong brightness/colors.\n", stderr);
        return 2;
    }
    Process target{OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, static_cast<DWORD>(pid))};
    if (!target.value || ChoiceCreationTime(target.value) != created || WaitForSingleObject(target.value, 0) != WAIT_TIMEOUT) {
        std::fputs("REFUSED target identity missing, reused or exited\n", stderr); return 3;
    }
    wchar_t hook_name[64]{}; gtg::research::SectionName(static_cast<DWORD>(pid), hook_name);
    HANDLE existing = OpenFileMappingW(FILE_MAP_READ, FALSE, hook_name);
    const DWORD existing_error = existing ? ERROR_SUCCESS : GetLastError();
    if (existing) CloseHandle(existing);
    if (existing || existing_error != ERROR_FILE_NOT_FOUND) {
        std::fputs("REFUSED existing/uncertain resident-hook namespace; restart target before choosing color\n", stderr); return 6;
    }
    ColorChoiceMapping choice;
    if (!choice.Create(static_cast<DWORD>(pid), created)) {
        std::fprintf(stderr, "REFUSED choice mapping unavailable/already exists Win32=%lu\n", GetLastError()); return 4;
    }
    std::printf("READY research SDR assumption for pid=%llu created=%llu; not observed SDR.\n"
        "Use GTG ALT+F10 to inject. Tool does not inject/toggle/unload. Close this tool to remove choice before injection.\n"
        "After injection the copied choice stays until game exit; ALT+F10 hides drawing.\n", pid, created);
    std::fflush(stdout);
    // Only a process handle wait; no polling loop or additional thread.
    return WaitForSingleObject(target.value, INFINITE) == WAIT_OBJECT_0 ? 0 : 5;
}
