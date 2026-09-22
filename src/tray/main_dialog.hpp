#pragma once

#include <cstdint>
#include <string>
#include <deque>
#include <optional>
#include <string_view>
#include <unordered_map>

#include <atlbase.h>
#include <atlapp.h>
#include <atlwin.h>
#include <atldlgs.h>

#include "core/protection.hpp"
#include "core/restore_prompt_gate.hpp"
#include "core/restore_policy.hpp"
#include "nvml/nvml_loader.hpp"
#include "resources/resource.h"
#include "ipc/protocol.hpp"
#include "tray/history_chart.hpp"
#include "tray/osd_overlay.hpp"
#include "telemetry/telemetry_history.hpp"
#include "snapshot/ui_snapshot.hpp"
#include "protection/local_protection_worker.hpp"
#include "fps/dxgi_observer.hpp"
#include "fps/fps_history.hpp"
#include "sysmem/host_memory.hpp"

namespace gtg::tray {

class MainDialog final : public ATL::CDialogImpl<MainDialog>, public WTL::CMessageFilter {
public:
    enum { IDD = IDD_MAIN };
    BOOL PreTranslateMessage(MSG* message) override { return IsDialogMessage(message); }

    BEGIN_MSG_MAP(MainDialog)
        MESSAGE_HANDLER(WM_INITDIALOG, OnInitDialog)
        MESSAGE_HANDLER(WM_TIMER, OnTimer)
        MESSAGE_HANDLER(WM_CLOSE, OnClose)
        MESSAGE_HANDLER(WM_DESTROY, OnDestroy)
        MESSAGE_HANDLER(WM_THEMECHANGED, OnThemeChanged)
        MESSAGE_HANDLER(WM_DPICHANGED, OnDpiChanged)
        MESSAGE_HANDLER(WM_DISPLAYCHANGE, OnDisplayConfigurationChanged)
        MESSAGE_HANDLER(WM_SETTINGCHANGE, OnDisplayConfigurationChanged)
        MESSAGE_HANDLER(WM_EXITSIZEMOVE, OnExitSizeMove)
        MESSAGE_HANDLER(WM_CTLCOLORSTATIC, OnCtlColorStatic)
        MESSAGE_HANDLER(WM_DRAWITEM, OnDrawItem)
        MESSAGE_HANDLER(kTrayMessage, OnTrayMessage)
        MESSAGE_HANDLER(kInitializeTrayMessage, OnInitializeTray)
        MESSAGE_HANDLER(kVerifyTrayMessage, OnVerifyTray)
        MESSAGE_HANDLER(kCaptureSnapshotMessage, OnCaptureSnapshot)
        MESSAGE_HANDLER(kReloadSnapshotIconMessage, OnReloadSnapshotIcon)
        MESSAGE_HANDLER(kRepairMainPlacementMessage, OnRepairMainPlacement)
        MESSAGE_RANGE_HANDLER(0xC000, 0xFFFF, OnRegisteredMessage)
        COMMAND_ID_HANDLER(IDC_SAVE_SETTINGS, OnValidateSettings)
        COMMAND_ID_HANDLER(IDC_HIDE_TO_TRAY, OnHideToTray)
        COMMAND_HANDLER(IDC_CLOSE_TO_TRAY, BN_CLICKED, OnCloseBehaviorChanged)
        COMMAND_HANDLER(IDC_OSD_ENABLED, BN_CLICKED, OnOsdToggle)
        COMMAND_HANDLER(IDC_SHOW_FPS, BN_CLICKED, OnFpsToggle)
        COMMAND_HANDLER(IDC_SHOW_RAM, BN_CLICKED, OnRamToggle)
        COMMAND_HANDLER(IDC_LANGUAGE, CBN_SELCHANGE, OnLanguageChanged)
        COMMAND_HANDLER(IDC_NORMAL_POWER, EN_CHANGE, OnProtectionSettingChanged)
        COMMAND_HANDLER(IDC_SAFE_POWER, EN_CHANGE, OnProtectionSettingChanged)
        COMMAND_HANDLER(IDC_TRIGGER_TEMP, EN_CHANGE, OnProtectionSettingChanged)
        COMMAND_HANDLER(IDC_AUTO_RESTORE, BN_CLICKED, OnProtectionSettingChanged)
        COMMAND_ID_HANDLER(IDC_CAPTURE_SNAPSHOT, OnManualSnapshot)
        COMMAND_ID_HANDLER(IDC_RESET_TRIGGER_COUNT, OnResetTriggerCount)
        COMMAND_ID_HANDLER(IDM_TRAY_OPEN, OnTrayOpen)
        COMMAND_ID_HANDLER(IDM_TRAY_REFRESH, OnTrayRefresh)
        COMMAND_ID_HANDLER(IDM_TRAY_OPEN_LOG, OnTrayOpenLog)
        COMMAND_ID_HANDLER(IDM_TRAY_TOGGLE_OSD, OnTrayToggleOsd)
        COMMAND_ID_HANDLER(IDM_TRAY_CAPTURE_SNAPSHOT, OnManualSnapshot)
        COMMAND_ID_HANDLER(IDM_TRAY_RESET_OSD_LAYOUT, OnResetOsdLayout)
        COMMAND_ID_HANDLER(IDM_TRAY_EXIT, OnTrayExit)
    END_MSG_MAP()

private:
    enum class StatusVisual {
        Neutral,
        Armed,
        Warning,
        Protected,
        Fault,
    };

    static constexpr UINT kTrayMessage = WM_APP + 42;
    static constexpr UINT kInitializeTrayMessage = WM_APP + 43;
    static constexpr UINT kVerifyTrayMessage = WM_APP + 44;
    static constexpr UINT kCaptureSnapshotMessage = WM_APP + 45;
    static constexpr UINT kReloadSnapshotIconMessage = WM_APP + 46;
    static constexpr UINT kRepairMainPlacementMessage = WM_APP + 47;
    static constexpr UINT_PTR kRefreshTimer = 1;
    static constexpr UINT kRefreshIntervalMs = 200;

    LRESULT OnInitDialog(UINT, WPARAM, LPARAM, BOOL&);
    LRESULT OnTimer(UINT, WPARAM, LPARAM, BOOL&);
    LRESULT OnClose(UINT, WPARAM, LPARAM, BOOL&);
    LRESULT OnDestroy(UINT, WPARAM, LPARAM, BOOL&);
    LRESULT OnThemeChanged(UINT, WPARAM, LPARAM, BOOL&);
    LRESULT OnDpiChanged(UINT, WPARAM, LPARAM, BOOL&);
    LRESULT OnDisplayConfigurationChanged(UINT, WPARAM, LPARAM, BOOL&);
    LRESULT OnExitSizeMove(UINT, WPARAM, LPARAM, BOOL&);
    LRESULT OnCtlColorStatic(UINT, WPARAM, LPARAM, BOOL&);
    LRESULT OnDrawItem(UINT, WPARAM, LPARAM, BOOL&);
    LRESULT OnTrayMessage(UINT, WPARAM, LPARAM, BOOL&);
    LRESULT OnInitializeTray(UINT, WPARAM, LPARAM, BOOL&);
    LRESULT OnVerifyTray(UINT, WPARAM, LPARAM, BOOL&);
    LRESULT OnCaptureSnapshot(UINT, WPARAM, LPARAM, BOOL&);
    LRESULT OnReloadSnapshotIcon(UINT, WPARAM, LPARAM, BOOL&);
    LRESULT OnRepairMainPlacement(UINT, WPARAM, LPARAM, BOOL&);
    LRESULT OnRegisteredMessage(UINT message, WPARAM, LPARAM, BOOL& handled);
    LRESULT OnValidateSettings(WORD, WORD, HWND, BOOL&);
    LRESULT OnHideToTray(WORD, WORD, HWND, BOOL&);
    LRESULT OnCloseBehaviorChanged(WORD, WORD, HWND, BOOL&);
    LRESULT OnOsdToggle(WORD, WORD, HWND, BOOL&);
    LRESULT OnFpsToggle(WORD, WORD, HWND, BOOL&);
    LRESULT OnRamToggle(WORD, WORD, HWND, BOOL&);
    LRESULT OnResetOsdLayout(WORD, WORD, HWND, BOOL&);
    void PersistCompactArrangement();
    LRESULT OnLanguageChanged(WORD, WORD, HWND, BOOL&);
    LRESULT OnProtectionSettingChanged(WORD, WORD, HWND, BOOL&);
    LRESULT OnManualSnapshot(WORD, WORD, HWND, BOOL&);
    LRESULT OnResetTriggerCount(WORD, WORD, HWND, BOOL&);
    LRESULT OnTrayOpen(WORD, WORD, HWND, BOOL&);
    LRESULT OnTrayRefresh(WORD, WORD, HWND, BOOL&);
    LRESULT OnTrayOpenLog(WORD, WORD, HWND, BOOL&);
    LRESULT OnTrayToggleOsd(WORD, WORD, HWND, BOOL&);
    LRESULT OnTrayExit(WORD, WORD, HWND, BOOL&);

    [[nodiscard]] bool AddTrayIcon();
    void RemoveTrayIcon() noexcept;
    void ShowTrayMenu();
    void SetOsdVisible(bool visible, bool persist);
    void ShowMainWindow();
    void RevealProtectionWithoutActivation();
    void RefreshSnapshot();
    void RenderUnavailable(const std::wstring& status);
    void RenderSnapshot(const nvml::DeviceSnapshot& snapshot);
    void HandleLocalProtection();
    void PollApplyCompletion();
    void JournalSnapshot(const nvml::DeviceSnapshot& snapshot,
                         const ProtectionDecision* decision = nullptr);
    void HandleServiceResponse(const ipc::Response& response);
    void ShowProtectionAlert(const wchar_t* title, const wchar_t* message, DWORD flags);
    void SetTrayVisual(UINT icon_id, const wchar_t* tooltip);
    void SetStatus(std::wstring_view text, StatusVisual visual);
    void SetControlText(int control_id, std::wstring_view text);
    void UpdateWallTime();
    void RequestUiSnapshot(snapshot::Reason reason, bool notify_user = false) noexcept;
    void ReloadSnapshotIcon();
    void RestoreMainWindowPosition();
    [[nodiscard]] bool FitMainWindowToWorkArea();
    void SaveMainWindowPosition() noexcept;
    void ApplyLocalization();
    void RefreshSettingsDirtyState();
    void SetSettingsDirty(bool dirty);
    [[nodiscard]] std::optional<double> SampleCpuUtilization();
    [[nodiscard]] UINT ReadUnsignedControl(int control_id, bool& ok) const;
    [[nodiscard]] static OsdVisual ToOsdVisual(StatusVisual visual) noexcept;

    nvml::Library nvml_;
    telemetry::History telemetry_history_;
    fps::History fps_history_;
    sysmem::History ram_history_;
    std::uint32_t saved_compact_layout_{};
    bool saved_compact_locked_{true};
    HistoryChart history_chart_;
    OsdOverlay osd_overlay_;
    fps::DxgiObserver fps_observer_;
    ProtectionConfig config_{};
    protection::LocalProtectionWorker local_protection_;
    NOTIFYICONDATAW tray_data_{};
    UINT taskbar_created_message_{0};
    UINT activate_message_{0};
    bool tray_added_{false};
    bool osd_ready_{false};
    bool show_fps_{true};
    bool show_ram_{true};
    std::uint64_t last_ram_sample_ms_{};
    std::uint64_t last_fps_refresh_ms_{};
    std::uint64_t last_tray_add_attempt_ms_{0};
    UINT current_tray_icon_id_{0};
    std::wstring current_tray_tooltip_;
    StatusVisual status_visual_{StatusVisual::Neutral};
    HICON status_icon_{nullptr};
    HICON snapshot_icon_{nullptr};
    UINT current_status_icon_id_{0};
    bool nvml_ready_{false};
    RestorePromptGate restore_prompt_gate_;
    unsigned int trigger_count_{0};
    std::uint32_t test_run_baseline_{0};
    std::uint64_t test_run_started_file_time_{0};
    bool service_connected_{false};
    std::uint64_t last_journal_snapshot_ms_{0};
    std::uint64_t last_worker_start_attempt_ms_{0};
    std::uint64_t last_local_trip_sequence_{0};
    bool gpu_unavailable_logged_{false};
    bool settings_dirty_{false};
    bool apply_pending_{false};
    bool settings_unsaved_{false};
    std::optional<unsigned int> minimum_power_limit_mw_;
    std::optional<unsigned int> maximum_power_limit_mw_;
    std::optional<unsigned int> gpu_max_temperature_c_;
    std::optional<std::uint64_t> previous_cpu_idle_;
    std::optional<std::uint64_t> previous_cpu_kernel_;
    std::optional<std::uint64_t> previous_cpu_user_;
    std::optional<double> latest_cpu_utilization_percent_;
    std::optional<double> vram_total_gib_;
    std::optional<bool> previous_service_latched_;
    std::unordered_map<int, std::wstring> control_text_cache_;
    struct SnapshotRequest {
        snapshot::Reason reason{snapshot::Reason::Manual};
        snapshot::WallTime wall_time{};
        bool notify_user{false};
    };
    std::deque<SnapshotRequest> snapshot_requests_;
};

}  // namespace gtg::tray
