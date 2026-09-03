#include "tray/osd_overlay.hpp"
#include "localization/localization.hpp"

#include <windows.h>
#include <windowsx.h>
#include <gdiplus.h>

#include <algorithm>
#include <cmath>
#include <format>
#include <limits>
#include <utility>
#include <vector>

namespace gtg::tray {
namespace {

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
                    const std::optional<double> threshold = std::nullopt,
                    const std::wstring_view annotation = {}) {
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
             Gdiplus::Color(255, 245, 248, 252), Gdiplus::StringAlignmentFar);

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

    std::vector<Gdiplus::PointF> segment;
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

    for (const auto& sample : samples) {
        if (sample.monotonic_ms < visible_start || sample.monotonic_ms > visible_end) continue;
        const auto& value = sample.*member;
        if (!value) {
            flush_segment();
            continue;
        }
        const double elapsed = static_cast<double>(sample.monotonic_ms - visible_start);
        const double duration = static_cast<double>(std::max<std::uint64_t>(1,
            visible_end - visible_start));
        const Gdiplus::REAL x = graph.X +
            static_cast<Gdiplus::REAL>(elapsed / duration) * graph.Width;
        segment.emplace_back(x, y_for(*value));
    }
    flush_segment();
}

}  // namespace

LRESULT OsdDragHandle::OnNcHitTest(UINT, WPARAM, LPARAM, BOOL&) { return HTCAPTION; }

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

    drag_handle_.SetOwner(this);
    const DWORD drag_ex_style = WS_EX_LAYERED | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW |
                                WS_EX_TOPMOST;
    if (drag_handle_.Create(nullptr, initial, L"GPU Thermal Guard OSD drag handle",
                            WS_POPUP, drag_ex_style) == nullptr) {
        DestroyWindow();
        return false;
    }
    SetLayeredWindowAttributes(drag_handle_, 0, 1, LWA_ALPHA);

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
    if (m_hWnd != nullptr) KillTimer(kRefreshTimerId);
    refresh_pending_ = false;
    drag_handle_.SetOwner(nullptr);
    if (drag_handle_.m_hWnd != nullptr) drag_handle_.DestroyWindow();
    if (m_hWnd != nullptr) DestroyWindow();
    history_ = nullptr;
    notification_window_ = nullptr;
}

void OsdOverlay::SetVisible(const bool should_show) {
    if (m_hWnd == nullptr || should_show == Visible()) return;
    if (should_show) {
        (void)Render();
        ShowWindow(SW_SHOWNOACTIVATE);
        SetWindowPos(HWND_TOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
        drag_handle_.ShowWindow(SW_SHOWNOACTIVATE);
        drag_handle_.SetWindowPos(HWND_TOPMOST, 0, 0, 0, 0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
        (void)SetTimer(kRefreshTimerId, static_cast<UINT>(kRenderIntervalMs));
    } else {
        KillTimer(kRefreshTimerId);
        refresh_pending_ = false;
        drag_handle_.ShowWindow(SW_HIDE);
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

void OsdOverlay::SetStatus(const std::wstring_view status, const OsdVisual visual) {
    status_.assign(status);
    visual_ = visual;
    RequestRefresh();
}

POINT OsdOverlay::Position() const noexcept {
    RECT bounds{};
    if (drag_handle_.m_hWnd != nullptr) {
        drag_handle_.GetWindowRect(&bounds);
    } else if (m_hWnd != nullptr) {
        GetWindowRect(&bounds);
    }
    return {bounds.left, bounds.top};
}

LRESULT OsdOverlay::OnNcHitTest(UINT, WPARAM, const LPARAM lparam, BOOL&) {
    POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
    ScreenToClient(&point);
    if (point.y >= 0 && point.y < CurrentLayout().drag_height) return HTCAPTION;
#if GTG_OSD_CLICK_THROUGH
    return HTTRANSPARENT;
#else
    return HTCLIENT;
#endif
}

LRESULT OsdOverlay::OnMouseActivate(UINT, WPARAM, LPARAM, BOOL&) { return MA_NOACTIVATE; }

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
    const bool repaired = FitToWorkArea(next, layout);
    ApplyPosition(next, layout);
    RequestRefresh();
    if (repaired && notification_window_ != nullptr) {
        ::PostMessageW(notification_window_, kPlacementChangedMessage, TRUE, 0);
    }
    return 0;
}

LRESULT OsdOverlay::OnExitSizeMove(UINT, WPARAM, LPARAM, BOOL&) {
    if (notification_window_ != nullptr) {
        ::PostMessageW(notification_window_, kPlacementChangedMessage, FALSE, 0);
    }
    return 0;
}

LRESULT OsdOverlay::OnRefreshTimer(UINT, const WPARAM timer_id, LPARAM, BOOL&) {
    if (timer_id != kRefreshTimerId) return 0;
    if (!refresh_pending_) return 0;
    refresh_pending_ = false;
    RenderLatest();
    return 0;
}

LRESULT OsdOverlay::OnEraseBackground(UINT, WPARAM, LPARAM, BOOL&) { return 1; }

void OsdOverlay::RenderLatest() noexcept {
    if (!Visible()) return;
    try {
        (void)Render();
    } catch (...) {
        // Rendering is observational and must never escape into protection processing.
    }
}

OsdOverlay::Layout OsdOverlay::CurrentLayout() const noexcept {
    const HWND dpi_window = drag_handle_.m_hWnd != nullptr ? drag_handle_.m_hWnd : m_hWnd;
    const UINT dpi = dpi_window == nullptr ? 96U : GetDpiForWindow(dpi_window);
    return {MulDiv(kWidthDip, static_cast<int>(dpi), 96),
            MulDiv(kHeightDip, static_cast<int>(dpi), 96),
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

void OsdOverlay::HandleDragMove(const int x, const int y) noexcept {
    if (synchronizing_position_ || m_hWnd == nullptr) return;
    SetWindowPos(HWND_TOPMOST, x, y, 0, 0,
                 SWP_NOSIZE | SWP_NOACTIVATE | (Visible() ? SWP_SHOWWINDOW : 0));
}

void OsdOverlay::HandleDragEnd() noexcept {
    if (notification_window_ != nullptr) {
        ::PostMessageW(notification_window_, kPlacementChangedMessage, FALSE, 0);
    }
}

bool OsdOverlay::Render() noexcept {
    if (m_hWnd == nullptr) return false;
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

        Gdiplus::Bitmap surface(layout.width, layout.height, layout.width * 4,
                                PixelFormat32bppPARGB, static_cast<BYTE*>(pixels));
        Gdiplus::Graphics graphics(&surface);
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
        DrawText(graphics, L"GPU THERMAL GUARD",
                 {10.0F * s, 2.0F * s, 142.0F * s, 22.0F * s}, title_font,
                 Gdiplus::Color(222, 237, 243, 251), Gdiplus::StringAlignmentNear);
        const Gdiplus::Color status_color = VisualColor(visual_);
        Gdiplus::SolidBrush dot(status_color);
        graphics.FillEllipse(&dot, 158.0F * s, 9.0F * s, 6.0F * s, 6.0F * s);
        DrawText(graphics, status_,
                 {169.0F * s, 2.0F * s, 209.0F * s, 22.0F * s}, status_font,
                 status_color, Gdiplus::StringAlignmentNear);
        Gdiplus::Pen handle(Gdiplus::Color(38, 203, 215, 231), 1.0F * s);
        graphics.DrawLine(&handle, 176.0F * s, 23.0F * s, 212.0F * s, 23.0F * s);

        const telemetry::Sample* latest = history_ == nullptr ? nullptr : history_->Latest();
        const std::uint64_t end = latest == nullptr ? GetTickCount64() : latest->monotonic_ms;
        const std::uint64_t start = end > kVisibleDurationMs ? end - kVisibleDurationMs : 0;
        static const std::deque<telemetry::Sample> empty;
        const auto& samples = history_ == nullptr ? empty : history_->Samples();
        const auto optional = [&](const MetricMember member) -> std::optional<double> {
            return latest == nullptr ? std::nullopt : latest->*member;
        };
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
            static_cast<double>(trigger_temperature_c_), temperature_maximum);

        const double power_maximum = std::max(100.0, maximum_power_w_.value_or(
            static_cast<double>(std::max(600, safe_power_w_))));
        DrawMetricLane(graphics, samples, start, end,
            {row_x, row_start + row_height + 2.0F * s, row_width, row_height},
            std::wstring(localization::Select(L"功率", L"Power")),
            FormatMetric(optional(&telemetry::Sample::power_w), L" W", 1),
            Gdiplus::Color(255, 77, 160, 255), 0.0, power_maximum,
            &telemetry::Sample::power_w, label_font, value_font,
            static_cast<double>(safe_power_w_));
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
            std::nullopt, vram_maximum);
        DrawMetricLane(graphics, samples, start, end,
            {row_x, row_start + (row_height + 2.0F * s) * 3.0F, row_width, row_height},
            L"GPU %", FormatMetric(optional(&telemetry::Sample::gpu_utilization_percent), L"%"),
            Gdiplus::Color(255, 75, 214, 139), 0.0, 100.0,
            &telemetry::Sample::gpu_utilization_percent, label_font, value_font);
        DrawMetricLane(graphics, samples, start, end,
            {row_x, row_start + (row_height + 2.0F * s) * 4.0F, row_width, row_height},
            L"CPU %", FormatMetric(optional(&telemetry::Sample::cpu_utilization_percent), L"%"),
            Gdiplus::Color(255, 183, 121, 255), 0.0, 100.0,
            &telemetry::Sample::cpu_utilization_percent, label_font, value_font);

        RECT window{};
        GetWindowRect(&window);
        POINT destination{window.left, window.top};
        POINT source{};
        SIZE size{layout.width, layout.height};
        BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
        const BOOL updated = UpdateLayeredWindow(m_hWnd, screen.value, &destination, &size,
            memory.value, &source, 0, &blend, ULW_ALPHA);
        SelectObject(memory.value, previous);
        return updated != FALSE;
    } catch (...) {
        return false;
    }
}

}  // namespace gtg::tray
