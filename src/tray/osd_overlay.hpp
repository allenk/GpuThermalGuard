#pragma once

#include <cstdint>
#include <optional>
#include <utility>
#include <string>
#include <string_view>

#include <atlbase.h>
#include <atlapp.h>
#include <atlwin.h>
#include <wtsapi32.h>

#include "telemetry/telemetry_history.hpp"
#include "telemetry/telemetry_freshness.hpp"
#include "tray/osd_compact.hpp"
#include "fps/fps_rate.hpp"
#include "fps/fps_history.hpp"
#include <atomic>
#include <thread>

#include "sysmem/host_memory.hpp"
#include "net/action.hpp"
#include "net/history.hpp"
#include "sysmem/reclaim.hpp"
#include "tray/osd_animation.hpp"

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

// A drag image is a layered popup owned by the overlay, showing the carried
// cell. Its content is set once at press, so moving it is a SetWindowPos and
// never a repaint -- which is why the 500 ms presentation cadence survives
// dragging untouched.
class OsdDragImage final : public ATL::CWindowImpl<OsdDragImage, ATL::CWindow> {
public:
    DECLARE_WND_CLASS_EX(L"GpuThermalGuard.OsdDragImage.v1", 0, 0)
    BEGIN_MSG_MAP(OsdDragImage)
        MESSAGE_HANDLER(WM_NCHITTEST, OnNcHitTest)
    END_MSG_MAP()
private:
    LRESULT OnNcHitTest(UINT, WPARAM, LPARAM, BOOL&) { return HTTRANSPARENT; }
};

// The busy indicator for a cell whose record is doing work.
//
// A second owned, topmost, layered popup, created over one cell and destroyed
// when the work is done and the minimum visible interval has elapsed. It earns
// its existence three times over:
//
//  - It animates on its own timer, so the overlay's 500 ms presentation
//    cadence is untouched. That cadence is why PulseAlpha had to be a
//    four-second breath; this sidesteps it entirely.
//  - It sits above the cell, so further clicks land on it. Blocking input
//    costs no branch in hit testing.
//  - Its existence *is* the in-flight guard. A separate boolean could
//    disagree with what the reader can see; a window cannot.
//
// It is deliberately NOT OsdDragImage. That one is WS_EX_TRANSPARENT and
// returns HTTRANSPARENT so the mouse passes straight through, which is the
// exact opposite contract. Flipping one window's input behaviour on a flag is
// how a control that can never be clicked gets shipped, and this project has
// already done that once.
class OsdBusyIndicator final
    : public ATL::CWindowImpl<OsdBusyIndicator, ATL::CWindow> {
public:
    DECLARE_WND_CLASS_EX(L"GpuThermalGuard.OsdBusyIndicator.v1", 0, 0)
    BEGIN_MSG_MAP(OsdBusyIndicator)
        MESSAGE_HANDLER(WM_NCHITTEST, OnNcHitTest)
        MESSAGE_HANDLER(WM_TIMER, OnTimer)
    END_MSG_MAP()

    // `accent` is ARGB so this header need not reach for GDI+.
    bool Begin(HWND owner, const RECT& screen_rect, std::uint32_t accent,
               float scale, std::wstring label,
               animation::Effect effect = animation::kDefaultEffect) noexcept;
    // Swap the sweep for a figure. The window stays where it is and the cell
    // goes back to being a cell when it comes down -- nothing pops up, nothing
    // takes focus, nothing interrupts a game.
    // `bars` draws a three-bar signal mark to the left of the figure, `n` of
    // three filled. Negative draws no mark, which is what a reclaim wants: a
    // number of gigabytes is not a strength of anything.
    void ShowResult(std::wstring text, std::uint32_t tint,
                    int bars = -1) noexcept;
    void End() noexcept;
    [[nodiscard]] bool Active() const noexcept { return m_hWnd != nullptr; }

private:
    // Swallow the pointer rather than pass it through: that is the whole
    // point of being here.
    LRESULT OnNcHitTest(UINT, WPARAM, LPARAM, BOOL&) { return HTCLIENT; }
    LRESULT OnTimer(UINT, WPARAM, LPARAM, BOOL&);
    void Paint() noexcept;

    static constexpr UINT_PTR kAnimationTimerId = 1;
    static constexpr UINT kAnimationIntervalMs = 33;   // its own cadence

    std::uint32_t accent_{};
    animation::Effect effect_{animation::kDefaultEffect};
    float scale_{1.0F};
    std::wstring label_;
    std::wstring result_;
    std::uint32_t result_tint_{};
    int result_bars_{-1};
    bool showing_result_{};
    std::uint64_t started_ms_{};
};

// Which action the busy indicator is currently standing for.
//
// One enum rather than one boolean per action: two booleans can both be true,
// and the whole point of routing every action through a single indicator is
// that only one can be running.
enum class CellAction {
    None,
    Reclaim,   // RAM: trim working sets, report what was freed
    Network,   // NET: measure the foreground program's path, report a verdict
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
    // A joinable std::thread member would call std::terminate if the overlay
    // were destroyed while an action was still running. Every worker is
    // bounded, so joining here is finite.
    ~OsdOverlay() { EndAction(); }

    // CS_DBLCLKS is required: without it Windows delivers no WM_LBUTTONDBLCLK
    // to this class at all, and a cell activation would simply never arrive.
    DECLARE_WND_CLASS_EX(L"GpuThermalGuard.OsdOverlay.v1",
                         CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS, 0)

    BEGIN_MSG_MAP(OsdOverlay)
        MESSAGE_HANDLER(WM_LBUTTONDOWN, OnTogglePointer)
        MESSAGE_HANDLER(WM_LBUTTONUP, OnTogglePointer)
        MESSAGE_HANDLER(WM_LBUTTONDBLCLK, OnCellActivate)
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
        MESSAGE_HANDLER(WM_TIMER, OnOverlayTimer)
        MESSAGE_HANDLER(WM_TIMER, OnRefreshTimer)
        MESSAGE_HANDLER(WM_MOUSEMOVE, OnDragMove)
        MESSAGE_HANDLER(WM_SETCURSOR, OnDragCursor)
    END_MSG_MAP()

    [[nodiscard]] bool Collapsed() const noexcept { return collapsed_; }
    // Set before Initialize, so the window is created at the size the reader
    // last left it rather than being resized in front of them. Persistence
    // itself stays with the caller, beside the position it already saves --
    // the overlay draws, it does not own the registry.
    void RestoreCollapsed(const bool collapsed) noexcept { collapsed_ = collapsed; }
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
    void SetFpsEnabled(bool enabled) noexcept;
    void SetFpsSnapshot(fps::Snapshot snapshot) noexcept;
    void SetFpsHistory(const fps::History* history) noexcept {
        fps_history_ = history;
        RequestRefresh();
    }
    // The reader's arrangement governs both the compact cells and the
    // expanded lanes; only the compact view can edit it.
    void SetCompactLayout(compact::Layout layout) noexcept;
    void SetCompactLocked(bool locked) noexcept;
    [[nodiscard]] compact::Layout compact_layout() const noexcept { return layout_; }
    [[nodiscard]] bool compact_locked() const noexcept { return compact_locked_; }
    void SetRamEnabled(bool enabled) noexcept;
    // Windows' own low-memory signal, sampled by the Tray on its existing
    // tick. The overlay only renders it; it never decides what "low" means.
    // The policy is settable so the tray's brute-force entry can pass its own
    // later, and so a test can exercise the indicator without modifying the
    // machine it runs on.
    void SetReclaimPolicy(const sysmem::reclaim::Policy& policy) noexcept {
        reclaim_policy_ = policy;
    }
    void SetHostMemoryLow(bool low) noexcept {
        if (host_memory_low_ == low) return;
        host_memory_low_ = low;
        RequestRefresh();
    }
    void SetNetHistory(const net::History* history) noexcept {
        net_history_ = history;
        RenderLatest();
    }
    void SetNetEnabled(bool enabled) noexcept;
    // A test seam, with the same justification as SetReclaimPolicy above:
    // without it the only way to exercise the network gesture is to enable
    // extended statistics on some other program's connections on whatever
    // machine the test is running on. The probe itself is verified against a
    // real process, by hand, elevated -- not here.
    void StubNetworkProbe(net::Verdict verdict, net::ProbeStatus status) noexcept {
        net_stub_ = std::pair{verdict, status};
    }
    // Which records are placed, in one place.
    //
    // This expression used to be written out at each of eight call sites, so
    // adding the eighth record meant finding all eight -- and the first
    // attempt found one, which sized the window for seven records while the
    // painter drew eight and put the last cell outside the window.
    [[nodiscard]] compact::Placement CurrentPlacement() const noexcept {
        return compact::Resolve(layout_, ram_enabled_, fps_enabled_, net_enabled_);
    }
    void SetRamHistory(const sysmem::History* history) noexcept {
        ram_history_ = history;
        RequestRefresh();
    }
    [[nodiscard]] POINT Position() const noexcept;

private:
    friend class OsdDragHandle;
    LRESULT OnTogglePointer(UINT, WPARAM, LPARAM, BOOL&);
    LRESULT HandleTogglePointer(UINT message, HWND input_window, LPARAM point) noexcept;
    LRESULT OnCellActivate(UINT, WPARAM, LPARAM point, BOOL& handled);
    LRESULT OnOverlayTimer(UINT, WPARAM id, LPARAM, BOOL& handled);
    [[nodiscard]] bool ReportActionResult(std::uint64_t now) noexcept;
    // Everything an action needs before its worker starts: the indicator over
    // the right cell, in that record's accent, with the timing reset. Written
    // once because the two actions differ only in what the worker does and
    // what the result says -- not in any of this.
    [[nodiscard]] bool BeginAction(int slot, compact::Metric metric) noexcept;
    void BeginReclaim(int slot) noexcept;
    void BeginNetworkProbe(int slot) noexcept;
    void EndAction() noexcept;
    [[nodiscard]] bool ToggleHit(POINT point) const noexcept;
    [[nodiscard]] bool LockHit(POINT point) const noexcept;
    void ToggleCollapsed() noexcept;
    [[nodiscard]] bool Arrangeable() const noexcept {
        return ShowsLock() && !compact_locked_;
    }
    void FinishArrangement(int from_slot, POINT point) noexcept;
    void BeginDragImage(compact::Metric metric, const compact::CellGrid& grid,
                        POINT cursor, POINT cell_origin) noexcept;
    void MoveDragImage(POINT cursor) noexcept;
    void EndDragImage() noexcept;
    // Arranging needs pointer input on the body, which click-through builds
    // deliberately do not have, so the control is not offered there.
    [[nodiscard]] bool ShowsLock() const noexcept {
#if GTG_OSD_CLICK_THROUGH
        return false;
#else
        return collapsed_;
#endif
    }
    bool collapsed_{};
    compact::Gesture toggle_gesture_;
    compact::Gesture lock_gesture_;
    int drag_from_{-1};
    OsdDragImage drag_image_;
    // Host memory reclaim. The indicator's existence is the in-flight guard:
    // no separate boolean can disagree with what the reader can see.
    OsdBusyIndicator busy_indicator_;
    CellAction active_action_{CellAction::None};
    std::uint64_t action_started_ms_{};
    std::uint64_t action_visible_until_ms_{};
    std::uint64_t action_result_until_ms_{};
    std::atomic<bool> action_done_{false};
    std::thread action_worker_;
    sysmem::reclaim::Outcome reclaim_outcome_{};
    sysmem::reclaim::Policy reclaim_policy_{};
    // Written by the worker before it sets action_done_, read by the message
    // loop after it observes it. The release/acquire pair on that flag is what
    // publishes them; they are not atomic themselves and do not need to be.
    net::Verdict net_verdict_{};
    net::ProbeStatus net_status_{net::ProbeStatus::NoLibrary};
    std::optional<std::pair<net::Verdict, net::ProbeStatus>> net_stub_;
    compact::Metric drag_metric_{compact::Metric::Temperature};
    POINT drag_hotspot_{};
    struct Layout {
        int width{};
        int height{};
        int drag_height{};
        float scale{1.0F};
        int columns{1};
        int rows{1};
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
    LRESULT OnDragMove(UINT, WPARAM, LPARAM, BOOL&);
    LRESULT OnDragCursor(UINT, WPARAM, LPARAM, BOOL&);

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
    static constexpr UINT_PTR kActionTimerId = 3;
    // Coarse on purpose: this poll only notices that the work finished and
    // that the floor elapsed. The animation runs on the indicator own timer.
    static constexpr UINT kActionPollIntervalMs = 50;
    static constexpr UINT kInitialTopmostRepairDelayMs = 350;
    static constexpr UINT kFollowupTopmostRepairDelayMs = 1'200;

    HWND notification_window_{nullptr};
    const telemetry::History* history_{nullptr};
    const fps::History* fps_history_{nullptr};
    int trigger_temperature_c_{85};
    int safe_power_w_{300};
    std::optional<double> maximum_power_w_;
    std::optional<double> current_power_limit_w_;
    std::wstring status_{L"保護狀態初始化中"};
    OsdVisual visual_{OsdVisual::Neutral};
    // Default off like FPS: MainDialog applies the stored preference.
    compact::Layout layout_{};
    bool compact_locked_{true};
    bool ram_enabled_{false};
    bool host_memory_low_{false};
    const sysmem::History* ram_history_{nullptr};
    const net::History* net_history_{nullptr};
    bool net_enabled_{false};
    bool fps_enabled_{false};
    fps::Snapshot fps_snapshot_{};
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
