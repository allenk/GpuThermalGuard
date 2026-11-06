// Stand-in for GpuThermalGuard.exe in the update script's tests. One binary
// plays the parts GTG's EXE plays, chosen by its command line the same way:
//   (none)                 the real supervisor (RunSupervisor), with names of
//                          its own so it never meets a running GTG
//   --supervised <mapping> a Tray: reports healthy every 200 ms, and on a
//                          request hands the update off with the real HandOff
//                          and the compiled-in script, then exits cleanly
//   --game <dll> <ms>      keeps a DLL loaded, as a game keeps the overlay
// A copy's build identity is the marker the harness appends to its file.
#include <windows.h>

#include <shellapi.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <string>

#include "supervision/supervisor.hpp"
#include "update/digest.hpp"
#include "update/files.hpp"
#include "update/handoff.hpp"
#include "update/plan.hpp"

namespace fs = std::filesystem;

namespace {

fs::path SelfPath() {
    std::wstring path(MAX_PATH, L'\0');
    for (;;) {
        const DWORD n = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
        if (n < path.size()) {
            path.resize(n);
            return path;
        }
        path.resize(path.size() * 2);
    }
}

std::string ReadAll(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

std::string BuildName(const fs::path& file) {
    const std::string data = ReadAll(file);
    const auto at = data.rfind("LABBUILD:");
    if (at == std::string::npos) return "unmarked";
    const auto begin = at + 9;
    const auto end = data.find('\n', begin);
    return data.substr(begin, end == std::string::npos ? std::string::npos : end - begin);
}

void Log(const fs::path& dir, const char* role, const std::string& text) {
    SYSTEMTIME t{};
    GetLocalTime(&t);
    const std::string line = std::format(
        "{:04}-{:02}-{:02} {:02}:{:02}:{:02}.{:03} pid={} {} {}\r\n", t.wYear, t.wMonth, t.wDay,
        t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, GetCurrentProcessId(), role, text);
    const HANDLE file = CreateFileW((dir / L"lab.log").c_str(), FILE_APPEND_DATA,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                    OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    (void)WriteFile(file, line.data(), static_cast<DWORD>(line.size()), &written, nullptr);
    CloseHandle(file);
}

void WriteText(const fs::path& file, const std::string& text) {
    const fs::path temporary = file.wstring() + L".tmp";
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        out << text;
    }
    MoveFileExW(temporary.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING);
}

bool Elevated() {
    HANDLE token{};
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
    TOKEN_ELEVATION elevation{};
    DWORD bytes{};
    const BOOL ok =
        GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &bytes);
    CloseHandle(token);
    return ok && elevation.TokenIsElevated != 0;
}

std::wstring Environment(const wchar_t* name, const wchar_t* fallback) {
    wchar_t value[1024]{};
    const DWORD n = GetEnvironmentVariableW(name, value, 1024);
    return n > 0 && n < 1024 ? std::wstring(value, n) : std::wstring(fallback);
}

unsigned EnvironmentMs(const wchar_t* name, const unsigned fallback) {
    const std::wstring text = Environment(name, L"");
    return text.empty() ? fallback : static_cast<unsigned>(std::wcstoul(text.c_str(), nullptr, 10));
}

// What GTG's Tray does once the reader has agreed and the moment is safe:
// describe the staged files in a plan and hand it to the script.
int HandOffUpdate(const fs::path& dir, const std::string& build, const bool tamper,
                  const bool hang) {
    using namespace gtg::update;
    const fs::path work = dir / kWorkFolderName;
    const fs::path staged = work / L"staged" / L"TEST";
    Sha256 sha;
    Plan plan;
    plan.install = dir;
    plan.entry = "app.exe";
    plan.old_version = "1.2.0";
    plan.old_build = build;
    plan.new_version = "1.2.1";
    plan.release = "TEST";
    for (const auto& entry : fs::directory_iterator(staged)) {
        const std::string data = ReadAll(entry.path());
        PlanFile file;
        file.name = entry.path().filename().string();
        file.staged = entry.path();
        file.size = data.size();
        file.sha256 = *sha.Of(data);
        if (file.name == "app.exe") plan.new_build = BuildName(entry.path());
        plan.files.push_back(file);
    }
    const DWORD supervisor_pid = gtg::supervision::SupervisorPid();
    const HANDLE supervisor = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, supervisor_pid);
    plan.wait.push_back({supervisor_pid, ProcessCreated(supervisor)});
    if (supervisor != nullptr) CloseHandle(supervisor);
    plan.wait.push_back({GetCurrentProcessId(), ProcessCreated(GetCurrentProcess())});
    plan.mutex = Environment(L"GTG_STANDIN_MUTEX", L"Local\\GtgUpdateStandin.Update");
    plan.wait_exit_ms = EnvironmentMs(L"GTG_STANDIN_WAIT_MS", 10'000);
    plan.ready_ms = EnvironmentMs(L"GTG_STANDIN_READY_MS", 15'000);
    auto script = EmbeddedScript(GetModuleHandleW(nullptr));
    if (!script) {
        Log(dir, "TRAY", "no compiled-in script");
        return 6;
    }
    // mutate_apply.ps1 runs the scenarios against changed copies of the script.
    const std::wstring mutant = Environment(L"GTG_STANDIN_SCRIPT", L"");
    const std::string mutant_text = mutant.empty() ? std::string() : ReadAll(mutant);
    if (!mutant.empty()) script = mutant_text;
    const HandOffResult result = HandOff(work, plan, *script, supervisor_pid);
    if (!result.ok) {
        std::string problem;
        for (const wchar_t c : result.problem)
            problem.push_back(c < 0x80 ? static_cast<char>(c) : '?');
        Log(dir, "TRAY", "hand-off failed: " + problem);
        return -1;
    }
    Log(dir, "TRAY",
        std::format("handed off script pid={} in_job={} expected={}", result.script_pid,
                    result.in_job ? 1 : 0, plan.new_build));
    if (tamper) {
        std::ofstream out(staged / L"app.exe", std::ios::binary | std::ios::app);
        out << 'X';
        Log(dir, "TRAY", "changed the staged app.exe after the hand-off");
    }
    if (hang) {
        Log(dir, "TRAY", "hangs instead of exiting");
        for (;;) {
            gtg::supervision::ReportHealth(true);
            Sleep(200);
        }
    }
    Log(dir, "TRAY", "exits cleanly for the update");
    gtg::supervision::MarkCleanExit();
    return 0;
}

int RunTray() {
    const fs::path self = SelfPath();
    const fs::path dir = self.parent_path();
    const std::string build = BuildName(self);
    if (build == "crash") {
        Log(dir, "TRAY", "build=crash exits before ready");
        return 3;
    }
    Log(dir, "TRAY",
        std::format("started build={} elevated={} supervisor={}", build, Elevated() ? 1 : 0,
                    gtg::supervision::SupervisorPid()));
    WriteText(dir / L"lab-ready.json",
              std::format("{{\"build\":\"{}\",\"pid\":{}}}", build, GetCurrentProcessId()));
    const fs::path request = dir / L"lab-request.json";
    for (;;) {
        gtg::supervision::ReportHealth(true);
        Sleep(200);
        const std::string text = ReadAll(request);
        if (text.empty()) continue;
        DeleteFileW(request.c_str());
        if (text.find("quit") != std::string::npos) {
            gtg::supervision::MarkCleanExit();
            return 0;
        }
        const int code = HandOffUpdate(dir, build, text.find("tamper") != std::string::npos,
                                       text.find("hang") != std::string::npos);
        if (code >= 0) return code;
        // A failed hand-off leaves this build running, as it leaves GTG.
    }
}

int RunGame(const std::wstring& dll, const DWORD ms) {
    const fs::path dir = fs::path(dll).parent_path();
    const HMODULE module = LoadLibraryW(dll.c_str());
    Log(dir, "GAME", module ? "loaded the overlay" : "load failed");
    Sleep(ms);
    return 0;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    const std::wstring mode = argc > 1 ? argv[1] : L"";
    if (mode == L"--supervised" && argc > 2) {
        if (!gtg::supervision::AttachChild(argv[2])) return 12;
        return RunTray();
    }
    if (mode == L"--game" && argc > 3) return RunGame(argv[2], std::wcstoul(argv[3], nullptr, 10));
    gtg::supervision::SupervisorOptions options;
    options.singleton_name = L"Local\\GtgUpdateStandin.Supervisor";
    options.journal = false;
    return gtg::supervision::RunSupervisor(SelfPath().wstring(), SW_HIDE, options);
}
