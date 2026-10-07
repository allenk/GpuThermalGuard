#pragma once

#include <cstdint>
#include <optional>

#include <atlbase.h>
#include <atlapp.h>
#include <atlwin.h>

#include "telemetry/telemetry_history.hpp"
#include "fps/fps_history.hpp"
#include "net/history.hpp"
#include "sysmem/host_memory.hpp"
#include "tray/trip_markers.hpp"

namespace gtg::tray {

// The chart's surface colours: everything that has to agree with the window
// behind it. The record colours (temperature red, power blue, ...) are not
// here -- they identify the record and stay the same in every theme.
//
// System() is the chart as it has always been drawn: two system colours and
// the fixed light values. A chart nobody gives a palette draws exactly that.
struct ChartPalette {
    COLORREF background;    // the client area around the lanes
    COLORREF plot;          // inside a lane
    COLORREF border;        // a lane's outline
    COLORREF grid;          // grid lines (drawn translucent)
    COLORREF muted_text;    // time labels
    COLORREF halo;          // outline behind value labels, so they read over a trace
    COLORREF track;         // timeline track
    COLORREF thumb_live;    // timeline thumb while following live
    COLORREF thumb_paused;  // timeline thumb while scrolled back
    COLORREF gap_shade;     // tint over FPS gaps
    COLORREF net_sent;      // transmit trace: too dark for a dark plot as is

    [[nodiscard]] static ChartPalette System() noexcept {
        return {::GetSysColor(COLOR_WINDOW), RGB(249, 250, 252), RGB(205, 211, 219),
                RGB(211, 217, 225),          ::GetSysColor(COLOR_GRAYTEXT),
                RGB(255, 255, 255),          RGB(214, 220, 228), RGB(47, 124, 211),
                RGB(91, 104, 125),           RGB(8, 19, 31),     RGB(62, 96, 140)};
    }
};

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
    void SetFpsHistory(const fps::History* history) noexcept {
        fps_history_ = history;
        Invalidate(FALSE);
    }
    // A null history removes the record entirely, as with FPS.
    void SetNetHistory(const net::History* history) noexcept {
        net_history_ = history;
        NotifyDataChanged();
    }
    void SetRamHistory(const sysmem::History* history) noexcept {
        ram_history_ = history;
        Invalidate(FALSE);
    }
    void NotifyDataChanged();
    void SetThresholds(int trigger_temperature_c, int safe_power_w,
                       std::optional<double> maximum_power_w);
    // A protection trip at `monotonic_ms` (GetTickCount64), drawn on the
    // temperature lane only. AF-20261004-main-ui-redesign.
    void AddTripMarker(std::uint64_t monotonic_ms);
    // A theme's surface colours; nullopt returns to ChartPalette::System().
    void SetPalette(const std::optional<ChartPalette>& palette) {
        palette_ = palette;
        if (m_hWnd != nullptr) Invalidate(FALSE);
    }

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
    const fps::History* fps_history_{nullptr};
    const sysmem::History* ram_history_{nullptr};
    const net::History* net_history_{nullptr};
    int trigger_temperature_c_{85};
    int safe_power_w_{300};
    std::optional<double> maximum_power_w_;
    TripMarkers trip_markers_;
    std::optional<std::uint64_t> view_end_ms_;
    bool follow_live_{true};
    std::optional<ChartPalette> palette_;
    bool dragging_timeline_{false};
    int timeline_drag_offset_px_{};
};

}  // namespace gtg::tray
