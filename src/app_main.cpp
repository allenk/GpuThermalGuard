#include <windows.h>

#include <atlbase.h>
#include <atlapp.h>

#include <shellapi.h>
#include <gdiplus.h>

#include "service/service_main.hpp"
#include "tray/main_dialog.hpp"
#include "logging/logger.hpp"
#include "supervision/supervisor.hpp"

WTL::CAppModule _Module;

namespace {
struct LoggerLifetime {
    ~LoggerLifetime() { gtg::logging::Shutdown(); }
};

struct GdiplusLifetime {
    GdiplusLifetime() {
        Gdiplus::GdiplusStartupInput input;
        if (Gdiplus::GdiplusStartup(&token, &input, nullptr) != Gdiplus::Ok) token = 0;
    }
    ~GdiplusLifetime() {
        if (token != 0) Gdiplus::GdiplusShutdown(token);
    }
    ULONG_PTR token{};
};
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show_command) {
    int argument_count = 0;
    wchar_t** arguments = CommandLineToArgvW(GetCommandLineW(), &argument_count);
    const bool parsed = arguments != nullptr;
    const bool service_mode = parsed && argument_count >= 2 &&
        _wcsicmp(arguments[1], L"--service") == 0;
    const bool tray_mode = !parsed || argument_count < 2 ||
        _wcsicmp(arguments[1], L"--tray") == 0;
    const bool supervised = parsed && argument_count == 3 &&
        _wcsicmp(arguments[1], L"--supervised") == 0 &&
        gtg::supervision::AttachChild(arguments[2]);
    if (arguments != nullptr) LocalFree(arguments);

    if (!service_mode && !tray_mode && !supervised) {
        MessageBoxW(nullptr, L"Usage: GpuThermalGuard.exe [--tray | --service]",
                    L"GPU Thermal Guard", MB_OK | MB_ICONINFORMATION);
        return ERROR_BAD_ARGUMENTS;
    }

    if (tray_mode) {
        wchar_t executable[32768]{};
        const DWORD length = GetModuleFileNameW(nullptr, executable, 32768);
        if (!length || length >= 32768) return ERROR_INSUFFICIENT_BUFFER;
        return gtg::supervision::RunSupervisor(executable, show_command);
    }

    (void)gtg::logging::Initialize(service_mode ? gtg::logging::Role::Service
                                                : gtg::logging::Role::Tray);
    LoggerLifetime logger_lifetime;
    gtg::logging::Info(service_mode ? L"process started in service mode"
                                    : L"process started in tray mode");
    if (service_mode) return gtg::service::Run();

    GdiplusLifetime gdiplus_lifetime;
    if (gdiplus_lifetime.token == 0) {
        gtg::logging::Warning(L"GDI+ initialization failed; history chart may be unavailable");
    }

    HANDLE single_instance = CreateMutexW(nullptr, FALSE, L"Local\\GpuThermalGuard.Tray.v1");
    if (single_instance == nullptr) return static_cast<int>(GetLastError());
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        gtg::supervision::MarkCleanExit();
        gtg::logging::Info(L"second tray instance requested activation of existing UI");
        const UINT activate = RegisterWindowMessageW(L"GpuThermalGuard.Activate.v1");
        DWORD_PTR ignored = 0;
        (void)SendMessageTimeoutW(HWND_BROADCAST, activate, 0, 0,
            SMTO_ABORTIFHUNG | SMTO_NORMAL, 1'000, &ignored);
        CloseHandle(single_instance);
        return 0;
    }

    WTL::CMessageLoop message_loop;
    if (FAILED(_Module.Init(nullptr, instance))) {
        CloseHandle(single_instance);
        return 1;
    }
    _Module.AddMessageLoop(&message_loop);

    gtg::tray::MainDialog dialog;
    if (dialog.Create(nullptr) == nullptr) {
        _Module.RemoveMessageLoop();
        _Module.Term();
        CloseHandle(single_instance);
        return 2;
    }
    message_loop.AddMessageFilter(&dialog);
    dialog.ShowWindow(show_command == SW_HIDE ? SW_SHOWNORMAL : show_command);
    const int result = message_loop.Run();

    message_loop.RemoveMessageFilter(&dialog);
    _Module.RemoveMessageLoop();
    _Module.Term();
    CloseHandle(single_instance);
    return result;
}
