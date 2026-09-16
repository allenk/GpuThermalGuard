#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include <atlbase.h>
#include <atlapp.h>
#include <atlwin.h>
#include <wtsapi32.h>

#include "telemetry/telemetry_history.hpp"
#include "telemetry/telemetry_freshness.hpp"
#include "tray/osd_compact.hpp"

namespace gtg::tray {

class OsdOverlay;

class OsdDragHandle final : public ATL::CWindowImpl<OsdDragHandle, ATL::CWindow> {
public:
    DECLARE_WND_CLASS_EX(L"GpuThermalGuard.OsdDragHandle.v1", 0, 0)

    BEGIN_MSG_MAP(OsdDragHandle)
        MESSAGE_HANDLER(WM_LBUTTONDOWN, OnTogglePointer)
        MESSAGE_HANDLER(WM_LBUTTONUP, OnTogglePointer)
        MESSAGE_HANDLER(WM_CAPTURECHANGED, OnTogglePointer)
        MESSAGE_HANDLER(WM_CANCELMODE, OnTogglePointer)
        MESSAGE_HANDLER(WM_NCHITTEST, OnNcHitTest)
        MESSAGE_HANDLER(WM_MOUSEACTIVATE, OnMouseActivate)
        MESSAGE_HANDLER(WM_WINDOWPOSCHANGED, OnWindowPosChanged)
        MESSAGE_HANDLER(WM_EXITSIZEMOVE, OnExitSizeMove)
        MESSAGE_HANDLER(WM_ERASEBKGND, OnEraseBackground)
    END_MSG_MAP()

    void SetOwner(OsdOverlay* owner) noexcept { owner_ = owner; }

private:
    LRESULT OnNcHitTest(UINT, WPARAM, LPARAM, BOOL&);
    LRESULT OnMouseActivate(UINT, WPARAM, LPARAM, BOOL&);
    LRESULT OnWindowPosChanged(UINT, WPARAM, LPARAM, BOOL&);
    LRESULT OnExitSizeMove(UINT, WPARAM, LPARAM, BOOL&);
    LRESULT OnEraseBackground(UINT, WPARAM, LPARAM, BOOL&);

    OsdOverlay* owner_{nullptr};
    LRESULT OnTogglePointer(UINT, WPARAM, LPARAM, BOOL&);
};

enum class OsdVisual {
    Neutral,
    Armed,
    Warning,
    Protected,
    Fault,
};

class OsdOverlay final : public ATL::CWindowImpl<OsdOverlay, ATL::CWindow> {
public:
    DECLARE_WND_CLASS_EX(L"GpuThermalGuard.OsdOverlay.v1", CS_HREDRAW | CS_VREDRAW, 0)

    BEGIN_MSG_MAP(OsdOverlay)
        MESSAGE_HANDLER(WM_LBUTTONDOWN, OnTogglePointer)
        MESSAGE_HANDLER(WM_LBUTTONUP, OnTogglePointer)
        MESSAGE_HANDLER(WM_CAPTURECHANGED, OnTogglePointer)
        MESSAGE_HANDLER(WM_CANCELMODE, OnTogglePointer)
        MESSAGE_HANDLER(WM_NCHITTEST, OnNcHitTest)
        MESSAGE_HANDLER(WM_MOUSEACTIVATE, OnMouseActivate)
        MESSAGE_HANDLER(WM_DPICHANGED, OnDpiChanged)
        MESSAGE_HANDLER(WM_DISPLAYCHANGE, OnDisplayChanged)
        MESSAGE_HANDLER(WM_SETTINGCHANGE, OnDisplayChanged)
        MESSAGE_HANDLER(WM_WTSSESSION_CHANGE, OnSessionChanged)
        MESSAGE_HANDLER(WM_POWERBROADCAST, OnPowerBroadcast)
        MESSAGE_HANDLER(WM_EXITSIZEMOVE, OnExitSizeMove)
        MESSAGE_HANDLER(WM_ERASEBKGND, OnEraseBackground)
        MESSAGE_HANDLER(WM_TIMER, OnRefreshTimer)
    END_MSG_MAP()

    [[nodiscard]] bool Initialize(HWND notification_window,
                                  const telemetry::History* history,
                                  bool has_saved_position,
                                  POINT saved_position,
                                  bool& placement_repaired);
    void Shutdown() noexcept;
    void SetVisible(bool visible);
    [[nodiscard]] bool Visible() const noexcept;
    void RequestRefresh() noexcept;
    void SetThresholds(int trigger_temperature_c, int safe_power_w,
                       std::optional<double> maximum_power_w) noexcept;
    void SetCurrentPowerLimit(std::optional<double> current_power_limit_w) noexcept;
    void SetStatus(std::wstring_view status, OsdVisual visual);
    [[nodiscard]] POINT Position() const noexcept;

private:
    friend class OsdDragHandle;
    LRESULT OnTogglePointer(UINT, WPARAM, LPARAM, BOOL&);
    LRESULT HandleTogglePointer(UINT message, HWND input_window, LPARAM point) noexcept;
    [[nodiscard]] bool ToggleHit(POINT point) const noexcept;
    void ToggleCollapsed() noexcept;
    bool collapsed_{};
    compact::Gesture toggle_gesture_;
    struct Layout {
        int width{};
        int height{};
        int drag_height{};
        float scale{1.0F};
    };

    LRESULT OnNcHitTest(UINT, WPARAM, LPARAM, BOOL&);
    LRESULT OnMouseActivate(UINT, WPARAM, LPARAM, BOOL&);
    LRESULT OnDpiChanged(UINT, WPARAM, LPARAM, BOOL&);
    LRESULT OnDisplayChanged(UINT, WPARAM, LPARAM, BOOL&);
    LRESULT OnSessionChanged(UINT, WPARAM, LPARAM, BOOL&);
    LRESULT OnPowerBroadcast(UINT, WPARAM, LPARAM, BOOL&);
    LRESULT OnExitSizeMove(UINT, WPARAM, LPARAM, BOOL&);
    LRESULT OnEraseBackground(UINT, WPARAM, LPARAM, BOOL&);
    LRESULT OnRefreshTimer(UINT, WPARAM, LPARAM, BOOL&);

    [[nodiscard]] Layout CurrentLayout() const noexcept;
    [[nodiscard]] bool FitToWorkArea(POINT& position, const Layout& layout) const noexcept;
    void ApplyPosition(POINT position, const Layout& layout) noexcept;
    void SynchronizeDragHandleToOverlay() noexcept;
    void HandleDragMove(int x, int y) noexcept;
    void HandleDragEnd() noexcept;
    void ScheduleTopmostRepair(std::wstring_view reason) noexcept;
    void RepairTopmost() noexcept;
    void RenderLatest() noexcept;
    void CheckZOrder() noexcept;
    [[nodiscard]] bool Render() noexcept;

    static constexpr int kWidthDip = 388;
    static constexpr int kHeightDip = 330;
    static constexpr int kDragHeightDip = 25;
    static constexpr std::uint64_t kVisibleDurationMs = 30'000;
    static constexpr std::uint64_t kRenderIntervalMs = 500;
    static constexpr UINT_PTR kRefreshTimerId = 1;
    static constexpr UINT_PTR kTopmostRepairTimerId = 2;
    static constexpr UINT kInitialTopmostRepairDelayMs = 350;
    static constexpr UINT kFollowupTopmostRepairDelayMs = 1'200;

    HWND notification_window_{nullptr};
    const telemetry::History* history_{nullptr};
    int trigger_temperature_c_{85};
    int safe_power_w_{300};
    std::optional<double> maximum_power_w_;
    std::optional<double> current_power_limit_w_;
    std::wstring status_{L"保護狀態初始化中"};
    OsdVisual visual_{OsdVisual::Neutral};
    OsdDragHandle drag_handle_;
    bool synchronizing_position_{false};
    bool refresh_pending_{false};
    bool session_notifications_registered_{false};
    HPOWERNOTIFY display_status_notification_{nullptr};
    std::optional<DWORD> display_status_;
    std::wstring topmost_repair_reason_;
    unsigned int topmost_repair_passes_remaining_{};
    telemetry::FreshnessState freshness_state_{telemetry::FreshnessState::NoData};
    std::uint64_t recovered_until_ms_{};
    std::uint64_t next_zorder_check_ms_{};
    std::uint64_t last_zorder_repair_ms_{};
    HWND previous_obstruction_{};
    bool session_locked_{};
    std::uint64_t last_render_error_ms_{};
    DWORD render_error_{};
    bool render_failed_{};
};

}  // namespace gtg::tray
