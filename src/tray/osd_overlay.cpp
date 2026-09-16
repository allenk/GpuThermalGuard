#include "tray/osd_overlay.hpp"
#include "localization/localization.hpp"
#include "logging/logger.hpp"

#include <windows.h>
#include <windowsx.h>
#include <gdiplus.h>
#include <dwmapi.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>
#include <limits>
#include <utility>
#include <vector>

namespace gtg::tray {
namespace {

// GUID_SESSION_DISPLAY_STATUS from the Windows SDK. Keeping the value local
// avoids adding PowrProf solely to provide storage for the GUID constant.
constexpr GUID kSessionDisplayStatusGuid{
    0x2b84c20e, 0xad23, 0x4ddf, {0x93, 0xdb, 0x05, 0xff, 0xbd, 0x7e, 0xfc, 0xa5}};
constexpr DWORD kDisplayOff = 0;
constexpr DWORD kDisplayOn = 1;
constexpr DWORD kDisplayDimmed = 2;

struct ScopedScreenDc {
    HDC value{GetDC(nullptr)};
    ~ScopedScreenDc() { if (value != nullptr) ReleaseDC(nullptr, value); }
};

struct ScopedMemoryDc {
    HDC value{};
    explicit ScopedMemoryDc(HDC compatible) : value(CreateCompatibleDC(compatible)) {}
    ~ScopedMemoryDc() { if (value != nullptr) DeleteDC(value); }
};

struct ScopedBitmap {
    HBITMAP value{};
    ~ScopedBitmap() { if (value != nullptr) DeleteObject(value); }
};

struct ScopedSelection {
    HDC dc;
    HGDIOBJ previous;
    ~ScopedSelection() { if (previous && previous != HGDI_ERROR) SelectObject(dc, previous); }
};

void AddRoundedRectangle(Gdiplus::GraphicsPath& path, const Gdiplus::RectF& rect,
                         const Gdiplus::REAL radius) {
    const Gdiplus::REAL diameter = radius * 2.0F;
    path.AddArc(rect.X, rect.Y, diameter, diameter, 180.0F, 90.0F);
    path.AddArc(rect.GetRight() - diameter, rect.Y, diameter, diameter, 270.0F, 90.0F);
    path.AddArc(rect.GetRight() - diameter, rect.GetBottom() - diameter,
                diameter, diameter, 0.0F, 90.0F);
    path.AddArc(rect.X, rect.GetBottom() - diameter, diameter, diameter, 90.0F, 90.0F);
    path.CloseFigure();
}

void DrawText(Gdiplus::Graphics& graphics, const std::wstring& text,
              const Gdiplus::RectF& bounds, Gdiplus::Font& font,
              const Gdiplus::Color color, const Gdiplus::StringAlignment alignment) {
    Gdiplus::SolidBrush brush(color);
    Gdiplus::StringFormat format;
    format.SetAlignment(alignment);
    format.SetLineAlignment(Gdiplus::StringAlignmentCenter);
    format.SetFormatFlags(Gdiplus::StringFormatFlagsNoWrap);
    format.SetTrimming(Gdiplus::StringTrimmingEllipsisCharacter);
    graphics.DrawString(text.c_str(), -1, &font, bounds, &format, &brush);
}

Gdiplus::Color VisualColor(const OsdVisual visual) {
    switch (visual) {
    case OsdVisual::Armed: return {255, 70, 210, 137};
    case OsdVisual::Warning: return {255, 255, 184, 72};
    case OsdVisual::Protected: return {255, 75, 174, 255};
    case OsdVisual::Fault: return {255, 255, 82, 92};
    case OsdVisual::Neutral: return {255, 170, 181, 198};
    }
    return {255, 170, 181, 198};
}

std::wstring FormatMetric(const std::optional<double> value, const wchar_t* suffix,
                          const int precision = 0) {
    if (!value) return L"—";
    return precision == 0 ? std::format(L"{:.0f}{}", *value, suffix)
                          : std::format(L"{:.1f}{}", *value, suffix);
}

using MetricMember = std::optional<double> telemetry::Sample::*;

void DrawCompactSparkline(Gdiplus::Graphics& graphics,
                          const std::deque<telemetry::Sample>& samples,
                          std::uint64_t start, std::uint64_t end,
                          MetricMember member, const Gdiplus::RectF& bounds,
                          Gdiplus::Color color, double minimum_span, float scale) {
    // Traverse only the visible tail, never the entire one-hour history.
    std::vector<std::pair<std::uint64_t, double>> values;
    double low = std::numeric_limits<double>::max();
    double high = std::numeric_limits<double>::lowest();
    for (auto it = samples.rbegin(); it != samples.rend(); ++it) {
        if (it->monotonic_ms < start) break;
        const auto value = (*it).*member;
        if (it->monotonic_ms > end || !value || !std::isfinite(*value)) continue;
        values.emplace_back(it->monotonic_ms, *value);
        low = std::min(low, *value);
        high = std::max(high, *value);
    }
    if (values.empty()) return;
    const auto [bottom, top] = compact::SparkRange(low, high, minimum_span);
    const double duration = static_cast<double>(std::max<std::uint64_t>(1, end - start));
    std::vector<Gdiplus::PointF> points;
    points.reserve(values.size());
    for (auto it = values.rbegin(); it != values.rend(); ++it) {
        points.emplace_back(bounds.X + static_cast<float>((it->first - start) / duration) * bounds.Width,
            bounds.GetBottom() - static_cast<float>((it->second - bottom) / (top - bottom)) * bounds.Height);
    }
    Gdiplus::SolidBrush fill(Gdiplus::Color(40, color.GetR(), color.GetG(), color.GetB()));
    Gdiplus::Pen pen(color, 1.0F * scale);
    pen.SetLineJoin(Gdiplus::LineJoinRound);
    if (points.size() == 1) {
        graphics.FillEllipse(&fill, points.front().X - scale, points.front().Y - scale, 2 * scale, 2 * scale);
        return;
    }
    Gdiplus::GraphicsPath area;
    area.AddLines(points.data(), static_cast<INT>(points.size()));
    area.AddLine(points.back(), {points.back().X, bounds.GetBottom()});
    area.AddLine(Gdiplus::PointF(points.back().X, bounds.GetBottom()),
                 Gdiplus::PointF(points.front().X, bounds.GetBottom()));
    area.CloseFigure();
    graphics.FillPath(&fill, &area);
    // Same solid interpolation convention as full charts; darker intervals are
    // inferred, not measured. Avoid extra tick marks/labels in this tiny view.
    Gdiplus::SolidBrush gap_fill(Gdiplus::Color(110, 8, 19, 31));
    for (std::size_t i = 1; i < points.size(); ++i) {
        const auto newer = values[values.size() - 1 - i].first;
        const auto older = values[values.size() - i].first;
        if (newer - older > telemetry::kPresentationGapMs) {
            graphics.FillRectangle(&gap_fill, points[i - 1].X, bounds.Y,
                std::max(scale, points[i].X - points[i - 1].X), bounds.Height);
        }
    }
    graphics.DrawLines(&pen, points.data(), static_cast<INT>(points.size()));
}

void DrawMetricLane(Gdiplus::Graphics& graphics,
                    const std::deque<telemetry::Sample>& samples,
                    const std::uint64_t visible_start,
                    const std::uint64_t visible_end,
                    const Gdiplus::RectF& row,
                    const std::wstring& label,
                    const std::wstring& current,
                    const Gdiplus::Color color,
                    const double minimum,
                    const double maximum,
                    const MetricMember member,
                    Gdiplus::Font& label_font,
                    Gdiplus::Font& value_font,
                    const std::vector<telemetry::PresentationGap>& gaps,
                    const bool annotate_gaps,
                    const std::optional<double> threshold = std::nullopt,
                    const std::wstring_view annotation = {},
                    const bool stale = false) {
    const Gdiplus::REAL scale = row.Height / 60.0F;
    Gdiplus::SolidBrush background(Gdiplus::Color(12, 255, 255, 255));
    graphics.FillRectangle(&background, row);
    Gdiplus::Pen separator(Gdiplus::Color(28, 190, 205, 224), 1.0F * scale);
    graphics.DrawLine(&separator, row.X, row.GetBottom(), row.GetRight(), row.GetBottom());

    const Gdiplus::RectF label_rect(row.X + 8.0F * scale, row.Y,
                                    70.0F * scale, row.Height);
    DrawText(graphics, label, label_rect, label_font, color,
             Gdiplus::StringAlignmentNear);
    if (!annotation.empty()) {
        const Gdiplus::RectF annotation_rect(row.X + 78.0F * scale, row.Y,
                                              row.Width - 226.0F * scale,
                                              22.0F * scale);
        DrawText(graphics, std::wstring(annotation), annotation_rect, label_font,
                 Gdiplus::Color(230, color.GetR(), color.GetG(), color.GetB()),
                 Gdiplus::StringAlignmentNear);
    }
    const Gdiplus::RectF value_rect(row.GetRight() - 140.0F * scale, row.Y,
                                    132.0F * scale, 22.0F * scale);
    DrawText(graphics, current, value_rect, value_font,
             stale ? Gdiplus::Color(190, 205, 214, 226)
                   : Gdiplus::Color(255, 245, 248, 252),
             Gdiplus::StringAlignmentFar);

    const Gdiplus::RectF graph(row.X + 78.0F * scale, row.Y + 23.0F * scale,
                               row.Width - 86.0F * scale, row.Height - 30.0F * scale);
    Gdiplus::Pen grid(Gdiplus::Color(24, 215, 224, 237), 1.0F * scale);
    graphics.DrawLine(&grid, graph.X, graph.Y + graph.Height / 2.0F,
                      graph.GetRight(), graph.Y + graph.Height / 2.0F);

    const auto y_for = [&](const double value) {
        const double fraction = std::clamp((value - minimum) / (maximum - minimum), 0.0, 1.0);
        return graph.GetBottom() - static_cast<Gdiplus::REAL>(fraction) * graph.Height;
    };
    if (threshold) {
        Gdiplus::Pen threshold_pen(Gdiplus::Color(118, color.GetR(), color.GetG(), color.GetB()),
                                   1.0F * scale);
        threshold_pen.SetDashStyle(Gdiplus::DashStyleDash);
        const Gdiplus::REAL y = y_for(*threshold);
        graphics.DrawLine(&threshold_pen, graph.X, y, graph.GetRight(), y);
    }

    const double visible_duration = static_cast<double>(
        std::max<std::uint64_t>(1, visible_end - visible_start));
    for (const auto& gap : gaps) {
        if (!annotate_gaps) break;
        const auto x_for_gap = [&](const std::uint64_t timestamp) {
            const double elapsed = static_cast<double>(timestamp - visible_start);
            return graph.X + static_cast<Gdiplus::REAL>(elapsed / visible_duration) *
                graph.Width;
        };
        const Gdiplus::REAL left = x_for_gap(gap.start_ms);
        const Gdiplus::REAL right = x_for_gap(gap.end_ms);
        // A shared quiet timing strip replaces repeated vertical edges/"!".
        // All five lanes still retain their darker inferred solid bridges.
        Gdiplus::SolidBrush gap_strip(Gdiplus::Color(120, 255, 184, 72));
        graphics.FillRectangle(&gap_strip, left, graph.GetBottom() - 2.0F * scale,
                               std::max(1.0F, right - left), 2.0F * scale);
    }

    std::vector<Gdiplus::PointF> segment;
    std::optional<std::uint64_t> previous_valid_ms;
    std::optional<Gdiplus::PointF> previous_valid_point;
    const auto flush_segment = [&] {
        if (segment.size() < 2) {
            segment.clear();
            return;
        }
        Gdiplus::GraphicsPath fill_path;
        fill_path.AddLines(segment.data(), static_cast<INT>(segment.size()));
        const Gdiplus::PointF bottom_right(segment.back().X, graph.GetBottom());
        const Gdiplus::PointF bottom_left(segment.front().X, graph.GetBottom());
        fill_path.AddLine(segment.back(), bottom_right);
        fill_path.AddLine(bottom_right, bottom_left);
        fill_path.CloseFigure();
        Gdiplus::SolidBrush fill(Gdiplus::Color(72, color.GetR(), color.GetG(), color.GetB()));
        graphics.FillPath(&fill, &fill_path);
        Gdiplus::Pen line(color, 1.35F * scale);
        line.SetLineJoin(Gdiplus::LineJoinRound);
        graphics.DrawLines(&line, segment.data(), static_cast<INT>(segment.size()));
        segment.clear();
    };
    const auto draw_gap_bridge = [&](const Gdiplus::PointF from,
                                     const Gdiplus::PointF to) {
        Gdiplus::GraphicsPath bridge_area;
        bridge_area.AddLine(from, to);
        bridge_area.AddLine(to, Gdiplus::PointF(to.X, graph.GetBottom()));
        bridge_area.AddLine(Gdiplus::PointF(to.X, graph.GetBottom()),
                            Gdiplus::PointF(from.X, graph.GetBottom()));
        bridge_area.CloseFigure();
        const BYTE dark_red = static_cast<BYTE>(color.GetR() * 2U / 5U);
        const BYTE dark_green = static_cast<BYTE>(color.GetG() * 2U / 5U);
        const BYTE dark_blue = static_cast<BYTE>(color.GetB() * 2U / 5U);
        const Gdiplus::REAL top = std::min(from.Y, to.Y);
        Gdiplus::LinearGradientBrush bridge_fill(
            Gdiplus::PointF(0.0F, top),
            Gdiplus::PointF(0.0F, std::max(top + 1.0F, graph.GetBottom())),
            Gdiplus::Color(178, dark_red, dark_green, dark_blue),
            Gdiplus::Color(62, dark_red, dark_green, dark_blue));
        graphics.FillPath(&bridge_fill, &bridge_area);
        Gdiplus::Pen bridge_line(color, 1.35F * scale);
        bridge_line.SetLineJoin(Gdiplus::LineJoinRound);
        graphics.DrawLine(&bridge_line, from, to);
    };

    for (const auto& sample : samples) {
        if (sample.monotonic_ms < visible_start || sample.monotonic_ms > visible_end) continue;
        const auto& value = sample.*member;
        if (!value) continue;
        const double elapsed = static_cast<double>(sample.monotonic_ms - visible_start);
        const double duration = static_cast<double>(std::max<std::uint64_t>(1,
            visible_end - visible_start));
        const Gdiplus::REAL x = graph.X +
            static_cast<Gdiplus::REAL>(elapsed / duration) * graph.Width;
        const Gdiplus::PointF point(x, y_for(*value));
        if (previous_valid_ms &&
            sample.monotonic_ms - *previous_valid_ms >
                telemetry::kPresentationGapMs) {
            flush_segment();
            if (previous_valid_point) draw_gap_bridge(*previous_valid_point, point);
        }
        segment.push_back(point);
        previous_valid_ms = sample.monotonic_ms;
        previous_valid_point = point;
    }
    flush_segment();
}

}  // namespace

LRESULT OsdDragHandle::OnNcHitTest(UINT, WPARAM, const LPARAM lparam, BOOL&) {
    POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
    ScreenToClient(&point);
    return owner_ != nullptr && owner_->ToggleHit(point) ? HTCLIENT : HTCAPTION;
}

LRESULT OsdDragHandle::OnTogglePointer(UINT message, WPARAM, LPARAM point, BOOL&) {
    return owner_ != nullptr ? owner_->HandleTogglePointer(message, m_hWnd, point) : 0;
}

LRESULT OsdDragHandle::OnMouseActivate(UINT, WPARAM, LPARAM, BOOL&) { return MA_NOACTIVATE; }

LRESULT OsdDragHandle::OnWindowPosChanged(UINT, WPARAM, const LPARAM lparam, BOOL&) {
    const auto* position = reinterpret_cast<const WINDOWPOS*>(lparam);
    if (owner_ != nullptr && (position->flags & SWP_NOMOVE) == 0) {
        owner_->HandleDragMove(position->x, position->y);
    }
    return 0;
}

LRESULT OsdDragHandle::OnExitSizeMove(UINT, WPARAM, LPARAM, BOOL&) {
    if (owner_ != nullptr) owner_->HandleDragEnd();
    return 0;
}

LRESULT OsdDragHandle::OnEraseBackground(UINT, WPARAM, LPARAM, BOOL&) { return 1; }

bool OsdOverlay::Initialize(HWND notification_window,
                            const telemetry::History* history,
                            const bool has_saved_position,
                            POINT saved_position,
                            bool& placement_repaired) {
    notification_window_ = notification_window;
    history_ = history;
    placement_repaired = false;
    if (m_hWnd != nullptr) return true;

    RECT initial{0, 0, kWidthDip, kHeightDip};
    DWORD ex_style = WS_EX_LAYERED | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW |
                     WS_EX_TOPMOST;
#if GTG_OSD_CLICK_THROUGH
    ex_style |= WS_EX_TRANSPARENT;
#endif
    if (Create(nullptr, initial, L"GPU Thermal Guard OSD", WS_POPUP,
               ex_style) == nullptr) {
        return false;
    }

#if GTG_OSD_CLICK_THROUGH
    drag_handle_.SetOwner(this);
    const DWORD drag_ex_style = WS_EX_LAYERED | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW |
                                WS_EX_TOPMOST;
    // This popup is owned by the visible OSD so Windows keeps it above its
    // owner. It exists only for click-through builds, where the OSD body must
    // ignore pointer input but the narrow drag strip remains interactive.
    if (drag_handle_.Create(m_hWnd, initial, L"GPU Thermal Guard OSD drag handle",
                            WS_POPUP, drag_ex_style) == nullptr) {
        DestroyWindow();
        return false;
    }
    SetLayeredWindowAttributes(drag_handle_, 0, 1, LWA_ALPHA);
#endif

    session_notifications_registered_ =
        WTSRegisterSessionNotification(m_hWnd, NOTIFY_FOR_THIS_SESSION) != FALSE;
    if (session_notifications_registered_) {
        (void)logging::TryInfo(L"OSD session-change notification registered");
    } else {
        (void)logging::TryWarning(std::format(
            L"OSD session-change notification registration failed; error={}",
            GetLastError()));
    }

    display_status_notification_ = RegisterPowerSettingNotification(
        m_hWnd, &kSessionDisplayStatusGuid, DEVICE_NOTIFY_WINDOW_HANDLE);
    if (display_status_notification_ != nullptr) {
        (void)logging::TryInfo(L"OSD session-display notification registered");
    } else {
        (void)logging::TryWarning(std::format(
            L"OSD session-display notification registration failed; error={}",
            GetLastError()));
    }

    const Layout layout = CurrentLayout();
    if (!has_saved_position) {
        MONITORINFO info{sizeof(info)};
        GetMonitorInfoW(MonitorFromWindow(notification_window_, MONITOR_DEFAULTTOPRIMARY), &info);
        saved_position = {info.rcWork.left + MulDiv(18, static_cast<int>(GetDpiForWindow(m_hWnd)), 96),
                          info.rcWork.top + MulDiv(18, static_cast<int>(GetDpiForWindow(m_hWnd)), 96)};
    }
    placement_repaired = FitToWorkArea(saved_position, layout);
    ApplyPosition(saved_position, layout);
    return Render();
}

void OsdOverlay::Shutdown() noexcept {
    toggle_gesture_.Cancel();
    if (::GetCapture() == m_hWnd || ::GetCapture() == drag_handle_.m_hWnd)
        ::ReleaseCapture();
    if (m_hWnd != nullptr) {
        KillTimer(kRefreshTimerId);
        KillTimer(kTopmostRepairTimerId);
    }
    topmost_repair_passes_remaining_ = 0;
    topmost_repair_reason_.clear();
    display_status_.reset();
    if (display_status_notification_ != nullptr) {
        if (UnregisterPowerSettingNotification(display_status_notification_) == FALSE) {
            (void)logging::TryWarning(std::format(
                L"OSD session-display notification unregister failed; error={}",
                GetLastError()));
        }
        display_status_notification_ = nullptr;
    }
    if (session_notifications_registered_ && m_hWnd != nullptr) {
        if (WTSUnRegisterSessionNotification(m_hWnd) == FALSE) {
            (void)logging::TryWarning(std::format(
                L"OSD session-change notification unregister failed; error={}",
                GetLastError()));
        }
        session_notifications_registered_ = false;
    }
    refresh_pending_ = false;
    drag_handle_.SetOwner(nullptr);
    if (drag_handle_.m_hWnd != nullptr) drag_handle_.DestroyWindow();
    if (m_hWnd != nullptr) DestroyWindow();
    history_ = nullptr;
    notification_window_ = nullptr;
}

void OsdOverlay::SetVisible(const bool should_show) {
    if (m_hWnd == nullptr) return;
    if (should_show) {
        // Reconcile placement before every show. Visibility is a two-window
        // state in click-through builds and cannot be inferred from the OSD
        // body alone.
        RECT bounds{};
        GetWindowRect(&bounds);
        POINT next{bounds.left, bounds.top};
        const Layout layout = CurrentLayout();
        (void)FitToWorkArea(next, layout);
        ApplyPosition(next, layout);
        (void)Render();
        ShowWindow(SW_SHOWNOACTIVATE);
        SetWindowPos(HWND_TOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
        if (drag_handle_.m_hWnd != nullptr) {
            SynchronizeDragHandleToOverlay();
            drag_handle_.ShowWindow(SW_SHOWNOACTIVATE);
            drag_handle_.SetWindowPos(HWND_TOPMOST, 0, 0, 0, 0,
                SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
        }
        (void)SetTimer(kRefreshTimerId, static_cast<UINT>(kRenderIntervalMs));
    } else {
        toggle_gesture_.Cancel();
        if (::GetCapture() == m_hWnd ||
            (drag_handle_.m_hWnd != nullptr && ::GetCapture() == drag_handle_.m_hWnd))
            ::ReleaseCapture();
        KillTimer(kRefreshTimerId);
        KillTimer(kTopmostRepairTimerId);
        topmost_repair_passes_remaining_ = 0;
        topmost_repair_reason_.clear();
        refresh_pending_ = false;
        if (drag_handle_.m_hWnd != nullptr) drag_handle_.ShowWindow(SW_HIDE);
        ShowWindow(SW_HIDE);
    }
}

bool OsdOverlay::Visible() const noexcept {
    return m_hWnd != nullptr && IsWindowVisible() != FALSE;
}

void OsdOverlay::RequestRefresh() noexcept {
    if (Visible()) refresh_pending_ = true;
}

void OsdOverlay::SetThresholds(const int trigger_temperature_c, const int safe_power_w,
                               const std::optional<double> maximum_power_w) noexcept {
    trigger_temperature_c_ = trigger_temperature_c;
    safe_power_w_ = safe_power_w;
    maximum_power_w_ = maximum_power_w;
}

void OsdOverlay::SetCurrentPowerLimit(
    const std::optional<double> current_power_limit_w) noexcept {
    current_power_limit_w_ = current_power_limit_w;
}

void OsdOverlay::SetStatus(const std::wstring_view status, const OsdVisual visual) {
    status_.assign(status);
    visual_ = visual;
    RequestRefresh();
}

POINT OsdOverlay::Position() const noexcept {
    RECT bounds{};
    // The painted OSD is the placement authority. A transparent helper must
    // never be allowed to persist or restore a stale coordinate.
    if (m_hWnd != nullptr) {
        GetWindowRect(&bounds);
    } else if (drag_handle_.m_hWnd != nullptr) {
        drag_handle_.GetWindowRect(&bounds);
    }
    return {bounds.left, bounds.top};
}

LRESULT OsdOverlay::OnNcHitTest(UINT, WPARAM, const LPARAM lparam, BOOL&) {
#if GTG_OSD_CLICK_THROUGH
    // The separate owned drag popup handles the top strip. The painted body
    // must never become a second draggable window or it can diverge from the
    // transparent helper.
    (void)lparam;
    return HTTRANSPARENT;
#else
    POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
    ScreenToClient(&point);
    if (ToggleHit(point)) return HTCLIENT;
    if (point.y >= 0 && point.y < CurrentLayout().drag_height) return HTCAPTION;
    return HTCLIENT;
#endif
}

LRESULT OsdOverlay::OnMouseActivate(UINT, WPARAM, LPARAM, BOOL&) { return MA_NOACTIVATE; }

bool OsdOverlay::ToggleHit(const POINT point) const noexcept {
    const auto layout = CurrentLayout();
    return compact::ToggleHit(point.x, point.y, layout.width, layout.drag_height);
}

LRESULT OsdOverlay::OnTogglePointer(UINT message, WPARAM, LPARAM point, BOOL&) {
    return HandleTogglePointer(message, m_hWnd, point);
}

LRESULT OsdOverlay::HandleTogglePointer(const UINT message, const HWND input_window,
                                       const LPARAM point) noexcept {
    const bool inside = ToggleHit({GET_X_LPARAM(point), GET_Y_LPARAM(point)});
    if (message == WM_LBUTTONDOWN) {
        toggle_gesture_.Press(inside);
        if (inside) ::SetCapture(input_window);
    } else if (message == WM_LBUTTONUP) {
        const bool toggle = toggle_gesture_.Release(inside && ::GetCapture() == input_window);
        if (::GetCapture() == input_window) ::ReleaseCapture();
        if (toggle) ToggleCollapsed();
    } else {
        toggle_gesture_.Cancel();
        if (message == WM_CANCELMODE && ::GetCapture() == input_window) ::ReleaseCapture();
    }
    return 0;
}

void OsdOverlay::ToggleCollapsed() noexcept {
    POINT next = Position();
    collapsed_ = !collapsed_;
    const auto layout = CurrentLayout();
    (void)FitToWorkArea(next, layout);
    ApplyPosition(next, layout);
    RenderLatest();
}

LRESULT OsdOverlay::OnDpiChanged(UINT, WPARAM, const LPARAM lparam, BOOL&) {
    const auto* suggested = reinterpret_cast<const RECT*>(lparam);
    POINT next{suggested->left, suggested->top};
    const Layout layout = CurrentLayout();
    (void)FitToWorkArea(next, layout);
    ApplyPosition(next, layout);
    RequestRefresh();
    return 0;
}

LRESULT OsdOverlay::OnDisplayChanged(UINT, WPARAM, LPARAM, BOOL&) {
    POINT next = Position();
    const Layout layout = CurrentLayout();
    (void)FitToWorkArea(next, layout);
    ApplyPosition(next, layout);
    RequestRefresh();
    return 0;
}

LRESULT OsdOverlay::OnSessionChanged(UINT, const WPARAM event, LPARAM, BOOL&) {
    if (event == WTS_SESSION_LOCK) {
        session_locked_ = true;
        (void)logging::TryInfo(L"Windows session locked; OSD remains registered");
    } else if (event == WTS_SESSION_UNLOCK) {
        session_locked_ = false;
        (void)logging::TryInfo(L"Windows session unlocked");
        ScheduleTopmostRepair(L"session unlock");
    }
    return 0;
}

LRESULT OsdOverlay::OnPowerBroadcast(UINT, const WPARAM event,
                                     const LPARAM data, BOOL&) {
    if (event == PBT_APMRESUMEAUTOMATIC) {
        (void)logging::TryInfo(L"System automatic resume received");
        ScheduleTopmostRepair(L"system resume");
        return TRUE;
    }
    if (event != PBT_POWERSETTINGCHANGE || data == 0) return TRUE;

    const auto* setting = reinterpret_cast<const POWERBROADCAST_SETTING*>(data);
    if (!IsEqualGUID(setting->PowerSetting, kSessionDisplayStatusGuid) ||
        setting->DataLength < sizeof(DWORD)) {
        return TRUE;
    }

    DWORD next_status{};
    std::memcpy(&next_status, setting->Data, sizeof(next_status));
    const std::optional<DWORD> previous_status = display_status_;
    display_status_ = next_status;
    const wchar_t* description = L"unknown";
    if (next_status == kDisplayOff) description = L"off";
    if (next_status == kDisplayOn) description = L"on";
    if (next_status == kDisplayDimmed) description = L"dimmed";
    (void)logging::TryInfo(std::format(
        L"Session display status -> {} ({})", description, next_status));

    if (next_status == kDisplayOn &&
        (!previous_status || *previous_status != kDisplayOn)) {
        ScheduleTopmostRepair(L"session display on");
    }
    return TRUE;
}

LRESULT OsdOverlay::OnExitSizeMove(UINT, WPARAM, LPARAM, BOOL&) {
    SynchronizeDragHandleToOverlay();
    return 0;
}

LRESULT OsdOverlay::OnRefreshTimer(UINT, const WPARAM timer_id, LPARAM, BOOL&) {
    if (timer_id == kRefreshTimerId) {
        refresh_pending_ = false;
        RenderLatest();
        CheckZOrder();
        return 0;
    }
    if (timer_id == kTopmostRepairTimerId) {
        KillTimer(kTopmostRepairTimerId);
        RepairTopmost();
    }
    return 0;
}

LRESULT OsdOverlay::OnEraseBackground(UINT, WPARAM, LPARAM, BOOL&) { return 1; }

void OsdOverlay::RenderLatest() noexcept {
    if (!Visible()) return;
    try {
        const bool success = Render();
        const auto now = GetTickCount64();
        if (!success && (!render_failed_ || now - last_render_error_ms_ >= 60'000)) {
            (void)logging::TryWarning(std::format(
                L"OSD render failed hwnd={} win32_error={}; prior frame retained where available",
                reinterpret_cast<std::uintptr_t>(m_hWnd), render_error_));
            last_render_error_ms_ = now;
        } else if (success && render_failed_) {
            (void)logging::TryInfo(L"OSD rendering recovered");
        }
        render_failed_ = !success;
    } catch (...) {
        // Rendering is observational and must never escape into protection processing.
    }
}

void OsdOverlay::CheckZOrder() noexcept {
    try {
    const auto now = GetTickCount64();
    if (now < next_zorder_check_ms_) return;
    next_zorder_check_ms_ = now + 3'000;
    if (!Visible() || session_locked_ ||
        (display_status_ && *display_status_ == kDisplayOff)) {
        previous_obstruction_ = nullptr;
        return;
    }
    // Never promote over the secure desktop or when composition cannot tell
    // us whether a window belongs to the currently presented desktop.
    HDESK desktop = OpenInputDesktop(0, FALSE, DESKTOP_READOBJECTS);
    if (!desktop) return;
    wchar_t input_name[128]{}, our_name[128]{};
    DWORD bytes = 0;
    const bool interactive = GetUserObjectInformationW(desktop, UOI_NAME,
        input_name, sizeof(input_name), &bytes) &&
        GetUserObjectInformationW(GetThreadDesktop(GetCurrentThreadId()), UOI_NAME,
            our_name, sizeof(our_name), &bytes) && wcscmp(input_name, our_name) == 0;
    CloseDesktop(desktop);
    if (!interactive) return;
    DWORD cloaked = 0;
    if (FAILED(DwmGetWindowAttribute(m_hWnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) ||
        cloaked != 0) return;
    RECT ours{};
    if (!GetWindowRect(&ours)) return;
    HWND obstruction = nullptr;
    HWND window = ::GetWindow(m_hWnd, GW_HWNDPREV);
    for (int inspected = 0; window && inspected < 128; ++inspected) {
        const HWND next = ::GetWindow(window, GW_HWNDPREV);
        if (window != drag_handle_.m_hWnd && ::IsWindowVisible(window) && !::IsIconic(window)) {
            RECT other{}, overlap{};
            DWORD window_cloaked = 0;
            if (::GetWindowRect(window, &other) && IntersectRect(&overlap, &ours, &other) &&
                SUCCEEDED(DwmGetWindowAttribute(window, DWMWA_CLOAKED,
                    &window_cloaked, sizeof(window_cloaked))) && window_cloaked == 0) {
                // A legitimate TOPMOST overlay/menu takes precedence. Avoid
                // an arms race even if an ordinary obstruction also exists.
                if ((::GetWindowLongPtrW(window, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0) {
                    previous_obstruction_ = nullptr;
                    return;
                }
                if (!obstruction) obstruction = window;
            }
        }
        window = next;
    }
    const bool confirmed = obstruction && obstruction == previous_obstruction_;
    previous_obstruction_ = obstruction;
    if (!confirmed || now - last_zorder_repair_ms_ < 30'000) return;
    last_zorder_repair_ms_ = now;
    const auto before_style = ::GetWindowLongPtrW(m_hWnd, GWL_EXSTYLE);
    DWORD owner = 0;
    ::GetWindowThreadProcessId(obstruction, &owner);
    const bool repaired = SetWindowPos(HWND_TOPMOST, 0, 0, 0, 0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_NOSENDCHANGING) != FALSE;
    const DWORD error = repaired ? ERROR_SUCCESS : GetLastError();
    if (repaired) SynchronizeDragHandleToOverlay();
    (void)logging::TryWarning(std::format(
        L"OSD Z-order anomaly: hwnd={} ex_style={} obstruction={} pid={} reasserted={} error={}",
        reinterpret_cast<std::uintptr_t>(m_hWnd), before_style,
        reinterpret_cast<std::uintptr_t>(obstruction), owner, repaired, error));
    previous_obstruction_ = nullptr;
    } catch (...) {
        // Diagnostics/repair must not terminate protection on allocation failure.
    }
}

void OsdOverlay::ScheduleTopmostRepair(const std::wstring_view reason) noexcept {
    if (!Visible()) {
        (void)logging::TryInfo(std::format(
            L"OSD topmost repair skipped while hidden; reason={}", reason));
        return;
    }

    topmost_repair_reason_.assign(reason);
    topmost_repair_passes_remaining_ = 2;
    KillTimer(kTopmostRepairTimerId);
    if (SetTimer(kTopmostRepairTimerId, kInitialTopmostRepairDelayMs) == 0) {
        topmost_repair_passes_remaining_ = 0;
        (void)logging::TryWarning(std::format(
            L"OSD topmost repair timer failed; reason={} error={}",
            topmost_repair_reason_, GetLastError()));
        return;
    }
    (void)logging::TryInfo(std::format(
        L"OSD topmost repair scheduled; reason={} passes=2",
        topmost_repair_reason_));
}

void OsdOverlay::RepairTopmost() noexcept {
    if (topmost_repair_passes_remaining_ == 0) return;
    const unsigned int pass = 3U - topmost_repair_passes_remaining_;
    if (!Visible()) {
        topmost_repair_passes_remaining_ = 0;
        (void)logging::TryInfo(std::format(
            L"OSD topmost repair cancelled while hidden; reason={}",
            topmost_repair_reason_));
        return;
    }

    POINT next = Position();
    const Layout layout = CurrentLayout();
    const bool placement_repaired = FitToWorkArea(next, layout);
    const bool rendered = Render();
    const BOOL overlay_promoted = SetWindowPos(
        HWND_TOPMOST, next.x, next.y, layout.width, layout.height,
        SWP_NOACTIVATE | SWP_SHOWWINDOW);
    BOOL drag_promoted = TRUE;
    if (drag_handle_.m_hWnd != nullptr) {
        drag_promoted = drag_handle_.SetWindowPos(
            HWND_TOPMOST, next.x, next.y, layout.width, layout.drag_height,
            SWP_NOACTIVATE | SWP_SHOWWINDOW);
    }

    (void)logging::TryInfo(std::format(
        L"OSD topmost repair pass {}/2; reason={} rendered={} overlay={} drag={} placement_repaired={}",
        pass, topmost_repair_reason_, rendered, overlay_promoted != FALSE,
        drag_promoted != FALSE, placement_repaired));

    --topmost_repair_passes_remaining_;
    if (topmost_repair_passes_remaining_ != 0) {
        if (SetTimer(kTopmostRepairTimerId, kFollowupTopmostRepairDelayMs) == 0) {
            topmost_repair_passes_remaining_ = 0;
            (void)logging::TryWarning(std::format(
                L"OSD follow-up topmost repair timer failed; reason={} error={}",
                topmost_repair_reason_, GetLastError()));
        }
    } else {
        topmost_repair_reason_.clear();
    }
}

OsdOverlay::Layout OsdOverlay::CurrentLayout() const noexcept {
    const HWND dpi_window = m_hWnd != nullptr ? m_hWnd : drag_handle_.m_hWnd;
    const UINT dpi = dpi_window == nullptr ? 96U : GetDpiForWindow(dpi_window);
    return {MulDiv(compact::Width(collapsed_), static_cast<int>(dpi), 96),
            MulDiv(compact::Height(collapsed_), static_cast<int>(dpi), 96),
            MulDiv(kDragHeightDip, static_cast<int>(dpi), 96),
            static_cast<float>(dpi) / 96.0F};
}

bool OsdOverlay::FitToWorkArea(POINT& point, const Layout& layout) const noexcept {
    RECT proposed{point.x, point.y, point.x + layout.width, point.y + layout.height};
    HMONITOR monitor = MonitorFromRect(&proposed, MONITOR_DEFAULTTONULL);
    const bool repaired = monitor == nullptr;
    if (monitor == nullptr) monitor = MonitorFromRect(&proposed, MONITOR_DEFAULTTONEAREST);
    MONITORINFO info{sizeof(info)};
    if (monitor == nullptr || GetMonitorInfoW(monitor, &info) == FALSE) return repaired;

    point.x = std::clamp(point.x, info.rcWork.left,
                         std::max(info.rcWork.left, info.rcWork.right - layout.width));
    point.y = std::clamp(point.y, info.rcWork.top,
                         std::max(info.rcWork.top, info.rcWork.bottom - layout.height));
    return repaired || point.x != proposed.left || point.y != proposed.top;
}

void OsdOverlay::ApplyPosition(const POINT point, const Layout& layout) noexcept {
    if (m_hWnd == nullptr) return;
    synchronizing_position_ = true;
    SetWindowPos(HWND_TOPMOST, point.x, point.y, layout.width, layout.height,
                 SWP_NOACTIVATE | (Visible() ? SWP_SHOWWINDOW : 0));
    if (drag_handle_.m_hWnd != nullptr) {
        drag_handle_.SetWindowPos(HWND_TOPMOST, point.x, point.y,
            layout.width, layout.drag_height,
            SWP_NOACTIVATE | (Visible() ? SWP_SHOWWINDOW : 0));
    }
    synchronizing_position_ = false;
}

void OsdOverlay::SynchronizeDragHandleToOverlay() noexcept {
    if (m_hWnd == nullptr || drag_handle_.m_hWnd == nullptr) return;
    RECT bounds{};
    GetWindowRect(&bounds);
    const Layout layout = CurrentLayout();
    synchronizing_position_ = true;
    drag_handle_.SetWindowPos(HWND_TOPMOST, bounds.left, bounds.top,
        layout.width, layout.drag_height,
        SWP_NOACTIVATE | (Visible() ? SWP_SHOWWINDOW : 0));
    synchronizing_position_ = false;
}

void OsdOverlay::HandleDragMove(const int x, const int y) noexcept {
    if (synchronizing_position_ || m_hWnd == nullptr) return;
    // Preserve the established Z-order. Promoting the painted OSD above the
    // drag popup would let the next drag move only one of the two HWNDs.
    SetWindowPos(nullptr, x, y, 0, 0,
                 SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOZORDER |
                     (Visible() ? SWP_SHOWWINDOW : 0));
}

void OsdOverlay::HandleDragEnd() noexcept {
    SynchronizeDragHandleToOverlay();
}

bool OsdOverlay::Render() noexcept {
    if (m_hWnd == nullptr) return false;
    render_error_ = ERROR_GEN_FAILURE;
    try {
        const Layout layout = CurrentLayout();
        ScopedScreenDc screen;
        if (screen.value == nullptr) return false;
        ScopedMemoryDc memory(screen.value);
        if (memory.value == nullptr) return false;

        BITMAPINFO bitmap_info{};
        bitmap_info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bitmap_info.bmiHeader.biWidth = layout.width;
        bitmap_info.bmiHeader.biHeight = -layout.height;
        bitmap_info.bmiHeader.biPlanes = 1;
        bitmap_info.bmiHeader.biBitCount = 32;
        bitmap_info.bmiHeader.biCompression = BI_RGB;
        void* pixels = nullptr;
        ScopedBitmap bitmap;
        bitmap.value = CreateDIBSection(screen.value, &bitmap_info, DIB_RGB_COLORS,
                                        &pixels, nullptr, 0);
        if (bitmap.value == nullptr || pixels == nullptr) return false;
        const HGDIOBJ previous = SelectObject(memory.value, bitmap.value);
        if (!previous || previous == HGDI_ERROR) return false;
        ScopedSelection selection{memory.value, previous};

        Gdiplus::Bitmap surface(layout.width, layout.height, layout.width * 4,
                                PixelFormat32bppPARGB, static_cast<BYTE*>(pixels));
        Gdiplus::Graphics graphics(&surface);
        if (surface.GetLastStatus() != Gdiplus::Ok ||
            graphics.GetLastStatus() != Gdiplus::Ok) return false;
        graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        graphics.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAliasGridFit);
        graphics.Clear(Gdiplus::Color(0, 0, 0, 0));

        const float s = layout.scale;
        const Gdiplus::RectF panel(0.0F, 0.0F,
                                   static_cast<Gdiplus::REAL>(layout.width),
                                   static_cast<Gdiplus::REAL>(layout.height));
        Gdiplus::GraphicsPath panel_path;
        AddRoundedRectangle(panel_path, panel, 7.0F * s);
        Gdiplus::LinearGradientBrush panel_brush(panel,
            Gdiplus::Color(220, 13, 28, 43), Gdiplus::Color(200, 8, 19, 31),
            Gdiplus::LinearGradientModeVertical);
        graphics.FillPath(&panel_brush, &panel_path);

        Gdiplus::FontFamily family(L"Segoe UI");
        Gdiplus::Font title_font(&family, 10.0F * s, Gdiplus::FontStyleBold,
                                 Gdiplus::UnitPixel);
        Gdiplus::Font status_font(&family, 9.5F * s, Gdiplus::FontStyleRegular,
                                  Gdiplus::UnitPixel);
        Gdiplus::Font label_font(&family, 13.0F * s, Gdiplus::FontStyleBold,
                                 Gdiplus::UnitPixel);
        Gdiplus::Font value_font(&family, 12.5F * s, Gdiplus::FontStyleBold,
                                 Gdiplus::UnitPixel);
        DrawText(graphics, L"GTG",
                 {10.0F * s, 2.0F * s, 30.0F * s, 22.0F * s}, title_font,
                 Gdiplus::Color(222, 237, 243, 251), Gdiplus::StringAlignmentNear);
        const std::uint64_t now_ms = GetTickCount64();
        static const std::deque<telemetry::Sample> empty;
        const auto& samples = history_ == nullptr ? empty : history_->Samples();
        const telemetry::Freshness freshness =
            telemetry::EvaluateFreshness(samples, now_ms);
        if (freshness.state != freshness_state_) {
            if (freshness.state == telemetry::FreshnessState::Fresh &&
                (freshness_state_ == telemetry::FreshnessState::Delayed ||
                 freshness_state_ == telemetry::FreshnessState::Unavailable)) {
                recovered_until_ms_ = now_ms + 3'000;
                (void)logging::TryInfo(L"OSD presentation telemetry recovered");
            } else if (freshness.state == telemetry::FreshnessState::Delayed) {
                (void)logging::TryWarning(std::format(
                    L"OSD presentation telemetry delayed; age_ms={}", freshness.age_ms));
            } else if (freshness.state == telemetry::FreshnessState::Unavailable) {
                (void)logging::TryWarning(std::format(
                    L"OSD presentation telemetry unavailable; age_ms={}", freshness.age_ms));
            }
            freshness_state_ = freshness.state;
        }

        const bool stale = freshness.state == telemetry::FreshnessState::Delayed ||
                           freshness.state == telemetry::FreshnessState::Unavailable;
        std::wstring displayed_status = status_;
        Gdiplus::Color status_color = VisualColor(visual_);
        const bool protection_alert = visual_ == OsdVisual::Protected;
        if (protection_alert || visual_ == OsdVisual::Fault) {
            // An informational recovery banner must never cover a safety alert.
            if (visual_ == OsdVisual::Fault) {
                displayed_status = localization::Select(
                    L"FAULT · 保護尚未確認", L"FAULT · Protection unverified");
            }
            if (stale || freshness.state == telemetry::FreshnessState::NoData) {
                displayed_status += localization::Select(L" · 資料過期", L" · stale");
            }
            status_color = protection_alert ? Gdiplus::Color(255, 255, 184, 72)
                                            : Gdiplus::Color(255, 255, 82, 92);
            Gdiplus::SolidBrush alert_accent(status_color);
            graphics.FillRectangle(&alert_accent, 2.0F * s, 5.0F * s, 3.0F * s, 15.0F * s);
        } else if (freshness.state == telemetry::FreshnessState::Delayed) {
            displayed_status = localization::Format(
                L"遙測延遲 · {:.1f} 秒", L"Telemetry delayed · {:.1f} s",
                static_cast<double>(freshness.age_ms) / 1'000.0);
            status_color = Gdiplus::Color(255, 255, 184, 72);
        } else if (freshness.state == telemetry::FreshnessState::Unavailable) {
            displayed_status = localization::Format(
                L"遙測無法使用 · {:.1f} 秒", L"Telemetry unavailable · {:.1f} s",
                static_cast<double>(freshness.age_ms) / 1'000.0);
            status_color = Gdiplus::Color(255, 255, 82, 92);
        } else if (freshness.state == telemetry::FreshnessState::Fresh &&
                   now_ms < recovered_until_ms_) {
            displayed_status = localization::Select(
                L"遙測已恢復 · 圖表缺口已保留", L"Telemetry recovered · gap retained");
            status_color = Gdiplus::Color(255, 70, 210, 137);
        }
        if (collapsed_) {
            const auto compact_status = compact::Resolve(visual_ == OsdVisual::Fault,
                protection_alert, freshness.state == telemetry::FreshnessState::Unavailable ||
                    freshness.state == telemetry::FreshnessState::NoData,
                freshness.state == telemetry::FreshnessState::Delayed,
                visual_ == OsdVisual::Warning, visual_ == OsdVisual::Armed);
            switch (compact_status) {
            case compact::Status::Fault:
                displayed_status = localization::Select(L"FAULT · 保護未確認", L"FAULT · Unverified"); break;
            case compact::Status::Protected:
                displayed_status = localization::Select(L"ALERT · 安全功率", L"ALERT · Safe power"); break;
            case compact::Status::Unavailable:
                displayed_status = localization::Select(L"無遙測資料", L"No telemetry");
                status_color = Gdiplus::Color(255, 255, 82, 92); break;
            case compact::Status::Delayed:
                displayed_status = localization::Select(L"遙測延遲", L"Telemetry delayed"); break;
            case compact::Status::Warning:
                displayed_status = localization::Select(L"溫度警示", L"Temperature warning");
                status_color = VisualColor(OsdVisual::Warning); break;
            case compact::Status::Monitoring:
                displayed_status = localization::Select(L"監控中", L"Monitoring"); break;
            case compact::Status::Initializing:
                displayed_status = localization::Select(L"初始化中", L"Initializing"); break;
            }
            if ((protection_alert || visual_ == OsdVisual::Fault) &&
                (stale || freshness.state == telemetry::FreshnessState::NoData)) {
                displayed_status += localization::Select(L" · 過期", L" · stale");
            }
        }
        BOOL animations = FALSE;
        if (collapsed_) SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &animations, 0);
        const bool attention = protection_alert || visual_ == OsdVisual::Fault ||
            visual_ == OsdVisual::Warning || stale || freshness.state == telemetry::FreshnessState::NoData;
        Gdiplus::SolidBrush dot(Gdiplus::Color(
            compact::PulseAlpha(collapsed_ && attention, animations != FALSE, now_ms),
            status_color.GetR(), status_color.GetG(), status_color.GetB()));
        graphics.FillEllipse(&dot, 46.0F * s, 9.0F * s, 6.0F * s, 6.0F * s);
        const float status_left = 58.0F * s;
        DrawText(graphics, displayed_status,
                 {status_left, 2.0F * s, layout.width - layout.drag_height - status_left - 4.0F * s,
                  22.0F * s}, status_font,
                 status_color, Gdiplus::StringAlignmentNear);
        Gdiplus::Pen handle(Gdiplus::Color(38, 203, 215, 231), 1.0F * s);
        graphics.DrawLine(&handle, layout.width / 2.0F - 10.0F * s, 23.0F * s,
                          layout.width / 2.0F + 10.0F * s, 23.0F * s);
        const float button_x = static_cast<float>(layout.width - layout.drag_height);
        Gdiplus::SolidBrush button_fill(Gdiplus::Color(22, 203, 215, 231));
        graphics.FillRectangle(&button_fill, button_x + 3.0F * s, 3.0F * s, 19.0F * s, 19.0F * s);
        Gdiplus::Pen chevron(Gdiplus::Color(235, 224, 235, 248), 1.5F * s);
        chevron.SetLineJoin(Gdiplus::LineJoinRound);
        const float edge_y = (collapsed_ ? 10.0F : 14.0F) * s;
        const float middle_y = (collapsed_ ? 14.0F : 10.0F) * s;
        const Gdiplus::PointF arrow[]{
            {button_x + 8.0F * s, edge_y}, {button_x + 12.5F * s, middle_y},
            {button_x + 17.0F * s, edge_y}};
        graphics.DrawLines(&chevron, arrow, 3);

        const telemetry::Sample* latest = history_ == nullptr ? nullptr : history_->Latest();
        const std::uint64_t end = freshness.latest_valid_ms.value_or(
            latest == nullptr ? now_ms : latest->monotonic_ms);
        const std::uint64_t start = end > kVisibleDurationMs ? end - kVisibleDurationMs : 0;
        const telemetry::Sample* displayed_sample = nullptr;
        if (freshness.latest_valid_ms) {
            for (auto it = samples.rbegin(); it != samples.rend(); ++it) {
                if (it->monotonic_ms == *freshness.latest_valid_ms) {
                    displayed_sample = &*it;
                    break;
                }
            }
        }
        const auto optional = [&](const MetricMember member) -> std::optional<double> {
            return displayed_sample == nullptr ? std::nullopt : displayed_sample->*member;
        };
        if (collapsed_) {
            // Same width/header/button coordinates as expanded mode; only the
            // chart body folds to one row. Never invent zero for missing data.
            Gdiplus::Font small_font(&family, 9.0F * s, Gdiplus::FontStyleRegular,
                                     Gdiplus::UnitPixel);
            const float cell_width = 72.0F * s;
            const auto cell = [&](int index, const std::wstring& label,
                                  const std::wstring& value, MetricMember member,
                                  const Gdiplus::Color color, double minimum_span) {
                const float x = (6.0F + index * 76.0F) * s;
                Gdiplus::SolidBrush background(Gdiplus::Color(15, color.GetR(), color.GetG(), color.GetB()));
                Gdiplus::Pen border(Gdiplus::Color(42, color.GetR(), color.GetG(), color.GetB()), s);
                Gdiplus::GraphicsPath path;
                AddRoundedRectangle(path, {x, 30.0F * s, cell_width, 51.0F * s}, 3.0F * s);
                graphics.FillPath(&background, &path);
                graphics.DrawPath(&border, &path);
                DrawText(graphics, label, {x + 4.0F * s, 31.0F * s, cell_width - 8.0F * s, 14.0F * s},
                         small_font, color, Gdiplus::StringAlignmentNear);
                DrawText(graphics, value, {x + 4.0F * s, 44.0F * s, cell_width - 8.0F * s, 22.0F * s},
                         value_font, stale ? Gdiplus::Color(180, 205, 214, 226)
                                          : Gdiplus::Color(255, 245, 248, 252),
                         Gdiplus::StringAlignmentNear);
                DrawCompactSparkline(graphics, samples, start, end, member,
                    {x + 5.0F * s, 67.0F * s, cell_width - 10.0F * s, 10.0F * s},
                    Gdiplus::Color(stale ? 140 : 235, color.GetR(), color.GetG(), color.GetB()),
                    minimum_span, s);
            };
            cell(0, std::wstring(localization::Select(L"溫度", L"Temp")),
                FormatMetric(optional(&telemetry::Sample::temperature_c), L" °C"),
                &telemetry::Sample::temperature_c, Gdiplus::Color(255, 255, 91, 94), 10.0);
            cell(1, std::wstring(localization::Select(L"功率", L"Power")),
                FormatMetric(optional(&telemetry::Sample::power_w), L" W", 1),
                &telemetry::Sample::power_w, Gdiplus::Color(255, 77, 160, 255), 100.0);
            cell(2, L"VRAM", FormatMetric(optional(&telemetry::Sample::vram_utilization_percent), L"%"),
                &telemetry::Sample::vram_utilization_percent, Gdiplus::Color(255, 255, 173, 69), 20.0);
            cell(3, L"GPU", FormatMetric(optional(&telemetry::Sample::gpu_utilization_percent), L"%"),
                &telemetry::Sample::gpu_utilization_percent, Gdiplus::Color(255, 75, 214, 139), 20.0);
            cell(4, L"CPU", FormatMetric(optional(&telemetry::Sample::cpu_utilization_percent), L"%"),
                &telemetry::Sample::cpu_utilization_percent, Gdiplus::Color(255, 183, 121, 255), 20.0);
        } else {
        const auto presentation_gaps = telemetry::FindPresentationGaps(samples, start, end);
        std::optional<double> maximum_temperature;
        std::optional<double> maximum_vram_percent;
        for (const auto& sample : samples) {
            if (sample.monotonic_ms < start || sample.monotonic_ms > end) continue;
            if (sample.temperature_c &&
                (!maximum_temperature || *sample.temperature_c > *maximum_temperature)) {
                maximum_temperature = sample.temperature_c;
            }
            if (sample.vram_utilization_percent &&
                (!maximum_vram_percent ||
                 *sample.vram_utilization_percent > *maximum_vram_percent)) {
                maximum_vram_percent = sample.vram_utilization_percent;
            }
        }

        const float row_height = 57.0F * s;
        const float row_width = 376.0F * s;
        const float row_x = 6.0F * s;
        const float row_start = 31.0F * s;
        const std::optional<double> temperature = optional(&telemetry::Sample::temperature_c);
        const std::wstring temperature_maximum = maximum_temperature
            ? localization::Format(L"最高 {:.0f} °C", L"max {:.0f} °C",
                                   *maximum_temperature) : L"";
        DrawMetricLane(graphics, samples, start, end,
            {row_x, row_start, row_width, row_height},
            std::wstring(localization::Select(L"溫度", L"Temp")),
            FormatMetric(temperature, L" °C"),
            Gdiplus::Color(255, 255, 91, 94), 30.0, 105.0,
            &telemetry::Sample::temperature_c, label_font, value_font,
            presentation_gaps, true,
            static_cast<double>(trigger_temperature_c_), temperature_maximum, stale);

        const double power_maximum = std::max(100.0, maximum_power_w_.value_or(
            static_cast<double>(std::max(600, safe_power_w_))));
        std::wstring power_value =
            FormatMetric(optional(&telemetry::Sample::power_w), L" W", 1);
        if (current_power_limit_w_) {
            power_value += std::format(L" ({:.0f} W)", *current_power_limit_w_);
        }
        DrawMetricLane(graphics, samples, start, end,
            {row_x, row_start + row_height + 2.0F * s, row_width, row_height},
            std::wstring(localization::Select(L"功率", L"Power")),
            power_value,
            Gdiplus::Color(255, 77, 160, 255), 0.0, power_maximum,
            &telemetry::Sample::power_w, label_font, value_font,
            presentation_gaps, false,
            static_cast<double>(safe_power_w_), {}, stale);
        const std::optional<double> vram_percent =
            optional(&telemetry::Sample::vram_utilization_percent);
        const std::optional<double> vram_used_gib =
            optional(&telemetry::Sample::vram_used_gib);
        std::wstring vram_value = L"—";
        if (vram_percent && vram_used_gib) {
            vram_value = std::format(L"{:.0f}% ({:.1f} GiB)", *vram_percent, *vram_used_gib);
        }
        const std::wstring vram_maximum = maximum_vram_percent
            ? localization::Format(L"最高 {:.0f}%", L"max {:.0f}%",
                                   *maximum_vram_percent) : L"";
        DrawMetricLane(graphics, samples, start, end,
            {row_x, row_start + (row_height + 2.0F * s) * 2.0F, row_width, row_height},
            L"VRAM %", vram_value,
            Gdiplus::Color(255, 255, 173, 69), 0.0, 100.0,
            &telemetry::Sample::vram_utilization_percent, label_font, value_font,
            presentation_gaps, false,
            std::nullopt, vram_maximum, stale);
        DrawMetricLane(graphics, samples, start, end,
            {row_x, row_start + (row_height + 2.0F * s) * 3.0F, row_width, row_height},
            L"GPU %", FormatMetric(optional(&telemetry::Sample::gpu_utilization_percent), L"%"),
            Gdiplus::Color(255, 75, 214, 139), 0.0, 100.0,
            &telemetry::Sample::gpu_utilization_percent, label_font, value_font,
            presentation_gaps, false,
            std::nullopt, {}, stale);
        DrawMetricLane(graphics, samples, start, end,
            {row_x, row_start + (row_height + 2.0F * s) * 4.0F, row_width, row_height},
            L"CPU %", FormatMetric(optional(&telemetry::Sample::cpu_utilization_percent), L"%"),
            Gdiplus::Color(255, 183, 121, 255), 0.0, 100.0,
            &telemetry::Sample::cpu_utilization_percent, label_font, value_font,
            presentation_gaps, false,
            std::nullopt, {}, stale);

        }
        RECT window{};
        GetWindowRect(&window);
        POINT destination{window.left, window.top};
        POINT source{};
        SIZE size{layout.width, layout.height};
        BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
        graphics.Flush(Gdiplus::FlushIntentionSync);
        const BOOL updated = UpdateLayeredWindow(m_hWnd, screen.value, &destination, &size,
            memory.value, &source, 0, &blend, ULW_ALPHA);
        render_error_ = updated ? ERROR_SUCCESS : GetLastError();
        return updated != FALSE;
    } catch (...) {
        return false;
    }
}

}  // namespace gtg::tray
