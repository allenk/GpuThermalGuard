#include "runtime.hpp"
#include "control.hpp"
#include "request_policy.hpp"
#include "render_policy.hpp"
#include "color_choice.hpp"
#include "consent_policy.hpp"
#include "../injector/load_protocol.hpp"
#include "localization/localization.hpp"
#include "../ipc/osd_section.hpp"
#include "../publisher/osd_geometry.hpp"
#include "target_admission.hpp"
#include "embed_policy.hpp"
#include "refused_targets.hpp"
#include "tray/osd_overlay.hpp"
#include "settings/settings.hpp"
#include "logging/logger.hpp"
#include "timing/ui_phase_timing.hpp"
#include <commctrl.h>
#include <appmodel.h>
#include <sddl.h>
#include <array>
#include <algorithm>
#include <cstring>
#include <filesystem>
#include <format>
#include <memory>
#include <stdexcept>
#include <span>
#include <string>

namespace gtg::overlay::integration {
namespace {
constexpr UINT_PTR kTimer = 0x475449;
constexpr UINT_PTR kSubclass = 0x475449;

struct Handle {
    HANDLE value{};

    ~Handle() {
        if (value) CloseHandle(value);
    }

    Handle() = default;
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
};

struct Mapping {
    Handle handle;
    void* view{};

    ~Mapping() {
        if (view) UnmapViewOfFile(view);
    }

    void Create(DWORD bytes, const wchar_t* name, bool writable = false) {
        PSECURITY_DESCRIPTOR descriptor{};
        if (name && !ConvertStringSecurityDescriptorToSecurityDescriptorW(
                        writable ? L"D:(A;;GA;;;BA)(A;;GA;;;SY)(A;;GA;;;IU)"
                                 : L"D:(A;;GA;;;BA)(A;;GA;;;SY)(A;;GR;;;IU)",
                        SDDL_REVISION_1, &descriptor, nullptr))
            throw std::runtime_error("overlay section ACL");
        SECURITY_ATTRIBUTES attributes{sizeof(attributes), descriptor, name ? FALSE : TRUE};
        handle.value =
            CreateFileMappingW(INVALID_HANDLE_VALUE, &attributes, PAGE_READWRITE, 0, bytes, name);
        const auto error = GetLastError();
        if (descriptor) LocalFree(descriptor);
        if (!handle.value || (name && error == ERROR_ALREADY_EXISTS))
            throw std::runtime_error("overlay section already exists or unavailable");
        view = MapViewOfFile(handle.value, FILE_MAP_ALL_ACCESS, 0, 0, bytes);
        if (!view) throw std::runtime_error("overlay section view");
    }
};

struct Session {
    ULONGLONG startup_started{};
    bool first_bitmap_published{};
    SessionPolicy policy;
    Handle target, helper;
    Mapping control_map, bitmap_map, request_map;
    std::unique_ptr<ColorChoiceMapping> color_choice;
    Control* control{};
    ipc::Section* bitmap{};
    ipc::Request* request{};
    std::uint64_t last_render{};
    unsigned width{}, height{}, image_serial{}, placement_serial{};
    bool image_valid{};
    tray::OsdOverlay::Bitmap image;

    ~Session() {
        if (bitmap) ipc::SetEnabled(*bitmap, false);
        if (control) {
            InterlockedExchange(&control->enabled, 0);
            InterlockedExchange(&control->stop, 1);
        }
        // No waiting, remote FreeLibrary, or hook removal during UI teardown.
    }
};

struct Runtime {
    tray::OsdOverlay* source{};
    HWND window{};
    bool enabled{};
    bool choosing{};
    int hotkey{};
    unsigned modifiers{}, key{};
    std::array<std::unique_ptr<Session>, 4> sessions;
    // AF-20261008-overlay-embedded-osd: the desktop OSD steps aside while a
    // drawing session's game is in front.
    bool hides_osd{true};
    ForegroundSettle settle;
    // Failed targets, outside the four slots; see refused_targets.hpp.
    RefusedTargets refused;
    unsigned prune_ticks{};
} runtime;

std::uint64_t NowUs() noexcept {
    LARGE_INTEGER now{}, frequency{};
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&frequency);
    return static_cast<std::uint64_t>(now.QuadPart / frequency.QuadPart) * 1'000'000 +
           static_cast<std::uint64_t>(now.QuadPart % frequency.QuadPart) * 1'000'000 /
               frequency.QuadPart;
}

void Launch(Session& s) {
    s.startup_started = GetTickCount64();
    wchar_t path[32768]{};
    const auto length = GetModuleFileNameW(nullptr, path, 32768);
    if (!length || length >= 32768) throw std::runtime_error("overlay executable path");
    s.control_map.Create(sizeof(Control), nullptr);
    s.control = new (s.control_map.view) Control;
    s.control->parent_pid = GetCurrentProcessId();
    s.control->parent_created = CreationTime(GetCurrentProcess());
    s.control->target = s.policy.Target();
    wchar_t name[96]{};
    swprintf_s(name, L"Local\\GTG.Research.Bitmap.%lu", static_cast<DWORD>(s.policy.Target().pid));
    s.bitmap_map.Create(static_cast<DWORD>(ipc::kSectionBytes), name);
    s.bitmap = static_cast<ipc::Section*>(s.bitmap_map.view);
    s.bitmap->magic = ipc::kMagic;
    s.bitmap->version = ipc::kVersion;
    s.bitmap->header_size = sizeof(ipc::Section);
    s.bitmap->writer_pid = GetCurrentProcessId();
    swprintf_s(name, L"Local\\GTG.Research.Request.%lu", static_cast<DWORD>(s.policy.Target().pid));
    s.request_map.Create(static_cast<DWORD>(ipc::kRequestBytes), name, true);
    s.request = static_cast<ipc::Request*>(s.request_map.view);
    s.request->magic = ipc::kRequestMagic;
    s.request->version = ipc::kRequestVersion;
    s.request->header_size = sizeof(ipc::Request);

    SIZE_T bytes{};
    InitializeProcThreadAttributeList(nullptr, 1, 0, &bytes);
    std::vector<std::byte> storage(bytes);
    auto* attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
    if (!InitializeProcThreadAttributeList(attributes, 1, 0, &bytes))
        throw std::runtime_error("overlay helper attributes");

    struct Guard {
        LPPROC_THREAD_ATTRIBUTE_LIST value;

        ~Guard() { DeleteProcThreadAttributeList(value); }
    } guard{attributes};

    if (!UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                   &s.control_map.handle.value, sizeof(HANDLE), nullptr, nullptr))
        throw std::runtime_error("overlay helper handle list");
    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESHOWWINDOW;
    startup.StartupInfo.wShowWindow = SW_HIDE;
    startup.lpAttributeList = attributes;
    auto command = L"\"" + std::wstring(path) + L"\" --overlay-helper " +
                   std::to_wstring(reinterpret_cast<std::uintptr_t>(s.control_map.handle.value));
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(path, command.data(), nullptr, nullptr, TRUE,
                        EXTENDED_STARTUPINFO_PRESENT | CREATE_NO_WINDOW | NORMAL_PRIORITY_CLASS,
                        nullptr, nullptr, &startup.StartupInfo, &process))
        throw std::runtime_error("overlay helper launch");
    s.helper.value = process.hProcess;
    CloseHandle(process.hThread);
    SetHandleInformation(s.control_map.handle.value, HANDLE_FLAG_INHERIT, 0);
}

bool NamespaceMissing(const wchar_t* name) noexcept {
    const auto mapping = OpenFileMappingW(FILE_MAP_READ, FALSE, name);
    const auto error = GetLastError();
    if (mapping) CloseHandle(mapping);
    return !mapping && error == ERROR_FILE_NOT_FOUND;
}

bool ChoiceNamespacesClear(DWORD pid) noexcept {
    wchar_t hook_name[64]{}, choice_name[96]{};
    research::SectionName(pid, hook_name);
    ColorChoiceName(pid, choice_name);
    return NamespaceMissing(hook_name) && NamespaceMissing(choice_name);
}

ColorConsent AskColorConsent(HANDLE target, DWORD pid) {
    wchar_t path[32768]{};
    DWORD length = 32768;
    if (!QueryFullProcessImageNameW(target, 0, path, &length)) return ColorConsent::Cancel;
    const auto executable = std::filesystem::path(path).filename().wstring();
    const auto text = localization::Format(
        L"目標：{}（PID {}）\n自動／嚴格不會猜測未知色彩空間。\n僅在確認遊戲使用 SDR 時，選擇本次以 SDR 解讀。這不是 HDR 支援；錯誤選擇可能造成亮度或色彩錯誤。\n選擇僅適用本次遊戲行程；DLL 載入後留到遊戲退出。",
        L"Target: {} (PID {})\nAutomatic / strict does not guess an unknown color space.\nChoose SDR interpretation only if this game is using SDR. This is not HDR support; a wrong choice may produce incorrect brightness or colors.\nThe choice applies only to this process session. Once loaded, the DLL remains until game exit.",
        executable, pid);
    const TASKDIALOG_BUTTON buttons[] = {
        {kConsentStrict,
         localization::Select(L"自動／嚴格（預設）", L"Automatic / strict (default)").data()},
        {kConsentSdr,
         localization::Select(L"本次以 SDR 解讀", L"Interpret this session as SDR").data()}};
    TASKDIALOGCONFIG config{};
    config.cbSize = sizeof(config);
    config.hwndParent = runtime.window;
    config.dwFlags = TDF_USE_COMMAND_LINKS | TDF_ALLOW_DIALOG_CANCELLATION;
    config.dwCommonButtons = TDCBF_CANCEL_BUTTON;
    config.pszWindowTitle = L"GPU Thermal Guard — Overlay";
    config.pszMainInstruction =
        localization::Select(L"選擇本次 Overlay 色彩策略", L"Choose overlay color policy").data();
    config.pszContent = text.c_str();
    config.cButtons = 2;
    config.pButtons = buttons;
    config.nDefaultButton = kConsentStrict;
    int button{};
    // TaskDialog requires comctl32 v6; an older activation context must fail
    // closed rather than introduce an unavailable ordinal at process startup.
    const auto controls = GetModuleHandleW(L"comctl32.dll");
    const auto task_dialog = controls ? reinterpret_cast<decltype(&TaskDialogIndirect)>(
                                            GetProcAddress(controls, "TaskDialogIndirect"))
                                      : nullptr;
    if (!task_dialog) return ColorConsent::Cancel;
    return SUCCEEDED(task_dialog(&config, &button, nullptr, nullptr)) ? DecodeConsent(button)
                                                                      : ColorConsent::Cancel;
}

// False when the program in front cannot take an overlay, or attaching
// was refused; the caller shows that (AF-20261008-overlay-o-button).
bool TryPress() {
    struct ChoiceGuard {
        ChoiceGuard() { runtime.choosing = true; }

        ~ChoiceGuard() { runtime.choosing = false; }
    } choice_guard;

    DWORD pid{};
    const HWND foreground = GetForegroundWindow();
    if (!foreground || foreground == GetDesktopWindow() || foreground == GetShellWindow())
        return false;
    GetWindowThreadProcessId(foreground, &pid);
    if (!pid || pid == GetCurrentProcessId()) return false;
    Handle target;
    target.value = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_INFORMATION, FALSE, pid);
    const Identity identity{pid, CreationTime(target.value)};
    if (!identity.created || WaitForSingleObject(target.value, 0) != WAIT_TIMEOUT) return false;
    // Failed here before: never retried while that process lives.
    if (runtime.refused.Contains(identity)) return false;
    for (auto& slot : runtime.sessions) {
        if (!slot || !(slot->policy.Target() == identity)) continue;
        const auto action = slot->policy.Press(true, identity);
        if (action == Action::Show || action == Action::Hide) {
            const bool visible = action == Action::Show;
            InterlockedExchange(&slot->control->enabled, visible);
            // Showing waits for the next validated publisher tick as well.
            ipc::SetEnabled(*slot->bitmap, false);
        }
        // Still installing is not a refusal; a failed session is.
        return action != Action::Refuse;
    }
    if (!TargetAdmitted(target.value)) {
        (void)logging::TryWarning(
            L"Overlay: foreground target failed read-only admission; no injection attempted");
        return false;
    }
    auto empty = std::find_if(runtime.sessions.begin(), runtime.sessions.end(),
                              [](const auto& s) { return !s; });
    if (empty == runtime.sessions.end()) return false;
    if (!ChoiceNamespacesClear(pid)) {
        (void)logging::TryWarning(
            L"Overlay: existing/uncertain hook or color-choice namespace; no new injection");
        return false;
    }
    const bool auto_sdr = settings::LoadOverlayAutoSdr();
    const auto choice =
        SelectColorConsent(auto_sdr, [&] { return AskColorConsent(target.value, pid); });
    if (!AcceptConsent(choice, identity, {pid, CreationTime(target.value)},
                       WaitForSingleObject(target.value, 0) == WAIT_TIMEOUT, runtime.enabled) ||
        !ChoiceNamespacesClear(pid) || !TargetAdmitted(target.value))
        return false;
    // The dialog pumps messages; find a free slot again after explicit consent.
    empty = std::find_if(runtime.sessions.begin(), runtime.sessions.end(),
                         [](const auto& s) { return !s; });
    if (empty == runtime.sessions.end()) return false;
    auto slot = std::make_unique<Session>();
    if (choice == ColorConsent::AssumeSdr) {
        slot->color_choice = std::make_unique<ColorChoiceMapping>();
        if (!slot->color_choice->Create(pid, identity.created)) {
            (void)logging::TryWarning(
                L"Overlay: session SDR choice publication refused; no injection");
            return false;
        }
    }
    slot->target.value = std::exchange(target.value, nullptr);
    (void)slot->policy.Press(true, identity);
    // Retain failed identities until process exit. A partial attach must never retry blindly.
    *empty = std::move(slot);
    try {
        Launch(**empty);
    } catch (...) {
        (*empty)->policy.Ready(false);
        throw;
    }
    (void)logging::TryInfo(
        L"Overlay feature: asynchronous attach started; DLL remains resident until game exit");
    (void)logging::TryInfo(
        auto_sdr
            ? L"Overlay color policy: saved Auto SDR preference (assumption, not observed color)"
        : choice == ColorConsent::AssumeSdr
            ? L"Overlay color policy: explicit session SDR interpretation (not observed color)"
            : L"Overlay color policy: automatic/strict");
    return true;
}

void Press() {
    if (!runtime.enabled || runtime.choosing) return;
    if (!TryPress() && runtime.source != nullptr) runtime.source->FlashOverlayRefusal();
}

bool ReadRequest(Session& s) noexcept {
    const auto serial = ipc::RequestSerial(*s.request);
    if (!serial) return false;
    const ipc::Request request = *s.request;
    if (!AcceptRequest(request, serial, ipc::RequestSerialBegun(*s.request), s.policy.Target().pid,
                       NowUs()))
        return false;
    s.width = request.swapchain_width;
    s.height = request.swapchain_height;
    return true;
}

void Publish(Session& s, std::uint64_t now) {
    timing::SlowUiScope phase(L"overlay.publish");
    ipc::Beat(*s.bitmap, NowUs());
    if (!ReadRequest(s)) {
        ipc::SetEnabled(*s.bitmap, false);
        return;
    }
    RECT rect{};
    MONITORINFO monitor{sizeof(monitor)};
    if (!GetWindowRect(runtime.window, &rect) ||
        !GetMonitorInfoW(MonitorFromWindow(runtime.window, MONITOR_DEFAULTTONEAREST), &monitor)) {
        ipc::SetEnabled(*s.bitmap, false);
        return;
    }
    const auto& m = monitor.rcMonitor;
    const auto geometry = geometry::Map({rect.left, rect.top, rect.right, rect.bottom},
                                        {m.left, m.top, m.right, m.bottom}, s.width, s.height);
    if (geometry.scale <= 0) {
        ipc::SetEnabled(*s.bitmap, false);
        return;
    }
    if (!s.last_render || now - s.last_render >= 500) {
        s.last_render = now;
        s.image_valid = false;
        float scale = settings::EffectiveOsdScale(runtime.source->ScalePercent(),
                                                  runtime.source->CurrentDpi()) *
                      static_cast<float>(geometry.scale);
        // First estimate from the desktop rectangle, then enforce the maximum
        // footprint across layouts before the rasterizer allocates anything.
        const double fit = std::min({1.0,
                                     static_cast<double>(ipc::kMaxWidth) /
                                         std::max(1.0, (rect.right - rect.left) * geometry.scale),
                                     static_cast<double>(ipc::kMaxHeight) /
                                         std::max(1.0, (rect.bottom - rect.top) * geometry.scale)});
        scale *= static_cast<float>(fit);
        scale = BoundRenderScale(scale, runtime.source->Collapsed());
        tray::OsdOverlay::Bitmap image;
        runtime.source->AdvanceTo(now);
        if (scale <= 0 ||
            !runtime.source->RenderToBitmap(scale, static_cast<int>(s.width / scale),
                                            static_cast<int>(s.height / scale), now,
                                            tray::OsdOverlay::AlphaMode::Straight, image) ||
            image.width <= 0 || image.height <= 0 || image.width > ipc::kMaxWidth ||
            image.height > ipc::kMaxHeight ||
            image.pixels.size() != static_cast<std::size_t>(image.width) * image.height * 4) {
            ipc::SetEnabled(*s.bitmap, false);
            return;
        }
        s.image_valid = true;
        const auto& old = s.bitmap->buffer[ipc::PublishedIndex(*s.bitmap)].header;
        if (!s.image_serial || old.frame_width != s.width || old.frame_height != s.height ||
            image.width != s.image.width || image.height != s.image.height ||
            image.pixels != s.image.pixels) {
            const auto index = 1U - ipc::PublishedIndex(*s.bitmap);
            auto& frame = s.bitmap->buffer[index];
            ipc::BeginFrame(frame, ++s.image_serial);
            frame.header.width = image.width;
            frame.header.height = image.height;
            frame.header.stride = image.width * 4;
            frame.header.byte_count = image.width * image.height * 4;
            frame.header.frame_width = s.width;
            frame.header.frame_height = s.height;
            frame.header.mode = runtime.source->Collapsed() ? 0 : 1;
            std::memcpy(frame.pixels, image.pixels.data(), frame.header.byte_count);
            ipc::FinishFrame(frame, s.image_serial);
            ipc::Publish(*s.bitmap, index);
            s.image = std::move(image);
        }
    }
    if (!s.image_serial || !s.image_valid) {
        ipc::SetEnabled(*s.bitmap, false);
        return;
    }
    const auto place = geometry::Place(geometry, s.image.width, s.image.height);
    const auto& old = s.bitmap->placement;
    if (old.left != place.left || old.top != place.top || old.width != place.Width() ||
        old.height != place.Height() || old.frame_width != s.width ||
        old.frame_height != s.height) {
        ipc::BeginPlacement(*s.bitmap, ++s.placement_serial);
        auto& p = s.bitmap->placement;
        p.left = place.left;
        p.top = place.top;
        p.width = place.Width();
        p.height = place.Height();
        p.frame_width = s.width;
        p.frame_height = s.height;
        p.written_us = NowUs();
        ipc::FinishPlacement(*s.bitmap, s.placement_serial);
    }
    ipc::SetEnabled(*s.bitmap, true);
    if (!s.first_bitmap_published) {
        s.first_bitmap_published = true;
        try {
            (void)logging::TryInfo(
                std::format(L"Overlay feature: first bitmap published; target={} elapsed_ms={}",
                            s.policy.Target().pid, GetTickCount64() - s.startup_started));
        } catch (...) {
            // Best-effort telemetry cannot disable an otherwise valid publisher.
        }
    }
}

// Whether the overlay stands in for the desktop OSD right now, and whether any
// game has a live overlay (the header's OVL badge). Runs on every tick; the
// process creation time is read only when the foreground pid matches a
// session, so the common case costs two user32 calls.
void UpdateEmbedding() noexcept {
    if (runtime.source == nullptr) return;
    std::array<EmbedSession, 4> sessions{};
    std::size_t count = 0;
    for (const auto& s : runtime.sessions) {
        if (!s) continue;
        const bool visible = runtime.enabled && s->policy.Current() == State::Visible;
        sessions[count++] = {s->policy.Target(), visible,
                             s->first_bitmap_published && s->image_valid};
    }
    Identity foreground{};
    DWORD pid = 0;
    if (const HWND window = GetForegroundWindow();
        window != nullptr && GetWindowThreadProcessId(window, &pid) != 0 && pid != 0) {
        foreground.pid = pid;
        for (std::size_t i = 0; i < count; ++i) {
            if (sessions[i].target.pid != pid) continue;
            if (const HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)) {
                foreground.created = CreationTime(process);
                CloseHandle(process);
            }
            break;
        }
    }
    const bool settled = runtime.settle.Settled(foreground.pid, GetTickCount64());
    const std::span<const EmbedSession> view{sessions.data(), count};
    const bool in_game = OverlayOwnsForeground(view, foreground);
    runtime.source->SetSuppressed(runtime.hides_osd && in_game && settled);
    // The O: what pressing it would do for the program in front.
    using Mark = tray::OsdOverlay::OverlayMark;
    bool front_live = false;
    for (const auto& s : view)
        front_live = front_live || (s.visible && s.target == foreground);
    bool front_installing = false;
    for (const auto& s : runtime.sessions)
        front_installing = front_installing || (s && s->policy.Current() == State::Installing &&
                                                s->policy.Target() == foreground);
    runtime.source->SetOverlayMark(!runtime.enabled   ? Mark::None
                                   : front_live       ? Mark::Live
                                   : front_installing ? Mark::Installing
                                                      : Mark::Attachable);
}

// A failed session gives its slot back. The identity is remembered so the same
// process is not tried again; the Session destructor only disables drawing
// and tells the helper to stop -- no waiting, no remote unload, no unhook.
void RetireRefused(std::unique_ptr<Session>& slot) noexcept {
    const auto target = slot->policy.Target();
    try {
        runtime.refused.Add(target);
    } catch (...) {
        // Not remembered: the pre-load Present byte check still refuses a retry.
    }
    slot.reset();
    try {
        (void)logging::TryInfo(std::format(
            L"Overlay session failed; slot released, retry refused until target {} exits",
            target.pid));
    } catch (...) {
    }
}

bool StillRunning(const Identity& target) noexcept {
    const HANDLE process =
        OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, target.pid);
    if (process == nullptr) return false;
    const bool alive =
        CreationTime(process) == target.created && WaitForSingleObject(process, 0) == WAIT_TIMEOUT;
    CloseHandle(process);
    return alive;
}

void Tick() {
    timing::SlowUiScope phase(L"overlay.timer");
    // About every two seconds, forget failed targets that have exited.
    if (++runtime.prune_ticks % 64 == 0) runtime.refused.Prune(StillRunning);
    for (auto& slot : runtime.sessions) {
        if (!slot) continue;
        if (WaitForSingleObject(slot->target.value, 0) != WAIT_TIMEOUT) {
            const auto retired_pid = slot->policy.Target().pid;
            const auto retired_at = GetTickCount64();
            {
                timing::SlowUiScope retire_phase(L"overlay.retire");
                slot.reset();
            }
            try {
                (void)logging::TryInfo(
                    std::format(L"Overlay session reclaimed: target={} elapsed_ms={}", retired_pid,
                                GetTickCount64() - retired_at));
            } catch (...) {
            }
            continue;
        }
        auto& s = *slot;
        if (s.policy.Current() == State::Failed) {
            RetireRefused(slot);
            continue;
        }
        if (!s.control) continue;
        const auto state =
            static_cast<HelperState>(InterlockedCompareExchange(&s.control->state, 0, 0));
        if (state == HelperState::Failed || state == HelperState::Closed ||
            (s.helper.value && WaitForSingleObject(s.helper.value, 0) != WAIT_TIMEOUT)) {
            ipc::SetEnabled(*s.bitmap, false);
            // Retain the identity and no retry; the DLL may already be resident.
            s.policy.Fail();
            if (state != HelperState::Closed && runtime.source != nullptr)
                runtime.source->FlashOverlayRefusal();
            (void)logging::TryWarning(
                L"Overlay feature: helper stopped/failed; drawing disabled, restart game before "
                L"retry");
            RetireRefused(slot);
            continue;
        }
        if (state == HelperState::Ready && s.policy.Current() == State::Installing) {
            s.policy.Ready(true);
            try {
                (void)logging::TryInfo(std::format(
                    L"Overlay feature: native graphics inspection passed; publisher active; target={} elapsed_ms={}",
                    s.policy.Target().pid, GetTickCount64() - s.startup_started));
            } catch (...) {
                // Logging allocation failure is not an overlay startup failure.
            }
        }
        if (runtime.enabled && s.policy.Current() == State::Visible) Publish(s, GetTickCount64());
    }
    // Before the timer can stop below: with no session left this restores the
    // OSD and clears the badge.
    UpdateEmbedding();
    if (!runtime.enabled && std::none_of(runtime.sessions.begin(), runtime.sessions.end(),
                                         [](const auto& s) { return s != nullptr; }))
        KillTimer(runtime.window, kTimer);
}

LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam, UINT_PTR,
                            DWORD_PTR) {
    try {
        if ((message == WM_HOTKEY && static_cast<int>(wparam) == runtime.hotkey) ||
            message == tray::OsdOverlay::kOverlayPressMessage) {
            Press();
            return 0;
        }
        if (message == WM_TIMER && wparam == kTimer) {
            Tick();
            return 0;
        }
    } catch (...) {
        for (auto& s : runtime.sessions)
            if (s && s->bitmap) {
                ipc::SetEnabled(*s->bitmap, false);
                s->image_valid = false;
            }
        (void)logging::TryWarning(L"Overlay feature operation failed; protection is independent");
        if (runtime.source != nullptr) runtime.source->FlashOverlayRefusal();
    }
    return DefSubclassProc(window, message, wparam, lparam);
}
}  // namespace

bool Configure(bool enabled, unsigned modifiers, unsigned key) noexcept {
    if (!runtime.window || (enabled && !ValidHotkey(modifiers, key))) return false;
    if (enabled && !SetTimer(runtime.window, kTimer, 34, nullptr)) return false;
    if (enabled && (!runtime.hotkey || runtime.modifiers != modifiers || runtime.key != key)) {
        const int next = runtime.hotkey == 0x4740 ? 0x4741 : 0x4740;
        if (!RegisterHotKey(runtime.window, next, modifiers | MOD_NOREPEAT, key)) {
            if (!runtime.enabled && std::none_of(runtime.sessions.begin(), runtime.sessions.end(),
                                                 [](const auto& s) { return s != nullptr; }))
                KillTimer(runtime.window, kTimer);
            return false;
        }
        if (runtime.hotkey) UnregisterHotKey(runtime.window, runtime.hotkey);
        runtime.hotkey = next;
        runtime.modifiers = modifiers;
        runtime.key = key;
    }
    if (!enabled) {
        if (runtime.hotkey) UnregisterHotKey(runtime.window, runtime.hotkey);
        runtime.hotkey = 0;
        for (auto& s : runtime.sessions)
            if (s) {
                if (s->bitmap) ipc::SetEnabled(*s->bitmap, false);
                if (s->control) InterlockedExchange(&s->control->enabled, 0);
                s->policy.Disable();
            }
    }
    runtime.enabled = enabled;
    if (!enabled && std::none_of(runtime.sessions.begin(), runtime.sessions.end(),
                                 [](const auto& s) { return s != nullptr; }))
        KillTimer(runtime.window, kTimer);
    return true;
}

void SetHidesOsd(const bool hides) noexcept {
    runtime.hides_osd = hides;
    // Off takes effect at once; on waits for the next tick's settle time.
    if (!hides && runtime.source != nullptr) runtime.source->SetSuppressed(false);
}

Status GetStatus() noexcept {
    Status status{runtime.enabled, runtime.modifiers, runtime.key};
    status.failed = static_cast<unsigned>(runtime.refused.Size());
    for (const auto& s : runtime.sessions)
        if (s) {
            switch (s->policy.Current()) {
                case State::Installing:
                    ++status.installing;
                    break;
                case State::Visible:
                    ++status.visible;
                    break;
                case State::Hidden:
                    ++status.hidden;
                    break;
                case State::Failed:
                    ++status.failed;
                    break;
                default:
                    break;
            }
        }
    return status;
}

void Start(tray::OsdOverlay& source) noexcept {
    if (runtime.source || !source.m_hWnd) return;
    runtime.source = &source;
    runtime.window = source.m_hWnd;
    if (!SetWindowSubclass(runtime.window, WindowProc, kSubclass, 0)) {
        Stop();
        return;
    }
    // MainDialog applies the shared UI preferences after OSD initialization.
}

void Stop() noexcept {
    if (!runtime.window) return;
    (void)Configure(false);
    KillTimer(runtime.window, kTimer);
    RemoveWindowSubclass(runtime.window, WindowProc, kSubclass);
    for (auto& slot : runtime.sessions)
        slot.reset();
    if (runtime.source != nullptr) {
        runtime.source->SetSuppressed(false);
        runtime.source->SetOverlayMark(tray::OsdOverlay::OverlayMark::None);
    }
    runtime.source = nullptr;
    runtime.window = nullptr;
}
}  // namespace gtg::overlay::integration
