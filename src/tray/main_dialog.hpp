#pragma once

#include <cstdint>
#include <string>
#include <deque>
#include <optional>
#include <string_view>
#include <unordered_map>
#include <atomic>
#include <thread>

#include <atlbase.h>
#include <atlapp.h>
#include <atlwin.h>
#include <atldlgs.h>

#include "core/power_envelope.hpp"
#include "core/protection.hpp"
#include "core/restore_prompt_gate.hpp"
#include "core/restore_policy.hpp"
#include "nvml/nvml_loader.hpp"
#include "resources/resource.h"
#include "settings/ui_preferences.hpp"
#include "ipc/protocol.hpp"
#include "tray/history_chart.hpp"
#include "tray/osd_overlay.hpp"
#include "telemetry/telemetry_history.hpp"
#include "snapshot/ui_snapshot.hpp"
#include "protection/local_protection_worker.hpp"
#include "fps/dxgi_observer.hpp"
#include "fps/fps_history.hpp"
#include "net/history.hpp"
#include "net/throughput_win32.hpp"
#include "sysmem/host_memory.hpp"
#include "sysmem/reclaim_nt.hpp"
#include "topapp/alert_rule.hpp"
#include "topapp/board.hpp"
#include "topapp/sampler.hpp"
#include "tray/hosts_controller.hpp"
#ifdef GTG_FEATURE_UPDATE
#include "update/hosts_updates.hpp"
#include "update/tray_updates.hpp"
#include "update/update_resource.h"
#endif

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
        MESSAGE_HANDLER(kDeepCleanDoneMessage, OnDeepCleanDone)
        MESSAGE_HANDLER(WM_THEMECHANGED, OnThemeChanged)
        MESSAGE_HANDLER(WM_DPICHANGED, OnDpiChanged)
        MESSAGE_HANDLER(WM_DISPLAYCHANGE, OnDisplayConfigurationChanged)
        // First, and declines the message, so the display handler below still
        // sees every WM_SETTINGCHANGE.
        MESSAGE_HANDLER(WM_SETTINGCHANGE, OnUiSettingChange)
        MESSAGE_HANDLER(WM_SETTINGCHANGE, OnDisplayConfigurationChanged)
        MESSAGE_HANDLER(WM_EXITSIZEMOVE, OnExitSizeMove)
        MESSAGE_HANDLER(WM_POWERBROADCAST, OnPowerBroadcast)
        MESSAGE_HANDLER(WM_ENDSESSION, OnEndSession)
        MESSAGE_HANDLER(WM_CTLCOLORSTATIC, OnCtlColorStatic)
        MESSAGE_HANDLER(WM_DRAWITEM, OnDrawItem)
        MESSAGE_HANDLER(kTrayMessage, OnTrayMessage)
        MESSAGE_HANDLER(kInitializeTrayMessage, OnInitializeTray)
        MESSAGE_HANDLER(kFeatureNoticeMessage, OnFeatureNotice)
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
        COMMAND_HANDLER(IDC_SHOW_NET, BN_CLICKED, OnNetToggle)
        COMMAND_HANDLER(IDC_SHOW_TOP_APP, BN_CLICKED, OnTopAppToggle)
        COMMAND_HANDLER(IDC_LANGUAGE, CBN_SELCHANGE, OnLanguageChanged)
        COMMAND_HANDLER(IDC_THEME, CBN_SELCHANGE, OnUiThemeSelected)
        COMMAND_HANDLER(IDC_OVERLAY_ENABLED, BN_CLICKED, OnOverlayToggle)
        COMMAND_HANDLER(IDC_OVERLAY_AUTO_SDR, BN_CLICKED, OnOverlayAutoSdr)
        COMMAND_HANDLER(IDC_OVERLAY_HIDES_OSD, BN_CLICKED, OnOverlayHidesOsd)
        COMMAND_ID_HANDLER(IDC_OVERLAY_HOTKEY, OnOverlayHotkey)
        COMMAND_ID_HANDLER(IDC_HOSTS, OnHosts)
        COMMAND_ID_HANDLER(IDM_TRAY_HOSTS, OnHosts)
        COMMAND_ID_HANDLER(IDM_TRAY_HOSTS_LOG, OnTrayHostsLog)
        COMMAND_HANDLER(IDC_OSD_SCALE, CBN_SELCHANGE, OnOsdScaleChanged)
        COMMAND_HANDLER(IDC_OSD_SCALE, CBN_DROPDOWN, OnOsdScaleDropDown)
        COMMAND_HANDLER(IDC_OSD_CARD, BN_CLICKED, OnOsdCardToggle)
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
        COMMAND_ID_HANDLER(IDM_TRAY_DEEP_MEMORY_CLEAN, OnDeepMemoryClean)
        COMMAND_ID_HANDLER(IDM_TRAY_EXIT, OnTrayExit)
#ifdef GTG_FEATURE_UPDATE
        COMMAND_RANGE_HANDLER(IDM_TRAY_UPDATE_CHECK, IDM_TRAY_UPDATE_SETTINGS, OnUpdateCommand)
#endif
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
    static constexpr UINT kDeepCleanDoneMessage = WM_APP + 48;
    static constexpr UINT kFeatureNoticeMessage = WM_APP + 49;

    // Every message this window posts to itself, checked for collisions at
    // compile time.
    //
    // This is not hypothetical tidiness. `kDeepCleanDoneMessage` was added at
    // WM_APP + 47, which `kRepairMainPlacementMessage` already held, and ATL's
    // message map takes the first matching entry -- so every display or DPI
    // change was delivered to `OnDeepCleanDone`, which read a value-initialised
    // status (the first enumerator, `Succeeded`) and announced that the system
    // had been asked to do a deep clean. The reader had asked for nothing; a
    // monitor had woken up. The placement repair the message was for never ran
    // at all, which is why the main window stopped re-fitting itself after a
    // layout change.
    //
    // A duplicate number is invisible to the compiler and produces a dialog
    // claiming a system-wide operation nobody requested, which is the worst
    // shape of wrong this program has. So it is now impossible to add one.
public:
    static constexpr UINT kSelfMessages[]{
        kTrayMessage,       kInitializeTrayMessage, kVerifyTrayMessage,
        kCaptureSnapshotMessage, kReloadSnapshotIconMessage,
        kRepairMainPlacementMessage, kDeepCleanDoneMessage,
        kFeatureNoticeMessage,
    };
    [[nodiscard]] static constexpr bool MessagesAreDistinct() noexcept {
        for (std::size_t i = 0; i < std::size(kSelfMessages); ++i)
            for (std::size_t j = i + 1; j < std::size(kSelfMessages); ++j)
                if (kSelfMessages[i] == kSelfMessages[j]) return false;
        return true;
    }
    // The assertion itself lives in main_dialog.cpp: inside the class body the
    // class is still incomplete and the array cannot be read as a constant.
private:
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
    LRESULT OnFeatureNotice(UINT, WPARAM, LPARAM, BOOL&);
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
    LRESULT OnNetToggle(WORD, WORD, HWND, BOOL&);
    LRESULT OnTopAppToggle(WORD, WORD, HWND, BOOL&);
    // On the 200 ms tick: which records are in alert, which names to float.
    void UpdateTopApps(std::uint64_t now_ms);
    void ClearTopApps();
    LRESULT OnResetOsdLayout(WORD, WORD, HWND, BOOL&);
    LRESULT OnDeepMemoryClean(WORD, WORD, HWND, BOOL&);
    LRESULT OnDeepCleanDone(UINT, WPARAM, LPARAM, BOOL&);
    void PersistCompactArrangement();
    LRESULT OnLanguageChanged(WORD, WORD, HWND, BOOL&);
    // Theme, and the overlay settings that are
    // stored but not yet acted on.
    LRESULT OnUiThemeSelected(WORD, WORD, HWND, BOOL&);
    LRESULT OnUiSettingChange(UINT, WPARAM, LPARAM, BOOL&);
    LRESULT OnOverlayToggle(WORD, WORD, HWND, BOOL&);
    LRESULT OnOverlayAutoSdr(WORD, WORD, HWND, BOOL&);
    LRESULT OnOverlayHidesOsd(WORD, WORD, HWND, BOOL&);
    LRESULT OnOverlayHotkey(WORD, WORD, HWND, BOOL&);
    LRESULT OnHosts(WORD, WORD, HWND, BOOL&);
    LRESULT OnTrayHostsLog(WORD, WORD, HWND, BOOL&);
    // Sleep and sign-out: paired hosts are told this machine is going, so
    // they show it offline rather than lost.
    LRESULT OnPowerBroadcast(UINT, WPARAM, LPARAM, BOOL&);
    LRESULT OnEndSession(UINT, WPARAM, LPARAM, BOOL&);
    // What this machine sends its paired hosts, from what the dashboard holds.
    [[nodiscard]] hosts::LocalState LocalHostState(std::uint64_t now_ms) const;
    void ApplyUiTheme();
    void RefreshThemeChoices();
    // Runs before darkmodelib's own subclass, so the coloured readings keep
    // their colours: darkmodelib answers every WM_CTLCOLORSTATIC otherwise.
    static LRESULT CALLBACK ReadingColorSubclass(HWND window, UINT message, WPARAM wparam,
                                                 LPARAM lparam, UINT_PTR id, DWORD_PTR data);
    LRESULT OnOsdScaleChanged(WORD, WORD, HWND, BOOL&);
    LRESULT OnOsdScaleDropDown(WORD, WORD, HWND, BOOL&);
    LRESULT OnOsdCardToggle(WORD, WORD, HWND, BOOL&);
    void RefreshOsdScaleChoices();
    LRESULT OnProtectionSettingChanged(WORD, WORD, HWND, BOOL&);
    LRESULT OnManualSnapshot(WORD, WORD, HWND, BOOL&);
    LRESULT OnResetTriggerCount(WORD, WORD, HWND, BOOL&);
    LRESULT OnTrayOpen(WORD, WORD, HWND, BOOL&);
    LRESULT OnTrayRefresh(WORD, WORD, HWND, BOOL&);
    LRESULT OnTrayOpenLog(WORD, WORD, HWND, BOOL&);
    LRESULT OnTrayToggleOsd(WORD, WORD, HWND, BOOL&);
    LRESULT OnTrayExit(WORD, WORD, HWND, BOOL&);
#ifdef GTG_FEATURE_UPDATE
    LRESULT OnUpdateCommand(WORD, WORD id, HWND, BOOL&);
    void StartUpdates();
    // What the updater needs to know to pick a moment that costs no protection.
    [[nodiscard]] update::SafeInputs UpdateSafeInputs(std::uint64_t now_ms) const;
    // Updates over Hosts, on the dashboard's tick: requests from paired hosts
    // to the updater, this machine's status and the reader's triggers to the
    // link.
    void TickHostsUpdates(std::uint64_t now_ms);
#endif

    [[nodiscard]] bool AddTrayIcon();
    void RemoveTrayIcon() noexcept;
    void ShowTrayMenu();
    void SetOsdVisible(bool visible, bool persist);
    void ShowMainWindow();
    void RevealProtectionWithoutActivation();
    void RefreshSnapshot();
    void RenderUnavailable(const std::wstring& status);
    void RenderSnapshot(const nvml::DeviceSnapshot& snapshot);
    void ApplyFirstRunDefaults(const nvml::DeviceSnapshot& snapshot);
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
    net::History net_history_;
    // Opened only while the record is shown: an adapter nobody is
    // looking at is a module handle and a route lookup for nothing.
    net::Sampler net_sampler_;
    // Created once and queried on the presentation tick; the query is
    // documented as non-blocking.
    sysmem::LowMemorySignal low_memory_signal_;
    // Deep Memory Clean runs off the message loop. Doing it inline froze the
    // UI for as long as the call took, which the owner saw immediately.
    std::thread deep_clean_worker_;
    std::atomic<bool> deep_clean_running_{false};
    sysmem::reclaim::nt::Status deep_clean_status_{};
    // How long the request took. Not how long the reclaim takes: the call
    // only asks the memory manager to start.
    std::uint64_t deep_clean_elapsed_ms_{};
    std::uint64_t saved_compact_layout_{};
    bool saved_compact_locked_{true};
    HistoryChart history_chart_;
    settings::UiTheme ui_theme_{settings::UiTheme::Auto};
    settings::OverlayHotkey overlay_hotkey_{settings::kDefaultOverlayHotkey};
    HWND tooltip_{nullptr};
    std::wstring tooltip_text_;   // owned here: a tooltip keeps the pointer it is given
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
    // Off unless asked for; see settings::LoadNetEnabled.
    bool show_net_{false};
    std::uint64_t last_net_sample_ms_{};
    // The app behind a sustained high load. The rules run here on the tick;
    // the sweep runs on the sampler's own thread and never on this one.
    bool show_top_app_{true};
    topapp::Sampler top_app_sampler_;
    std::array<topapp::AlertRule, topapp::kRecordCount> top_app_rules_{
        topapp::AlertRule(topapp::kGpuThresholds), topapp::AlertRule(topapp::kVramThresholds),
        topapp::AlertRule(topapp::kCpuThresholds), topapp::AlertRule(topapp::kRamThresholds)};
    topapp::Board top_app_board_{};
    // The records the top-app rules have in alert on this tick, as Bit()
    // flags: Tiny raises its bars by them, and paired hosts are sent them.
    std::uint32_t top_app_alerts_{};
    // Paired GTGs. The link thread exists only while Hosts is on.
    HostsController hosts_;
#ifdef GTG_FEATURE_UPDATE
    // Updates from the source this machine's administrator configured.
    update::TrayUpdates updates_;
    // Armed or monitoring only, and not latched, as of the last snapshot.
    bool protection_idle_{};
    // Asking paired hosts to update.
    update::HostsUpdates host_updates_;
    std::uint64_t last_update_status_ms_{};
    // The tooltip SetTrayVisual was last given and the update line it added,
    // so the tick can redo the tray icon when only the badge changes.
    std::wstring tray_base_tooltip_;
    std::wstring tray_badge_line_;
#endif
    // Hands the dashboard what it draws for Hosts, on the presentation tick.
    void UpdateHostsRecord(std::uint64_t now_ms);
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
    // First run means "no stored settings", which until now was
    // indistinguishable from a configured install because LoadResult::loaded
    // was read and then discarded.
    bool first_run_{false};
    bool feature_notice_attempted_{false};
    bool first_run_defaults_applied_{false};
    std::optional<unsigned int> minimum_power_limit_mw_;
    std::optional<unsigned int> maximum_power_limit_mw_;
    std::optional<unsigned int> gpu_max_temperature_c_;
    std::optional<unsigned int> gpu_slowdown_temperature_c_;
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
