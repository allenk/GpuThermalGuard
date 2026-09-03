#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include <atlbase.h>
#include <atlapp.h>
#include <atlwin.h>

#include "telemetry/telemetry_history.hpp"

namespace gtg::tray {

class OsdOverlay;

class OsdDragHandle final : public ATL::CWindowImpl<OsdDragHandle, ATL::CWindow> {
public:
    DECLARE_WND_CLASS_EX(L"GpuThermalGuard.OsdDragHandle.v1", 0, 0)

    BEGIN_MSG_MAP(OsdDragHandle)
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
        MESSAGE_HANDLER(WM_NCHITTEST, OnNcHitTest)
        MESSAGE_HANDLER(WM_MOUSEACTIVATE, OnMouseActivate)
        MESSAGE_HANDLER(WM_DPICHANGED, OnDpiChanged)
        MESSAGE_HANDLER(WM_DISPLAYCHANGE, OnDisplayChanged)
        MESSAGE_HANDLER(WM_SETTINGCHANGE, OnDisplayChanged)
        MESSAGE_HANDLER(WM_EXITSIZEMOVE, OnExitSizeMove)
        MESSAGE_HANDLER(WM_ERASEBKGND, OnEraseBackground)
        MESSAGE_HANDLER(WM_TIMER, OnRefreshTimer)
    END_MSG_MAP()

    static constexpr UINT kPlacementChangedMessage = WM_APP + 45;

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
    void SetStatus(std::wstring_view status, OsdVisual visual);
    [[nodiscard]] POINT Position() const noexcept;

private:
    friend class OsdDragHandle;
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
    LRESULT OnExitSizeMove(UINT, WPARAM, LPARAM, BOOL&);
    LRESULT OnEraseBackground(UINT, WPARAM, LPARAM, BOOL&);
    LRESULT OnRefreshTimer(UINT, WPARAM, LPARAM, BOOL&);

    [[nodiscard]] Layout CurrentLayout() const noexcept;
    [[nodiscard]] bool FitToWorkArea(POINT& position, const Layout& layout) const noexcept;
    void ApplyPosition(POINT position, const Layout& layout) noexcept;
    void HandleDragMove(int x, int y) noexcept;
    void HandleDragEnd() noexcept;
    void RenderLatest() noexcept;
    [[nodiscard]] bool Render() noexcept;

    static constexpr int kWidthDip = 388;
    static constexpr int kHeightDip = 330;
    static constexpr int kDragHeightDip = 25;
    static constexpr std::uint64_t kVisibleDurationMs = 30'000;
    static constexpr std::uint64_t kRenderIntervalMs = 500;
    static constexpr UINT_PTR kRefreshTimerId = 1;

    HWND notification_window_{nullptr};
    const telemetry::History* history_{nullptr};
    int trigger_temperature_c_{85};
    int safe_power_w_{300};
    std::optional<double> maximum_power_w_;
    std::wstring status_{L"保護狀態初始化中"};
    OsdVisual visual_{OsdVisual::Neutral};
    OsdDragHandle drag_handle_;
    bool synchronizing_position_{false};
    bool refresh_pending_{false};
};

}  // namespace gtg::tray
