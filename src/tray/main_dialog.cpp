#include "tray/main_dialog.hpp"

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
    CenterWindow();
    const auto loaded_settings = settings::Load();
    config_ = loaded_settings.config;
    trigger_count_ = settings::LoadTriggerCount();
    SetDlgItemInt(IDC_NORMAL_POWER, static_cast<UINT>(config_.normal_power_w), FALSE);
    SetDlgItemInt(IDC_SAFE_POWER, static_cast<UINT>(config_.safe_power_w), FALSE);
    SetDlgItemInt(IDC_TRIGGER_TEMP, static_cast<UINT>(config_.trigger_temperature_c), FALSE);
    CheckDlgButton(IDC_AUTO_RESTORE, config_.auto_restore ? BST_CHECKED : BST_UNCHECKED);
    ApplyLocalization();
    CheckDlgButton(IDC_CLOSE_TO_TRAY,
                   settings::LoadCloseToTrayPreference() ? BST_CHECKED : BST_UNCHECKED);
    history_chart_.SubclassWindow(GetDlgItem(IDC_HISTORY));
    history_chart_.SetHistory(&telemetry_history_);
    const settings::OsdPreference osd_preference = settings::LoadOsdPreference();
    bool placement_repaired = false;
    osd_ready_ = osd_overlay_.Initialize(m_hWnd, &telemetry_history_,
        osd_preference.has_position, {osd_preference.x, osd_preference.y}, placement_repaired);
    CheckDlgButton(IDC_OSD_ENABLED,
                   osd_ready_ && osd_preference.enabled ? BST_CHECKED : BST_UNCHECKED);
    if (placement_repaired && osd_ready_) {
        const POINT position = osd_overlay_.Position();
        std::wstring position_error;
        if (settings::SaveOsdPosition(position.x, position.y, position_error)) {
            logging::Info(L"OSD placement repaired to a visible monitor work area");
        } else {
            logging::Warning(position_error);
        }
    }
    taskbar_created_message_ = RegisterWindowMessageW(L"TaskbarCreated");
    activate_message_ = RegisterWindowMessageW(L"GpuThermalGuard.Activate.v1");
    nvml_ready_ = nvml_.Initialize();
    logging::Info(nvml_ready_
        ? std::format(L"NVML initialized; driver={}", Utf8ToWide(nvml_.driver_version()))
        : std::format(L"NVML initialization failed: {}", Utf8ToWide(nvml_.last_error())));
    controller_ = std::make_unique<ProtectionController>(config_);
    if (settings::LoadSafeLatch()) {
        (void)controller_->RestorePersistedSafeLatch();
        logging::Warning(L"persisted safe-power latch restored at startup");
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
        DestroyWindow();
    }
    return 0;
}

LRESULT MainDialog::OnDestroy(UINT, WPARAM, LPARAM, BOOL&) {
    KillTimer(kRefreshTimer);
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
    handled = FALSE;
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

LRESULT MainDialog::OnOsdPlacementChanged(UINT, const WPARAM repaired, LPARAM, BOOL&) {
    if (!osd_ready_) return 0;
    const POINT position = osd_overlay_.Position();
    std::wstring error;
    if (!settings::SaveOsdPosition(position.x, position.y, error)) {
        logging::Warning(error);
    } else if (repaired != FALSE) {
        logging::Info(L"OSD placement repaired after display configuration change");
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
    if (service_connected_) {
        MessageBoxW(localization::Select(L"Service 正在執行。請先停止 Service 再修改設定，避免兩邊使用不同參數。",
                                         L"The Service is running. Stop it before changing settings so both components use the same parameters.").data(),
                    L"GPU Thermal Guard", MB_OK | MB_ICONWARNING);
        return 0;
    }
    if (controller_ && controller_->safe_latched()) {
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
    std::wstring save_error;
    if (!settings::Save(candidate, save_error)) {
        MessageBoxW(save_error.c_str(), localization::Select(L"保存設定失敗", L"Save Failed").data(), MB_OK | MB_ICONERROR);
        return 0;
    }
    config_ = candidate;
    controller_ = std::make_unique<ProtectionController>(config_);
    history_chart_.SetThresholds(config_.trigger_temperature_c, config_.safe_power_w,
                                 maximum_power_limit_mw_
                                     ? std::optional<double>(*maximum_power_limit_mw_ / 1000.0)
                                     : std::nullopt);
    osd_overlay_.SetThresholds(config_.trigger_temperature_c, config_.safe_power_w,
                               maximum_power_limit_mw_
                                   ? std::optional<double>(*maximum_power_limit_mw_ / 1000.0)
                                   : std::nullopt);
    logging::Info(std::format(L"settings changed; normal={} W safe={} W trigger={} C auto_restore={}",
        config_.normal_power_w, config_.safe_power_w, config_.trigger_temperature_c,
        config_.auto_restore ? L"true" : L"false"));
    restore_prompt_gate_.Reset();
    trigger_count_ = 0;
    std::wstring counter_error;
    if (!settings::SaveTriggerCount(trigger_count_, counter_error)) logging::Warning(counter_error);
    ApplyLocalization();
    MessageBoxW(localization::Select(L"設定已保存到 HKLM，Tray 與 Service 下次啟動時都會使用它。只有觸發保護時才會降低 GPU 功率。",
                                     L"Settings were saved to HKLM. Tray and Service will use them on next start; GPU power is reduced only on a protection trigger.").data(),
                L"GPU Thermal Guard", MB_OK | MB_ICONINFORMATION);
    return 0;
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

LRESULT MainDialog::OnManualSnapshot(WORD, WORD, HWND, BOOL&) {
    RequestUiSnapshot(snapshot::Reason::Manual, true);
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
LRESULT MainDialog::OnTrayExit(WORD, WORD, HWND, BOOL&) { DestroyWindow(); return 0; }

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
    ShowWindow(SW_RESTORE);
    BringWindowToTop();
    if (SetForegroundWindow(m_hWnd) == FALSE) {
        FLASHWINFO flash{sizeof(flash), m_hWnd, FLASHW_TRAY | FLASHW_TIMERNOFG, 3, 0};
        FlashWindowEx(&flash);
    }
}

void MainDialog::RevealProtectionWithoutActivation() {
    if (!IsWindowVisible() || IsIconic()) {
        ShowWindow(SW_SHOWNOACTIVATE);
    }
    FLASHWINFO flash{sizeof(flash), m_hWnd,
                     FLASHW_TRAY | FLASHW_TIMERNOFG, 5, 0};
    FlashWindowEx(&flash);
}

void MainDialog::RefreshSnapshot() {
    if (!nvml_ready_) {
        if (!gpu_unavailable_logged_) {
            logging::Error(std::format(L"GPU unavailable: {}", Utf8ToWide(nvml_.last_error())));
            gpu_unavailable_logged_ = true;
        }
        RenderUnavailable(Utf8ToWide(nvml_.last_error()));
        return;
    }
    const auto devices = nvml_.ProbeDevices();
    if (devices.empty()) {
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
    RenderSnapshot(devices.front());
    ipc::Response service_response{};
    if (ipc::Send(ipc::Command::Query, service_response)) {
        if (!service_connected_) logging::Info(L"connected to protection service; tray is read-only");
        service_connected_ = true;
        JournalSnapshot(devices.front(), nullptr);
        HandleServiceResponse(service_response);
    } else {
        if (service_connected_) {
            logging::Warning(L"protection service disconnected; tray resumed local protection");
            controller_ = std::make_unique<ProtectionController>(config_);
            restore_prompt_gate_.Reset();
            previous_service_latched_.reset();
        }
        service_connected_ = false;
        EvaluateProtection(devices.front());
    }
}

void MainDialog::RenderUnavailable(const std::wstring& status) {
    latest_cpu_utilization_percent_ = SampleCpuUtilization();
    telemetry_history_.AddSample({GetTickCount64(), std::nullopt, std::nullopt,
                                  std::nullopt, std::nullopt, std::nullopt,
                                  latest_cpu_utilization_percent_});
    history_chart_.NotifyDataChanged();
    osd_overlay_.RequestRefresh();
    SetControlText(IDC_GPU_NAME, localization::Select(L"無法取得", L"Unavailable"));
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

void MainDialog::EvaluateProtection(const nvml::DeviceSnapshot& s) {
    if (!controller_ || !s.temperature_c.has_value()) return;
    const auto now = static_cast<std::int64_t>(GetTickCount64());
    ProtectionDecision decision = controller_->ObserveTemperature(now, *s.temperature_c);

    if (decision.action == ProtectionAction::ApplySafePower) {
        const bool applied = nvml_.SetPowerLimitWatts(
            s.index, static_cast<unsigned int>(config_.safe_power_w));
        std::wstring latch_error;
        (void)settings::SaveSafeLatch(true, latch_error);
        logging::Warning(std::format(
            L"TRIGGER reason={} temp={} C rise={:.2f} C/s predicted={:.1f} C; requested={} W result={}",
            Utf8ToWide(ToString(decision.trip_reason)), *s.temperature_c,
            decision.rise_c_per_s, decision.predicted_temperature_c, config_.safe_power_w,
            applied ? L"verified" : L"failed"));
        if (applied) {
            ++trigger_count_;
            std::wstring counter_error;
            if (!settings::SaveTriggerCount(trigger_count_, counter_error)) {
                logging::Warning(counter_error);
            }
            ApplyLocalization();
            logging::Warning(std::format(L"safe power applied and verified: {} W",
                                         config_.safe_power_w));
            SetStatus(localization::Format(L"已觸發保護：安全功率 {} W 已鎖定",
                                           L"Protection triggered: safe power locked at {} W",
                                           config_.safe_power_w), StatusVisual::Protected);
            SetTrayVisual(IDI_TRAY_PROTECTED,
                          localization::Select(L"GPU Thermal Guard — 安全功率已鎖定",
                                               L"GPU Thermal Guard — Safe power locked").data());
            if (decision.trip_reason != TripReason::None) {
                RequestUiSnapshot(snapshot::Reason::ThermalTrigger);
            }
            RevealProtectionWithoutActivation();
            ShowProtectionAlert(localization::Select(L"GPU Thermal Guard 已介入",
                                                      L"GPU Thermal Guard intervened").data(),
                config_.auto_restore
                    ? localization::Select(L"GPU 溫度已達到保護條件，功率已降至安全設定；穩定冷卻後將自動恢復。",
                                           L"GPU temperature reached the protection condition. Safe power is active and will restore automatically after stable cooling.").data()
                    : localization::Select(L"GPU 溫度已達到保護條件，功率已降至安全設定；穩定冷卻後可手動恢復。",
                                           L"GPU temperature reached the protection condition. Safe power is active; restore is manual after stable cooling.").data(),
                NIIF_WARNING);
        } else {
            logging::Error(std::format(L"safe power write failed: {}",
                                       Utf8ToWide(nvml_.last_error())));
            SetStatus(Utf8ToWide(nvml_.last_error()), StatusVisual::Fault);
            SetTrayVisual(IDI_TRAY_FAULT,
                          localization::Select(L"GPU Thermal Guard — 降低功率失敗",
                                               L"GPU Thermal Guard — Power reduction failed").data());
            ShowProtectionAlert(localization::Select(L"GPU Thermal Guard 寫入失敗",
                                                      L"GPU Thermal Guard write failed").data(),
                localization::Select(L"已偵測到高溫，但 NVML 無法套用安全功率。請立即降低負載。",
                                     L"High temperature was detected, but NVML could not apply safe power. Reduce load immediately.").data(), NIIF_ERROR);
        }
        last_safe_retry_ms_ = now;
    }

    // Journaling follows the safety-critical setter so file I/O can never
    // delay the first power-limit intervention.
    JournalSnapshot(s, &decision);
    if (!last_logged_state_.has_value() || *last_logged_state_ != decision.state) {
        logging::Info(std::format(L"protection state -> {}; temp={} C rise={:.2f} C/s predicted={:.1f} C",
            Utf8ToWide(ToString(decision.state)), *s.temperature_c, decision.rise_c_per_s,
            decision.predicted_temperature_c));
        last_logged_state_ = decision.state;
    }

    if (ShouldAutomaticallyRestore(config_.auto_restore, decision.state) &&
        (last_auto_restore_attempt_ms_ == 0 || now - last_auto_restore_attempt_ms_ >= 5'000)) {
        last_auto_restore_attempt_ms_ = now;
        logging::Info(std::format(L"automatic restore eligible; temp={} C target={} W trips={}",
                                  *s.temperature_c, config_.normal_power_w, trigger_count_));
        if (nvml_.SetPowerLimitWatts(s.index,
                                     static_cast<unsigned int>(config_.normal_power_w))) {
            decision = controller_->RequestRestore(now, *s.temperature_c);
            std::wstring latch_error;
            (void)settings::SaveSafeLatch(false, latch_error);
            restore_prompt_gate_.Reset();
            logging::Info(std::format(
                L"automatic normal-power restore verified: {} W; trips retained={}; protection rearmed",
                config_.normal_power_w, trigger_count_));
        } else {
            logging::Error(std::format(
                L"automatic normal-power restore failed; safe latch retained; retry in 5 s: {}",
                Utf8ToWide(nvml_.last_error())));
        }
    }

    if (decision.safe_latched) {
        const bool limit_is_safe = s.configured_power_limit_mw.has_value() &&
            *s.configured_power_limit_mw <= static_cast<unsigned int>(config_.safe_power_w * 1000);
        if (!limit_is_safe && now - last_safe_retry_ms_ >= 2'000) {
            if (nvml_.SetPowerLimitWatts(s.index, static_cast<unsigned int>(config_.safe_power_w))) {
                logging::Warning(std::format(L"safe power re-applied after limit drift: {} W",
                                             config_.safe_power_w));
            } else {
                logging::Error(std::format(L"safe power retry failed: {}",
                                           Utf8ToWide(nvml_.last_error())));
            }
            last_safe_retry_ms_ = now;
        }
        SetTrayVisual(decision.state == ProtectionState::ReadyToRestore ? IDI_TRAY_WARNING :
                      IDI_TRAY_PROTECTED,
                      decision.state == ProtectionState::ReadyToRestore
                          ? (config_.auto_restore
                              ? localization::Select(L"GPU Thermal Guard — 已冷卻，準備自動恢復",
                                                     L"GPU Thermal Guard — Cooled; automatic restore pending").data()
                              : localization::Select(L"GPU Thermal Guard — 已冷卻，等待手動恢復",
                                                     L"GPU Thermal Guard — Cooled; awaiting manual restore").data())
                          : localization::Select(L"GPU Thermal Guard — 安全功率已鎖定",
                                                 L"GPU Thermal Guard — Safe power locked").data());
        SetStatus(
            decision.state == ProtectionState::ReadyToRestore
                ? (config_.auto_restore
                    ? localization::Select(L"GPU 已穩定冷卻，正在準備自動恢復",
                                           L"GPU has cooled; preparing automatic restore")
                    : localization::Select(L"GPU 已穩定冷卻，等待手動恢復；目前仍保持安全功率",
                                           L"GPU has cooled; awaiting manual restore while safe power remains locked"))
                : localization::Select(L"安全功率已鎖定；持續監控中",
                                       L"Safe power is locked; monitoring continues"),
            decision.state == ProtectionState::ReadyToRestore
                ? StatusVisual::Warning : StatusVisual::Protected);
    } else if (decision.state == ProtectionState::PreTrip) {
        SetStatus(localization::Select(L"接近觸發溫度且正在升溫；等待第二次趨勢確認",
                                       L"Near trigger temperature and rising; awaiting trend confirmation"),
                  StatusVisual::Warning);
        SetTrayVisual(IDI_TRAY_WARNING,
                      localization::Select(L"GPU Thermal Guard — 接近觸發溫度",
                                           L"GPU Thermal Guard — Near trigger temperature").data());
    } else if (decision.state == ProtectionState::Armed) {
        SetStatus(nvml_.can_set_power_limit()
            ? localization::Select(L"保護已待命；觸發時將鎖定安全功率",
                                   L"Protection armed; safe power will lock on trigger")
            : localization::Select(L"唯讀監控：此 NVML 不支援設定功率限制",
                                   L"Read-only monitoring: NVML power-limit control unavailable"),
            nvml_.can_set_power_limit() ? StatusVisual::Armed : StatusVisual::Warning);
        SetTrayVisual(IDI_TRAY_ARMED,
            localization::Format(L"GPU Thermal Guard — {} °C · 保護待命",
                                 L"GPU Thermal Guard — {} °C · Armed",
                                 *s.temperature_c).c_str());
    }

    if (!config_.auto_restore && restore_prompt_gate_.ShouldPrompt(
            decision.safe_latched, decision.state == ProtectionState::ReadyToRestore)) {
        ShowMainWindow();
        const int answer = MessageBoxW(
            localization::Format(L"GPU 已持續冷卻到 {} °C 以下。\n\n要將功率恢復為 {} W 嗎？",
                                 L"GPU has remained below {} °C.\n\nRestore power to {} W?",
                                 controller_->recovery_temperature_c(),
                                 config_.normal_power_w).c_str(),
            localization::Select(L"GPU Thermal Guard — 可安全恢復",
                                 L"GPU Thermal Guard — Safe to Restore").data(),
            MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2);
        logging::Info(answer == IDYES ? L"user accepted normal-power restore"
                                      : L"user declined normal-power restore; safe latch retained");
        if (answer == IDYES) {
            if (nvml_.SetPowerLimitWatts(s.index, static_cast<unsigned int>(config_.normal_power_w))) {
                (void)controller_->RequestRestore(now, *s.temperature_c);
                std::wstring latch_error;
                (void)settings::SaveSafeLatch(false, latch_error);
                if (ShouldResetTriggerCount(RestoreInitiator::Manual)) trigger_count_ = 0;
                std::wstring counter_error;
                if (!settings::SaveTriggerCount(trigger_count_, counter_error)) {
                    logging::Warning(counter_error);
                }
                ApplyLocalization();
                SetStatus(localization::Format(L"已手動恢復至 {} W",
                                               L"Manually restored to {} W",
                                               config_.normal_power_w),
                          StatusVisual::Armed);
                SetTrayVisual(IDI_TRAY_ARMED,
                              localization::Select(L"GPU Thermal Guard — 保護已待命",
                                                   L"GPU Thermal Guard — Armed").data());
                logging::Info(std::format(L"normal power restored and verified: {} W",
                                          config_.normal_power_w));
            } else {
                logging::Error(std::format(L"normal power restore failed: {}",
                                           Utf8ToWide(nvml_.last_error())));
                MessageBoxW(Utf8ToWide(nvml_.last_error()).c_str(),
                            localization::Select(L"恢復功率失敗", L"Power Restore Failed").data(),
                            MB_OK | MB_ICONERROR);
            }
        }
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
        ? Utf8ToWide(ToString(decision->state)) : L"ServiceOwned";
    logging::Info(std::format(
        L"snapshot temp_c={} power_w={} vram_used_gib={} vram_total_gib={} vram_pct={} "
        L"gpu_load_pct={} cpu_load_pct={} limit_w={} state={}",
        temperature, power, vram_used, vram_total, vram_percent, gpu_load, cpu_load,
        limit, state));
}

void MainDialog::HandleServiceResponse(const ipc::Response& response) {
    const bool latched = (response.flags & ipc::SafeLatched) != 0;
    const bool ready = (response.flags & ipc::ReadyToRestore) != 0;
    const bool newly_latched = previous_service_latched_.has_value() &&
                               !*previous_service_latched_ && latched;
    const bool newly_counted_trip = HasNewTriggerCount(trigger_count_, response.trigger_count);
    previous_service_latched_ = latched;
    if (trigger_count_ != response.trigger_count) {
        trigger_count_ = response.trigger_count;
        ApplyLocalization();
    }
    if (newly_latched || newly_counted_trip) {
        RequestUiSnapshot(snapshot::Reason::ServiceTrigger);
        RevealProtectionWithoutActivation();
    }
    if (latched) {
        SetStatus(ready
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

    if (!config_.auto_restore && restore_prompt_gate_.ShouldPrompt(latched, ready)) {
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
                if (ShouldResetTriggerCount(RestoreInitiator::Manual)) trigger_count_ = 0;
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
    SetControlText(IDC_TEMP_LABEL, text(L"溫度", L"Temp"));
    SetControlText(IDC_POWER_LABEL, text(L"功率", L"Power"));
    SetControlText(IDC_VRAM_LABEL, L"VRAM");
    SetControlText(IDC_LIMIT_LABEL, text(L"功率限制", L"Power Limit"));
    SetControlText(IDC_THERMAL_LABEL, text(L"韌體門檻", L"T.Limits"));
    SetControlText(IDC_HISTORY_GROUP,
                   text(L"歷史趨勢（5 分鐘視野／1 小時）",
                        L"History (5-minute view / 1 hour)"));
    SetControlText(IDC_SETTINGS_GROUP,
                   text(L"保護設定（觸發時才會寫入 GPU）",
                        L"Protection Settings (written only when triggered)"));
    SetControlText(IDC_NORMAL_POWER_LABEL, text(L"正常功率 (W)", L"Normal Power (W)"));
    SetControlText(IDC_SAFE_POWER_LABEL, text(L"安全功率 (W)", L"Safe Power (W)"));
    SetControlText(IDC_TRIGGER_TEMP_LABEL, text(L"觸發溫度 (°C)", L"Trigger Temp (°C)"));
    SetControlText(IDC_SETTINGS_NOTE,
                   text(L"超溫後鎖定安全功率；穩定冷卻後可手動或選擇自動恢復。",
                        L"Safe power locks after a trip; choose manual or automatic restore after cooling."));
    SetControlText(IDC_AUTO_RESTORE, text(L"自動 Restore", L"Auto Restore"));
    SetControlText(IDC_CLOSE_TO_TRAY, text(L"[X] 隱藏到系統匣", L"[X] Hide to tray"));
    SetControlText(IDC_OSD_ENABLED, text(L"顯示 OSD", L"Show OSD"));
    SetControlText(IDC_SAVE_SETTINGS, text(L"保存並套用", L"Save & Apply"));
    SetControlText(IDC_HIDE_TO_TRAY, text(L"隱藏到系統匣", L"Hide to Tray"));
    SetControlText(IDC_TRIGGER_COUNT,
                   localization::Format(L"觸發次數：{}", L"Trips: {}", trigger_count_));

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
