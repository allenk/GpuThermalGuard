#include "tray/main_dialog.hpp"
#include "tray/window_position.hpp"
#include "supervision/supervisor.hpp"

#include <algorithm>
#include <format>
#include <chrono>

#include <shellapi.h>
#include <uxtheme.h>

#include "settings/settings.hpp"
#include "ipc/client.hpp"
#include "logging/logger.hpp"
#include "localization/localization.hpp"
#include "snapshot/ui_snapshot.hpp"

namespace gtg::tray {
namespace {

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
    return static_cast<HICON>(LoadImageW(ATL::_AtlBaseModule.GetResourceInstance(),
        MAKEINTRESOURCEW(resource_id), IMAGE_ICON, size, size, LR_DEFAULTCOLOR));
}

HICON LoadDpiIcon(HWND control, const UINT resource_id, const int size_dip) {
    const int size = MulDiv(size_dip, static_cast<int>(GetDpiForWindow(control)), 96);
    return static_cast<HICON>(LoadImageW(ATL::_AtlBaseModule.GetResourceInstance(),
        MAKEINTRESOURCEW(resource_id), IMAGE_ICON, size, size, LR_DEFAULTCOLOR));
}

bool SaveOsdWindowPosition(const OsdOverlay& overlay,
                           const std::wstring_view reason) noexcept {
    const auto position = ReadWindowPosition(overlay.m_hWnd);
    if (!position) return false;
    std::wstring error;
    if (!settings::SaveOsdPosition(position->x, position->y, error)) {
        logging::Warning(error);
        return false;
    }
    logging::Info(std::format(
        L"OSD placement saved; position=({}, {}) reason={}",
        position->x, position->y, reason));
    return true;
}

bool RestoreOsdWindowPosition(HWND notification_window,
                              const telemetry::History* history,
                              OsdOverlay& overlay,
                              settings::OsdPreference& preference) {
    preference = settings::LoadOsdPreference();
    bool placement_repaired = false;
    const bool ready = overlay.Initialize(
        notification_window, history, preference.has_position,
        {preference.x, preference.y}, placement_repaired);
    if (!ready) {
        logging::Warning(L"OSD placement restore failed because overlay initialization failed");
        return false;
    }

    const POINT applied = overlay.Position();
    if (preference.has_position) {
        logging::Info(std::format(
            L"OSD placement restored; saved=({}, {}) applied=({}, {}) repaired={}",
            preference.x, preference.y, applied.x, applied.y, placement_repaired));
    } else {
        logging::Info(std::format(
            L"OSD placement initialized from default; applied=({}, {})",
            applied.x, applied.y));
    }

    return true;
}

}  // namespace

LRESULT MainDialog::OnInitDialog(UINT, WPARAM, LPARAM, BOOL&) {
    localization::SetCurrent(settings::LoadUiLanguagePreference());
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
    settings::OsdPreference osd_preference;
    osd_ready_ = RestoreOsdWindowPosition(
        m_hWnd, &telemetry_history_, osd_overlay_, osd_preference);
    CheckDlgButton(IDC_OSD_ENABLED,
                   osd_ready_ && osd_preference.enabled ? BST_CHECKED : BST_UNCHECKED);
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
    if (!loaded_settings.warning.empty()) {
        SetStatus(loaded_settings.warning, StatusVisual::Warning);
    }
    SetTimer(kRefreshTimer, kRefreshIntervalMs);
    PostMessageW(kInitializeTrayMessage);
    return TRUE;
}

LRESULT MainDialog::OnTimer(UINT, WPARAM id, LPARAM, BOOL&) {
    PollApplyCompletion();
    if (id == kRefreshTimer) {
        const auto now = static_cast<std::uint64_t>(GetTickCount64());
        if (!tray_added_ && now - last_tray_add_attempt_ms_ >= 2'000) {
            if (AddTrayIcon()) PostMessageW(kVerifyTrayMessage);
        }
        RefreshSnapshot();
        UpdateWallTime();
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
    SaveMainWindowPosition();
    // Hidden OSDs retain their HWND/position. Save before any potentially
    // blocking worker shutdown and before destroying the painted overlay.
    if (osd_ready_) (void)SaveOsdWindowPosition(osd_overlay_, L"normal exit");
    local_protection_.Stop();
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
    return 0;
}

LRESULT MainDialog::OnDpiChanged(UINT, WPARAM, LPARAM, BOOL& handled) {
    // Let the dialog manager finish Per-Monitor V2 layout first, then reload an
    // exact-size ICO frame for the new device-pixel scale.
    PostMessageW(kReloadSnapshotIconMessage);
    PostMessageW(kRepairMainPlacementMessage);
    handled = FALSE;
    return 0;
}

LRESULT MainDialog::OnDisplayConfigurationChanged(UINT, WPARAM, LPARAM, BOOL&) {
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
    SetTextColor(dc, text_color);
    SetBkColor(dc, GetSysColor(COLOR_BTNFACE));
    SetBkMode(dc, OPAQUE);
    return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_BTNFACE));
}

LRESULT MainDialog::OnDrawItem(UINT, WPARAM, const LPARAM lparam, BOOL& handled) {
    const auto* item = reinterpret_cast<const DRAWITEMSTRUCT*>(lparam);
    if (item == nullptr || item->CtlType != ODT_BUTTON ||
        item->CtlID != IDC_SAVE_SETTINGS) {
        handled = FALSE;
        return 0;
    }

    const bool selected = (item->itemState & ODS_SELECTED) != 0;
    const bool disabled = (item->itemState & ODS_DISABLED) != 0;
    const COLORREF fill = settings_dirty_
        ? (selected ? RGB(205, 132, 18) : RGB(244, 172, 54))
        : (selected ? GetSysColor(COLOR_3DSHADOW) : GetSysColor(COLOR_BTNFACE));
    const COLORREF border = settings_dirty_ ? RGB(176, 104, 0)
                                            : GetSysColor(COLOR_BTNSHADOW);
    HBRUSH brush = CreateSolidBrush(fill);
    HPEN pen = CreatePen(PS_SOLID, 1, border);
    const HGDIOBJ previous_brush = SelectObject(item->hDC, brush);
    const HGDIOBJ previous_pen = SelectObject(item->hDC, pen);
    RoundRect(item->hDC, item->rcItem.left, item->rcItem.top,
              item->rcItem.right, item->rcItem.bottom, 6, 6);
    SelectObject(item->hDC, previous_pen);
    SelectObject(item->hDC, previous_brush);
    DeleteObject(pen);
    DeleteObject(brush);

    wchar_t label[128]{};
    ::GetWindowTextW(item->hwndItem, label, static_cast<int>(_countof(label)));
    SetBkMode(item->hDC, TRANSPARENT);
    SetTextColor(item->hDC, disabled ? GetSysColor(COLOR_GRAYTEXT)
                                    : (settings_dirty_ ? RGB(45, 31, 8)
                                                       : GetSysColor(COLOR_BTNTEXT)));
    RECT text_rect = item->rcItem;
    if (selected) OffsetRect(&text_rect, 1, 1);
    DrawTextW(item->hDC, label, -1, &text_rect,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    if ((item->itemState & ODS_FOCUS) != 0) {
        RECT focus = item->rcItem;
        InflateRect(&focus, -3, -3);
        DrawFocusRect(item->hDC, &focus);
    }
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
    return 0;
}

LRESULT MainDialog::OnRepairMainPlacement(UINT, WPARAM, LPARAM, BOOL&) {
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
    if (gpu_max_temperature_c_ &&
        candidate.trigger_temperature_c > static_cast<int>(*gpu_max_temperature_c_)) {
        MessageBoxW(localization::Select(L"觸發溫度不能高於 GPU 韌體回報的 Max T.Limit。",
                                         L"Trigger temperature cannot exceed the GPU firmware Max T.Limit.").data(),
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
    SetOsdVisible(!osd_overlay_.Visible(), true);
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
    AppendMenuW(menu, MF_STRING | (osd_overlay_.Visible() ? MF_CHECKED : MF_UNCHECKED),
                IDM_TRAY_TOGGLE_OSD,
                localization::Select(L"顯示即時 OSD", L"Show Live OSD").data());
    AppendMenuW(menu, MF_STRING, IDM_TRAY_CAPTURE_SNAPSHOT,
                localization::Select(L"保存監控快照", L"Save Monitoring Snapshot").data());
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
    if (!nvml_ready_) {
        if (!service_connected_) HandleLocalProtection();
        if (!gpu_unavailable_logged_) {
            logging::Error(std::format(L"GPU unavailable: {}", Utf8ToWide(nvml_.last_error())));
            gpu_unavailable_logged_ = true;
        }
        RenderUnavailable(Utf8ToWide(nvml_.last_error()));
        return;
    }
    const auto devices = nvml_.ProbeDevices();
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
    if (ipc::Send(ipc::Command::Query, service_response)) {
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

void MainDialog::RenderSnapshot(const nvml::DeviceSnapshot& s) {
    minimum_power_limit_mw_ = s.minimum_power_limit_mw;
    maximum_power_limit_mw_ = s.maximum_power_limit_mw;
    gpu_max_temperature_c_ = s.gpu_max_tlimit_c;
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
    const auto snapshot = local_protection_.Snapshot();
    if (trigger_count_ != snapshot.trigger_count) {
        trigger_count_ = snapshot.trigger_count;
        ApplyLocalization();
    }

    if (snapshot.trip_sequence > last_local_trip_sequence_) {
        last_local_trip_sequence_ = snapshot.trip_sequence;
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
    logging::Info(std::format(
        L"snapshot temp_c={} power_w={} vram_used_gib={} vram_total_gib={} vram_pct={} "
        L"gpu_load_pct={} cpu_load_pct={} limit_w={} state={}",
        temperature, power, vram_used, vram_total, vram_percent, gpu_load, cpu_load,
        limit, state));
}

void MainDialog::HandleServiceResponse(const ipc::Response& response) {
    const bool latched = (response.flags & ipc::SafeLatched) != 0;
    const bool ready = (response.flags & ipc::ReadyToRestore) != 0;
    const auto service_state = static_cast<ProtectionState>(response.protection_state);
    const bool newly_latched = previous_service_latched_.has_value() &&
                               !*previous_service_latched_ && latched;
    const bool newly_counted_trip = HasNewTriggerCount(trigger_count_, response.trigger_count);
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
    Shell_NotifyIconW(NIM_MODIFY, &tray_data_);
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
    SetWindowTextW(L"GPU Thermal Guard");
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
    SetControlText(IDC_CLOSE_TO_TRAY, text(L"[X] 隱藏到系統匣", L"[X] Hide to tray"));
    SetControlText(IDC_OSD_ENABLED, text(L"顯示 OSD", L"Show OSD"));
    SetControlText(IDC_SAVE_SETTINGS,
                   apply_pending_ ? text(L"套用中…", L"Applying…")
                   : settings_dirty_ ? text(L"保存並套用 ●", L"Save & Apply ●")
                                   : text(L"保存並套用", L"Save & Apply"));
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

void MainDialog::SetSettingsDirty(const bool dirty) {
    if (settings_dirty_ == dirty) return;
    settings_dirty_ = dirty;
    SetControlText(IDC_SAVE_SETTINGS,
        settings_dirty_
            ? localization::Select(L"保存並套用 ●", L"Save & Apply ●")
            : localization::Select(L"保存並套用", L"Save & Apply"));
    HWND button = GetDlgItem(IDC_SAVE_SETTINGS);
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

}  // namespace gtg::tray
