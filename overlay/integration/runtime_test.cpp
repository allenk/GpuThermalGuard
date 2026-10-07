// Owned process/window test. The helper uses a counter section in this process,
// never opens a third-party process for writing and never injects a DLL.
#include "runtime.cpp"
#include "../injector/hook_protocol.hpp"
#include <gdiplus.h>
#include <cstdio>
WTL::CAppModule _Module;
int GtgFeatureSession(gtg::research::HookSection&, HANDLE) noexcept;
gtg::overlay::integration::ChoiceRead GtgFeatureColorChoice(DWORD, std::uint64_t,
    gtg::overlay::integration::ColorPolicy&) noexcept;
int GtgFeatureAttach(gtg::overlay::integration::Identity target, const wchar_t*, const wchar_t*) {
    HANDLE process =
        OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, target.pid);
    if (!process || gtg::overlay::integration::CreationTime(process) != target.created) return 1;
    auto section = std::make_unique<gtg::research::HookSection>();
    gtg::overlay::integration::ColorPolicy policy{};
    if (GtgFeatureColorChoice(target.pid, target.created, policy) ==
        gtg::overlay::integration::ChoiceRead::Invalid) { CloseHandle(process); return 1; }
    section->product_color_policy = static_cast<DWORD>(policy);
    const int result = GtgFeatureSession(*section, process);
    CloseHandle(process);
    return result;
}
namespace {
void Require(bool ok, const char* why) {
    if (!ok) throw std::runtime_error(why);
}
struct ExitEvidence {
    DWORD helper_pid{};
    volatile LONG exit_now{};
};
int ParentExitFixture(DWORD target_pid) {
    using namespace gtg::overlay::integration;
    wchar_t name[96]{};
    swprintf_s(name, L"Local\\GTG.OverlayExitTest.%lu", target_pid);
    HANDLE mapping = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, name);
    auto* evidence = mapping ? static_cast<ExitEvidence*>(MapViewOfFile(
                                   mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(ExitEvidence)))
                             : nullptr;
    if (!evidence) return 1;
    Session session;
    session.target.value =
        OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, target_pid);
    (void)session.policy.Press(true, {target_pid, CreationTime(session.target.value)});
    Launch(session);
    const auto deadline = GetTickCount64() + 12000;
    while (GetTickCount64() < deadline) {
        if (InterlockedCompareExchange(&session.control->state, 0, 0) ==
            static_cast<LONG>(HelperState::Ready)) {
            InterlockedExchange(reinterpret_cast<volatile LONG*>(&evidence->helper_pid),
                                static_cast<LONG>(GetProcessId(session.helper.value)));
            if (InterlockedCompareExchange(&evidence->exit_now, 0, 0)) ExitProcess(0);
        }
        Sleep(10);
    }
    ExitProcess(2);
}
void CheckParentExit() {
    using namespace gtg::overlay::integration;
    wchar_t name[96]{}, path[32768]{};
    swprintf_s(name, L"Local\\GTG.OverlayExitTest.%lu", GetCurrentProcessId());
    Mapping evidence_map;
    evidence_map.Create(sizeof(ExitEvidence), name, true);
    auto* evidence = static_cast<ExitEvidence*>(evidence_map.view);
    Require(GetModuleFileNameW(nullptr, path, 32768) != 0, "fixture path");
    auto command =
        L"\"" + std::wstring(path) + L"\" --parent-exit " + std::to_wstring(GetCurrentProcessId());
    STARTUPINFOW startup{sizeof(startup)};
    startup.dwFlags = STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION process{};
    Require(CreateProcessW(path, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                           nullptr, &startup, &process) != FALSE,
            "owned parent fixture");
    Handle fixture;
    fixture.value = process.hProcess;
    CloseHandle(process.hThread);
    const auto deadline = GetTickCount64() + 10000;
    DWORD helper_pid{};
    while (GetTickCount64() < deadline && !helper_pid) {
        helper_pid = static_cast<DWORD>(InterlockedCompareExchange(
            reinterpret_cast<volatile LONG*>(&evidence->helper_pid), 0, 0));
        Sleep(10);
    }
    Handle helper;
    helper.value = OpenProcess(SYNCHRONIZE, FALSE, helper_pid);
    Require(helper.value != nullptr, "fixture helper ready");
    InterlockedExchange(&evidence->exit_now, 1);
    Require(WaitForSingleObject(fixture.value, 5000) == WAIT_OBJECT_0,
            "parent exits without cleanup");
    Require(WaitForSingleObject(helper.value, 5000) == WAIT_OBJECT_0,
            "helper exits when parent dies, target still alive");
}
}  // namespace
int wmain(int argc, wchar_t** argv) {
    using namespace gtg::overlay::integration;
    namespace ipc = gtg::overlay::ipc;
    if (argc == 3 && wcscmp(argv[1], L"--overlay-helper") == 0) return HelperMain(argv[2]);
    if (argc == 3 && wcscmp(argv[1], L"--parent-exit") == 0)
        return ParentExitFixture(wcstoul(argv[2], nullptr, 10));
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    ULONG_PTR token{};
    Gdiplus::GdiplusStartupInput input;
    Gdiplus::GdiplusStartup(&token, &input, nullptr);
    _Module.Init(nullptr, GetModuleHandleW(nullptr));
    HWND window = CreateWindowExW(0, L"STATIC", L"GTG integration test", WS_POPUP, 40, 40, 388, 330,
                                  nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    int result = 1;
    try {
        Require(token && window, "owned GDI/window setup");
        gtg::tray::OsdOverlay source;
        gtg::telemetry::History history;
        bool repaired{};
        Require(source.Initialize(nullptr, &history, true, {40, 40}, repaired),
                "actual OSD HWND initialized");
        struct OsdLifetime {
            gtg::tray::OsdOverlay& source;
            ~OsdLifetime() { source.Shutdown(); }
        } osd_lifetime{source};
        runtime.source = &source;
        runtime.window = source.m_hWnd;
        Require(source.CurrentDpi() != 0, "actual OSD DPI available");
        Require(!Configure(true, MOD_ALT, VK_F12), "reserved hotkey refused");
        Require(RegisterHotKey(window, 19, MOD_ALT | MOD_CONTROL, VK_F10),
                "conflict fixture registration");
        Require(!Configure(true, MOD_ALT | MOD_CONTROL, VK_F10), "conflicting hotkey refused");
        Require(Configure(true, MOD_ALT | MOD_CONTROL, VK_F11), "alternate hotkey registered");
        const auto id = runtime.hotkey;
        Require(!Configure(true, MOD_ALT | MOD_CONTROL, VK_F10) && runtime.hotkey == id,
                "failed replacement retains old binding");
        UnregisterHotKey(window, 19);
        auto owned = std::make_unique<Session>();
        const Identity identity{GetCurrentProcessId(), CreationTime(GetCurrentProcess())};
        owned->target.value =
            OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, identity.pid);
        (void)owned->policy.Press(true, identity);
        runtime.sessions[0] = std::move(owned);
        auto& s = *runtime.sessions[0];
        s.color_choice = std::make_unique<ColorChoiceMapping>();
        Require(s.color_choice->Create(identity.pid, identity.created), "GTG owns SDR choice before helper launch");
        ColorPolicy selected{};
        Require(ReadColorChoice(identity.pid, identity.created, selected) == ChoiceRead::Chosen &&
            selected == ColorPolicy::ResearchAssumeSdr, "in-process session choice needs no external executable");
        Launch(s);
        Require(Configure(false), "disable during helper startup");
        const auto deadline = GetTickCount64() + 15000;
        while (GetTickCount64() < deadline && s.policy.Current() == State::Installing) {
            Tick();
            Sleep(10);
        }
        Require(s.policy.Current() == State::Hidden, "late helper Ready preserves hidden intent");
        Require(Configure(true, MOD_ALT | MOD_CONTROL, VK_F11), "reenable feature");
        Require(s.policy.Press(true, identity) == Action::Show, "first explicit toggle shows");
        InterlockedExchange(&s.control->enabled, 1);
        ipc::BeginRequest(*s.request, 1);
        s.request->writer_pid = identity.pid;
        s.request->swapchain_width = s.request->display_width = 1920;
        s.request->swapchain_height = s.request->display_height = 1080;
        ipc::BeatRequest(*s.request, NowUs());
        ipc::FinishRequest(*s.request, 1);
        const auto heartbeat = s.request->heartbeat_us;
        Publish(s, 1000);
        Require(s.image_serial && ipc::Enabled(*s.bitmap), "real OSD bitmap published");
        Require(s.first_bitmap_published, "first publication telemetry is one-shot local state");
        Require(s.request->heartbeat_us == heartbeat, "publisher never writes game heartbeat");
        const auto serial = s.image_serial;
        const auto placement = s.placement_serial;
        SetWindowPos(runtime.window, nullptr, 240, 140, 0, 0,
                     SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        Publish(s, 1034);
        Require(s.image_serial == serial && s.placement_serial > placement,
                "motion without bitmap upload");
        s.image_valid = false;
        Publish(s, 1068);
        Require(!ipc::Enabled(*s.bitmap), "failed bitmap cannot reenable before successful render");
        Publish(s, 1500);
        Require(ipc::Enabled(*s.bitmap), "successful render recovers publisher");
        ipc::BeatRequest(*s.request, NowUs() - 10'000'001);
        Publish(s, 1534);
        Require(!ipc::Enabled(*s.bitmap), "stale reverse request hides");
        ipc::BeatRequest(*s.request, NowUs());
        source.SetFpsEnabled(true);
        source.SetRamEnabled(true);
        source.SetNetEnabled(true);
        std::uint64_t render_clock = 2000;
        for (bool compact : {false, true}) {
            source.RestoreCollapsed(compact);
            for (int breaks : {0, 127}) {
                gtg::tray::compact::Layout arrangement;
                arrangement.breaks = static_cast<std::uint8_t>(breaks);
                source.SetCompactLayout(arrangement);
                for (int scale : {100, 200, 250}) {
                    source.SetScalePercent(scale);
                    s.request->swapchain_width = s.request->display_width = 16384;
                    s.request->swapchain_height = s.request->display_height = 16384;
                    Publish(s, render_clock);
                    render_clock += 500;
                    Require(s.image_valid && s.image.width <= ipc::kMaxWidth &&
                                s.image.height <= ipc::kMaxHeight,
                            "actual OSD layout/scale bounded before publication");
                }
            }
        }
        InterlockedExchange(&s.control->stop, 1);
        Require(WaitForSingleObject(s.helper.value, 5000) == WAIT_OBJECT_0,
                "helper observes stop without target kill");
        Tick();
        Require(s.policy.Current() == State::Failed && !ipc::Enabled(*s.bitmap),
                "helper exit fails closed");
        Stop();
        Require(!runtime.source && !runtime.hotkey && !runtime.sessions[0],
                "nonblocking teardown releases state");
        Require(ReadColorChoice(identity.pid, identity.created, selected) == ChoiceRead::Missing,
                "GTG session teardown releases color choice mapping");
        CheckParentExit();
        std::puts(
            "PASS hotkeys, inherited helper control, late disable, bitmap, movement, freshness, "
            "stop and parent death");
        result = 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL %s (%lu)\n", error.what(), GetLastError());
        Stop();
        UnregisterHotKey(window, 19);
    }
    if (window) DestroyWindow(window);
    _Module.Term();
    if (token) Gdiplus::GdiplusShutdown(token);
    return result;
}
