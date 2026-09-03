#pragma once

#include <cstdint>
#include <optional>

#include <atlbase.h>
#include <atlapp.h>
#include <atlwin.h>

#include "telemetry/telemetry_history.hpp"

namespace gtg::tray {

class HistoryChart final : public ATL::CWindowImpl<HistoryChart, ATL::CWindow> {
public:
    DECLARE_WND_SUPERCLASS(nullptr, L"STATIC")

    BEGIN_MSG_MAP(HistoryChart)
        MESSAGE_HANDLER(WM_PAINT, OnPaint)
        MESSAGE_HANDLER(WM_PRINTCLIENT, OnPrintClient)
        MESSAGE_HANDLER(WM_ERASEBKGND, OnEraseBackground)
        MESSAGE_HANDLER(WM_THEMECHANGED, OnVisualChanged)
        MESSAGE_HANDLER(WM_SYSCOLORCHANGE, OnVisualChanged)
        MESSAGE_HANDLER(WM_LBUTTONDOWN, OnLButtonDown)
        MESSAGE_HANDLER(WM_MOUSEMOVE, OnMouseMove)
        MESSAGE_HANDLER(WM_LBUTTONUP, OnLButtonUp)
        MESSAGE_HANDLER(WM_CAPTURECHANGED, OnCaptureChanged)
    END_MSG_MAP()

    void SetHistory(const telemetry::History* history) noexcept { history_ = history; }
    void NotifyDataChanged();
    void SetThresholds(int trigger_temperature_c, int safe_power_w,
                       std::optional<double> maximum_power_w);

private:
    LRESULT OnPaint(UINT, WPARAM, LPARAM, BOOL&);
    LRESULT OnPrintClient(UINT, WPARAM, LPARAM, BOOL&);
    LRESULT OnEraseBackground(UINT, WPARAM, LPARAM, BOOL&);
    LRESULT OnVisualChanged(UINT, WPARAM, LPARAM, BOOL&);
    LRESULT OnLButtonDown(UINT, WPARAM, LPARAM, BOOL&);
    LRESULT OnMouseMove(UINT, WPARAM, LPARAM, BOOL&);
    LRESULT OnLButtonUp(UINT, WPARAM, LPARAM, BOOL&);
    LRESULT OnCaptureChanged(UINT, WPARAM, LPARAM, BOOL&);
    void Paint(HDC target, const RECT& bounds);
    [[nodiscard]] RECT TimelineHitRect() const;
    [[nodiscard]] std::uint64_t EffectiveViewEnd(std::uint64_t now) const;
    void UpdateViewFromThumbLeft(int thumb_left, std::uint64_t now);
    [[nodiscard]] const std::deque<telemetry::Sample>& Samples() const noexcept;

    static constexpr std::uint64_t kVisibleHistoryMs = 5ULL * 60ULL * 1000ULL;
    const telemetry::History* history_{nullptr};
    int trigger_temperature_c_{85};
    int safe_power_w_{300};
    std::optional<double> maximum_power_w_;
    std::optional<std::uint64_t> view_end_ms_;
    bool follow_live_{true};
    bool dragging_timeline_{false};
    int timeline_drag_offset_px_{};
};

}  // namespace gtg::tray
