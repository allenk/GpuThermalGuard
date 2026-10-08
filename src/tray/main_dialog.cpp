#include "tray/main_dialog.hpp"
#include "gtg_version.h"
#include "tray/window_position.hpp"
#include "tray/control_baseline.hpp"
#include "timing/ui_phase_timing.hpp"
#include "supervision/supervisor.hpp"

#include <algorithm>
#include <format>
#include <chrono>

#include <shellapi.h>
#include <uxtheme.h>

#include "settings/settings.hpp"
#include "ipc/client.hpp"
#include "logging/logger.hpp"
#include "sysmem/reclaim_nt.hpp"
#include "localization/localization.hpp"
#include "snapshot/ui_snapshot.hpp"
#include "tray/ui_theme.hpp"
#include "tray/hotkey_capture.hpp"
#ifdef GTG_FEATURE_OVERLAY
#include "../../overlay/integration/runtime.hpp"
#endif

#include <commctrl.h>
#include <vector>

namespace gtg::tray {

// Checked here because the class has to be complete to read its own table.
static_assert(MainDialog::MessagesAreDistinct(),
              "two window messages share a value; ATL's map delivers both to "
              "whichever handler is registered first, and the loser never runs");
namespace {

void AlignSettingsEdits(HWND dialog) noexcept {
    for (const auto [label, edit] : {
             std::pair{IDC_NORMAL_POWER_LABEL, IDC_NORMAL_POWER},
             std::pair{IDC_SAFE_POWER_LABEL, IDC_SAFE_POWER},
             std::pair{IDC_TRIGGER_TEMP_LABEL, IDC_TRIGGER_TEMP}}) {
        (void)AlignEditToLabel(dialog, GetDlgItem(dialog, label), GetDlgItem(dialog, edit));
    }
}

constexpr double kBytesPerGiB = 1024.0 * 1024.0 * 1024.0;

// Stable identity lets Explorer distinguish this icon across restarts and
// prevents collisions with other applications that happen to use uID == 1.
constexpr GUID kTrayIconGuid{
    0x7b0ec2a1, 0x73c9, 0x4cf4, {0xa5, 0x8f, 0x48, 0x52, 0x52, 0x7a, 0x6b, 0x91}};

std::uint64_t CurrentFileTimeValue() noexcept {
    FILETIME file_time{};
    GetSystemTimePreciseAsFileTime(&file_time);
    ULARGE_INTEGER value{};
    value.LowPart = file_time.dwLowDateTime;
    value.HighPart = file_time.dwHighDateTime;
    return value.QuadPart;
}

std::wstring FormatCompactLocalTime(const std::uint64_t file_time_value) {
    if (file_time_value == 0) return {};
    ULARGE_INTEGER value{};
    value.QuadPart = file_time_value;
    FILETIME utc{value.LowPart, value.HighPart};
    FILETIME local{};
    SYSTEMTIME system_time{};
    if (FileTimeToLocalFileTime(&utc, &local) == FALSE ||
        FileTimeToSystemTime(&local, &system_time) == FALSE) {
        return {};
    }
    return std::format(L"{:02}-{:02} {:02}:{:02}",
                       system_time.wMonth, system_time.wDay,
                       system_time.wHour, system_time.wMinute);
}

std::wstring Utf8ToWide(const std::string& text) {
    if (text.empty()) return {};
    const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                                         static_cast<int>(text.size()), nullptr, 0);
    if (size <= 0) return L"<invalid UTF-8>";
    std::wstring result(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                        static_cast<int>(text.size()), result.data(), size);
    return result;
}

HICON LoadSmallIcon(HWND window, UINT resource_id) {
    const int size = MulDiv(16, static_cast<int>(GetDpiForWindow(window)), 96);
    return static_cast<HICON>(LoadImageW(ATL::_AtlBaseModule.GetResourceInstance(),
        MAKEINTRESOURCEW(resource_id), IMAGE_ICON, size, size, LR_DEFAULTCOLOR));
}

HICON LoadStatusIcon(HWND control, UINT resource_id) {
    RECT bounds{};
    ::GetClientRect(control, &bounds);
    int size = std::min(bounds.right - bounds.left, bounds.bottom - bounds.top);
    if (size <= 0) {
        size = MulDiv(48, static_cast<int>(GetDpiForWindow(control)), 96);
    }
    // The control is sized in dialog units, so its pixel size follows the DPI
    // and rarely equals a frame. LoadIconWithScaleDown always starts from a
    // larger frame and filters down; LoadImage would stretch the nearest one,
    // which with 48 px as the largest frame meant upscaling and a soft icon.
    // The status icons now carry frames up to 128 px
    // (tools/icons/build_status_icons.py).
    HICON icon = nullptr;
    if (SUCCEEDED(LoadIconWithScaleDown(ATL::_AtlBaseModule.GetResourceInstance(),
                                        MAKEINTRESOURCEW(resource_id), size, size, &icon))) {
        return icon;
    }
    return static_cast<HICON>(LoadImageW(ATL::_AtlBaseModule.GetResourceInstance(),
        MAKEINTRESOURCEW(resource_id), IMAGE_ICON, size, size, LR_DEFAULTCOLOR));
}

HICON LoadDpiIcon(HWND control, const UINT resource_id, const int size_dip) {
    const int size = MulDiv(size_dip, static_cast<int>(GetDpiForWindow(control)), 96);
    return static_cast<HICON>(LoadImageW(ATL::_AtlBaseModule.GetResourceInstance(),
        MAKEINTRESOURCEW(resource_id), IMAGE_ICON, size, size, LR_DEFAULTCOLOR));
}

// Both shapes, each under its own name: OsdX/OsdY for expanded, CompactX/
// CompactY for collapsed (AF-20261004-main-ui-redesign).
bool SaveOsdWindowPosition(const OsdOverlay& overlay,
                           const std::wstring_view reason) noexcept {
    if (!ReadWindowPosition(overlay.m_hWnd)) return false;
    const auto positions = overlay.Positions();
    std::wstring error;
    if (positions.expanded &&
        !settings::SaveOsdPosition(positions.expanded->x, positions.expanded->y, error)) {
        logging::Warning(error);
        return false;
    }
    if (positions.collapsed &&
        !settings::SaveCompactPosition(positions.collapsed->x, positions.collapsed->y, error)) {
        logging::Warning(error);
        return false;
    }
    const auto text = [](const std::optional<POINT>& point) {
        return point ? std::format(L"({}, {})", point->x, point->y) : std::wstring(L"none");
    };
    logging::Info(std::format(
        L"OSD placement saved; expanded={} compact={} collapsed={} reason={}",
        text(positions.expanded), text(positions.collapsed), overlay.Collapsed(), reason));
    return true;
}

bool RestoreOsdWindowPosition(HWND notification_window,
                              const telemetry::History* history,
                              OsdOverlay& overlay,
                              settings::OsdPreference& preference) {
    preference = settings::LoadOsdPreference();
    // Before Initialize: the overlay sizes itself on creation, so restoring
    // the shape afterwards would show the reader an expanded window that
    // immediately collapses.
    overlay.RestoreCollapsed(preference.collapsed);
    overlay.SetScalePercent(preference.scale_percent);
    const auto positions = placement::FromStored(
        preference.has_position ? std::optional<POINT>{POINT{preference.x, preference.y}}
                                : std::nullopt,
        preference.has_compact_position
            ? std::optional<POINT>{POINT{preference.compact_x, preference.compact_y}}
            : std::nullopt);
    overlay.RestorePositions(positions);
    const std::optional<POINT> saved = positions.For(preference.collapsed);
    bool placement_repaired = false;
    const bool ready = overlay.Initialize(
        notification_window, history, saved.has_value(),
        saved.value_or(POINT{}), placement_repaired);
    if (!ready) {
        logging::Warning(L"OSD placement restore failed because overlay initialization failed");
        return false;
    }

    const POINT applied = overlay.Position();
    if (saved) {
        logging::Info(std::format(
            L"OSD placement restored; collapsed={} saved=({}, {}) applied=({}, {}) repaired={}",
            preference.collapsed, saved->x, saved->y, applied.x, applied.y,
            placement_repaired));
    } else {
        logging::Info(std::format(
            L"OSD placement initialized from default; applied=({}, {})",
            applied.x, applied.y));
    }

    return true;
}

// One EXE serves both downloads (AF-20261006-overlay-default-on): the
// overlay exists only when gtg_overlay.dll is beside the executable. Asked
// once; adding the DLL takes a restart.
bool OverlayDllPresent() noexcept {
#ifdef GTG_FEATURE_OVERLAY
    static const bool present = [] {
        wchar_t path[MAX_PATH * 2]{};
        const DWORD length = GetModuleFileNameW(nullptr, path, static_cast<DWORD>(std::size(path)));
        if (length == 0 || length >= std::size(path)) return false;
        std::wstring dll(path, length);
        const auto slash = dll.find_last_of(L"\\/");
        if (slash == std::wstring::npos) return false;
        dll.resize(slash + 1);
        dll += L"gtg_overlay.dll";
        const DWORD attributes = GetFileAttributesW(dll.c_str());
        return attributes != INVALID_FILE_ATTRIBUTES &&
               (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
    }();
    return present;
#else
    return false;
#endif
}

// Without the overlay its controls are not shown at all: a checkbox that
// cannot do anything is worse than none.
void HideOverlayControls(const HWND dialog) noexcept {
    for (const int id : {IDC_OVERLAY_ENABLED, IDC_OVERLAY_AUTO_SDR, IDC_OVERLAY_HOTKEY,
                         IDC_OVERLAY_HIDES_OSD})
        if (const HWND control = ::GetDlgItem(dialog, id)) ::ShowWindow(control, SW_HIDE);
}

// The program a play-time count belongs to: its executable, so a game that
// is restarted carries on. FNV-1a over the lower-cased path; 0 when the path
// cannot be read, which counts nothing rather than guessing. Cached for the
// one target it was last asked about -- the target changes rarely and the
// question is asked twice a second.
std::optional<std::uint64_t> ProgramKey(const fps::Identity& identity) {
    static fps::Identity cached_identity{};
    static std::uint64_t cached_key{};
    if (identity == cached_identity) {
        return cached_key != 0 ? std::optional<std::uint64_t>{cached_key} : std::nullopt;
    }
    cached_identity = identity;
    cached_key = 0;
    if (HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, identity.pid)) {
        wchar_t path[MAX_PATH * 2]{};
        DWORD length = static_cast<DWORD>(std::size(path));
        if (QueryFullProcessImageNameW(process, 0, path, &length) != FALSE && length != 0) {
            std::uint64_t hash = 14695981039346656037ULL;
            for (DWORD k = 0; k < length; ++k) {
                hash ^= static_cast<std::uint64_t>(towlower(path[k]));
                hash *= 1099511628211ULL;
            }
            cached_key = hash == 0 ? 1 : hash;
        }
        CloseHandle(process);
    }
    return cached_key != 0 ? std::optional<std::uint64_t>{cached_key} : std::nullopt;
}

// A key's name as the keyboard layout spells it: "F10", "O", "Page Up".
std::wstring KeyName(const UINT vk) {
    const UINT scan = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC_EX);
    LONG lparam = static_cast<LONG>((scan & 0xFFU) << 16);
    // The navigation keys share scan codes with the numeric keypad; without
    // the extended bit "Page Up" is named "Num 9".
    switch (vk) {
    case VK_PRIOR: case VK_NEXT: case VK_END: case VK_HOME: case VK_LEFT: case VK_UP:
    case VK_RIGHT: case VK_DOWN: case VK_INSERT: case VK_DELETE: case VK_DIVIDE:
        lparam |= 1L << 24;
        break;
    default:
        if ((scan & 0xFF00U) == 0xE000U) lparam |= 1L << 24;
        break;
    }
    wchar_t name[64]{};
    if (scan != 0 && GetKeyNameTextW(lparam, name, static_cast<int>(std::size(name))) > 0)
        return name;
    return std::format(L"VK {:#04x}", vk);
}

// The keys of a combination in the order PowerToys shows them. Modifier names
// are fixed English words, as on the keycaps, in either UI language.
std::vector<std::wstring> HotkeyParts(const settings::OverlayHotkey& hotkey) {
    std::vector<std::wstring> parts;
    if ((hotkey.modifiers & MOD_WIN) != 0) parts.emplace_back(L"Win");
    if ((hotkey.modifiers & MOD_CONTROL) != 0) parts.emplace_back(L"Ctrl");
    if ((hotkey.modifiers & MOD_ALT) != 0) parts.emplace_back(L"Alt");
    if ((hotkey.modifiers & MOD_SHIFT) != 0) parts.emplace_back(L"Shift");
    if (hotkey.key != 0) parts.push_back(KeyName(hotkey.key));
    return parts;
}

std::wstring HotkeyText(const settings::OverlayHotkey& hotkey) {
    std::wstring text;
    for (const auto& part : HotkeyParts(hotkey)) {
        if (!text.empty()) text += L"+";
        text += part;
    }
    return text;
}

// Whether Windows or another program already holds the combination, asked
// the way PowerToys asks: register it for a moment and let it go.
bool HotkeyTaken(const settings::OverlayHotkey& hotkey) noexcept {
#ifdef GTG_FEATURE_OVERLAY
    const auto current = overlay::integration::GetStatus();
    if (current.enabled && current.modifiers == hotkey.modifiers && current.key == hotkey.key)
        return false;
#endif
    constexpr int kProbeId = 0x0FFF;
    if (RegisterHotKey(nullptr, kProbeId, hotkey.modifiers | MOD_NOREPEAT, hotkey.key) != FALSE) {
        UnregisterHotKey(nullptr, kProbeId);
        return false;
    }
    return GetLastError() == ERROR_HOTKEY_ALREADY_REGISTERED;
}

// Choosing the in-game overlay's hotkey, after PowerToys' shortcut dialog.
//
// Keys are read by a low-level keyboard hook that exists only while this
// dialog is open and acts only while it is the foreground window. It swallows
// what it records, so Alt does not open a menu, F10 does not activate one and
// Win does not open Start. The system hotkey control this replaces drew its
// own empty text in the Windows display language ("無" under an English UI)
// and flickered under the dark theme.
//
// The overlay itself is not in this build (it lands with
// feature/overlay-remote-load), so the choice is stored and nothing is
// registered; the note says so.
class OverlayHotkeyDialog final : public ATL::CDialogImpl<OverlayHotkeyDialog> {
public:
    enum { IDD = IDD_OVERLAY_HOTKEY };

    explicit OverlayHotkeyDialog(const settings::OverlayHotkey hotkey) : hotkey_(hotkey) {}
    [[nodiscard]] settings::OverlayHotkey hotkey() const noexcept { return hotkey_; }

    BEGIN_MSG_MAP(OverlayHotkeyDialog)
        MESSAGE_HANDLER(WM_INITDIALOG, OnInitDialog)
        MESSAGE_HANDLER(WM_DESTROY, OnDestroy)
        MESSAGE_HANDLER(WM_ACTIVATE, OnActivate)
        MESSAGE_HANDLER(WM_DRAWITEM, OnDrawItem)
        COMMAND_ID_HANDLER(IDOK, OnOk)
        COMMAND_ID_HANDLER(IDCANCEL, OnCancel)
        COMMAND_ID_HANDLER(IDC_HOTKEY_DEFAULT, OnDefault)
    END_MSG_MAP()

private:
    LRESULT OnInitDialog(UINT, WPARAM, LPARAM, BOOL&) {
        SetWindowTextW(localization::Select(L"Overlay 熱鍵", L"Overlay Hotkey").data());
        SetDlgItemTextW(IDC_HOTKEY_PROMPT,
            localization::Select(L"按下要顯示遊戲內 Overlay 的組合鍵。",
                                 L"Press the key combination that shows the in-game overlay.")
                .data());
        SetDlgItemTextW(IDC_HOTKEY_DEFAULT, localization::Select(L"預設", L"Default").data());
        SetDlgItemTextW(IDOK, localization::Select(L"保存", L"Save").data());
        SetDlgItemTextW(IDCANCEL, localization::Select(L"取消", L"Cancel").data());
        capture_.Show(hotkey_);
        theme::AttachDialog(m_hWnd);
        theme::Repaint(m_hWnd);
        active_ = this;
        hook_ = SetWindowsHookExW(WH_KEYBOARD_LL, KeyboardHook, GetModuleHandleW(nullptr), 0);
        if (hook_ == nullptr) {
            hook_error_ = GetLastError();
            logging::Warning(std::format(L"hotkey capture unavailable; SetWindowsHookEx error={}",
                                         hook_error_));
        }
        Refresh();
        return TRUE;
    }

    LRESULT OnDestroy(UINT, WPARAM, LPARAM, BOOL& handled) {
        if (hook_ != nullptr) UnhookWindowsHookEx(hook_);
        hook_ = nullptr;
        active_ = nullptr;
        handled = FALSE;
        return 0;
    }

    LRESULT OnActivate(UINT, const WPARAM state, LPARAM, BOOL& handled) {
        // Keys released while another window had the keyboard never reach the
        // hook's "while foreground" path, so nothing may stay held.
        if (LOWORD(state) == WA_INACTIVE) {
            capture_.ReleaseModifiers();
            Refresh();
        }
        handled = FALSE;
        return 0;
    }

    static LRESULT CALLBACK KeyboardHook(const int code, const WPARAM message,
                                         const LPARAM data) {
        OverlayHotkeyDialog* dialog = active_;
        if (code == HC_ACTION && dialog != nullptr && dialog->m_hWnd != nullptr &&
            GetForegroundWindow() == dialog->m_hWnd) {
            const auto* key = reinterpret_cast<const KBDLLHOOKSTRUCT*>(data);
            const bool injected = (key->flags & LLKHF_INJECTED) != 0;
            if (!injected && !hotkey::PassesThrough(key->vkCode, dialog->capture_.Held())) {
                const bool down = message == WM_KEYDOWN || message == WM_SYSKEYDOWN;
                if (down) dialog->capture_.KeyDown(key->vkCode);
                else dialog->capture_.KeyUp(key->vkCode);
                dialog->Refresh();
                return 1;   // recorded, so not also delivered
            }
        }
        return CallNextHookEx(nullptr, code, message, data);
    }

    // The note, the Save button and the keycaps, from the capture's state.
    void Refresh() {
        const settings::OverlayHotkey shown = capture_.Shown();
        verdict_ = capture_.Judge();
        taken_ = false;
        if (verdict_ == hotkey::Verdict::Acceptable) {
            // Probed only when the combination changes, not on every key-up.
            if (!(probed_ == shown)) {
                probed_ = shown;
                probed_taken_ = HotkeyTaken(shown);
            }
            taken_ = probed_taken_;
        }
        const auto text = [](const wchar_t* zh, const wchar_t* en) {
            return std::wstring(localization::Select(zh, en));
        };
        std::wstring note;
        if (hook_ == nullptr) {
            note = localization::Format(L"無法讀取按鍵（Win32 {}）。",
                                        L"Keys cannot be read (Win32 {}).", hook_error_);
        } else if (taken_) {
            note = text(L"這個組合已被 Windows 或其他程式使用。",
                        L"Windows or another program already uses this combination.");
        } else {
            switch (verdict_) {
            case hotkey::Verdict::Empty:
                note = text(L"按下組合鍵，例如 Alt+F10。",
                            L"Press a combination, for example Alt+F10.");
                break;
            case hotkey::Verdict::Incomplete:
                note = text(L"再按一個鍵。", L"Now press a key.");
                break;
            case hotkey::Verdict::NeedsModifier:
                note = text(L"需要 Ctrl、Alt、Shift 或 Win。",
                            L"Needs Ctrl, Alt, Shift or Win.");
                break;
            case hotkey::Verdict::Reserved:
                note = text(L"F12 由 Windows 保留給偵錯工具，不能使用。",
                            L"Windows reserves F12 for debuggers; it cannot be used.");
                break;
            case hotkey::Verdict::Acceptable:
#ifdef GTG_FEATURE_OVERLAY
                note = text(L"啟用 Overlay 後，熱鍵會對目前前景程式啟用或隱藏 OSD。DLL 留存至遊戲退出。",
                            L"When enabled, the hotkey attaches or toggles the foreground app's OSD. "
                            L"The DLL remains loaded until the game exits.");
#else
                note = text(
                    L"遊戲內 Overlay 尚未包含在這個版本；設定會先保存，在它加入之前不會註冊任何熱鍵。",
                    L"The in-game overlay is not in this build yet. The choice is saved; no "
                    L"hotkey is registered until it is.");
#endif
                break;
            }
        }
        SetDlgItemTextW(IDC_HOTKEY_NOTE, note.c_str());
        ::EnableWindow(GetDlgItem(IDOK), verdict_ == hotkey::Verdict::Acceptable && !taken_);
        // No erase: the owner-drawn preview paints every pixel itself.
        ::InvalidateRect(GetDlgItem(IDC_HOTKEY_EDIT), nullptr, FALSE);
    }

    // The combination as keycaps, drawn off-screen and copied once so the
    // control never shows a half-painted frame.
    LRESULT OnDrawItem(UINT, WPARAM, const LPARAM data, BOOL& handled) {
        const auto* item = reinterpret_cast<const DRAWITEMSTRUCT*>(data);
        if (item == nullptr || item->CtlID != IDC_HOTKEY_EDIT) {
            handled = FALSE;
            return 0;
        }
        const RECT bounds = item->rcItem;
        const int width = bounds.right - bounds.left;
        const int height = bounds.bottom - bounds.top;
        HDC memory = CreateCompatibleDC(item->hDC);
        HBITMAP bitmap = CreateCompatibleBitmap(item->hDC, width, height);
        const HGDIOBJ previous_bitmap = SelectObject(memory, bitmap);
        const HGDIOBJ previous_font = SelectObject(memory, GetFont());
        RECT local{0, 0, width, height};
        HBRUSH background = CreateSolidBrush(theme::DialogBackground());
        FillRect(memory, &local, background);
        DeleteObject(background);

        const bool rejected = taken_ || verdict_ == hotkey::Verdict::NeedsModifier ||
                              verdict_ == hotkey::Verdict::Reserved;
        const bool complete = capture_.Complete();
        const COLORREF accent = theme::Accent(RGB(37, 103, 184));
        const COLORREF fill = rejected ? RGB(196, 43, 28) : accent;
        const COLORREF ink = rejected || !theme::IsDark() ? RGB(255, 255, 255) : RGB(16, 24, 40);
        const int dpi = static_cast<int>(GetDpiForWindow(m_hWnd));
        const int pad = MulDiv(10, dpi, 96);
        const int gap = MulDiv(6, dpi, 96);
        const int radius = MulDiv(8, dpi, 96);
        SetBkMode(memory, TRANSPARENT);
        int x = 0;
        for (const auto& part : HotkeyParts(capture_.Shown())) {
            SIZE extent{};
            GetTextExtentPoint32W(memory, part.c_str(), static_cast<int>(part.size()), &extent);
            RECT cap{x, 0, x + extent.cx + pad * 2, height};
            if (cap.right > width) break;
            HBRUSH brush = CreateSolidBrush(fill);
            // A modifier still waiting for its key is drawn as an outline.
            HPEN pen = CreatePen(PS_SOLID, MulDiv(1, dpi, 96), fill);
            const HGDIOBJ previous_brush = SelectObject(
                memory, complete ? static_cast<HGDIOBJ>(brush) : GetStockObject(NULL_BRUSH));
            const HGDIOBJ previous_pen = SelectObject(memory, pen);
            RoundRect(memory, cap.left, cap.top, cap.right, cap.bottom, radius, radius);
            SelectObject(memory, previous_pen);
            SelectObject(memory, previous_brush);
            DeleteObject(pen);
            DeleteObject(brush);
            SetTextColor(memory, complete ? ink : theme::Text());
            DrawTextW(memory, part.c_str(), static_cast<int>(part.size()), &cap,
                      DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            x = cap.right + gap;
        }
        BitBlt(item->hDC, bounds.left, bounds.top, width, height, memory, 0, 0, SRCCOPY);
        SelectObject(memory, previous_font);
        SelectObject(memory, previous_bitmap);
        DeleteObject(bitmap);
        DeleteDC(memory);
        return TRUE;
    }

    LRESULT OnOk(WORD, WORD, HWND, BOOL&) {
        if (verdict_ != hotkey::Verdict::Acceptable || taken_) {
            MessageBeep(MB_ICONWARNING);
            return 0;
        }
        hotkey_ = capture_.Shown();
        EndDialog(IDOK);
        return 0;
    }

    LRESULT OnCancel(WORD, WORD, HWND, BOOL&) {
        EndDialog(IDCANCEL);
        return 0;
    }

    LRESULT OnDefault(WORD, WORD, HWND, BOOL&) {
        capture_.Show(settings::kDefaultOverlayHotkey);
        Refresh();
        return 0;
    }

    // One modal dialog at a time; the hook has no other way to find it.
    inline static OverlayHotkeyDialog* active_{nullptr};
    settings::OverlayHotkey hotkey_;
    hotkey::Capture capture_;
    hotkey::Verdict verdict_{hotkey::Verdict::Empty};
    settings::OverlayHotkey probed_{};
    bool probed_taken_{};
    bool taken_{};
    HHOOK hook_{nullptr};
    DWORD hook_error_{};
};

}  // namespace

LRESULT MainDialog::OnInitDialog(UINT, WPARAM, LPARAM, BOOL&) {
    localization::SetCurrent(settings::LoadUiLanguagePreference());
    ui_theme_ = settings::LoadUiThemePreference();
    overlay_hotkey_ = settings::LoadOverlayHotkey();
    // Configured before any control paints, so a dark window never flashes
    // light first.
    (void)theme::Configure(ui_theme_);
    (void)EnableThemeDialogTexture(m_hWnd, ETDT_ENABLE);
    SetIcon(static_cast<HICON>(LoadImageW(ATL::_AtlBaseModule.GetResourceInstance(),
        MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON, 32, 32, LR_DEFAULTCOLOR)), TRUE);
    SetIcon(static_cast<HICON>(LoadImageW(ATL::_AtlBaseModule.GetResourceInstance(),
        MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON, 16, 16, LR_DEFAULTCOLOR)), FALSE);
    ApplyLocalization();
    SetStatus(localization::Select(L"初始化中…", L"Initializing…"), StatusVisual::Neutral);
    UpdateWallTime();
    ReloadSnapshotIcon();
    RestoreMainWindowPosition();
    const auto loaded_settings = settings::Load();
    config_ = loaded_settings.config;
    first_run_ = !loaded_settings.loaded;
    if (first_run_) {
        // Start with no headroom, before the card has even been read. The
        // built-in constants cannot be right on an unknown card -- 350 W is
        // unwritable on a 320 W one -- and a guard that caps someone's GPU
        // under a configuration they never chose is the thing this tool exists
        // to avoid. Equal limits make that impossible by construction: there
        // is nothing to give up, so nothing can be written, until the reader
        // asks for it.
        config_.safe_power_w = config_.normal_power_w;
    }
    trigger_count_ = settings::LoadTriggerCount();
    const settings::TriggerTestRun test_run = settings::LoadTriggerTestRun();
    if (test_run.loaded) {
        test_run_baseline_ = test_run.baseline;
        test_run_started_file_time_ = test_run.started_file_time;
    }
    SetDlgItemInt(IDC_NORMAL_POWER, static_cast<UINT>(config_.normal_power_w), FALSE);
    SetDlgItemInt(IDC_SAFE_POWER, static_cast<UINT>(config_.safe_power_w), FALSE);
    SetDlgItemInt(IDC_TRIGGER_TEMP, static_cast<UINT>(config_.trigger_temperature_c), FALSE);
    CheckDlgButton(IDC_AUTO_RESTORE, config_.auto_restore ? BST_CHECKED : BST_UNCHECKED);
    SetSettingsDirty(false);
    ApplyLocalization();
    CheckDlgButton(IDC_CLOSE_TO_TRAY,
                   settings::LoadCloseToTrayPreference() ? BST_CHECKED : BST_UNCHECKED);
    history_chart_.SubclassWindow(GetDlgItem(IDC_HISTORY));
    history_chart_.SetHistory(&telemetry_history_);
    // After every control exists and after the chart's own subclass, as in the
    // theme lab. The reading subclass goes on last so it runs first.
    theme::AttachDialog(m_hWnd);
    ::SetWindowSubclass(m_hWnd, ReadingColorSubclass, 1, reinterpret_cast<DWORD_PTR>(this));
    CheckDlgButton(IDC_OVERLAY_ENABLED,
                   settings::LoadOverlayEnabled() ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(IDC_OVERLAY_AUTO_SDR,
                   settings::LoadOverlayAutoSdr() ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(IDC_OVERLAY_HIDES_OSD,
                   settings::LoadOverlayHidesOsd() ? BST_CHECKED : BST_UNCHECKED);
    tooltip_ = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr,
                               WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX, CW_USEDEFAULT,
                               CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, m_hWnd, nullptr,
                               ATL::_AtlBaseModule.GetModuleInstance(), nullptr);
    if (tooltip_ != nullptr) {
        SendMessageW(tooltip_, TTM_SETMAXTIPWIDTH, 0, 360);
        for (const int id : {IDC_OVERLAY_ENABLED, IDC_OVERLAY_HOTKEY, IDC_OVERLAY_AUTO_SDR,
                             IDC_OVERLAY_HIDES_OSD}) {
            TTTOOLINFOW tool{sizeof(tool)};
            tool.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
            tool.hwnd = m_hWnd;
            tool.uId = reinterpret_cast<UINT_PTR>(GetDlgItem(id).m_hWnd);
            tool.lpszText = LPSTR_TEXTCALLBACKW;
            SendMessageW(tooltip_, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&tool));
        }
    }
    ApplyUiTheme();
    // Everything that decides the dashboard's width is in place before the
    // saved position is applied. Restored with the default single row first,
    // a saved single column docked on a right edge was clamped left to fit a
    // row eight cells wide, and narrowing to the column afterwards kept the
    // clamped X: the compact window reopened with Y right and X wrong.
    // Before Initialize these setters only record state.
    saved_compact_layout_ = compact::PackLayout(
        compact::SanitizeLayout(settings::LoadCompactLayout()));
    osd_overlay_.SetFpsEnabled(settings::LoadFpsEnabled());
    osd_overlay_.SetNetEnabled(settings::LoadNetEnabled());
    osd_overlay_.SetRamEnabled(settings::LoadRamEnabled());
    osd_overlay_.SetCompactLayout(compact::SanitizeLayout(saved_compact_layout_));
    settings::OsdPreference osd_preference;
    osd_ready_ = RestoreOsdWindowPosition(
        m_hWnd, &telemetry_history_, osd_overlay_, osd_preference);
    RefreshOsdScaleChoices();
    CheckDlgButton(IDC_OSD_ENABLED,
                   osd_ready_ && osd_preference.enabled ? BST_CHECKED : BST_UNCHECKED);
    show_fps_ = settings::LoadFpsEnabled();
    CheckDlgButton(IDC_SHOW_FPS, show_fps_ ? BST_CHECKED : BST_UNCHECKED);
    history_chart_.SetFpsHistory(show_fps_ ? &fps_history_ : nullptr);
    if (osd_ready_) osd_overlay_.SetFpsHistory(show_fps_ ? &fps_history_ : nullptr);
    if (osd_ready_) osd_overlay_.SetFpsEnabled(show_fps_);
    show_net_ = settings::LoadNetEnabled();
    CheckDlgButton(IDC_SHOW_NET, show_net_ ? BST_CHECKED : BST_UNCHECKED);
    if (show_net_ && !net_sampler_.Open()) {
        // Not an error the reader needs a dialog for: a machine with no route
        // out, or a Windows without the export. The cell says so by showing
        // nothing, and the log says which.
        logging::Info(std::format(L"network sampling unavailable; reason={}",
                                  static_cast<int>(net_sampler_.availability())));
    }
    history_chart_.SetNetHistory(show_net_ ? &net_history_ : nullptr);
    if (osd_ready_) osd_overlay_.SetNetHistory(show_net_ ? &net_history_ : nullptr);
    if (osd_ready_) osd_overlay_.SetNetEnabled(show_net_);
    show_ram_ = settings::LoadRamEnabled();
    CheckDlgButton(IDC_SHOW_RAM, show_ram_ ? BST_CHECKED : BST_UNCHECKED);
    history_chart_.SetRamHistory(show_ram_ ? &ram_history_ : nullptr);
    if (osd_ready_) osd_overlay_.SetRamHistory(show_ram_ ? &ram_history_ : nullptr);
    if (osd_ready_) osd_overlay_.SetRamEnabled(show_ram_);
    // The stored arrangement is validated before use: a value that is not a
    // permutation of every record is discarded whole for the default, so no
    // registry content can hide a record. Loaded above, before the OSD was
    // placed.
    saved_compact_locked_ = settings::LoadCompactLocked();
    if (osd_ready_) {
        osd_overlay_.SetCompactLayout(
            compact::SanitizeLayout(saved_compact_layout_));
        osd_overlay_.SetCompactLocked(saved_compact_locked_);
        // The placement the reader actually sees once startup has applied
        // every setting; it must equal the "applied" position logged above.
        const POINT settled = osd_overlay_.Position();
        logging::Info(std::format(L"OSD placement settled; position=({}, {})",
                                  settled.x, settled.y));
    }
    taskbar_created_message_ = RegisterWindowMessageW(L"TaskbarCreated");
    activate_message_ = RegisterWindowMessageW(L"GpuThermalGuard.Activate.v1");
    nvml_ready_ = nvml_.Initialize();
    if (!nvml_ready_) supervision::RequestRecovery();
    logging::Info(nvml_ready_
        ? std::format(L"NVML initialized; driver={}", Utf8ToWide(nvml_.driver_version()))
        : std::format(L"NVML initialization failed: {}", Utf8ToWide(nvml_.last_error())));
    ipc::Response startup_service{};
    service_connected_ = ipc::Send(ipc::Command::Query, startup_service);
    if (service_connected_) {
        HandleServiceResponse(startup_service);
    } else {
        last_worker_start_attempt_ms_ = GetTickCount64();
        if (local_protection_.Start(config_, settings::LoadSafeLatch())) {
            logging::Info(L"dedicated local protection worker started; cadence=200 ms");
        } else {
            logging::Error(L"unable to start dedicated local protection worker");
        }
    }
    history_chart_.SetThresholds(config_.trigger_temperature_c, config_.safe_power_w,
                                 std::nullopt);
    osd_overlay_.SetThresholds(config_.trigger_temperature_c, config_.safe_power_w,
                               std::nullopt);
    logging::Info(std::format(L"tray ready; normal={} W safe={} W trigger={} C log={}",
        config_.normal_power_w, config_.safe_power_w, config_.trigger_temperature_c,
        logging::Path().wstring()));
    RefreshSnapshot();
    if (osd_ready_ && osd_preference.enabled) osd_overlay_.SetVisible(true);
#ifdef GTG_FEATURE_OVERLAY
    overlay::integration::SetHidesOsd(settings::LoadOverlayHidesOsd());
    if (!OverlayDllPresent()) {
        HideOverlayControls(m_hWnd);
        logging::Info(L"overlay unavailable: gtg_overlay.dll is not beside the executable; "
                      L"overlay controls hidden, no hotkey registered");
    } else if (!overlay::integration::Configure(settings::LoadOverlayEnabled(),
                                                overlay_hotkey_.modifiers, overlay_hotkey_.key)) {
        CheckDlgButton(IDC_OVERLAY_ENABLED, BST_UNCHECKED);
        logging::Warning(L"Overlay unavailable: OSD initialization or hotkey registration failed");
        SetStatus(localization::Select(L"Overlay 熱鍵無法註冊；請更換組合鍵。",
                                       L"Overlay hotkey unavailable; choose another shortcut."),
                  StatusVisual::Warning);
    }
#else
    HideOverlayControls(m_hWnd);
#endif
    if (!loaded_settings.warning.empty()) {
        SetStatus(loaded_settings.warning, StatusVisual::Warning);
    }
    SetTimer(kRefreshTimer, kRefreshIntervalMs);
    PostMessageW(kInitializeTrayMessage);
#ifdef GTG_FEATURE_OVERLAY
    PostMessageW(kFeatureNoticeMessage);
#endif
    return TRUE;
}

LRESULT MainDialog::OnTimer(UINT, WPARAM id, LPARAM, BOOL&) {
    timing::SlowUiScope timer_phase(L"main.timer");
    PollApplyCompletion();
    if (id == kRefreshTimer) {
        const auto now = static_cast<std::uint64_t>(GetTickCount64());
        if (!tray_added_ && now - last_tray_add_attempt_ms_ >= 2'000) {
            if (AddTrayIcon()) PostMessageW(kVerifyTrayMessage);
        }
        RefreshSnapshot();
        UpdateWallTime();
        if (now - last_fps_refresh_ms_ >= 500) {
            timing::SlowUiScope fps_phase(L"main.fps.refresh");
            last_fps_refresh_ms_ = now;
            if (show_fps_) {
                const auto target = fps::ForegroundCandidate();
                fps_observer_.SetTarget(target);
                // What the session that just ended actually received. The
                // event ids it depends on are not a documented contract, so a
                // session that saw neither side is worth a line: without it,
                // the only symptom of a Windows update moving them is a reader
                // wondering why the number stopped. Try-only, on the message
                // loop, so a full queue drops it rather than waiting.
                if (const auto counts = fps_observer_.TakeLastSessionCounts();
                    counts.valid) {
                    (void)logging::TryInfo(std::format(
                        L"fps session ended: displayed={} kernel_presents={}",
                        counts.displayed_frames, counts.kernel_presents));
                }
                const auto observed = fps_observer_.TryRead();
                const auto matched = target && observed.identity == *target
                    ? observed : fps::Snapshot{};
                fps_history_.Record(now, target, matched);
                // Only a stable reading counts as play time (play_clock.hpp).
                fps_history_.TickPlayClock(now,
                    matched.status == fps::Status::Ready ? ProgramKey(matched.identity)
                                                         : std::nullopt);
                history_chart_.NotifyDataChanged();
                if (osd_ready_ && (osd_overlay_.Visible()
#ifdef GTG_FEATURE_OVERLAY
                                  || overlay::integration::GetStatus().enabled
#endif
                                  ))
                    osd_overlay_.SetFpsSnapshot(matched);
            } else {
                fps_observer_.SetTarget(std::nullopt);
            }
        }
        // Auxiliary work on the presentation thread only. Sampled on every
        // 200 ms tick, matching the thermal telemetry rather than the 2 Hz FPS
        // cadence, so the RAM curve spans the same window as the records it
        // sits beside. One kernel32 call, no lock shared with protection.
        // RefreshSnapshot() above already invalidated the chart on this tick,
        // so this path records without asking for a second repaint.
        // Beside RAM, on the same tick and for the same reason: the curve
        // has to span the same window as the records drawn next to it.
        // Measured at 25 us typical and 150 us worst on this machine, which is
        // a duty cycle of 0.01 % here and nothing at all on the protection
        // worker, which this timer is not.
        if (show_net_ && now != last_net_sample_ms_) {
            timing::SlowUiScope phase(L"main.net.refresh");
            last_net_sample_ms_ = now;
            if (const auto counters = net_sampler_.Read(now)) {
                net_history_.Record(now, *counters);
            } else {
                // The adapter went away or the route did. Forget the previous
                // counters, or the next difference spans two interfaces and
                // reports a rate no link ever carried.
                net_history_.Interrupt();
            }
        }
        if (show_ram_ && now != last_ram_sample_ms_) {
            timing::SlowUiScope phase(L"main.ram.refresh");
            last_ram_sample_ms_ = now;
            ram_history_.Record(now, sysmem::Query());
            // Windows decides what "low" means; we only pass its answer on.
            // Inventing our own percentage would let the overlay disagree with
            // the operating system in front of the reader.
            if (osd_ready_) {
                osd_overlay_.SetHostMemoryLow(low_memory_signal_.Low());
            }
        }
        PersistCompactArrangement();
    }
    return 0;
}

LRESULT MainDialog::OnClose(UINT, WPARAM, LPARAM, BOOL&) {
    const bool close_to_tray = IsDlgButtonChecked(IDC_CLOSE_TO_TRAY) == BST_CHECKED;
    if (close_to_tray && tray_added_) {
        ShowWindow(SW_HIDE);
    } else {
        if (close_to_tray && !tray_added_) {
            MessageBoxW(localization::Select(L"系統匣圖示不可用，為避免程式留在背景而無法操作，現在將結束 Tray UI。",
                                              L"The tray icon is unavailable. The Tray UI will exit to avoid becoming inaccessible.").data(),
                        L"GPU Thermal Guard", MB_OK | MB_ICONWARNING);
        }
        supervision::MarkCleanExit();
        DestroyWindow();
    }
    return 0;
}

LRESULT MainDialog::OnDestroy(UINT, WPARAM, LPARAM, BOOL&) {
    KillTimer(kRefreshTimer);
    // A joinable std::thread member would call std::terminate at destruction.
    // The call it runs is finite, so this join is too.
    if (deep_clean_worker_.joinable()) deep_clean_worker_.join();
    SaveMainWindowPosition();
    // Hidden OSDs retain their HWND/position. Save before any potentially
    // blocking worker shutdown and before destroying the painted overlay.
    if (osd_ready_) {
        (void)SaveOsdWindowPosition(osd_overlay_, L"normal exit");
        // Saved on the same occasion as the position, and for the same reason:
        // pressing the chevron once per launch, forever, is the program making
        // the reader restate a preference it already knows.
        std::wstring collapsed_error;
        if (!settings::SaveOsdCollapsed(osd_overlay_.Collapsed(), collapsed_error)) {
            logging::Warning(collapsed_error);
        }
    }
    local_protection_.Stop();
    fps_observer_.Stop();
    osd_overlay_.Shutdown();
    osd_ready_ = false;
    RemoveTrayIcon();
    SendDlgItemMessageW(IDC_STATUS_ICON, STM_SETICON, 0, 0);
    if (status_icon_ != nullptr) DestroyIcon(status_icon_);
    status_icon_ = nullptr;
    SendDlgItemMessageW(IDC_CAPTURE_SNAPSHOT, BM_SETIMAGE, 0, 0);
    if (snapshot_icon_ != nullptr) DestroyIcon(snapshot_icon_);
    snapshot_icon_ = nullptr;
    logging::Info(L"tray UI stopped");
    PostQuitMessage(0);
    return 0;
}

LRESULT MainDialog::OnThemeChanged(UINT, WPARAM, LPARAM, BOOL&) {
    (void)EnableThemeDialogTexture(m_hWnd, ETDT_ENABLE);
    RedrawWindow(nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
    PostMessageW(kReloadSnapshotIconMessage);
    return 0;
}

LRESULT MainDialog::OnDpiChanged(UINT, const WPARAM dpi, const LPARAM suggested,
                                 BOOL& handled) {
    // Recorded before anything is done about it.
    //
    // A game taking exclusive fullscreen changes the display mode, and Windows
    // recomputes the recommended scale for the new resolution -- so a 150 %
    // desktop can legitimately become 100 % while the game holds the display.
    // Coming back should raise it again. The owner saw it stay at 100 %, and
    // that is two different failures with the same symptom: the second change
    // never arriving, or arriving and not being applied. Guessing between them
    // would be guessing, so both sides are written down: what the system said,
    // and what the window actually became.
    const auto* rect = reinterpret_cast<const RECT*>(suggested);
    RECT before{};
    (void)GetWindowRect(&before);
    // The iconic flag is here because SaveMainWindowPosition already guards
    // against it: something in this window's past made somebody aware it gets
    // minimised. A game taking the display exclusively is exactly when that
    // happens, and a minimised window's rect is not the rect the dialog
    // manager would be resizing.
    (void)logging::TryInfo(std::format(
        L"dpi changed: dpi={} suggested={}x{} window_was={}x{} reported={} iconic={}",
        LOWORD(dpi),
        rect != nullptr ? rect->right - rect->left : 0,
        rect != nullptr ? rect->bottom - rect->top : 0,
        before.right - before.left, before.bottom - before.top,
        m_hWnd != nullptr ? GetDpiForWindow(m_hWnd) : 0,
        IsIconic() != FALSE ? 1 : 0));
    // Let the dialog manager finish Per-Monitor V2 layout first, then reload an
    // exact-size ICO frame for the new device-pixel scale.
    PostMessageW(kReloadSnapshotIconMessage);
    PostMessageW(kRepairMainPlacementMessage);
    handled = FALSE;
    return 0;
}

LRESULT MainDialog::OnDisplayConfigurationChanged(UINT, WPARAM, LPARAM, BOOL&) {
    timing::SlowUiScope phase(L"main.display-change");
    // The other half of the picture: a mode change that produced no
    // WM_DPICHANGED at all would show up here and nowhere else.
    RECT bounds{};
    (void)GetWindowRect(&bounds);
    (void)logging::TryInfo(std::format(
        L"display changed: window={}x{} dpi={} iconic={}",
        bounds.right - bounds.left, bounds.bottom - bounds.top,
        m_hWnd != nullptr ? GetDpiForWindow(m_hWnd) : 0,
        IsIconic() != FALSE ? 1 : 0));
    PostMessageW(kRepairMainPlacementMessage);
    return 0;
}

LRESULT MainDialog::OnExitSizeMove(UINT, WPARAM, LPARAM, BOOL&) {
    (void)FitMainWindowToWorkArea();
    SaveMainWindowPosition();
    return 0;
}

LRESULT MainDialog::OnCtlColorStatic(UINT, WPARAM wparam, LPARAM lparam, BOOL& handled) {
    const int control_id = ::GetDlgCtrlID(reinterpret_cast<HWND>(lparam));
    COLORREF text_color{};
    switch (control_id) {
    case IDC_TEMP_LABEL:
    case IDC_GPU_TEMP:
        text_color = RGB(201, 58, 55);
        break;
    case IDC_POWER_LABEL:
    case IDC_GPU_POWER:
        text_color = RGB(37, 103, 184);
        break;
    case IDC_VRAM_LABEL:
    case IDC_GPU_VRAM:
        text_color = RGB(184, 103, 21);
        break;
    case IDC_STATUS:
        switch (status_visual_) {
        case StatusVisual::Armed: text_color = RGB(25, 118, 71); break;
        case StatusVisual::Warning: text_color = RGB(164, 92, 0); break;
        case StatusVisual::Protected: text_color = RGB(25, 98, 156); break;
        case StatusVisual::Fault: text_color = RGB(190, 45, 48); break;
        case StatusVisual::Neutral: text_color = GetSysColor(COLOR_WINDOWTEXT); break;
        }
        break;
    default:
        handled = FALSE;
        return 0;
    }

    HDC dc = reinterpret_cast<HDC>(wparam);
    // Neutral is the theme's text; every accent is lifted for a dark
    // background. In classic both are exactly the colours above.
    SetTextColor(dc, theme::IsDark() && status_visual_ == StatusVisual::Neutral &&
                             control_id == IDC_STATUS
                         ? theme::Text()
                         : theme::Accent(text_color));
    SetBkColor(dc, theme::DialogBackground());
    SetBkMode(dc, OPAQUE);
    return reinterpret_cast<LRESULT>(theme::DialogBackgroundBrush());
}

LRESULT CALLBACK MainDialog::ReadingColorSubclass(HWND window, const UINT message,
                                                  const WPARAM wparam, const LPARAM lparam,
                                                  const UINT_PTR id, const DWORD_PTR data) {
    auto* dialog = reinterpret_cast<MainDialog*>(data);
    // Only while dark: in classic darkmodelib passes the message through and
    // the dialog's own map answers it as it always has.
    if (message == WM_CTLCOLORSTATIC && theme::IsDark()) {
        BOOL handled = TRUE;
        const LRESULT brush = dialog->OnCtlColorStatic(message, wparam, lparam, handled);
        if (handled) return brush;
    } else if (message == WM_NOTIFY &&
               reinterpret_cast<const NMHDR*>(lparam)->code == TTN_GETDISPINFOW) {
        // The overlay controls' tooltip, built when shown so it names the
        // current hotkey in the current language.
        auto* info = reinterpret_cast<NMTTDISPINFOW*>(lparam);
        if (info->hdr.idFrom == reinterpret_cast<UINT_PTR>(dialog->GetDlgItem(IDC_OVERLAY_HIDES_OSD).m_hWnd)) {
            dialog->tooltip_text_ = localization::Select(
                L"遊戲內 overlay 顯示時，以它取代桌面 OSD；離開遊戲（ALT+TAB、關閉 overlay 或結束遊戲）後 OSD 會回來。",
                L"The in-game overlay replaces the desktop OSD while its game is in front. The OSD returns when you leave the game (ALT+TAB, overlay off, or the game exits).");
            info->lpszText = dialog->tooltip_text_.data();
            return 0;
        }
        if (info->hdr.idFrom == reinterpret_cast<UINT_PTR>(dialog->GetDlgItem(IDC_OVERLAY_AUTO_SDR).m_hWnd)) {
            dialog->tooltip_text_ = localization::Select(
                L"勾選：新 Overlay 工作階段以 SDR 解讀，不詢問。取消：顯示色彩策略選擇器。HDR 支援有限；這不是 HDR 偵測，錯誤解讀可能影響亮度與顏色。已注入的工作階段不會改變。",
                L"Checked: new overlay sessions assume SDR without asking. Unchecked: show the color selector. HDR support is limited; this is not HDR detection and may give incorrect brightness/colors. Existing injected sessions are unchanged.");
            info->lpszText = dialog->tooltip_text_.data();
            return 0;
        }
        dialog->tooltip_text_ = localization::Format(
#ifdef GTG_FEATURE_OVERLAY
            L"遊戲內 Overlay（D3D11／D3D12）。熱鍵：{}。對前景程式啟用／隱藏；DLL 留存至遊戲退出。",
            L"In-game overlay (D3D11/D3D12). Hotkey: {}. Attaches to or toggles the foreground game; the DLL stays until the game exits.",
#else
            L"遊戲內 Overlay 尚未包含在這個版本；設定會先保存。熱鍵：{}",
            L"The in-game overlay is not in this build yet; the setting is saved. Hotkey: {}",
#endif
            HotkeyText(dialog->overlay_hotkey_));
        info->lpszText = dialog->tooltip_text_.data();
        return 0;
    } else if (message == WM_NCDESTROY) {
        ::RemoveWindowSubclass(window, ReadingColorSubclass, id);
    }
    return ::DefSubclassProc(window, message, wparam, lparam);
}

LRESULT MainDialog::OnDrawItem(UINT, WPARAM, const LPARAM lparam, BOOL& handled) {
    const auto* item = reinterpret_cast<const DRAWITEMSTRUCT*>(lparam);
    if (item == nullptr || item->CtlType != ODT_BUTTON ||
        item->CtlID != IDC_SAVE_SETTINGS) {
        handled = FALSE;
        return 0;
    }

    const bool selected = (item->itemState & ODS_SELECTED) != 0;
    timing::SlowUiScope phase(L"main.save-button.draw");
    const int saved_dc = SaveDC(item->hDC);
    if (!saved_dc) { handled = FALSE; return 0; }
    // Owner draw must also repaint the pixels outside its rounded outline.
    FillRect(item->hDC, &item->rcItem, theme::DialogBackgroundBrush());
    const auto font = reinterpret_cast<HFONT>(::SendMessageW(item->hwndItem, WM_GETFONT, 0, 0));
    if (font) SelectObject(item->hDC, font);
    const bool disabled = (item->itemState & ODS_DISABLED) != 0;
    // The dirty state is the same orange in both themes: it is a warning,
    // and it reads on either background. Clean follows the theme.
    const COLORREF fill = settings_dirty_
        ? (selected ? RGB(205, 132, 18) : RGB(244, 172, 54))
        : (selected ? theme::HotBackground() : theme::ControlBackground());
    const COLORREF border = settings_dirty_ ? RGB(176, 104, 0) : theme::Edge();
    HBRUSH brush = CreateSolidBrush(fill);
    HPEN pen = CreatePen(PS_SOLID, 1, border);
    const HGDIOBJ previous_brush = SelectObject(item->hDC, brush);
    const HGDIOBJ previous_pen = SelectObject(item->hDC, pen);
    const int diameter = ButtonCornerDiameter(GetDpiForWindow(item->hwndItem));
    RoundRect(item->hDC, item->rcItem.left, item->rcItem.top,
              item->rcItem.right, item->rcItem.bottom, diameter, diameter);
    SelectObject(item->hDC, previous_pen);
    SelectObject(item->hDC, previous_brush);
    DeleteObject(pen);
    DeleteObject(brush);

    wchar_t label[128]{};
    ::GetWindowTextW(item->hwndItem, label, static_cast<int>(_countof(label)));
    SetBkMode(item->hDC, TRANSPARENT);
    SetTextColor(item->hDC, disabled ? theme::DisabledText()
                                    : (settings_dirty_ ? RGB(45, 31, 8) : theme::Text()));
    RECT text_rect = item->rcItem;
    if (selected) OffsetRect(&text_rect, 1, 1);
    // Prefix processing on, as a push button does it: the label is written
    // "Save && Apply" so both of its looks show one ampersand.
    DrawTextW(item->hDC, label, -1, &text_rect,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_HIDEPREFIX);
    if ((item->itemState & ODS_FOCUS) != 0) {
        RECT focus = item->rcItem;
        InflateRect(&focus, -3, -3);
        DrawFocusRect(item->hDC, &focus);
    }
    RestoreDC(item->hDC, saved_dc);
    return TRUE;
}

LRESULT MainDialog::OnTrayMessage(UINT, WPARAM, LPARAM event, BOOL&) {
    switch (LOWORD(event)) {
    case WM_LBUTTONDBLCLK: ShowMainWindow(); break;
    case WM_CONTEXTMENU:
    case WM_RBUTTONUP: ShowTrayMenu(); break;
    default: break;
    }
    return 0;
}

LRESULT MainDialog::OnFeatureNotice(UINT, WPARAM, LPARAM, BOOL&) {
#ifdef GTG_FEATURE_OVERLAY
    if (!OverlayDllPresent()) return 0;   // nothing to announce without the DLL
    if (feature_notice_attempted_) return 0;
    feature_notice_attempted_ = true;
    std::uint32_t seen{};
    const bool valid = settings::LoadFeatureNoticeSerial(seen);
    if (!settings::NeedsFeatureNotice(valid, seen)) return 0;
    const int result = MessageBoxW(localization::Select(
        L"新增遊戲 Overlay 與可自訂熱鍵。\n\nOverlay 預設啟用（保留已儲存的關閉選擇），不會自動注入。遊戲在前景時按預設 ALT+F10。自動 SDR 預設勾選，不詢問；取消勾選才顯示色彩策略選擇器。同一行程後續按熱鍵切換顯示。\n\nSDR 解讀不是 HDR 偵測；HDR 支援有限，錯誤解讀可能影響亮度與顏色。已注入的策略不會改變。DLL 留到遊戲退出；隱藏不會卸載。GTG 溫度保護核心獨立運作。\n\n按確定記錄已讀；取消則下次啟動再提示。",
        L"New: game overlay and a customizable hotkey.\n\nOverlay is enabled by default (saved Off choices are respected); there is no automatic injection. Foreground the game and press default ALT+F10. Auto SDR is checked by default and skips the prompt; uncheck it to show the color selector. Later presses toggle visibility for that process.\n\nSDR interpretation is not HDR detection. HDR support is limited; incorrect interpretation may affect brightness/colors. Existing injected policies are unchanged. The DLL stays until game exit; hiding does not unload it. Thermal protection remains independent.\n\nOK acknowledges this feature notice; Cancel leaves it for the next startup.").data(),
        localization::Select(L"GPU Thermal Guard — 新功能", L"GPU Thermal Guard — What's new").data(),
        MB_OKCANCEL | MB_ICONINFORMATION);
    if (result == IDOK) {
        std::wstring error;
        if (!settings::SaveFeatureNoticeSerial(settings::kFeatureNoticeSerial, error))
            logging::Warning(error);
    }
#endif
    return 0;
}

LRESULT MainDialog::OnInitializeTray(UINT, WPARAM, LPARAM, BOOL&) {
    if (!AddTrayIcon()) {
        SetStatus(localization::Select(L"Explorer 尚未接受系統匣圖示；程式將持續重試",
                                       L"Explorer has not accepted the tray icon; retrying"),
                  StatusVisual::Warning);
        return 0;
    }
    PostMessageW(kVerifyTrayMessage);
    return 0;
}

LRESULT MainDialog::OnVerifyTray(UINT, WPARAM, LPARAM, BOOL&) {
    if (!tray_added_) return 0;
    NOTIFYICONIDENTIFIER identifier{};
    identifier.cbSize = sizeof(identifier);
    identifier.hWnd = m_hWnd;
    identifier.uID = tray_data_.uID;
    identifier.guidItem = kTrayIconGuid;
    RECT icon_rect{};
    if (FAILED(Shell_NotifyIconGetRect(&identifier, &icon_rect))) {
        RemoveTrayIcon();
        SetStatus(localization::Select(L"Explorer 未確認系統匣圖示；程式將在 2 秒內重新註冊",
                                       L"Explorer did not confirm the tray icon; retrying in 2 seconds"),
                  StatusVisual::Warning);
    }
    return 0;
}

LRESULT MainDialog::OnCaptureSnapshot(UINT, WPARAM, LPARAM, BOOL&) {
    if (snapshot_requests_.empty()) return 0;
    const SnapshotRequest request = snapshot_requests_.front();
    snapshot_requests_.pop_front();

    // Freeze the requested wall time into the captured UI. WM_PRINT renders the
    // current controls and custom chart without showing or activating the dialog.
    SetControlText(IDC_WALL_TIME, snapshot::FormatDisplayTime(request.wall_time));
    const snapshot::CaptureResult result =
        snapshot::CaptureIncidentSnapshot(m_hWnd, request.reason, request.wall_time);
    if (result) {
        logging::Info(std::format(L"UI incident snapshot saved: {}", result.path.wstring()));
        if (request.notify_user) {
            MessageBoxW(localization::Format(L"監控快照已保存：\n\n{}",
                                              L"Monitoring snapshot saved:\n\n{}",
                                              result.path.wstring()).c_str(),
                        L"GPU Thermal Guard", MB_OK | MB_ICONINFORMATION);
        }
    } else {
        logging::Error(std::format(L"UI incident snapshot failed: {}", result.error));
        if (request.notify_user) {
            MessageBoxW(localization::Format(L"無法保存監控快照：\n\n{}",
                                              L"Unable to save monitoring snapshot:\n\n{}",
                                              result.error).c_str(),
                        L"GPU Thermal Guard", MB_OK | MB_ICONERROR);
        }
    }
    UpdateWallTime();
    if (!snapshot_requests_.empty()) PostMessageW(kCaptureSnapshotMessage);
    return 0;
}

LRESULT MainDialog::OnReloadSnapshotIcon(UINT, WPARAM, LPARAM, BOOL&) {
    ReloadSnapshotIcon();
    AlignSettingsEdits(m_hWnd);
    return 0;
}

LRESULT MainDialog::OnRepairMainPlacement(UINT, WPARAM, LPARAM, BOOL&) {
    timing::SlowUiScope phase(L"main.placement.repair");
    if (FitMainWindowToWorkArea()) {
        SaveMainWindowPosition();
        logging::Info(L"main window placement repaired to a visible monitor work area");
    }
    return 0;
}

LRESULT MainDialog::OnRegisteredMessage(UINT message, WPARAM, LPARAM, BOOL& handled) {
    if (message == taskbar_created_message_) {
        if (tray_data_.hIcon != nullptr) DestroyIcon(tray_data_.hIcon);
        tray_data_.hIcon = nullptr;
        current_tray_icon_id_ = 0;
        current_tray_tooltip_.clear();
        tray_added_ = false;
        (void)AddTrayIcon();
        PostMessageW(kVerifyTrayMessage);
        return 0;
    }
    if (message == activate_message_) {
        ShowMainWindow();
        return 0;
    }
    handled = FALSE;
    return 0;
}

LRESULT MainDialog::OnValidateSettings(WORD, WORD, HWND, BOOL&) {
    if (apply_pending_) return 0;
    if (service_connected_) {
        MessageBoxW(localization::Select(L"Service 正在執行。請先停止 Service 再修改設定，避免兩邊使用不同參數。",
                                         L"The Service is running. Stop it before changing settings so both components use the same parameters.").data(),
                    L"GPU Thermal Guard", MB_OK | MB_ICONWARNING);
        return 0;
    }
    if (local_protection_.Snapshot().safe_latched) {
        MessageBoxW(localization::Select(L"安全功率仍在鎖定中，現在不能更改設定。",
                                         L"Safe power is locked; settings cannot be changed now.").data(), L"GPU Thermal Guard",
                    MB_OK | MB_ICONWARNING);
        return 0;
    }
    bool normal_ok{}, safe_ok{}, temp_ok{};
    ProtectionConfig candidate = config_;
    candidate.normal_power_w = static_cast<int>(ReadUnsignedControl(IDC_NORMAL_POWER, normal_ok));
    candidate.safe_power_w = static_cast<int>(ReadUnsignedControl(IDC_SAFE_POWER, safe_ok));
    candidate.trigger_temperature_c = static_cast<int>(ReadUnsignedControl(IDC_TRIGGER_TEMP, temp_ok));
    candidate.auto_restore = IsDlgButtonChecked(IDC_AUTO_RESTORE) == BST_CHECKED;
    if (!normal_ok || !safe_ok || !temp_ok) {
        MessageBoxW(localization::Select(L"請輸入有效的正整數。", L"Enter valid positive integers.").data(),
                    localization::Select(L"設定錯誤", L"Invalid Settings").data(), MB_OK | MB_ICONWARNING);
        return 0;
    }
    if (const auto error = ValidateConfig(candidate); error.has_value()) {
        MessageBoxW(Utf8ToWide(*error).c_str(), localization::Select(L"設定錯誤", L"Invalid Settings").data(), MB_OK | MB_ICONWARNING);
        return 0;
    }
    if (minimum_power_limit_mw_ && maximum_power_limit_mw_) {
        const auto normal_mw = static_cast<unsigned int>(candidate.normal_power_w) * 1000U;
        const auto safe_mw = static_cast<unsigned int>(candidate.safe_power_w) * 1000U;
        if (normal_mw < *minimum_power_limit_mw_ || normal_mw > *maximum_power_limit_mw_ ||
            safe_mw < *minimum_power_limit_mw_ || safe_mw > *maximum_power_limit_mw_) {
            MessageBoxW(localization::Format(L"功率必須落在此 GPU 的 {:.0f}–{:.0f} W 範圍內。",
                                              L"Power must be within this GPU's {:.0f}–{:.0f} W range.",
                *minimum_power_limit_mw_ / 1000.0, *maximum_power_limit_mw_ / 1000.0).c_str(),
                localization::Select(L"設定錯誤", L"Invalid Settings").data(), MB_OK | MB_ICONWARNING);
            return 0;
        }
    }
    // The ceiling is the firmware's slowdown point, not its maximum operating
    // temperature. At or above slowdown a trigger cannot pre-empt anything --
    // the firmware is already throttling -- and pre-empting it is the whole
    // premise of this tool. That does permit a trigger above the vendor's
    // stated maximum operating temperature; it is the owner's decision, and
    // the reasoning is in AF-20260924-monitor-only-and-device-defaults.
    if (gpu_slowdown_temperature_c_ &&
        candidate.trigger_temperature_c >=
            static_cast<int>(*gpu_slowdown_temperature_c_)) {
        MessageBoxW(localization::Format(
                        L"觸發溫度必須低於韌體的 Slowdown T.Limit（{} °C），"
                        L"否則韌體已經先動作了。",
                        L"Trigger temperature must be below the firmware Slowdown T.Limit "
                        L"({} °C); at or above it the firmware is already acting.",
                        *gpu_slowdown_temperature_c_).c_str(),
                    localization::Select(L"設定錯誤", L"Invalid Settings").data(),
                    MB_OK | MB_ICONWARNING);
        return 0;
    }
    if (!local_protection_.RequestApply(candidate)) {
        MessageBoxW(localization::Select(L"監控核心尚未就緒或有待處理的套用要求。",
            L"Protection worker is unavailable or an Apply request is pending.").data(),
            L"GPU Thermal Guard", MB_OK | MB_ICONWARNING);
        return 0;
    }
    apply_pending_ = true;
    for (const int id : {IDC_NORMAL_POWER, IDC_SAFE_POWER, IDC_TRIGGER_TEMP, IDC_AUTO_RESTORE, IDC_SAVE_SETTINGS})
        ::EnableWindow(GetDlgItem(id), FALSE);
    SetControlText(IDC_SAVE_SETTINGS, localization::Select(L"套用中…", L"Applying…"));
    return 0;
}

void MainDialog::PollApplyCompletion() {
    if (!apply_pending_) return;
    auto completion = local_protection_.TakeApplyCompletion();
    if (!completion && local_protection_.IsRunning()) return;
    // The owner may publish completion between the first read and running=false.
    // Re-read after observing its exit before reporting failure/restarting it.
    if (!completion) completion = local_protection_.TakeApplyCompletion();
    apply_pending_ = false; // clear before any modal dialog pumps messages
    for (const int id : {IDC_NORMAL_POWER, IDC_SAFE_POWER, IDC_TRIGGER_TEMP, IDC_AUTO_RESTORE, IDC_SAVE_SETTINGS})
        ::EnableWindow(GetDlgItem(id), TRUE);
    const bool applied = completion && (completion->result.status == ApplyStatus::Applied ||
                                        completion->result.status == ApplyStatus::AppliedNotSaved);
    if (applied) {
        config_ = completion->config;
        settings_unsaved_ = completion->result.status == ApplyStatus::AppliedNotSaved;
        const auto max_power = maximum_power_limit_mw_
            ? std::optional<double>(*maximum_power_limit_mw_ / 1000.0) : std::nullopt;
        history_chart_.SetThresholds(config_.trigger_temperature_c, config_.safe_power_w, max_power);
        osd_overlay_.SetThresholds(config_.trigger_temperature_c, config_.safe_power_w, max_power);
    }
    SetSettingsDirty(!applied || settings_unsaved_);
    ApplyLocalization();
    std::wstring message;
    if (applied && !settings_unsaved_) {
        message = localization::Format(L"工作功率上限 {} W 已寫入並讀回驗證，設定已保存。",
            L"Working power limit {} W was written, verified and saved.", config_.normal_power_w);
    } else if (applied) {
        message = localization::Select(L"工作功率已套用，但設定保存失敗。重啟可能使用舊設定；請重試並查看 LOG。",
            L"Working power is active, but saving failed. Restart may use old settings; retry and check the log.");
    } else if (completion && completion->result.status == ApplyStatus::WriteFailed) {
        message = completion->result.safe_verified
            ? localization::Select(L"工作功率驗證失敗；已回到安全功率並鎖定。請查看 LOG。",
                L"Working power verification failed; safe power verified and latched. See log.")
            : localization::Select(L"工作功率及安全回退均未驗證！保護核心持續重試，請查看 LOG。",
                L"Working power and safe fallback are UNVERIFIED! Protection keeps retrying; see log.");
    } else {
        message = localization::Select(L"未套用：GPU 已鎖定、接近新舊溫度門檻，或監控資料不新鮮。請稍後重試。",
            L"Not applied: GPU is latched, near the old/new temperature threshold, or monitoring data is unavailable/stale. Retry later.");
    }
    if (completion && !completion->detail.empty()) message += L"\n\n" + completion->detail;
    MessageBoxW(message.c_str(), L"GPU Thermal Guard",
        MB_OK | (applied && !settings_unsaved_ ? MB_ICONINFORMATION : MB_ICONWARNING));
}

LRESULT MainDialog::OnHideToTray(WORD, WORD, HWND, BOOL&) {
    if (!tray_added_) {
        MessageBoxW(localization::Select(L"系統匣圖示目前不可用，因此不能隱藏視窗。",
                                         L"The window cannot be hidden because the tray icon is unavailable.").data(), L"GPU Thermal Guard",
                    MB_OK | MB_ICONWARNING);
        return 0;
    }
    ShowWindow(SW_HIDE);
    return 0;
}

LRESULT MainDialog::OnCloseBehaviorChanged(WORD, WORD, HWND, BOOL&) {
    const bool enabled = IsDlgButtonChecked(IDC_CLOSE_TO_TRAY) == BST_CHECKED;
    std::wstring error;
    if (!settings::SaveCloseToTrayPreference(enabled, error)) {
        CheckDlgButton(IDC_CLOSE_TO_TRAY, enabled ? BST_UNCHECKED : BST_CHECKED);
        MessageBoxW(error.c_str(), localization::Select(L"無法保存偏好設定", L"Unable to Save Preference").data(), MB_OK | MB_ICONERROR);
        return 0;
    }
    if (enabled && !tray_added_ && !AddTrayIcon()) {
        MessageBoxW(localization::Select(L"已記住這項偏好，但目前 Explorer 尚未接受系統匣圖示；程式會持續重試。",
                                         L"The preference was saved, but Explorer has not accepted the tray icon yet; retrying.").data(),
                    L"GPU Thermal Guard", MB_OK | MB_ICONWARNING);
    } else if (enabled && tray_added_) {
        PostMessageW(kVerifyTrayMessage);
    }
    return 0;
}
LRESULT MainDialog::OnOsdToggle(WORD, WORD, HWND, BOOL&) {
    SetOsdVisible(IsDlgButtonChecked(IDC_OSD_ENABLED) == BST_CHECKED, true);
    return 0;
}

// The overlay owns the arrangement while the reader is changing it; this side
// notices the result and stores it. A failed write updates the remembered
// value anyway so a broken registry cannot turn into a warning every 200 ms.
void MainDialog::PersistCompactArrangement() {
    if (!osd_ready_) return;
    const auto packed = compact::PackLayout(osd_overlay_.compact_layout());
    if (packed != saved_compact_layout_) {
        saved_compact_layout_ = packed;
        std::wstring error;
        if (!settings::SaveCompactLayout(packed, error)) logging::Warning(error);
    }
    const bool locked = osd_overlay_.compact_locked();
    if (locked != saved_compact_locked_) {
        saved_compact_locked_ = locked;
        std::wstring error;
        if (!settings::SaveCompactLocked(locked, error)) logging::Warning(error);
    }
}

LRESULT MainDialog::OnResetOsdLayout(WORD, WORD, HWND, BOOL&) {
    if (!osd_ready_) return 0;
    osd_overlay_.SetCompactLayout(compact::Layout{});
    PersistCompactArrangement();
    return 0;
}

// The only undocumented call in this project, and the only one that trims the
// process the reader is looking at. The dialog is the consent: nothing happens
// until it is answered.
//
// The wording is deliberately free of implementation terms. Someone deciding
// whether to press yes needs to know what happens and what it costs them --
// not "working set", not which Windows interface is involved. Naming the
// mechanism would trade real information for anxiety. The accurate technical
// description belongs in the finding and the journal, where engineers read.
//
// A modal dialog here does not conflict with the rule that nothing may
// interrupt a game. That rule governs the compact overlay; the tray menu
// cannot be reached from a fullscreen game at all.
LRESULT MainDialog::OnDeepMemoryClean(WORD, WORD, HWND, BOOL&) {
    if (!sysmem::reclaim::nt::Available()) {
        (void)::MessageBoxW(
            m_hWnd,
            localization::Select(
                L"你的 Windows 版本不支援這個功能。",
                L"This feature is not available on your version of "
                L"Windows.").data(),
            localization::Select(L"深度釋放記憶體", L"Deep Memory Clean").data(),
            MB_OK | MB_ICONINFORMATION);
        return 0;
    }

    const int answer = ::MessageBoxW(
        m_hWnd,
        localization::Select(
            L"這會一次釋放整台電腦的記憶體，包含你正在使用的程式。\n\n"
            L"之後切換回其他程式時可能會短暫變慢，因為它們需要重新載入。\n\n"
            L"要繼續嗎？",
            L"This frees memory across the whole computer, including the "
            L"programs you are using right now.\n\n"
            L"Afterwards, switching back to other programs may be briefly "
            L"slow while they reload.\n\n"
            L"Continue?").data(),
        localization::Select(L"深度釋放記憶體", L"Deep Memory Clean").data(),
        MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2);
    if (answer != IDYES) return 0;

    // Off the message loop. Running it inline froze the UI for as long as the
    // call took -- which the finding had already said not to do, and tier 1
    // had already got right.
    if (deep_clean_running_.exchange(true)) return 0;
    if (deep_clean_worker_.joinable()) deep_clean_worker_.join();
    // The one cue a tray action can give while it works. The window may be
    // hidden, so there is nowhere to draw a progress indicator.
    (void)SetCursor(LoadCursorW(nullptr, IDC_WAIT));
    deep_clean_worker_ = std::thread([this]() noexcept {
        const auto started = GetTickCount64();
        auto status = sysmem::reclaim::nt::Status::Refused;
        try {
            status = sysmem::reclaim::nt::EmptyAllWorkingSets();
        } catch (...) {
            // Nothing here throws today, but the completion message must be
            // posted whatever happens or the running flag never clears.
        }
        deep_clean_status_ = status;
        const auto now = GetTickCount64();
        // How long the request took, and nothing more. The reclaim itself is
        // not finished when this returns -- see the note at the handler.
        deep_clean_elapsed_ms_ = now > started ? now - started : 0;
        PostMessageW(kDeepCleanDoneMessage, 0, 0);
    });
    return 0;
}

LRESULT MainDialog::OnDeepCleanDone(UINT, WPARAM, LPARAM, BOOL&) {
    if (deep_clean_worker_.joinable()) deep_clean_worker_.join();
    deep_clean_running_.store(false);
    (void)SetCursor(LoadCursorW(nullptr, IDC_ARROW));
    const auto status = deep_clean_status_;

    // Two separate things happen, and conflating them produced two bugs in a
    // row here. The call does real work: it walks the system signalling every
    // working set, which the owner measured at three to five seconds on a
    // first run and near-instant on a second, because by then there is little
    // left to signal. That is why it cannot sit on the message loop. But
    // finishing the signalling is not finishing the reclaim -- available
    // memory then falls progressively over the following seconds, bottoms
    // out, and climbs again as processes fault their pages back in.
    //
    // So request_ms is genuinely informative -- it says how much there was to
    // signal -- and there is still no freed figure to report. An earlier
    // version sampled memory before and after the call and wrote the
    // difference anyway. The samples were microseconds apart, so it read
    // close to zero while the reclaim was working, which a later reader would
    // take as "nothing happened".
    (void)logging::TryInfo(std::format(
        L"deep memory clean requested: status={} request_ms={}",
        sysmem::reclaim::nt::Describe(status), deep_clean_elapsed_ms_));

    if (status == sysmem::reclaim::nt::Status::Succeeded) {
        // Say what was actually achieved -- the request was accepted -- and
        // what the reader will see, because what they will see is gradual and
        // would otherwise look like nothing happened.
        (void)::MessageBoxW(
            m_hWnd,
            localization::Select(
                L"已通知系統進行深度清理。\n\n"
                L"記憶體會在接下來幾秒內逐步釋放，不會立刻完成。",
                L"The system has been asked to do a deep clean.\n\n"
                L"Memory is freed gradually over the next few seconds rather "
                L"than all at once.").data(),
            localization::Select(L"深度釋放記憶體", L"Deep Memory Clean").data(),
            MB_OK | MB_ICONINFORMATION);
    } else {
        // Fails closed and says so. It never falls back to the documented
        // tier: a reader who asked for the blunt tool must not be told they
        // got it when they did not.
        (void)::MessageBoxW(
            m_hWnd,
            localization::Select(
                L"沒有執行。詳細原因記錄在診斷記錄檔裡。",
                L"Nothing was done. The diagnostic log has the details.").data(),
            localization::Select(L"深度釋放記憶體", L"Deep Memory Clean").data(),
            MB_OK | MB_ICONWARNING);
    }
    return 0;
}

LRESULT MainDialog::OnRamToggle(WORD, WORD, HWND, BOOL&) {
    const bool enabled = IsDlgButtonChecked(IDC_SHOW_RAM) == BST_CHECKED;
    std::wstring error;
    if (!settings::SaveRamEnabled(enabled, error)) {
        CheckDlgButton(IDC_SHOW_RAM, show_ram_ ? BST_CHECKED : BST_UNCHECKED);
        logging::Warning(error);
        return 0;
    }
    show_ram_ = enabled;
    history_chart_.SetRamHistory(enabled ? &ram_history_ : nullptr);
    if (osd_ready_) osd_overlay_.SetRamHistory(enabled ? &ram_history_ : nullptr);
    if (osd_ready_) osd_overlay_.SetRamEnabled(enabled);
    // Disabling stops acquisition and clears the retained samples at once.
    if (!enabled) ram_history_.Clear();
    history_chart_.NotifyDataChanged();
    return 0;
}

LRESULT MainDialog::OnNetToggle(WORD, WORD, HWND, BOOL&) {
    const bool enabled = IsDlgButtonChecked(IDC_SHOW_NET) == BST_CHECKED;
    std::wstring error;
    if (!settings::SaveNetEnabled(enabled, error)) {
        CheckDlgButton(IDC_SHOW_NET, show_net_ ? BST_CHECKED : BST_UNCHECKED);
        logging::Warning(error);
        return 0;
    }
    show_net_ = enabled;
    if (enabled) {
        if (!net_sampler_.Open()) {
            logging::Info(std::format(L"network sampling unavailable; reason={}",
                                      static_cast<int>(net_sampler_.availability())));
        }
    } else {
        // Closing releases the module handle; the retained samples go with it,
        // so turning it back on starts a fresh curve rather than resuming one
        // with an hour-long hole in the middle.
        net_sampler_.Close();
        net_history_.Clear();
    }
    history_chart_.SetNetHistory(enabled ? &net_history_ : nullptr);
    if (osd_ready_) osd_overlay_.SetNetHistory(enabled ? &net_history_ : nullptr);
    if (osd_ready_) osd_overlay_.SetNetEnabled(enabled);
    history_chart_.NotifyDataChanged();
    return 0;
}

LRESULT MainDialog::OnFpsToggle(WORD, WORD, HWND, BOOL&) {
    const bool enabled = IsDlgButtonChecked(IDC_SHOW_FPS) == BST_CHECKED;
    std::wstring error;
    if (!settings::SaveFpsEnabled(enabled, error)) {
        CheckDlgButton(IDC_SHOW_FPS, show_fps_ ? BST_CHECKED : BST_UNCHECKED);
        logging::Warning(error);
        return 0;
    }
    show_fps_ = enabled;
    history_chart_.SetFpsHistory(enabled ? &fps_history_ : nullptr);
    if (osd_ready_) osd_overlay_.SetFpsHistory(enabled ? &fps_history_ : nullptr);
    if (osd_ready_) osd_overlay_.SetFpsEnabled(enabled);
    if (!enabled) {
        fps_observer_.SetTarget(std::nullopt);
        fps_history_.Clear();
        osd_overlay_.SetFpsSnapshot({});
    }
    return 0;
}

namespace {

// The sizes offered, in hundredths, with 0 for "follow the display".
//
// It reaches below 1.0 deliberately. The case that prompted this setting was
// 4K at 100 %, where the dashboard is 16 % of the screen width and too small;
// but 1080p at 100 % puts the same dashboard at 32 %, and that reader needs
// the other direction. A list that only climbs serves half of them.
constexpr int kOsdScaleChoices[] = {0, 75, 100, 125, 150, 175, 200, 250};

int OsdScaleIndexOf(const int stored) noexcept {
    for (std::size_t i = 0; i < std::size(kOsdScaleChoices); ++i) {
        if (kOsdScaleChoices[i] == stored) return static_cast<int>(i);
    }
    // A value this build does not offer -- written by a later version, or by
    // hand -- shows as Auto, which is also how it is being drawn.
    return 0;
}

}  // namespace

LRESULT MainDialog::OnOsdScaleDropDown(WORD, WORD, HWND, BOOL&) {
    // The overlay may have been dragged to another display since the dialog
    // was built, so what Auto means may have changed. This is the moment it
    // is about to be read.
    RefreshOsdScaleChoices();
    return 0;
}

LRESULT MainDialog::OnOsdScaleChanged(WORD, WORD, HWND, BOOL&) {
    const LRESULT selection = SendDlgItemMessageW(IDC_OSD_SCALE, CB_GETCURSEL, 0, 0);
    if (selection < 0 ||
        static_cast<std::size_t>(selection) >= std::size(kOsdScaleChoices)) {
        return 0;
    }
    const int chosen = kOsdScaleChoices[selection];
    // Applied before it is stored: the overlay is on screen while this combo
    // is in use, so the reader judges the choice by looking at it rather than
    // by pressing Save & Apply. The other OSD controls on this row behave the
    // same way.
    osd_overlay_.SetScalePercent(chosen);
    std::wstring error;
    if (!settings::SaveOsdScale(chosen, error)) {
        logging::Warning(error);
    }
    return 0;
}

LRESULT MainDialog::OnLanguageChanged(WORD, WORD, HWND, BOOL&) {
    const LRESULT selection = SendDlgItemMessageW(IDC_LANGUAGE, CB_GETCURSEL, 0, 0);
    if (selection == CB_ERR) return 0;
    const auto language = selection == 1 ? localization::UiLanguage::English
                                         : localization::UiLanguage::TraditionalChinese;
    localization::SetCurrent(language);
    std::wstring error;
    if (!settings::SaveUiLanguagePreference(language, error)) {
        MessageBoxW(error.c_str(), L"GPU Thermal Guard", MB_OK | MB_ICONERROR);
    }
    ApplyLocalization();
    history_chart_.NotifyDataChanged();
    osd_overlay_.RequestRefresh();
    return 0;
}

LRESULT MainDialog::OnProtectionSettingChanged(WORD, WORD, HWND, BOOL&) {
    RefreshSettingsDirtyState();
    return 0;
}

LRESULT MainDialog::OnManualSnapshot(WORD, WORD, HWND, BOOL&) {
    RequestUiSnapshot(snapshot::Reason::Manual, true);
    return 0;
}

LRESULT MainDialog::OnResetTriggerCount(WORD, WORD, HWND, BOOL&) {
    const std::uint32_t current_run_count =
        TestRunTriggerCount(trigger_count_, test_run_baseline_);
    const int answer = MessageBoxW(
        localization::Format(
            L"確定開始新一輪測試並將本輪觸發次數歸零嗎？\n\n"
            L"目前本輪觸發：{} 次。現有 Log 不會刪除。",
            L"Start a new test run and reset its trip count to zero?\n\n"
            L"Current run trips: {}. Existing logs will not be deleted.",
            current_run_count).c_str(),
        localization::Select(L"GPU Thermal Guard — 重設測試輪次",
                             L"GPU Thermal Guard — Reset Test Run").data(),
        MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2);
    if (answer != IDYES) return 0;

    std::uint32_t observed_total = trigger_count_;
    if (service_connected_) {
        ipc::Response response{};
        if (ipc::Send(ipc::Command::Query, response)) {
            observed_total = response.trigger_count;
        }
    } else if (local_protection_.IsRunning()) {
        observed_total = local_protection_.Snapshot().trigger_count;
    } else {
        observed_total = settings::LoadTriggerCount();
    }

    const std::uint64_t started = CurrentFileTimeValue();
    std::wstring error;
    if (!settings::SaveTriggerTestRun(observed_total, started, error)) {
        MessageBoxW(error.c_str(), L"GPU Thermal Guard", MB_OK | MB_ICONERROR);
        logging::Warning(error);
        return 0;
    }

    const std::uint32_t previous_run_count =
        TestRunTriggerCount(observed_total, test_run_baseline_);
    trigger_count_ = observed_total;
    test_run_baseline_ = observed_total;
    test_run_started_file_time_ = started;
    ApplyLocalization();
    logging::Info(std::format(
        L"test run manually reset; previous_run_trips={} lifetime_total={} "
        L"started={} normal={} W safe={} W trigger={} C auto_restore={}",
        previous_run_count, observed_total, FormatCompactLocalTime(started),
        config_.normal_power_w, config_.safe_power_w,
        config_.trigger_temperature_c,
        config_.auto_restore ? L"true" : L"false"));
    return 0;
}

LRESULT MainDialog::OnTrayOpen(WORD, WORD, HWND, BOOL&) { ShowMainWindow(); return 0; }
LRESULT MainDialog::OnTrayRefresh(WORD, WORD, HWND, BOOL&) {
    RefreshSnapshot();
    return 0;
}
LRESULT MainDialog::OnTrayOpenLog(WORD, WORD, HWND, BOOL&) {
    const auto path = logging::Path();
    if (path.empty()) {
        MessageBoxW(localization::Select(L"記錄檔尚未能建立。", L"The diagnostic log is not available yet.").data(),
                    L"GPU Thermal Guard", MB_OK | MB_ICONWARNING);
        return 0;
    }
    const std::wstring parameters = std::format(L"/select,\"{}\"", path.wstring());
    (void)ShellExecuteW(m_hWnd, L"open", L"explorer.exe", parameters.c_str(), nullptr,
                        SW_SHOWNORMAL);
    return 0;
}
LRESULT MainDialog::OnTrayToggleOsd(WORD, WORD, HWND, BOOL&) {
    // The user's switch, not the window: while the overlay stands in for the
    // OSD the window is hidden but still switched on.
    SetOsdVisible(!osd_overlay_.Wanted(), true);
    return 0;
}
LRESULT MainDialog::OnTrayExit(WORD, WORD, HWND, BOOL&) {
    supervision::MarkCleanExit();
    DestroyWindow();
    return 0;
}

bool MainDialog::AddTrayIcon() {
    if (tray_added_) return true;
    last_tray_add_attempt_ms_ = static_cast<std::uint64_t>(GetTickCount64());
    tray_data_ = {};
    tray_data_.cbSize = sizeof(tray_data_);
    tray_data_.hWnd = m_hWnd;
    tray_data_.uID = 1;

    // Remove either identity that a previous build of this same window may
    // have registered before adding the canonical GUID identity.
    NOTIFYICONDATAW legacy{};
    legacy.cbSize = sizeof(legacy);
    legacy.hWnd = m_hWnd;
    legacy.uID = tray_data_.uID;
    (void)Shell_NotifyIconW(NIM_DELETE, &legacy);
    NOTIFYICONDATAW canonical{};
    canonical.cbSize = sizeof(canonical);
    canonical.hWnd = m_hWnd;
    canonical.uFlags = NIF_GUID;
    canonical.guidItem = kTrayIconGuid;
    (void)Shell_NotifyIconW(NIM_DELETE, &canonical);
    tray_data_.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP | NIF_GUID;
    tray_data_.uCallbackMessage = kTrayMessage;
    tray_data_.guidItem = kTrayIconGuid;
    tray_data_.hIcon = LoadSmallIcon(m_hWnd, IDI_TRAY_ARMED);
    if (tray_data_.hIcon == nullptr) return false;
    current_tray_icon_id_ = IDI_TRAY_ARMED;
    wcscpy_s(tray_data_.szTip,
             localization::Select(L"GPU Thermal Guard — 保護待命",
                                  L"GPU Thermal Guard — Armed").data());
    current_tray_tooltip_ = tray_data_.szTip;
    tray_added_ = Shell_NotifyIconW(NIM_ADD, &tray_data_) != FALSE;
    if (tray_added_) {
        tray_data_.uVersion = NOTIFYICON_VERSION_4;
        Shell_NotifyIconW(NIM_SETVERSION, &tray_data_);
    } else {
        DestroyIcon(tray_data_.hIcon);
        tray_data_.hIcon = nullptr;
        current_tray_icon_id_ = 0;
        current_tray_tooltip_.clear();
    }
    return tray_added_;
}

void MainDialog::RemoveTrayIcon() noexcept {
    if (tray_added_) {
        tray_data_.uFlags = NIF_GUID;
        Shell_NotifyIconW(NIM_DELETE, &tray_data_);
    }
    tray_added_ = false;
    if (tray_data_.hIcon != nullptr) DestroyIcon(tray_data_.hIcon);
    tray_data_.hIcon = nullptr;
    current_tray_icon_id_ = 0;
    current_tray_tooltip_.clear();
}

void MainDialog::ShowTrayMenu() {
    POINT cursor{};
    GetCursorPos(&cursor);
    HMENU menu = CreatePopupMenu();
    if (menu == nullptr) return;
    AppendMenuW(menu, MF_STRING | MF_DEFAULT, IDM_TRAY_OPEN,
                localization::Select(L"開啟 GPU Thermal Guard", L"Open GPU Thermal Guard").data());
    AppendMenuW(menu, MF_STRING, IDM_TRAY_REFRESH,
                localization::Select(L"立即重新整理", L"Refresh Now").data());
    AppendMenuW(menu, MF_STRING | (osd_overlay_.Wanted() ? MF_CHECKED : MF_UNCHECKED),
                IDM_TRAY_TOGGLE_OSD,
                localization::Select(L"顯示即時 OSD", L"Show Live OSD").data());
    AppendMenuW(menu, MF_STRING, IDM_TRAY_CAPTURE_SNAPSHOT,
                localization::Select(L"保存監控快照", L"Save Monitoring Snapshot").data());
    AppendMenuW(menu, MF_STRING, IDM_TRAY_RESET_OSD_LAYOUT,
                localization::Select(L"重設 OSD 版面", L"Reset OSD Layout").data());
    // The ellipsis carries its usual Windows meaning: this opens a dialog and
    // does nothing until it is answered. Greyed out entirely when ntdll has no
    // such export, rather than offering something that cannot work.
    AppendMenuW(menu,
                MF_STRING | ((sysmem::reclaim::nt::Available() &&
                              !deep_clean_running_.load()) ? MF_ENABLED
                                                           : MF_GRAYED),
                IDM_TRAY_DEEP_MEMORY_CLEAN,
                localization::Select(L"深度釋放記憶體...",
                                     L"Deep Memory Clean...").data());
    AppendMenuW(menu, MF_STRING, IDM_TRAY_OPEN_LOG,
                localization::Select(L"開啟診斷記錄檔", L"Open Diagnostic Log").data());
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, IDM_TRAY_EXIT,
                localization::Select(L"結束", L"Exit").data());
    SetForegroundWindow(m_hWnd);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN | TPM_LEFTALIGN,
                   cursor.x, cursor.y, 0, m_hWnd, nullptr);
    DestroyMenu(menu);
    PostMessageW(WM_NULL);
}

void MainDialog::SetOsdVisible(const bool should_show, const bool persist) {
    if (!osd_ready_) {
        CheckDlgButton(IDC_OSD_ENABLED, BST_UNCHECKED);
        logging::Warning(L"OSD toggle ignored because overlay initialization failed");
        return;
    }
    osd_overlay_.SetVisible(should_show);
    if (!should_show) {
        // Checked FPS still records the current foreground App for the Main
        // UI history while the OSD is hidden; only its painted value resets.
        osd_overlay_.SetFpsSnapshot({});
    }
    CheckDlgButton(IDC_OSD_ENABLED, should_show ? BST_CHECKED : BST_UNCHECKED);
    if (persist) {
        std::wstring error;
        if (!settings::SaveOsdEnabled(should_show, error)) logging::Warning(error);
    }
    logging::Info(should_show ? L"OSD enabled" : L"OSD disabled");
}

void MainDialog::ShowMainWindow() {
    if (FitMainWindowToWorkArea()) SaveMainWindowPosition();
    ShowWindow(SW_RESTORE);
    BringWindowToTop();
    if (SetForegroundWindow(m_hWnd) == FALSE) {
        FLASHWINFO flash{sizeof(flash), m_hWnd, FLASHW_TRAY | FLASHW_TIMERNOFG, 3, 0};
        FlashWindowEx(&flash);
    }
}

void MainDialog::RevealProtectionWithoutActivation() {
    if (FitMainWindowToWorkArea()) SaveMainWindowPosition();
    if (!IsWindowVisible() || IsIconic()) {
        ShowWindow(SW_SHOWNOACTIVATE);
    }
    FLASHWINFO flash{sizeof(flash), m_hWnd,
                     FLASHW_TRAY | FLASHW_TIMERNOFG, 5, 0};
    FlashWindowEx(&flash);
}

void MainDialog::RefreshSnapshot() {
    timing::SlowUiScope phase(L"main.snapshot.refresh");
    if (!nvml_ready_) {
        if (!service_connected_) HandleLocalProtection();
        if (!gpu_unavailable_logged_) {
            logging::Error(std::format(L"GPU unavailable: {}", Utf8ToWide(nvml_.last_error())));
            gpu_unavailable_logged_ = true;
        }
        RenderUnavailable(Utf8ToWide(nvml_.last_error()));
        return;
    }
    const auto devices = [&] {
        timing::SlowUiScope probe_phase(L"main.nvml.probe");
        return nvml_.ProbeDevices();
    }();
    if (devices.empty()) {
        if (!service_connected_) HandleLocalProtection();
        if (!gpu_unavailable_logged_) {
            logging::Error(std::format(L"GPU probe failed: {}", Utf8ToWide(nvml_.last_error())));
            gpu_unavailable_logged_ = true;
        }
        RenderUnavailable(nvml_.last_error().empty()
                              ? std::wstring(localization::Select(
                                    L"未找到 NVIDIA GPU", L"No NVIDIA GPU found")) :
                          Utf8ToWide(nvml_.last_error()));
        return;
    }
    if (gpu_unavailable_logged_) logging::Info(L"GPU snapshot stream recovered");
    gpu_unavailable_logged_ = false;
    const auto target_uuid = supervision::TargetUuid();
    const auto selected = target_uuid.empty() ? devices.begin() :
        std::find_if(devices.begin(), devices.end(), [&](const auto& device) {
            return device.uuid == target_uuid;
        });
    if (selected == devices.end()) {
        HandleLocalProtection();
        RenderUnavailable(localization::Select(L"原 GPU 暫時無法使用，等待恢復",
                                              L"Original GPU unavailable; waiting for recovery").data());
        return;
    }
    RenderSnapshot(*selected);
    ipc::Response service_response{};
    const bool queried = [&] {
        timing::SlowUiScope query_phase(L"main.service.query");
        return ipc::Send(ipc::Command::Query, service_response);
    }();
    if (queried) {
        // In Service mode the supervisor observes transport availability only;
        // service recovery itself remains owned by SCM.
        supervision::ReportHealth(true, false);
        if (local_protection_.IsRunning()) {
            local_protection_.Stop();
            logging::Info(L"local protection worker stopped; Service owns GPU protection");
        }
        if (!service_connected_) logging::Info(L"connected to protection service; tray is read-only");
        service_connected_ = true;
        JournalSnapshot(*selected, nullptr);
        HandleServiceResponse(service_response);
    } else {
        if (service_connected_) {
            logging::Warning(L"protection service disconnected; tray resumed local protection");
            restore_prompt_gate_.Reset();
            previous_service_latched_.reset();
        }
        service_connected_ = false;
        const auto now = static_cast<std::uint64_t>(GetTickCount64());
        if (!apply_pending_ && !local_protection_.IsRunning() &&
            now - last_worker_start_attempt_ms_ >= 2'000) {
            last_worker_start_attempt_ms_ = now;
            last_local_trip_sequence_ = 0;
            if (local_protection_.Start(config_, settings::LoadSafeLatch())) {
                logging::Info(L"dedicated local protection worker started; cadence=200 ms");
            } else {
                logging::Error(L"unable to start dedicated local protection worker");
            }
        }
        HandleLocalProtection();
        JournalSnapshot(*selected, nullptr);
    }
}

void MainDialog::RenderUnavailable(const std::wstring& status) {
    osd_overlay_.SetCurrentPowerLimit(std::nullopt);
    latest_cpu_utilization_percent_ = SampleCpuUtilization();
    telemetry_history_.AddSample({GetTickCount64(), std::nullopt, std::nullopt,
                                  std::nullopt, std::nullopt, std::nullopt,
                                  latest_cpu_utilization_percent_});
    history_chart_.NotifyDataChanged();
    osd_overlay_.RequestRefresh();
    SetControlText(IDC_GPU_NAME, localization::Select(L"無法取得", L"Unavailable"));
    SetControlText(IDC_GPU_VBIOS, L"—");
    SetControlText(IDC_GPU_TEMP, L"—");
    SetControlText(IDC_GPU_POWER, L"—");
    SetControlText(IDC_GPU_VRAM, L"—");
    SetControlText(IDC_GPU_LIMIT, L"—");
    SetControlText(IDC_GPU_THERMAL, L"—");
    SetStatus(status, StatusVisual::Fault);
    SetTrayVisual(IDI_TRAY_FAULT,
                  localization::Select(L"GPU Thermal Guard — GPU 無法存取",
                                       L"GPU Thermal Guard — GPU unavailable").data());
}

namespace {

// NVML reports milliwatts; the dialog works in watts. Rounded rather than
// truncated: a limit reported as 319 999 mW belongs to a 320 W card, and
// truncating would put the card's own maximum out of reach.
[[nodiscard]] int WattsFrom(const std::optional<unsigned int>& milliwatts) noexcept {
    return milliwatts ? static_cast<int>((*milliwatts + 500U) / 1000U) : 0;
}

}  // namespace

// Initial values taken from the card, once, on an install that has never been
// configured. It runs on the first snapshot carrying all four readings rather
// than at startup, because the dialog reads NVML only after it is up.
void MainDialog::ApplyFirstRunDefaults(const nvml::DeviceSnapshot& s) {
    const PowerEnvelope envelope{
        WattsFrom(s.minimum_power_limit_mw), WattsFrom(s.maximum_power_limit_mw),
        WattsFrom(s.configured_power_limit_mw), WattsFrom(s.default_power_limit_mw)};
    // An incomplete reading waits for the next snapshot rather than being
    // patched up here. Inventing a limit is how the constants got here.
    if (!IsUsable(envelope)) return;
    first_run_defaults_applied_ = true;

    const PowerDefaults defaults = Derive(envelope);
    config_.normal_power_w = defaults.working_w;
    config_.safe_power_w = defaults.safe_w;
    // The trigger comes from the same place for the same reason: a constant
    // cannot know where a given card's firmware starts throttling.
    const ThermalEnvelope thermal{
        s.gpu_max_tlimit_c ? static_cast<int>(*s.gpu_max_tlimit_c) : 0,
        s.slowdown_tlimit_c ? static_cast<int>(*s.slowdown_tlimit_c) : 0,
        s.shutdown_tlimit_c ? static_cast<int>(*s.shutdown_tlimit_c) : 0};
    config_.trigger_temperature_c =
        DeriveTriggerTemperature(thermal, config_.trigger_temperature_c);
    SetDlgItemInt(IDC_TRIGGER_TEMP, static_cast<UINT>(config_.trigger_temperature_c), FALSE);
    SetDlgItemInt(IDC_NORMAL_POWER, static_cast<UINT>(config_.normal_power_w), FALSE);
    SetDlgItemInt(IDC_SAFE_POWER, static_cast<UINT>(config_.safe_power_w), FALSE);
    // Dirty on purpose: these are proposals the reader has not accepted, and
    // Save & Apply is the act that turns protection on.
    SetSettingsDirty(true);
    logging::Info(std::format(
        L"first run; derived working={} W safe={} W trigger={} C from min={} max={} "
        L"current={} default={} slowdown={} C",
        defaults.working_w, defaults.safe_w, config_.trigger_temperature_c,
        envelope.minimum_w, envelope.maximum_w, envelope.current_w, envelope.default_w,
        thermal.slowdown_c));

    // Said once, never again. A reader who wants the dashboard and nothing
    // else should reach it in a single dismissal.
    const std::wstring message =
        HasHeadroom(defaults)
            ? localization::Format(
                  L"GPU Thermal Guard 正在監控這張卡，"
                  L"並記錄溫度、功率與使用率。\n\n"
                  L"這張卡目前的功率上限高於原廠值，"
                  L"因此建議的安全功率是 {} W。"
                  L"按下「儲存並套用」才會啟用保護；"
                  L"在那之前不會有任何功率被改動。",
                  L"GPU Thermal Guard is watching this card and recording temperature, "
                  L"power and utilisation.\n\n"
                  L"This card's power limit is currently above its stock value, so the "
                  L"suggested Safe Power is {} W. Protection starts when you press "
                  L"Save & Apply; nothing is changed before that.",
                  defaults.safe_w)
            : localization::Format(
                  L"GPU Thermal Guard 正在監控這張卡，"
                  L"並記錄溫度、功率與使用率。\n\n"
                  L"工作上限與安全功率目前都是 {} W，"
                  L"相同的兩個值代表沒有可讓出的功率，"
                  L"所以它不會改動任何設定。\n\n"
                  L"若希望它在過熱時自動降低功率，"
                  L"請把安全功率設成低於工作上限的值，"
                  L"再按「儲存並套用」。",
                  L"GPU Thermal Guard is watching this card and recording temperature, "
                  L"power and utilisation.\n\n"
                  L"The Working Limit and Safe Power are both {} W. Equal limits mean there "
                  L"is no power to give up, so nothing will be changed.\n\n"
                  L"To have it lower power automatically when the card gets hot, set a Safe "
                  L"Power below the Working Limit and press Save & Apply.",
                  defaults.working_w);
    MessageBoxW(message.c_str(), L"GPU Thermal Guard", MB_OK | MB_ICONINFORMATION);
}

void MainDialog::RenderSnapshot(const nvml::DeviceSnapshot& s) {
    timing::SlowUiScope phase(L"main.snapshot.render");
    minimum_power_limit_mw_ = s.minimum_power_limit_mw;
    maximum_power_limit_mw_ = s.maximum_power_limit_mw;
    if (first_run_ && !first_run_defaults_applied_) ApplyFirstRunDefaults(s);
    gpu_max_temperature_c_ = s.gpu_max_tlimit_c;
    gpu_slowdown_temperature_c_ = s.slowdown_tlimit_c;
    vram_total_gib_ = s.memory_total_bytes
        ? std::optional<double>(static_cast<double>(*s.memory_total_bytes) / kBytesPerGiB)
        : std::nullopt;
    const std::optional<double> vram_used_gib = s.memory_used_bytes
        ? std::optional<double>(static_cast<double>(*s.memory_used_bytes) / kBytesPerGiB)
        : std::nullopt;
    const std::optional<double> vram_utilization_percent =
        s.memory_used_bytes && s.memory_total_bytes && *s.memory_total_bytes > 0
            ? std::optional<double>(100.0 * static_cast<double>(*s.memory_used_bytes) /
                                    static_cast<double>(*s.memory_total_bytes))
            : std::nullopt;
    history_chart_.SetThresholds(config_.trigger_temperature_c, config_.safe_power_w,
        s.maximum_power_limit_mw
            ? std::optional<double>(*s.maximum_power_limit_mw / 1000.0) : std::nullopt);
    osd_overlay_.SetThresholds(config_.trigger_temperature_c, config_.safe_power_w,
        s.maximum_power_limit_mw
            ? std::optional<double>(*s.maximum_power_limit_mw / 1000.0) : std::nullopt);
    osd_overlay_.SetCurrentPowerLimit(s.configured_power_limit_mw
        ? std::optional<double>(*s.configured_power_limit_mw / 1000.0) : std::nullopt);
    latest_cpu_utilization_percent_ = SampleCpuUtilization();
    telemetry_history_.AddSample({GetTickCount64(),
        s.temperature_c ? std::optional<double>(*s.temperature_c) : std::nullopt,
        s.power_usage_mw ? std::optional<double>(*s.power_usage_mw / 1000.0) : std::nullopt,
        vram_utilization_percent,
        vram_used_gib,
        s.gpu_utilization_percent
            ? std::optional<double>(*s.gpu_utilization_percent) : std::nullopt,
        latest_cpu_utilization_percent_});
    history_chart_.NotifyDataChanged();
    osd_overlay_.RequestRefresh();
    SetControlText(IDC_GPU_NAME, Utf8ToWide(s.name));
    SetControlText(IDC_GPU_VBIOS,
                   s.vbios_version.empty() ? L"—" : Utf8ToWide(s.vbios_version));
    SetControlText(IDC_GPU_TEMP, s.temperature_c ? std::format(L"{} °C", *s.temperature_c) : L"—");
    SetControlText(IDC_GPU_POWER, s.power_usage_mw ? std::format(L"{:.1f} W", *s.power_usage_mw / 1000.0) : L"—");
    SetControlText(IDC_GPU_VRAM,
        vram_utilization_percent && vram_used_gib && vram_total_gib_
            ? std::format(L"{:.0f}% · {:.1f} / {:.1f} GiB", *vram_utilization_percent,
                          *vram_used_gib, *vram_total_gib_)
            : L"—");
    if (s.configured_power_limit_mw && s.default_power_limit_mw) {
        SetControlText(IDC_GPU_LIMIT,
            localization::Format(L"目前 {:.0f} W / 預設 {:.0f} W",
                                 L"Current {:.0f} W / Default {:.0f} W",
                                 *s.configured_power_limit_mw / 1000.0,
                                 *s.default_power_limit_mw / 1000.0));
    } else SetControlText(IDC_GPU_LIMIT, L"—");
    SetControlText(IDC_GPU_THERMAL, std::format(L"Max {} °C / Slowdown {} °C / Shutdown {} °C",
        s.gpu_max_tlimit_c.value_or(0), s.slowdown_tlimit_c.value_or(0),
        s.shutdown_tlimit_c.value_or(0)));
}

void MainDialog::HandleLocalProtection() {
    timing::SlowUiScope phase(L"main.protection.presentation");
    const auto snapshot = local_protection_.Snapshot();
    if (trigger_count_ != snapshot.trigger_count) {
        trigger_count_ = snapshot.trigger_count;
        ApplyLocalization();
    }

    if (snapshot.trip_sequence > last_local_trip_sequence_) {
        last_local_trip_sequence_ = snapshot.trip_sequence;
        history_chart_.AddTripMarker(GetTickCount64());
        restore_prompt_gate_.Reset();
        RequestUiSnapshot(snapshot::Reason::ThermalTrigger);
        RevealProtectionWithoutActivation();
        ShowProtectionAlert(
            localization::Select(L"GPU Thermal Guard 已介入",
                                 L"GPU Thermal Guard intervened").data(),
            config_.auto_restore
                ? localization::Select(
                    L"GPU 溫度已達到保護條件，功率已降至安全設定；穩定冷卻後將自動恢復。",
                    L"GPU temperature reached the protection condition. Safe power is active and will restore automatically after stable cooling.").data()
                : localization::Select(
                    L"GPU 溫度已達到保護條件，功率已降至安全設定；穩定冷卻後可手動恢復。",
                    L"GPU temperature reached the protection condition. Safe power is active; restore is manual after stable cooling.").data(),
            NIIF_WARNING);
    }

    if (!snapshot.running || !snapshot.nvml_ready) {
        if (!snapshot.error.empty()) {
            SetStatus(Utf8ToWide(snapshot.error), StatusVisual::Fault);
            SetTrayVisual(IDI_TRAY_FAULT,
                localization::Select(L"GPU Thermal Guard — 保護核心無法使用",
                                     L"GPU Thermal Guard — Protection worker unavailable").data());
        }
        return;
    }

    if (snapshot.state == ProtectionState::GpuUnavailable) {
        SetStatus(
            snapshot.safe_latched
                ? localization::Select(
                    L"保護遙測無法使用；已驗證的安全功率仍保持鎖定",
                    L"Protection telemetry unavailable; verified safe power remains locked")
                : localization::Select(
                    L"保護遙測無法使用；尚未能驗證安全功率",
                    L"Protection telemetry unavailable; safe power is not yet verified"),
            snapshot.safe_latched ? StatusVisual::Warning : StatusVisual::Fault);
        SetTrayVisual(snapshot.safe_latched ? IDI_TRAY_WARNING : IDI_TRAY_FAULT,
            snapshot.safe_latched
                ? localization::Select(L"GPU Thermal Guard — 遙測中斷，安全功率已鎖定",
                                       L"GPU Thermal Guard — Telemetry lost; safe power locked").data()
                : localization::Select(L"GPU Thermal Guard — 保護遙測無法使用",
                                       L"GPU Thermal Guard — Protection telemetry unavailable").data());
    } else if (snapshot.state == ProtectionState::Fault) {
        SetStatus(localization::Select(
                      L"安全功率尚未驗證；保護核心將持續重試",
                      L"Safe power is not verified; protection worker will keep retrying"),
                  StatusVisual::Fault);
        SetTrayVisual(IDI_TRAY_FAULT,
            localization::Select(L"GPU Thermal Guard — 安全功率尚未驗證",
                                 L"GPU Thermal Guard — Safe power not verified").data());
    } else if (snapshot.safe_latched) {
        const bool ready = snapshot.state == ProtectionState::ReadyToRestore;
        SetStatus(
            snapshot.auto_restore_exhausted
                ? localization::Select(L"自動恢復重試已停止；安全功率鎖定，請手動恢復並查看 LOG",
                                       L"Auto restore retries stopped; safe power locked. Restore manually; see log.")
                : ready
                ? (config_.auto_restore
                    ? localization::Select(L"GPU 已穩定冷卻，正在準備自動恢復",
                                           L"GPU has cooled; preparing automatic restore")
                    : localization::Select(L"GPU 已穩定冷卻，等待手動恢復；目前仍保持安全功率",
                                           L"GPU has cooled; awaiting manual restore while safe power remains locked"))
                : localization::Select(L"安全功率已鎖定；獨立保護核心持續監控中",
                                       L"Safe power is locked; dedicated protection worker continues monitoring"),
            ready ? StatusVisual::Warning : StatusVisual::Protected);
        SetTrayVisual(ready ? IDI_TRAY_WARNING : IDI_TRAY_PROTECTED,
            ready
                ? localization::Select(L"GPU Thermal Guard — 已冷卻，等待恢復",
                                       L"GPU Thermal Guard — Cooled; awaiting restore").data()
                : localization::Select(L"GPU Thermal Guard — 安全功率已鎖定",
                                       L"GPU Thermal Guard — Safe power locked").data());
    } else if (snapshot.state == ProtectionState::PreTrip) {
        SetStatus(localization::Select(L"接近觸發溫度且正在升溫；等待第二次趨勢確認",
                                       L"Near trigger temperature and rising; awaiting trend confirmation"),
                  StatusVisual::Warning);
        SetTrayVisual(IDI_TRAY_WARNING,
            localization::Select(L"GPU Thermal Guard — 接近觸發溫度",
                                 L"GPU Thermal Guard — Near trigger temperature").data());
    } else if (snapshot.state == ProtectionState::MonitorOnly) {
        // Neutral, not Warning: this is a supported way to run the tool, and a
        // colour that reads as trouble on a correctly behaving machine teaches
        // the reader to stop looking at it. The words still refuse to say
        // `armed`, because what happens when the card gets hot is nothing.
        SetStatus(localization::Select(
                      L"以 200 ms 監控中；設定安全功率即可啟用保護",
                      L"Monitoring at 200 ms; set a Safe Power to enable protection"),
                  StatusVisual::Neutral);
        if (snapshot.temperature_c.has_value()) {
            SetTrayVisual(IDI_APP,
                localization::Format(L"GPU Thermal Guard — {} °C · 監控中",
                                     L"GPU Thermal Guard — {} °C · Monitoring",
                                     *snapshot.temperature_c).c_str());
        }
    } else if (snapshot.state == ProtectionState::Armed) {
        SetStatus(localization::Select(L"保護已待命；獨立核心以 200 ms 節拍監控",
                                       L"Protection armed; dedicated worker monitors at 200 ms"),
                  StatusVisual::Armed);
        if (snapshot.temperature_c.has_value()) {
            SetTrayVisual(IDI_TRAY_ARMED,
                localization::Format(L"GPU Thermal Guard — {} °C · 保護待命",
                                     L"GPU Thermal Guard — {} °C · Armed",
                                     *snapshot.temperature_c).c_str());
        }
    }

    // Presentation only: cooling does not release a verified safety latch.
    if (snapshot.safe_latched && status_visual_ != StatusVisual::Fault) {
        osd_overlay_.SetStatus(
            snapshot.state == ProtectionState::ReadyToRestore
                ? localization::Select(L"ALERT · 等待恢復", L"ALERT · Awaiting restore")
                : localization::Select(L"ALERT · 安全功率已鎖定", L"ALERT · Safe power locked"),
            OsdVisual::Protected);
    }

    if ((!config_.auto_restore || snapshot.auto_restore_exhausted) && restore_prompt_gate_.ShouldPrompt(
            snapshot.safe_latched,
            snapshot.state == ProtectionState::ReadyToRestore)) {
        ShowMainWindow();
        const int answer = MessageBoxW(
            localization::Format(
                L"GPU 已持續冷卻到 {} °C 以下。\n\n要將功率恢復為 {} W 嗎？",
                L"GPU has remained below {} °C.\n\nRestore power to {} W?",
                config_.trigger_temperature_c - config_.recovery_delta_c,
                config_.normal_power_w).c_str(),
            localization::Select(L"GPU Thermal Guard — 可安全恢復",
                                 L"GPU Thermal Guard — Safe to Restore").data(),
            MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2);
        logging::Info(answer == IDYES
            ? L"user requested dedicated-worker normal-power restore"
            : L"user declined restore; safe latch retained");
        if (answer == IDYES) local_protection_.RequestManualRestore();
    }
}

void MainDialog::JournalSnapshot(const nvml::DeviceSnapshot& s,
                                 const ProtectionDecision* decision) {
    timing::SlowUiScope phase(L"main.snapshot.journal");
    const auto now = static_cast<std::uint64_t>(GetTickCount64());
    if (now - last_journal_snapshot_ms_ < 5'000) return;
    last_journal_snapshot_ms_ = now;
    const std::wstring temperature = s.temperature_c
        ? std::format(L"{}", *s.temperature_c) : L"null";
    const std::wstring power = s.power_usage_mw
        ? std::format(L"{:.1f}", *s.power_usage_mw / 1000.0) : L"null";
    const std::wstring limit = s.configured_power_limit_mw
        ? std::format(L"{:.0f}", *s.configured_power_limit_mw / 1000.0) : L"null";
    const std::wstring gpu_load = s.gpu_utilization_percent
        ? std::format(L"{}", *s.gpu_utilization_percent) : L"null";
    const std::wstring cpu_load = latest_cpu_utilization_percent_
        ? std::format(L"{:.1f}", *latest_cpu_utilization_percent_) : L"null";
    const std::wstring vram_used = s.memory_used_bytes
        ? std::format(L"{:.2f}", static_cast<double>(*s.memory_used_bytes) / kBytesPerGiB)
        : L"null";
    const std::wstring vram_total = s.memory_total_bytes
        ? std::format(L"{:.2f}", static_cast<double>(*s.memory_total_bytes) / kBytesPerGiB)
        : L"null";
    const std::wstring vram_percent = s.memory_used_bytes && s.memory_total_bytes &&
        *s.memory_total_bytes > 0
        ? std::format(L"{:.1f}", 100.0 * static_cast<double>(*s.memory_used_bytes) /
                                     static_cast<double>(*s.memory_total_bytes))
        : L"null";
    const std::wstring state = decision
        ? Utf8ToWide(ToString(decision->state))
        : (service_connected_
            ? L"ServiceOwned"
            : Utf8ToWide(ToString(local_protection_.Snapshot().state)));
    // Periodic telemetry must never wait on the shared sink mutex, disk or
    // debugger. Queue-full drops this optional record, without a sync fallback.
    (void)logging::TryInfo(std::format(
        L"snapshot temp_c={} power_w={} vram_used_gib={} vram_total_gib={} vram_pct={} "
        L"gpu_load_pct={} cpu_load_pct={} limit_w={} state={} sample_tick_ms={}",
        temperature, power, vram_used, vram_total, vram_percent, gpu_load, cpu_load,
        limit, state, now));
}

void MainDialog::HandleServiceResponse(const ipc::Response& response) {
    timing::SlowUiScope phase(L"main.service.presentation");
    const bool latched = (response.flags & ipc::SafeLatched) != 0;
    const bool ready = (response.flags & ipc::ReadyToRestore) != 0;
    const auto service_state = static_cast<ProtectionState>(response.protection_state);
    const bool newly_latched = previous_service_latched_.has_value() &&
                               !*previous_service_latched_ && latched;
    const bool newly_counted_trip = HasNewTriggerCount(trigger_count_, response.trigger_count);
    // The first response can report trips the service counted while no UI was
    // running; those happened at some earlier time, not now, so they get no
    // marker on the curve.
    if (previous_service_latched_.has_value() && (newly_counted_trip ||
            (!*previous_service_latched_ && latched))) {
        history_chart_.AddTripMarker(GetTickCount64());
    }
    if (newly_counted_trip || service_state == ProtectionState::Armed) {
        restore_prompt_gate_.Reset();
    }
    previous_service_latched_ = latched;
    if (trigger_count_ != response.trigger_count) {
        trigger_count_ = response.trigger_count;
        ApplyLocalization();
    }
    if (newly_latched || newly_counted_trip) {
        RequestUiSnapshot(snapshot::Reason::ServiceTrigger);
        RevealProtectionWithoutActivation();
    }
    if (service_state == ProtectionState::GpuUnavailable) {
        SetStatus(
            latched
                ? localization::Select(
                    L"Service：保護遙測無法使用；安全功率仍保持鎖定",
                    L"Service: protection telemetry unavailable; safe power remains locked")
                : localization::Select(
                    L"Service：保護遙測無法使用；尚未能驗證安全功率",
                    L"Service: protection telemetry unavailable; safe power is not verified"),
            latched ? StatusVisual::Warning : StatusVisual::Fault);
        SetTrayVisual(latched ? IDI_TRAY_WARNING : IDI_TRAY_FAULT,
            latched
                ? localization::Select(L"GPU Thermal Guard Service — 遙測中斷，安全功率已鎖定",
                                       L"GPU Thermal Guard Service — Telemetry lost; safe power locked").data()
                : localization::Select(L"GPU Thermal Guard Service — 保護遙測無法使用",
                                       L"GPU Thermal Guard Service — Protection telemetry unavailable").data());
    } else if (service_state == ProtectionState::Fault) {
        SetStatus(localization::Select(
                      L"Service：安全功率尚未驗證，將持續重試",
                      L"Service: safe power is not verified; retrying"),
                  StatusVisual::Fault);
        SetTrayVisual(IDI_TRAY_FAULT,
            localization::Select(L"GPU Thermal Guard Service — 安全功率尚未驗證",
                                 L"GPU Thermal Guard Service — Safe power not verified").data());
    } else if (latched) {
        SetStatus(response.win32_error == ERROR_RETRY
            ? localization::Select(L"Service：自動恢復重試已停止；安全功率鎖定，請查看 LOG 並手動恢復",
                                   L"Service: auto retries stopped; safe power locked. See log; restore manually.")
            : ready
            ? (config_.auto_restore
                ? localization::Select(L"Service：GPU 已冷卻，自動恢復失敗或等待重試",
                                       L"Service: GPU cooled; automatic restore failed or is awaiting retry")
                : localization::Select(L"Service：GPU 已冷卻，等待使用者同意恢復",
                                       L"Service: GPU cooled; awaiting approval to restore"))
            : localization::Select(L"Service：安全功率已鎖定",
                                   L"Service: safe power locked"),
            ready ? StatusVisual::Warning : StatusVisual::Protected);
        SetTrayVisual(ready ? IDI_TRAY_WARNING : IDI_TRAY_PROTECTED,
                      ready ? (config_.auto_restore
                                   ? localization::Select(L"GPU Thermal Guard Service — 等待自動恢復重試",
                                                          L"GPU Thermal Guard Service — Awaiting automatic restore retry").data()
                                   : localization::Select(L"GPU Thermal Guard Service — 等待手動恢復",
                                                          L"GPU Thermal Guard Service — Awaiting manual restore").data())
                            : localization::Select(L"GPU Thermal Guard Service — 安全功率已鎖定",
                                                   L"GPU Thermal Guard Service — Safe power locked").data());
    } else {
        SetStatus(localization::Select(L"Service 保護核心已連線並待命",
                                       L"Service protection core connected and armed"), StatusVisual::Armed);
        SetTrayVisual(IDI_TRAY_ARMED,
                      localization::Select(L"GPU Thermal Guard Service — 保護已待命",
                                           L"GPU Thermal Guard Service — Armed").data());
    }

    if (latched && status_visual_ != StatusVisual::Fault) {
        osd_overlay_.SetStatus(
            ready
                ? localization::Select(L"ALERT · 等待恢復", L"ALERT · Awaiting restore")
                : localization::Select(L"ALERT · 安全功率已鎖定", L"ALERT · Safe power locked"),
            OsdVisual::Protected);
    }

    if ((!config_.auto_restore || response.win32_error == ERROR_RETRY) && restore_prompt_gate_.ShouldPrompt(latched, ready)) {
        ShowMainWindow();
        const int answer = MessageBoxW(
            localization::Format(L"Service 回報 GPU 已穩定冷卻。\n\n要恢復為 {} W 嗎？",
                                 L"The Service reports that the GPU has cooled.\n\nRestore to {} W?",
                                 response.normal_power_w).c_str(),
            localization::Select(L"GPU Thermal Guard — 可安全恢復",
                                 L"GPU Thermal Guard — Safe to Restore").data(),
            MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2);
        logging::Info(answer == IDYES ? L"user accepted service normal-power restore"
                                      : L"user declined service restore; safe latch retained");
        if (answer == IDYES) {
            ipc::Response restore_response{};
            if (!ipc::Send(ipc::Command::RestoreNormalPower, restore_response) ||
                (restore_response.flags & ipc::LastCommandSucceeded) == 0) {
                MessageBoxW(localization::Select(L"Service 未能恢復功率；安全功率會繼續保持。",
                                                 L"The Service could not restore power; safe power remains locked.").data(),
                            localization::Select(L"恢復失敗", L"Restore Failed").data(),
                            MB_OK | MB_ICONERROR);
                logging::Error(std::format(L"service restore request failed; win32={}",
                                           restore_response.win32_error));
            } else {
                ApplyLocalization();
                SetStatus(localization::Select(L"Service 已依使用者要求恢復正常功率",
                                               L"Service restored normal power as requested"), StatusVisual::Armed);
                SetTrayVisual(IDI_TRAY_ARMED,
                              localization::Select(L"GPU Thermal Guard Service — 保護已待命",
                                                   L"GPU Thermal Guard Service — Armed").data());
                logging::Info(L"service confirmed normal-power restore");
            }
        }
    }
}

void MainDialog::ShowProtectionAlert(const wchar_t* title, const wchar_t* message, DWORD flags) {
    if (!tray_added_) return;
    tray_data_.uFlags = NIF_INFO | NIF_GUID;
    wcsncpy_s(tray_data_.szInfoTitle, title, _TRUNCATE);
    wcsncpy_s(tray_data_.szInfo, message, _TRUNCATE);
    tray_data_.dwInfoFlags = flags | NIIF_RESPECT_QUIET_TIME;
    Shell_NotifyIconW(NIM_MODIFY, &tray_data_);
}

void MainDialog::SetTrayVisual(UINT icon_id, const wchar_t* tooltip) {
    timing::SlowUiScope phase(L"main.tray.visual");
    if (!tray_added_) return;
    if (icon_id == current_tray_icon_id_ && current_tray_tooltip_ == tooltip) return;
    tray_data_.uFlags = NIF_TIP | NIF_SHOWTIP | NIF_GUID;
    HICON old = nullptr;
    if (icon_id != current_tray_icon_id_) {
        HICON replacement = LoadSmallIcon(m_hWnd, icon_id);
        if (replacement == nullptr) return;
        old = tray_data_.hIcon;
        tray_data_.hIcon = replacement;
        current_tray_icon_id_ = icon_id;
        tray_data_.uFlags |= NIF_ICON;
    }
    wcsncpy_s(tray_data_.szTip, tooltip, _TRUNCATE);
    current_tray_tooltip_ = tray_data_.szTip;
    {
        timing::SlowUiScope notify_phase(L"main.tray.notify-icon");
        Shell_NotifyIconW(NIM_MODIFY, &tray_data_);
    }
    if (old != nullptr) DestroyIcon(old);
}

void MainDialog::SetStatus(const std::wstring_view text, const StatusVisual visual) {
    const bool visual_changed = status_visual_ != visual;
    status_visual_ = visual;
    UINT icon_id = IDI_APP;
    switch (visual) {
    case StatusVisual::Armed: icon_id = IDI_TRAY_ARMED; break;
    case StatusVisual::Warning: icon_id = IDI_TRAY_WARNING; break;
    case StatusVisual::Protected: icon_id = IDI_TRAY_PROTECTED; break;
    case StatusVisual::Fault: icon_id = IDI_TRAY_FAULT; break;
    case StatusVisual::Neutral: icon_id = IDI_APP; break;
    }

    if (icon_id != current_status_icon_id_) {
        HWND icon_control = ::GetDlgItem(m_hWnd, IDC_STATUS_ICON);
        if (HICON replacement = LoadStatusIcon(icon_control, icon_id); replacement != nullptr) {
            SendDlgItemMessageW(IDC_STATUS_ICON, STM_SETICON,
                                reinterpret_cast<WPARAM>(replacement), 0);
            if (status_icon_ != nullptr) DestroyIcon(status_icon_);
            status_icon_ = replacement;
            current_status_icon_id_ = icon_id;
        }
    }
    SetControlText(IDC_STATUS, text);
    if (visual_changed) {
        ::InvalidateRect(::GetDlgItem(m_hWnd, IDC_STATUS), nullptr, TRUE);
    }
    osd_overlay_.SetStatus(text, ToOsdVisual(visual));
}

OsdVisual MainDialog::ToOsdVisual(const StatusVisual visual) noexcept {
    switch (visual) {
    case StatusVisual::Armed: return OsdVisual::Armed;
    case StatusVisual::Warning: return OsdVisual::Warning;
    case StatusVisual::Protected: return OsdVisual::Protected;
    case StatusVisual::Fault: return OsdVisual::Fault;
    case StatusVisual::Neutral: return OsdVisual::Neutral;
    }
    return OsdVisual::Neutral;
}

void MainDialog::SetControlText(const int control_id, const std::wstring_view text) {
    const auto existing = control_text_cache_.find(control_id);
    if (existing != control_text_cache_.end() && existing->second == text) return;
    control_text_cache_.insert_or_assign(control_id, std::wstring(text));
    SetDlgItemTextW(control_id, control_text_cache_.at(control_id).c_str());
}

void MainDialog::ApplyLocalization() {
    const auto text = [](const std::wstring_view zh, const std::wstring_view en) {
        return localization::Select(zh, en);
    };
    // The version, without spending a line of the window on it.
    SetWindowTextW(L"GPU Thermal Guard - v" GTG_VERSION_DISPLAY);
    SetControlText(IDC_STATUS_GROUP,
                   text(L"GPU 即時狀態（唯讀）", L"GPU Status (Read Only)"));
    SetControlText(IDC_GPU_LABEL, L"GPU");
    SetControlText(IDC_VBIOS_LABEL, L"VBIOS");
    SetControlText(IDC_TEMP_LABEL, text(L"溫度", L"Temp"));
    SetControlText(IDC_POWER_LABEL, text(L"功率", L"Power"));
    SetControlText(IDC_VRAM_LABEL, L"VRAM");
    SetControlText(IDC_LIMIT_LABEL, text(L"功率限制", L"Power Limit"));
    SetControlText(IDC_THERMAL_LABEL, text(L"韌體門檻", L"T.Limits"));
    SetControlText(IDC_HISTORY_GROUP,
                   text(L"歷史趨勢（5 分鐘視野／1 小時）",
                        L"History (5-minute view / 1 hour)"));
    SetControlText(IDC_SETTINGS_GROUP,
                   text(L"保護設定（APPLY 會套用工作功率上限）",
                        L"Protection Settings (Apply sets working power limit)"));
    SetControlText(IDC_NORMAL_POWER_LABEL, text(L"工作功率上限 (W)", L"Working Limit (W)"));
    SetControlText(IDC_SAFE_POWER_LABEL, text(L"安全功率 (W)", L"Safe Power (W)"));
    SetControlText(IDC_TRIGGER_TEMP_LABEL, text(L"觸發溫度 (°C)", L"Trigger Temp (°C)"));
    SetControlText(IDC_SETTINGS_NOTE,
                   text(L"超溫後鎖定安全功率；穩定冷卻後可手動或選擇自動恢復。",
                        L"Safe power locks after a trip; choose manual or automatic restore after cooling."));
    SetControlText(IDC_AUTO_RESTORE, text(L"自動 Restore", L"Auto Restore"));
    SetControlText(IDC_CLOSE_TO_TRAY, text(L"[X] 關閉到系統匣", L"[X] Close to Tray"));
    SetControlText(IDC_DISPLAY_LABEL, text(L"顯示：", L"Display:"));
    SetControlText(IDC_OSD_ENABLED, L"OSD");
    SetControlText(IDC_SHOW_FPS, L"FPS");
    SetControlText(IDC_SHOW_RAM, L"RAM");
    SetControlText(IDC_SHOW_NET, text(L"網路", L"NET"));
    SetControlText(IDC_OVERLAY_ENABLED, L"Overlay");
    SetControlText(IDC_OVERLAY_HIDES_OSD,
                   text(L"遊戲中隱藏桌面 OSD", L"Hide desktop OSD in game"));
    SetControlText(IDC_OVERLAY_AUTO_SDR, text(L"自動 SDR（不詢問）", L"Auto SDR (skip prompt)"));
    SetControlText(IDC_OVERLAY_HOTKEY, text(L"Overlay 熱鍵", L"Overlay Hotkey"));
    SetControlText(IDC_THEME_LABEL, text(L"主題", L"Theme"));
    SetControlText(IDC_LANGUAGE_LABEL, text(L"語言", L"Language"));
    RefreshThemeChoices();
    SetControlText(IDC_SAVE_SETTINGS,
                   apply_pending_ ? text(L"套用中…", L"Applying…")
                   : settings_dirty_ ? text(L"保存並套用 ●", L"Save && Apply ●")
                                   : text(L"保存並套用", L"Save && Apply"));
    SetControlText(IDC_HIDE_TO_TRAY, text(L"隱藏到系統匣", L"Hide to Tray"));
    SetControlText(IDC_RESET_TRIGGER_COUNT, text(L"重設", L"Reset"));
    const std::uint32_t run_count =
        TestRunTriggerCount(trigger_count_, test_run_baseline_);
    const std::wstring started = FormatCompactLocalTime(test_run_started_file_time_);
    SetControlText(IDC_TRIGGER_COUNT,
        started.empty()
            ? localization::Format(L"本輪觸發：{} · 尚未重設",
                                   L"Run trips: {} · not reset", run_count)
            : localization::Format(L"本輪觸發：{} · 自 {}",
                                   L"Run trips: {} · since {}", run_count, started));

    HWND combo = GetDlgItem(IDC_LANGUAGE);
    const int selected = localization::Current() == localization::UiLanguage::English ? 1 : 0;
    SendMessageW(combo, WM_SETREDRAW, FALSE, 0);
    SendMessageW(combo, CB_RESETCONTENT, 0, 0);
    SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"正體中文"));
    SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"English"));
    SendMessageW(combo, CB_SETCURSEL, selected, 0);
    SendMessageW(combo, WM_SETREDRAW, TRUE, 0);
    ::InvalidateRect(combo, nullptr, TRUE);

    SetControlText(IDC_OSD_SCALE_LABEL, text(L"OSD 大小", L"OSD Size"));
    RefreshOsdScaleChoices();
}

void MainDialog::RefreshOsdScaleChoices() {
    HWND scale_combo = GetDlgItem(IDC_OSD_SCALE);
    if (scale_combo == nullptr) return;
    // Auto carries the percentage it resolves to, read from the display the
    // OVERLAY is on -- not this dialog's, which can be a different monitor at
    // a different scale. Without that number a reader at 100 % has no way to
    // learn that Auto is why the dashboard is small, and therefore no way to
    // discover the setting that fixes it.
    //
    // Recomputed here rather than cached, and this runs again when the list
    // is dropped down, so the figure is read at the moment somebody looks at
    // it. A stale percentage would be a confidently wrong number, and this
    // project would rather show none.
    const unsigned dpi = osd_overlay_.CurrentDpi();
    const std::wstring automatic = dpi == 0
        ? std::wstring(localization::Select(L"自動", L"Auto"))
        : localization::Format(L"自動（{}%）", L"Auto ({}%)",
                               MulDiv(static_cast<int>(dpi), 100, 96));
    SendMessageW(scale_combo, WM_SETREDRAW, FALSE, 0);
    SendMessageW(scale_combo, CB_RESETCONTENT, 0, 0);
    for (const int percent : kOsdScaleChoices) {
        const std::wstring entry = percent == 0
            ? automatic
            : std::format(L"\u00d7{}.{:02}", percent / 100, percent % 100);
        SendMessageW(scale_combo, CB_ADDSTRING, 0,
                     reinterpret_cast<LPARAM>(entry.c_str()));
    }
    SendMessageW(scale_combo, CB_SETCURSEL,
                 OsdScaleIndexOf(osd_overlay_.ScalePercent()), 0);
    SendMessageW(scale_combo, WM_SETREDRAW, TRUE, 0);
    ::InvalidateRect(scale_combo, nullptr, TRUE);
}

void MainDialog::RefreshSettingsDirtyState() {
    if (apply_pending_) return;
    bool normal_ok{}, safe_ok{}, temp_ok{};
    const UINT normal = ReadUnsignedControl(IDC_NORMAL_POWER, normal_ok);
    const UINT safe = ReadUnsignedControl(IDC_SAFE_POWER, safe_ok);
    const UINT trigger = ReadUnsignedControl(IDC_TRIGGER_TEMP, temp_ok);
    const bool auto_restore = IsDlgButtonChecked(IDC_AUTO_RESTORE) == BST_CHECKED;
    const bool dirty = settings_unsaved_ || !normal_ok || !safe_ok || !temp_ok ||
        normal != static_cast<UINT>(config_.normal_power_w) ||
        safe != static_cast<UINT>(config_.safe_power_w) ||
        trigger != static_cast<UINT>(config_.trigger_temperature_c) ||
        auto_restore != config_.auto_restore;
    SetSettingsDirty(dirty);
}

namespace {

// Save & Apply is a plain push button whenever it has nothing to say, so the
// system and the theme draw it exactly like its neighbours -- shape, outline,
// hover, pressed and focus included. Only while settings are unsaved does it
// become owner-drawn, for the orange that says so. Drawing the clean state by
// hand never matched: in Light it came out grey where the others are near
// white, and in Dark its outline differed.
//
// Owner-drawn buttons answer WM_GETDLGCODE without DLGC_*DEFPUSHBUTTON, so the
// dialog manager leaves the style alone while it is orange; switching back to
// a push button hands it back.
void SetSaveButtonOwnerDrawn(const HWND button, const bool owner_drawn) noexcept {
    if (button == nullptr) return;
    const LONG_PTR style = ::GetWindowLongPtrW(button, GWL_STYLE);
    const LONG_PTR type = owner_drawn ? BS_OWNERDRAW : BS_PUSHBUTTON;
    if ((style & BS_TYPEMASK) == type) return;
    ::SetWindowLongPtrW(button, GWL_STYLE, (style & ~static_cast<LONG_PTR>(BS_TYPEMASK)) | type);
    ::SetWindowPos(button, nullptr, 0, 0, 0, 0,
                   SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
}

}  // namespace

void MainDialog::SetSettingsDirty(const bool dirty) {
    if (settings_dirty_ == dirty) return;
    settings_dirty_ = dirty;
    SetControlText(IDC_SAVE_SETTINGS,
        settings_dirty_
            ? localization::Select(L"保存並套用 ●", L"Save && Apply ●")
            : localization::Select(L"保存並套用", L"Save && Apply"));
    HWND button = GetDlgItem(IDC_SAVE_SETTINGS);
    SetSaveButtonOwnerDrawn(button, settings_dirty_);
    if (button != nullptr) ::InvalidateRect(button, nullptr, TRUE);
}

void MainDialog::UpdateWallTime() {
    SetControlText(IDC_WALL_TIME, snapshot::FormatDisplayTime(snapshot::LocalWallTime()));
}

void MainDialog::RequestUiSnapshot(const snapshot::Reason reason,
                                   const bool notify_user) noexcept {
    try {
        const bool post_message = snapshot_requests_.empty();
        snapshot_requests_.push_back({reason, snapshot::LocalWallTime(), notify_user});
        if (post_message) PostMessageW(kCaptureSnapshotMessage);
    } catch (...) {
        // Snapshot diagnostics are downstream of verified power intervention
        // and must never unwind into the protection state machine.
        logging::Error(L"unable to queue UI incident snapshot");
    }
}

void MainDialog::ReloadSnapshotIcon() {
    HWND control = GetDlgItem(IDC_CAPTURE_SNAPSHOT);
    HICON replacement = LoadDpiIcon(control, IDI_CAMERA, 24);
    if (replacement == nullptr) return;
    SendDlgItemMessageW(IDC_CAPTURE_SNAPSHOT, BM_SETIMAGE, IMAGE_ICON,
                        reinterpret_cast<LPARAM>(replacement));
    if (snapshot_icon_ != nullptr) DestroyIcon(snapshot_icon_);
    snapshot_icon_ = replacement;
}

void MainDialog::RestoreMainWindowPosition() {
    const auto saved = settings::LoadMainWindowPosition();
    if (saved.has_position) {
        SetWindowPos(nullptr, saved.x, saved.y, 0, 0,
                     SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        if (FitMainWindowToWorkArea()) {
            logging::Info(L"saved main window placement repaired for current displays");
            SaveMainWindowPosition();
        }
        return;
    }
    CenterWindow();
}

bool MainDialog::FitMainWindowToWorkArea() {
    RECT bounds{};
    if (GetWindowRect(&bounds) == FALSE) return false;
    const int width = bounds.right - bounds.left;
    const int height = bounds.bottom - bounds.top;
    HMONITOR monitor = MonitorFromRect(&bounds, MONITOR_DEFAULTTONEAREST);
    MONITORINFO info{sizeof(info)};
    if (monitor == nullptr || GetMonitorInfoW(monitor, &info) == FALSE) return false;

    const int fitted_x = std::clamp(
        bounds.left, info.rcWork.left,
        std::max(info.rcWork.left, info.rcWork.right - width));
    const int fitted_y = std::clamp(
        bounds.top, info.rcWork.top,
        std::max(info.rcWork.top, info.rcWork.bottom - height));
    if (fitted_x == bounds.left && fitted_y == bounds.top) return false;
    SetWindowPos(nullptr, fitted_x, fitted_y, 0, 0,
                 SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    return true;
}

void MainDialog::SaveMainWindowPosition() noexcept {
    if (m_hWnd == nullptr || IsIconic()) return;
    RECT bounds{};
    if (GetWindowRect(&bounds) == FALSE) return;
    std::wstring error;
    if (!settings::SaveMainWindowPosition(bounds.left, bounds.top, error)) {
        logging::Warning(error);
    }
}

std::optional<double> MainDialog::SampleCpuUtilization() {
    FILETIME idle_time{}, kernel_time{}, user_time{};
    if (!GetSystemTimes(&idle_time, &kernel_time, &user_time)) return std::nullopt;
    const auto to_ticks = [](const FILETIME& value) {
        ULARGE_INTEGER ticks{};
        ticks.LowPart = value.dwLowDateTime;
        ticks.HighPart = value.dwHighDateTime;
        return ticks.QuadPart;
    };
    const std::uint64_t idle = to_ticks(idle_time);
    const std::uint64_t kernel = to_ticks(kernel_time);
    const std::uint64_t user = to_ticks(user_time);
    if (!previous_cpu_idle_ || !previous_cpu_kernel_ || !previous_cpu_user_) {
        previous_cpu_idle_ = idle;
        previous_cpu_kernel_ = kernel;
        previous_cpu_user_ = user;
        return std::nullopt;
    }
    if (idle < *previous_cpu_idle_ || kernel < *previous_cpu_kernel_ ||
        user < *previous_cpu_user_) {
        previous_cpu_idle_ = idle;
        previous_cpu_kernel_ = kernel;
        previous_cpu_user_ = user;
        return std::nullopt;
    }
    const std::uint64_t idle_delta = idle - *previous_cpu_idle_;
    const std::uint64_t total_delta = (kernel - *previous_cpu_kernel_) +
                                      (user - *previous_cpu_user_);
    previous_cpu_idle_ = idle;
    previous_cpu_kernel_ = kernel;
    previous_cpu_user_ = user;
    if (total_delta == 0 || idle_delta > total_delta) return std::nullopt;
    return std::clamp(100.0 * static_cast<double>(total_delta - idle_delta) /
                      static_cast<double>(total_delta), 0.0, 100.0);
}

UINT MainDialog::ReadUnsignedControl(int control_id, bool& ok) const {
    BOOL translated = FALSE;
    const UINT value = GetDlgItemInt(control_id, &translated, FALSE);
    ok = translated != FALSE;
    return value;
}

void MainDialog::RefreshThemeChoices() {
    HWND combo = GetDlgItem(IDC_THEME);
    if (combo == nullptr) return;
    SendMessageW(combo, WM_SETREDRAW, FALSE, 0);
    SendMessageW(combo, CB_RESETCONTENT, 0, 0);
    // Same order as settings::UiTheme, so the index is the value.
    for (const std::wstring_view entry : {
             localization::Select(L"自動（跟隨 Windows）", L"Auto (follow Windows)"),
             localization::Select(L"深色", L"Dark"),
             localization::Select(L"淺色", L"Light")}) {
        SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(entry.data()));
    }
    SendMessageW(combo, CB_SETCURSEL, static_cast<WPARAM>(ui_theme_), 0);
    SendMessageW(combo, WM_SETREDRAW, TRUE, 0);
    ::InvalidateRect(combo, nullptr, TRUE);
}

// One path for every change: the reader's choice, the Windows app mode, high
// contrast. Configure resolves what the window will actually be.
void MainDialog::ApplyUiTheme() {
    const auto appearance = theme::Configure(ui_theme_);
    // The tab-texture background belongs to the light look; dark paints its
    // own and the texture would show through behind statics.
    (void)EnableThemeDialogTexture(
        m_hWnd, appearance == settings::Appearance::Dark ? ETDT_DISABLE : ETDT_ENABLE);
    history_chart_.SetPalette(theme::HistoryChartPalette());
    theme::Repaint(m_hWnd);
    AlignSettingsEdits(m_hWnd);
    logging::Info(std::format(L"ui theme applied; preference={} appearance={}",
                              settings::RegistryValue(ui_theme_),
                              appearance == settings::Appearance::Dark ? L"dark" : L"classic"));
}

LRESULT MainDialog::OnUiThemeSelected(WORD, WORD, HWND, BOOL&) {
    const LRESULT selection = SendDlgItemMessageW(IDC_THEME, CB_GETCURSEL, 0, 0);
    if (selection == CB_ERR || selection > static_cast<LRESULT>(settings::UiTheme::Light))
        return 0;
    ui_theme_ = static_cast<settings::UiTheme>(selection);
    std::wstring error;
    if (!settings::SaveUiThemePreference(ui_theme_, error)) logging::Warning(error);
    ApplyUiTheme();
    return 0;
}

LRESULT MainDialog::OnUiSettingChange(UINT, const WPARAM wparam, const LPARAM lparam,
                                      BOOL& handled) {
    handled = FALSE;   // the display handler still needs to see it
    const auto* area = reinterpret_cast<const wchar_t*>(lparam);
    const bool color_scheme =
        area != nullptr && std::wstring_view(area) == L"ImmersiveColorSet";
    // High contrast overrides every choice, so it is followed whatever was
    // chosen; the app mode only matters to Auto.
    if (wparam == SPI_SETHIGHCONTRAST ||
        (color_scheme && ui_theme_ == settings::UiTheme::Auto)) {
        ApplyUiTheme();
    }
    return 0;
}

LRESULT MainDialog::OnOverlayToggle(WORD, WORD, HWND, BOOL&) {
    const bool enabled = IsDlgButtonChecked(IDC_OVERLAY_ENABLED) == BST_CHECKED;
#ifdef GTG_FEATURE_OVERLAY
    const auto previous = overlay::integration::GetStatus();
    if (!overlay::integration::Configure(enabled, overlay_hotkey_.modifiers, overlay_hotkey_.key)) {
        CheckDlgButton(IDC_OVERLAY_ENABLED, previous.enabled ? BST_CHECKED : BST_UNCHECKED);
        SetStatus(localization::Select(L"Overlay 熱鍵無法註冊；請更換組合鍵。",
                                       L"Overlay hotkey unavailable; choose another shortcut."),
                  StatusVisual::Warning);
        return 0;
    }
#endif
    std::wstring error;
    if (!settings::SaveOverlayEnabled(enabled, error)) {
#ifdef GTG_FEATURE_OVERLAY
        if (!overlay::integration::Configure(previous.enabled, previous.modifiers, previous.key)) {
            (void)overlay::integration::Configure(false);
            logging::Warning(L"Overlay binding rollback failed; overlay disabled");
        }
#endif
        CheckDlgButton(IDC_OVERLAY_ENABLED, enabled ? BST_UNCHECKED : BST_CHECKED);
#ifdef GTG_FEATURE_OVERLAY
        CheckDlgButton(IDC_OVERLAY_ENABLED,
                       overlay::integration::GetStatus().enabled ? BST_CHECKED : BST_UNCHECKED);
#endif
        logging::Warning(error);
        return 0;
    }
    logging::Info(std::format(
        L"overlay preference saved; enabled={}", enabled));
    return 0;
}

LRESULT MainDialog::OnOverlayAutoSdr(WORD, WORD, HWND, BOOL&) {
    const bool enabled = IsDlgButtonChecked(IDC_OVERLAY_AUTO_SDR) == BST_CHECKED;
    std::wstring error;
    if (!settings::SaveOverlayAutoSdr(enabled, error)) {
        CheckDlgButton(IDC_OVERLAY_AUTO_SDR, enabled ? BST_UNCHECKED : BST_CHECKED);
        logging::Warning(error);
        return 0;
    }
    logging::Info(std::format(L"overlay Auto SDR preference saved; enabled={}; new sessions only", enabled));
    return 0;
}

LRESULT MainDialog::OnOverlayHidesOsd(WORD, WORD, HWND, BOOL&) {
    const bool enabled = IsDlgButtonChecked(IDC_OVERLAY_HIDES_OSD) == BST_CHECKED;
    std::wstring error;
    if (!settings::SaveOverlayHidesOsd(enabled, error)) {
        CheckDlgButton(IDC_OVERLAY_HIDES_OSD, enabled ? BST_UNCHECKED : BST_CHECKED);
        logging::Warning(error);
        return 0;
    }
#ifdef GTG_FEATURE_OVERLAY
    overlay::integration::SetHidesOsd(enabled);
#endif
    logging::Info(std::format(L"overlay hides desktop OSD in game; enabled={}", enabled));
    return 0;
}

LRESULT MainDialog::OnOverlayHotkey(WORD, WORD, HWND, BOOL&) {
    OverlayHotkeyDialog dialog(overlay_hotkey_);
    if (dialog.DoModal(m_hWnd) != IDOK) return 0;
#ifdef GTG_FEATURE_OVERLAY
    const auto previous = overlay::integration::GetStatus();
    if (!overlay::integration::Configure(previous.enabled, dialog.hotkey().modifiers,
                                         dialog.hotkey().key)) {
        SetStatus(localization::Select(L"Overlay 熱鍵無法註冊；保留原組合鍵。",
                                       L"Overlay hotkey unavailable; previous shortcut retained."),
                  StatusVisual::Warning);
        return 0;
    }
#endif
    std::wstring error;
    if (!settings::SaveOverlayHotkey(dialog.hotkey(), error)) {
#ifdef GTG_FEATURE_OVERLAY
        if (!overlay::integration::Configure(previous.enabled, previous.modifiers, previous.key)) {
            (void)overlay::integration::Configure(false);
            logging::Warning(L"Overlay binding rollback failed; overlay disabled");
        }
        CheckDlgButton(IDC_OVERLAY_ENABLED,
                       overlay::integration::GetStatus().enabled ? BST_CHECKED : BST_UNCHECKED);
#endif
        logging::Warning(error);
        return 0;
    }
    overlay_hotkey_ = dialog.hotkey();
    logging::Info(std::format(
        L"overlay hotkey saved; hotkey={}",
        HotkeyText(overlay_hotkey_)));
    return 0;
}

}  // namespace gtg::tray
