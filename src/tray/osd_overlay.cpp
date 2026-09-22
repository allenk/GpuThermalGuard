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

// Two overlaid series on a shared 0-100 % scale: Virtual Commit first, then
// physical RAM, so RAM always reads as the subject. Unavailable readings break
// the stroke rather than dropping to a fabricated 0 %.
void DrawMemorySparkline(Gdiplus::Graphics& graphics,
                         const std::deque<sysmem::HistorySample>& samples,
                         const std::uint64_t start, const std::uint64_t end,
                         const Gdiplus::RectF& bounds,
                         const Gdiplus::Color physical_color,
                         const Gdiplus::Color commit_color, const float scale) {
    const double duration = static_cast<double>(std::max<std::uint64_t>(1, end - start));
    const auto x_for = [&](const std::uint64_t timestamp) {
        return bounds.X +
               static_cast<float>((timestamp - start) / duration) * bounds.Width;
    };

    // A history younger than the window leaves a real absence at the left. Mark
    // it the way FPS does, so it reads as "not observed yet" rather than as a
    // broken curve. The first sample is never dragged to the edge: that would
    // draw a value nobody measured.
    std::optional<std::uint64_t> first_visible;
    for (const auto& sample : samples) {
        if (sample.monotonic_ms < start || sample.monotonic_ms > end) continue;
        if (!sample.physical_percent && !sample.commit_percent) continue;
        first_visible = sample.monotonic_ms;
        break;
    }
    const float data_left = first_visible ? x_for(*first_visible) : bounds.GetRight();
    if (data_left > bounds.X + 1.0F) {
        Gdiplus::Pen no_data(
            Gdiplus::Color(120, physical_color.GetR(), physical_color.GetG(),
                           physical_color.GetB()),
            std::max(1.0F, 0.8F * scale));
        no_data.SetDashStyle(Gdiplus::DashStyleDot);
        const float y = bounds.Y + bounds.Height / 2.0F;
        graphics.DrawLine(&no_data, bounds.X, y, data_left, y);
    }

    const auto stroke = [&](const bool commit, const Gdiplus::Color color,
                            const float width) {
        Gdiplus::Pen line(color, std::max(1.0F, width * scale));
        line.SetLineJoin(Gdiplus::LineJoinRound);
        std::vector<Gdiplus::PointF> points;
        const auto flush = [&] {
            if (points.size() >= 2)
                graphics.DrawLines(&line, points.data(),
                                   static_cast<INT>(points.size()));
            points.clear();
        };
        for (const auto& sample : samples) {
            if (sample.monotonic_ms < start || sample.monotonic_ms > end) continue;
            const auto value = commit ? sample.commit_percent
                                      : sample.physical_percent;
            if (!value) {
                flush();
                continue;
            }
            const float fraction = std::clamp(*value / 100.0F, 0.0F, 1.0F);
            points.push_back({x_for(sample.monotonic_ms),
                              bounds.GetBottom() - fraction * bounds.Height});
        }
        flush();
    };
    stroke(true, commit_color, 0.8F);
    stroke(false, physical_color, 1.0F);
}

void DrawFpsSparkline(Gdiplus::Graphics& graphics,
                      const std::deque<fps::HistorySample>& samples,
                      const std::uint64_t start, const std::uint64_t end,
                      const Gdiplus::RectF& bounds,
                      const Gdiplus::Color color, const float scale) {
    double maximum = 60.0;
    for (auto it = samples.rbegin(); it != samples.rend(); ++it) {
        if (it->monotonic_ms < start) break;
        if (it->monotonic_ms <= end && it->displayed_fps)
            maximum = std::max(maximum, *it->displayed_fps * 1.1);
    }
    Gdiplus::Pen no_data(Gdiplus::Color(145, color.GetR(), color.GetG(), color.GetB()),
                         std::max(1.0F, 0.8F * scale));
    no_data.SetDashStyle(Gdiplus::DashStyleDot);
    graphics.DrawLine(&no_data, bounds.X, bounds.GetBottom(),
                      bounds.GetRight(), bounds.GetBottom());

    const double duration = static_cast<double>(std::max<std::uint64_t>(1, end - start));
    const auto point_for = [&](const fps::HistorySample& sample) {
        const float x = bounds.X +
            static_cast<float>((sample.monotonic_ms - start) / duration) * bounds.Width;
        const float fraction = static_cast<float>(*sample.displayed_fps / maximum);
        return Gdiplus::PointF(x, bounds.GetBottom() - fraction * bounds.Height);
    };
    Gdiplus::Pen line(color, std::max(1.0F, scale));
    line.SetLineJoin(Gdiplus::LineJoinRound);
    std::vector<Gdiplus::PointF> points;
    const fps::HistorySample* last_valid = nullptr;
    const fps::HistorySample* last_sample = nullptr;
    for (const auto& sample : samples) {
        if (sample.monotonic_ms < start || sample.monotonic_ms > end) continue;
        last_sample = &sample;
        if (sample.displayed_fps) {
            points.push_back(point_for(sample));
            last_valid = &sample;
        }
    }
    if (last_valid != nullptr && last_sample != nullptr &&
        !last_sample->displayed_fps &&
        last_sample->monotonic_ms > last_valid->monotonic_ms) {
        const float x = bounds.X +
            static_cast<float>((last_sample->monotonic_ms - start) / duration) * bounds.Width;
        points.emplace_back(x, point_for(*last_valid).Y);
    }
    if (points.size() >= 2) {
        Gdiplus::GraphicsPath area;
        area.AddLines(points.data(), static_cast<INT>(points.size()));
        area.AddLine(points.back(), {points.back().X, bounds.GetBottom()});
        area.AddLine(Gdiplus::PointF(points.back().X, bounds.GetBottom()),
                     Gdiplus::PointF(points.front().X, bounds.GetBottom()));
        area.CloseFigure();
        Gdiplus::SolidBrush fill(Gdiplus::Color(40, color.GetR(),
                                     color.GetG(), color.GetB()));
        graphics.FillPath(&fill, &area);
        graphics.DrawLines(&line, points.data(), static_cast<INT>(points.size()));
    } else if (points.size() == 1) {
        Gdiplus::SolidBrush dot(color);
        graphics.FillEllipse(&dot, points.front().X - scale,
                             points.front().Y - scale, 2 * scale, 2 * scale);
    }
    Gdiplus::SolidBrush gap_fill(Gdiplus::Color(95, 8, 19, 31));
    Gdiplus::Pen delayed(Gdiplus::Color(230,
        static_cast<BYTE>(color.GetR() * 0.45F),
        static_cast<BYTE>(color.GetG() * 0.55F),
        static_cast<BYTE>(color.GetB() * 0.55F)),
        std::max(1.0F, 0.9F * scale));
    fps::ForEachHistoryStroke(samples, start, end,
        [&](const fps::HistorySample& from, const fps::HistorySample& to,
            const bool dim) {
            if (!dim) return;
            const auto a = point_for(from);
            const float x = bounds.X +
                static_cast<float>((to.monotonic_ms - start) / duration) * bounds.Width;
            const Gdiplus::PointF b(x, to.displayed_fps ? point_for(to).Y : a.Y);
            Gdiplus::GraphicsPath area;
            area.AddLine(a, b);
            area.AddLine(b, {b.X, bounds.GetBottom()});
            area.AddLine(Gdiplus::PointF(b.X, bounds.GetBottom()),
                         Gdiplus::PointF(a.X, bounds.GetBottom()));
            area.CloseFigure();
            graphics.FillPath(&gap_fill, &area);
            graphics.DrawLine(&delayed, a, b);
        });
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
    const Gdiplus::REAL scale = compact::LaneScale(row.Height);
    Gdiplus::SolidBrush background(Gdiplus::Color(12, 255, 255, 255));
    graphics.FillRectangle(&background, row);
    Gdiplus::Pen separator(Gdiplus::Color(28, 190, 205, 224), 1.0F * scale);
    graphics.DrawLine(&separator, row.X, row.GetBottom(), row.GetRight(), row.GetBottom());

    // 84, not 70: "VRAM %" overran a 70 dip box and ellipsized to "VRA...".
    const auto label_span = compact::LaneLabelSpan(row.X, scale);
    const auto graph_span = compact::LaneGraphSpan(row.X, row.Width, scale);
    const Gdiplus::RectF label_rect(label_span.x, row.Y, label_span.width,
                                    row.Height);
    DrawText(graphics, label, label_rect, label_font, color,
             Gdiplus::StringAlignmentNear);
    if (!annotation.empty()) {
        const Gdiplus::RectF annotation_rect(graph_span.x, row.Y,
                                              row.Width - 240.0F * scale,
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

    const Gdiplus::RectF graph(graph_span.x, row.Y + 23.0F * scale,
                               graph_span.width, row.Height - 30.0F * scale);
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

void OsdOverlay::SetCompactLayout(const compact::Layout layout) noexcept {
    if (!compact::IsValidLayout(layout)) return;
    layout_ = layout;
    if (m_hWnd == nullptr) return;
    POINT next = Position();
    const Layout geometry = CurrentLayout();
    (void)FitToWorkArea(next, geometry);
    ApplyPosition(next, geometry);
    RequestRefresh();
}

void OsdOverlay::SetCompactLocked(const bool locked) noexcept {
    if (compact_locked_ == locked) return;
    compact_locked_ = locked;
    RequestRefresh();
}

void OsdOverlay::SetRamEnabled(const bool enabled) noexcept {
    if (ram_enabled_ == enabled) return;
    ram_enabled_ = enabled;
    if (m_hWnd == nullptr) return;
    POINT next = Position();
    const Layout layout = CurrentLayout();
    (void)FitToWorkArea(next, layout);
    ApplyPosition(next, layout);
    RequestRefresh();
}

void OsdOverlay::SetFpsEnabled(const bool enabled) noexcept {
    if (fps_enabled_ == enabled) return;
    fps_enabled_ = enabled;
    if (m_hWnd == nullptr) return;
    POINT next = Position();
    const Layout layout = CurrentLayout();
    (void)FitToWorkArea(next, layout);
    ApplyPosition(next, layout);
    RequestRefresh();
}

void OsdOverlay::SetFpsSnapshot(const fps::Snapshot snapshot) noexcept {
    fps_snapshot_ = snapshot;
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
    // Both header controls must be carved out of the caption band or Windows
    // turns the press into a window drag and no button message ever arrives.
    if (ToggleHit(point) || LockHit(point)) return HTCLIENT;
    if (point.y >= 0 && point.y < CurrentLayout().drag_height) return HTCAPTION;
    return HTCLIENT;
#endif
}

LRESULT OsdOverlay::OnMouseActivate(UINT, WPARAM, LPARAM, BOOL&) { return MA_NOACTIVATE; }

bool OsdOverlay::ToggleHit(const POINT point) const noexcept {
    const auto layout = CurrentLayout();
    return compact::ToggleHit(point.x, point.y, layout.width, layout.drag_height);
}

bool OsdOverlay::LockHit(const POINT point) const noexcept {
    if (!ShowsLock()) return false;
    const auto layout = CurrentLayout();
    return compact::LockHit(point.x, point.y, layout.width, layout.drag_height);
}

LRESULT OsdOverlay::OnTogglePointer(UINT message, WPARAM, LPARAM point, BOOL&) {
    return HandleTogglePointer(message, m_hWnd, point);
}

LRESULT OsdOverlay::OnDragMove(UINT, WPARAM, const LPARAM point, BOOL& handled) {
    if (drag_from_ < 0) {
        handled = FALSE;
        return 0;
    }
    MoveDragImage({GET_X_LPARAM(point), GET_Y_LPARAM(point)});
    return 0;
}

LRESULT OsdOverlay::OnDragCursor(UINT, WPARAM, LPARAM, BOOL& handled) {
    if (drag_from_ < 0) {
        handled = FALSE;
        return 0;
    }
    POINT cursor{};
    ::GetCursorPos(&cursor);
    ::ScreenToClient(m_hWnd, &cursor);
    const Layout geometry = CurrentLayout();
    const auto grid = compact::MakeCellGrid(geometry.columns, geometry.width,
                                            geometry.scale);
    // A release outside the two rows changes nothing, so say so before the
    // reader commits to it.
    const bool droppable =
        compact::DropTargetAt(grid, cursor.x, cursor.y).row >= 0;
    ::SetCursor(::LoadCursorW(nullptr, droppable ? IDC_SIZEALL : IDC_NO));
    return TRUE;
}

namespace {

Gdiplus::Color CompactAccent(const compact::Metric metric) noexcept {
    switch (metric) {
        case compact::Metric::Temperature: return {255, 255, 91, 94};
        case compact::Metric::Power: return {255, 77, 160, 255};
        case compact::Metric::Vram: return {255, 255, 173, 69};
        case compact::Metric::Gpu: return {255, 75, 214, 139};
        case compact::Metric::Cpu: return {255, 183, 121, 255};
        case compact::Metric::Ram: return {255, 255, 122, 196};
        case compact::Metric::Fps: return {255, 92, 220, 215};
    }
    return {255, 245, 248, 252};
}

std::wstring CompactLabel(const compact::Metric metric) {
    switch (metric) {
        case compact::Metric::Temperature:
            return std::wstring(localization::Select(L"溫度", L"Temp"));
        case compact::Metric::Power:
            return std::wstring(localization::Select(L"功率", L"Power"));
        case compact::Metric::Vram: return L"VRAM";
        case compact::Metric::Gpu: return L"GPU";
        case compact::Metric::Cpu: return L"CPU";
        case compact::Metric::Ram: return L"RAM";
        case compact::Metric::Fps: return L"FPS";
    }
    return L"";
}

}  // namespace

void OsdOverlay::BeginDragImage(const compact::Metric metric,
                                const compact::CellGrid& grid,
                                const POINT cursor,
                                const POINT cell_origin) noexcept {
    try {
        EndDragImage();
        drag_metric_ = metric;
        drag_hotspot_ = {cursor.x - cell_origin.x, cursor.y - cell_origin.y};
        const int width =
            static_cast<int>(grid.cell_width_dip * grid.scale);
        const int height = static_cast<int>(compact::kCellHeightDip * grid.scale);
        if (width <= 0 || height <= 0) return;

        const DWORD ex_style = WS_EX_LAYERED | WS_EX_TRANSPARENT |
                               WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW |
                               WS_EX_TOPMOST;
        RECT bounds{0, 0, width, height};
        // Owned by the overlay so Windows keeps it above its owner, the same
        // arrangement the click-through drag strip already relies on.
        if (drag_image_.Create(m_hWnd, bounds, nullptr, WS_POPUP, ex_style) ==
            nullptr)
            return;

        ScopedScreenDc screen;
        if (screen.value == nullptr) { EndDragImage(); return; }
        ScopedMemoryDc memory(screen.value);
        if (memory.value == nullptr) { EndDragImage(); return; }
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = width;
        info.bmiHeader.biHeight = -height;
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        void* pixels = nullptr;
        ScopedBitmap bitmap;
        bitmap.value = CreateDIBSection(screen.value, &info, DIB_RGB_COLORS,
                                        &pixels, nullptr, 0);
        if (bitmap.value == nullptr) { EndDragImage(); return; }
        const HGDIOBJ previous = SelectObject(memory.value, bitmap.value);
        ScopedSelection selection{memory.value, previous};
        {
            Gdiplus::Graphics graphics(memory.value);
            graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
            graphics.SetTextRenderingHint(
                Gdiplus::TextRenderingHintClearTypeGridFit);
            const Gdiplus::Color accent = CompactAccent(metric);
            const Gdiplus::RectF face(0.0F, 0.0F, static_cast<float>(width),
                                      static_cast<float>(height));
            Gdiplus::GraphicsPath rounded;
            AddRoundedRectangle(rounded, face, 3.0F * grid.scale);
            Gdiplus::SolidBrush fill(Gdiplus::Color(
                150, accent.GetR(), accent.GetG(), accent.GetB()));
            graphics.FillPath(&fill, &rounded);
            Gdiplus::Pen border(Gdiplus::Color(220, accent.GetR(), accent.GetG(),
                                               accent.GetB()),
                                grid.scale);
            graphics.DrawPath(&border, &rounded);
            Gdiplus::FontFamily family(L"Segoe UI");
            Gdiplus::Font font(&family, 9.0F * grid.scale,
                               Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
            DrawText(graphics, CompactLabel(metric),
                     {4.0F * grid.scale, 0.0F,
                      static_cast<float>(width) - 8.0F * grid.scale,
                      static_cast<float>(height)},
                     font, Gdiplus::Color(255, 16, 26, 38),
                     Gdiplus::StringAlignmentNear);
            graphics.Flush(Gdiplus::FlushIntentionSync);
        }
        POINT origin{cursor.x - drag_hotspot_.x, cursor.y - drag_hotspot_.y};
        ::ClientToScreen(m_hWnd, &origin);
        POINT source{};
        SIZE size{width, height};
        BLENDFUNCTION blend{AC_SRC_OVER, 0, 235, AC_SRC_ALPHA};
        ::UpdateLayeredWindow(drag_image_, screen.value, &origin, &size,
                              memory.value, &source, 0, &blend, ULW_ALPHA);
        ::ShowWindow(drag_image_, SW_SHOWNOACTIVATE);
    } catch (...) {
        EndDragImage();
    }
}

void OsdOverlay::MoveDragImage(const POINT cursor) noexcept {
    if (drag_image_.m_hWnd == nullptr) return;
    POINT origin{cursor.x - drag_hotspot_.x, cursor.y - drag_hotspot_.y};
    ::ClientToScreen(m_hWnd, &origin);
    // Static content: this is a move, not a repaint.
    ::SetWindowPos(drag_image_, HWND_TOPMOST, origin.x, origin.y, 0, 0,
                   SWP_NOSIZE | SWP_NOACTIVATE);
}

void OsdOverlay::EndDragImage() noexcept {
    if (drag_image_.m_hWnd != nullptr) drag_image_.DestroyWindow();
}

void OsdOverlay::FinishArrangement(const int from_slot, const POINT point) noexcept {
    const Layout geometry = CurrentLayout();
    const auto placed = compact::Resolve(layout_, ram_enabled_, fps_enabled_);
    const auto grid = compact::MakeCellGrid(geometry.columns, geometry.width,
                                            geometry.scale);
    const auto target = compact::DropTargetAt(grid, point.x, point.y);
    if (target.row < 0) return;  // released outside the cells: change nothing
    const auto next =
        compact::ApplyDrop(layout_, placed, from_slot, target.row, target.column);
    if (next.order == layout_.order && next.row1 == layout_.row1) return;
    SetCompactLayout(next);
    RenderLatest();
}

LRESULT OsdOverlay::HandleTogglePointer(const UINT message, const HWND input_window,
                                       const LPARAM point) noexcept {
    const POINT cursor{GET_X_LPARAM(point), GET_Y_LPARAM(point)};
    const Layout geometry = CurrentLayout();
    const bool on_chevron =
        compact::ToggleHit(cursor.x, cursor.y, geometry.width, geometry.drag_height);
    const bool on_lock = ShowsLock() &&
        compact::LockHit(cursor.x, cursor.y, geometry.width, geometry.drag_height);

    if (message == WM_LBUTTONDOWN) {
        if (on_lock) {
            lock_gesture_.Press(true);
            ::SetCapture(input_window);
            return 0;
        }
        // A press only begins a drag when it lands squarely on a cell, so an
        // aim that misses leaves the chevron and the drag bar their own input.
        if (Arrangeable()) {
            const auto placed =
                compact::Resolve(layout_, ram_enabled_, fps_enabled_);
            const int slot = compact::CellSlotAt(
                compact::MakeCellGrid(geometry.columns, geometry.width,
                                      geometry.scale),
                placed.count, cursor.x, cursor.y);
            if (slot >= 0) {
                drag_from_ = slot;
                ::SetCapture(input_window);
                const auto grid = compact::MakeCellGrid(
                    geometry.columns, geometry.width, geometry.scale);
                const auto origin = compact::CellOriginAt(grid, slot);
                BeginDragImage(placed.cells[static_cast<std::size_t>(slot)],
                               grid, cursor,
                               POINT{static_cast<LONG>(origin.x),
                                     static_cast<LONG>(origin.y)});
                return 0;
            }
        }
        toggle_gesture_.Press(on_chevron);
        if (on_chevron) ::SetCapture(input_window);
        return 0;
    }

    if (message == WM_LBUTTONUP) {
        const bool captured = ::GetCapture() == input_window;
        // Every gesture is settled and capture released exactly once, whichever
        // one was in flight; the outcomes are mutually exclusive.
        const int from_slot = drag_from_;
        drag_from_ = -1;
        const bool lock_click = lock_gesture_.Release(on_lock && captured);
        const bool toggle = toggle_gesture_.Release(on_chevron && captured);
        if (captured) ::ReleaseCapture();
        EndDragImage();
        if (from_slot >= 0) {
            if (captured) FinishArrangement(from_slot, cursor);
        } else if (lock_click) {
            SetCompactLocked(!compact_locked_);
            RenderLatest();
        } else if (toggle) {
            ToggleCollapsed();
        }
        return 0;
    }

    // Capture loss or cancellation ends every gesture and changes nothing. A
    // drag abandoned this way must leave the arrangement exactly as it was.
    drag_from_ = -1;
    lock_gesture_.Cancel();
    toggle_gesture_.Cancel();
    EndDragImage();
    if (message == WM_CANCELMODE && ::GetCapture() == input_window)
        ::ReleaseCapture();
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
        // Our own drag image is topmost by design; without this it would look
        // like a legitimate overlay and silently suspend the watchdog.
        if (window != drag_handle_.m_hWnd && window != drag_image_.m_hWnd &&
            ::IsWindowVisible(window) && !::IsIconic(window)) {
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
    int work_width_dip = 1920;
    int work_height_dip = 1080;
    const HMONITOR monitor = dpi_window == nullptr ? nullptr :
        MonitorFromWindow(dpi_window, MONITOR_DEFAULTTONEAREST);
    MONITORINFO info{sizeof(info)};
    if (monitor != nullptr && GetMonitorInfoW(monitor, &info) != FALSE) {
        work_width_dip = MulDiv(info.rcWork.right - info.rcWork.left, 96,
                                static_cast<int>(dpi));
        work_height_dip = MulDiv(info.rcWork.bottom - info.rcWork.top, 96,
                                 static_cast<int>(dpi));
    }
    const auto footprint = compact::ChooseFootprint(
        collapsed_, compact::Resolve(layout_, ram_enabled_, fps_enabled_),
        work_width_dip, work_height_dip);
    return {MulDiv(footprint.width, static_cast<int>(dpi), 96),
            MulDiv(footprint.height, static_cast<int>(dpi), 96),
            MulDiv(kDragHeightDip, static_cast<int>(dpi), 96),
            static_cast<float>(dpi) / 96.0F, footprint.columns, footprint.rows};
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
        // The lock takes one more button width from the status line. At the
        // narrowest arrangement this still leaves every Traditional Chinese
        // status intact: the longest is 210 dip against 274 available.
        const float header_buttons = static_cast<float>(layout.drag_height) *
                                     (ShowsLock() ? 2.0F : 1.0F);
        DrawText(graphics, displayed_status,
                 {status_left, 2.0F * s, layout.width - header_buttons - status_left - 4.0F * s,
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

        // The lock is offered only where arranging is possible: the compact
        // dashboard, and only in builds whose body receives pointer input.
        if (ShowsLock()) {
            const float lock_x = button_x - static_cast<float>(layout.drag_height);
            graphics.FillRectangle(&button_fill, lock_x + 3.0F * s, 3.0F * s,
                                   19.0F * s, 19.0F * s);
            // Locked is the resting state of an always-on-top window, so it
            // carries exactly the weight of the chevron beside it: the same
            // tint, the same pen, outline only. Unlocked is an alert -- amber
            // and filled. The two differ in weight and shape as well as
            // colour, so the state reads without relying on hue.
            //
            // The glyph is drawn a size smaller than its 19 dip button and
            // centred in it. A padlock is taller than a chevron at equal
            // width, so matching their widths would still leave the lock the
            // louder of the two; the extra padding is what evens them out.
            // The button itself, and the hit rectangle, are unchanged.
            const Gdiplus::Color lock_tint = compact_locked_
                ? Gdiplus::Color(235, 224, 235, 248)
                : Gdiplus::Color(235, 255, 184, 72);
            Gdiplus::Pen lock_pen(lock_tint, 1.4F * s);
            lock_pen.SetLineJoin(Gdiplus::LineJoinRound);
            // Closed: the shackle sits on the body. Open: it lifts and shifts.
            const float body_top = 12.0F * s;
            const float arc_x = lock_x + (compact_locked_ ? 9.0F : 10.75F) * s;
            graphics.DrawArc(&lock_pen, arc_x, body_top - 5.25F * s, 7.0F * s,
                             7.0F * s, 180.0F, 180.0F);
            const Gdiplus::RectF body(lock_x + 7.75F * s, body_top, 9.5F * s,
                                      6.5F * s);
            if (compact_locked_) {
                graphics.DrawRectangle(&lock_pen, body);
            } else {
                Gdiplus::SolidBrush fill(lock_tint);
                graphics.FillRectangle(&fill, body);
            }
        }

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
            const float cell_width_dip = layout.columns == 3
                ? (static_cast<float>(layout.width) / s - 20.0F) / 3.0F
                : 72.0F;
            const float cell_width = cell_width_dip * s;
            // Cells wrap at layout.columns and the overlay grows downward, so
            // a preference toggle never changes the overlay's width.
            const int per_row = std::max(1, layout.columns);
            const float column_stride_dip =
                layout.columns == 3 ? cell_width_dip + 4.0F : 76.0F;
            const auto cell_origin = [&](const int index) {
                return std::pair<float, float>{
                    (6.0F + (index % per_row) * column_stride_dip) * s,
                    (30.0F + (index / per_row) *
                                 static_cast<float>(compact::kCollapsedRowPitch)) * s};
            };
            const auto cell = [&](int index, const std::wstring& label,
                                  const std::wstring& value, MetricMember member,
                                  const Gdiplus::Color color, double minimum_span,
                                  std::optional<bool> unavailable_override =
                                      std::nullopt) {
                const auto [x, y] = cell_origin(index);
                const bool unavailable = unavailable_override
                    ? *unavailable_override
                    : (member == nullptr
                           ? fps_snapshot_.status != fps::Status::Ready : stale);
                Gdiplus::SolidBrush background(Gdiplus::Color(15, color.GetR(), color.GetG(), color.GetB()));
                Gdiplus::Pen border(Gdiplus::Color(42, color.GetR(), color.GetG(), color.GetB()), s);
                Gdiplus::GraphicsPath path;
                AddRoundedRectangle(path, {x, y, cell_width, 51.0F * s}, 3.0F * s);
                graphics.FillPath(&background, &path);
                graphics.DrawPath(&border, &path);
                DrawText(graphics, label, {x + 4.0F * s, y + 1.0F * s, cell_width - 8.0F * s, 14.0F * s},
                         small_font, color, Gdiplus::StringAlignmentNear);
                DrawText(graphics, value, {x + 4.0F * s, y + 14.0F * s, cell_width - 8.0F * s, 22.0F * s},
                         value_font, unavailable ? Gdiplus::Color(180, 205, 214, 226)
                                          : Gdiplus::Color(255, 245, 248, 252),
                         Gdiplus::StringAlignmentNear);
                if (member != nullptr) {
                    DrawCompactSparkline(graphics, samples, start, end, member,
                        {x + 5.0F * s, y + 37.0F * s, cell_width - 10.0F * s, 10.0F * s},
                        Gdiplus::Color(stale ? 140 : 235, color.GetR(), color.GetG(), color.GetB()),
                        minimum_span, s);
                }
            };
            const Gdiplus::Color ram_color(255, 255, 122, 196);
            const Gdiplus::Color commit_color(255, 228, 196, 76);
            // Cells are drawn by walking the reader's arrangement, so the
            // order lives in one place instead of being implied by a sequence
            // of calls. A record appears once or not at all; nothing here can
            // place one twice or drop one.
            const auto placed =
                compact::Resolve(layout_, ram_enabled_, fps_enabled_);
            for (int slot = 0; slot < placed.count; ++slot) {
                switch (placed.cells[static_cast<std::size_t>(slot)]) {
                case compact::Metric::Temperature:
                    cell(slot, std::wstring(localization::Select(L"溫度", L"Temp")),
                        FormatMetric(optional(&telemetry::Sample::temperature_c), L" °C"),
                        &telemetry::Sample::temperature_c,
                        Gdiplus::Color(255, 255, 91, 94), 10.0);
                    break;
                case compact::Metric::Power:
                    cell(slot, std::wstring(localization::Select(L"功率", L"Power")),
                        FormatMetric(optional(&telemetry::Sample::power_w), L" W", 1),
                        &telemetry::Sample::power_w,
                        Gdiplus::Color(255, 77, 160, 255), 100.0);
                    break;
                case compact::Metric::Vram:
                    cell(slot, L"VRAM",
                        FormatMetric(optional(&telemetry::Sample::vram_utilization_percent), L"%"),
                        &telemetry::Sample::vram_utilization_percent,
                        Gdiplus::Color(255, 255, 173, 69), 20.0);
                    break;
                case compact::Metric::Gpu:
                    cell(slot, L"GPU",
                        FormatMetric(optional(&telemetry::Sample::gpu_utilization_percent), L"%"),
                        &telemetry::Sample::gpu_utilization_percent,
                        Gdiplus::Color(255, 75, 214, 139), 20.0);
                    break;
                case compact::Metric::Cpu:
                    cell(slot, L"CPU",
                        FormatMetric(optional(&telemetry::Sample::cpu_utilization_percent), L"%"),
                        &telemetry::Sample::cpu_utilization_percent,
                        Gdiplus::Color(255, 183, 121, 255), 20.0);
                    break;
                case compact::Metric::Ram: {
                    // One coherent live reading, as in the expanded lane.
                    const auto live_ram = sysmem::Query();
                    const std::wstring ram_value =
                        live_ram.physical_percent
                            ? std::format(L"{:.0f}%", *live_ram.physical_percent)
                            : L"-";
                    cell(slot, L"RAM", ram_value, nullptr, ram_color, 0.0,
                         !live_ram.physical_percent);
                    if (ram_history_ != nullptr) {
                        const auto [x, y] = cell_origin(slot);
                        DrawMemorySparkline(graphics, ram_history_->Samples(),
                            start, end,
                            {x + 5.0F * s, y + 37.0F * s,
                             cell_width - 10.0F * s, 10.0F * s},
                            Gdiplus::Color(235, ram_color.GetR(),
                                           ram_color.GetG(), ram_color.GetB()),
                            Gdiplus::Color(200, commit_color.GetR(),
                                           commit_color.GetG(),
                                           commit_color.GetB()),
                            s);
                    }
                    break;
                }
                case compact::Metric::Fps: {
                    const std::wstring fps_value =
                        fps_snapshot_.status == fps::Status::Ready
                            ? std::format(L"{:.0f}", fps_snapshot_.displayed_fps)
                            : L"-";
                    cell(slot, L"FPS", fps_value, nullptr,
                         Gdiplus::Color(255, 92, 220, 215), 0.0);
                    if (fps_history_ != nullptr) {
                        const auto [x, y] = cell_origin(slot);
                        DrawFpsSparkline(graphics, fps_history_->Samples(),
                            start, end,
                            {x + 5.0F * s, y + 37.0F * s,
                             cell_width - 10.0F * s, 10.0F * s},
                            Gdiplus::Color(235, 92, 220, 215), s);
                    }
                    break;
                }
                }
            }
        } else {
        const std::wstring fps_value = fps_snapshot_.status == fps::Status::Ready
            ? std::format(L"{:.1f}", fps_snapshot_.displayed_fps) : L"-";
        if (fps_enabled_ && layout.columns == 2) {
            const float card_width_dip =
                (static_cast<float>(layout.width) / s - 18.0F) / 2.0F;
            const auto card = [&](const int index, const std::wstring& label,
                                  const std::wstring& value,
                                  const Gdiplus::Color color) {
                const float x = (6.0F + (index % 2) *
                    (card_width_dip + 6.0F)) * s;
                const float y = (31.0F + (index / 2) * 53.0F) * s;
                const Gdiplus::RectF bounds(x, y, card_width_dip * s, 50.0F * s);
                Gdiplus::SolidBrush background(Gdiplus::Color(
                    18, color.GetR(), color.GetG(), color.GetB()));
                graphics.FillRectangle(&background, bounds);
                DrawText(graphics, label,
                    {x + 7.0F * s, y + 2.0F * s,
                     bounds.Width - 14.0F * s, 19.0F * s},
                    label_font, color, Gdiplus::StringAlignmentNear);
                DrawText(graphics, value,
                    {x + 7.0F * s, y + (index == 5 ? 18.0F : 22.0F) * s,
                     bounds.Width - 14.0F * s, (index == 5 ? 20.0F : 24.0F) * s},
                    value_font, Gdiplus::Color(255, 245, 248, 252),
                    Gdiplus::StringAlignmentNear);
            };
            card(0, std::wstring(localization::Select(L"溫度", L"Temp")),
                 FormatMetric(optional(&telemetry::Sample::temperature_c), L" °C"),
                 Gdiplus::Color(255, 255, 91, 94));
            card(1, std::wstring(localization::Select(L"功率", L"Power")),
                 FormatMetric(optional(&telemetry::Sample::power_w), L" W", 1),
                 Gdiplus::Color(255, 77, 160, 255));
            card(2, L"VRAM %",
                 FormatMetric(optional(&telemetry::Sample::vram_utilization_percent), L"%"),
                 Gdiplus::Color(255, 255, 173, 69));
            card(3, L"GPU %",
                 FormatMetric(optional(&telemetry::Sample::gpu_utilization_percent), L"%"),
                 Gdiplus::Color(255, 75, 214, 139));
            card(4, L"CPU %",
                 FormatMetric(optional(&telemetry::Sample::cpu_utilization_percent), L"%"),
                 Gdiplus::Color(255, 183, 121, 255));
            card(5, L"FPS", fps_value, Gdiplus::Color(255, 92, 220, 215));
            if (fps_history_ != nullptr) {
                const float x = (12.0F + card_width_dip + 6.0F) * s;
                const float y = (31.0F + 2.0F * 53.0F) * s;
                DrawFpsSparkline(graphics, fps_history_->Samples(), start, end,
                    {x + 7.0F * s, y + 41.0F * s,
                     card_width_dip * s - 14.0F * s, 6.0F * s},
                    Gdiplus::Color(235, 92, 220, 215), s);
            }
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

        // Derived, not hard-coded: this reproduces the previous 57 and 52 for
        // five and six records and keeps a seventh inside the window.
        const int expanded_lanes =
            5 + (ram_enabled_ ? 1 : 0) + (fps_enabled_ ? 1 : 0);
        const float row_height =
            ((static_cast<float>(layout.height) / s - 31.0F) /
                 static_cast<float>(expanded_lanes) - 2.0F) * s;
        const float row_width = 376.0F * s;
        const float row_x = 6.0F * s;
        const float row_start = 31.0F * s;
        // Lanes follow the same arrangement as the compact cells. Each record
        // asks where it sits rather than being placed by the order of the
        // calls below, so the two views cannot drift apart.
        const auto expanded_placed =
            compact::Resolve(layout_, ram_enabled_, fps_enabled_);
        const auto lane_row = [&](const compact::Metric metric) {
            int slot = 0;
            for (int i = 0; i < expanded_placed.count; ++i) {
                if (expanded_placed.cells[static_cast<std::size_t>(i)] == metric) {
                    slot = i;
                    break;
                }
            }
            return Gdiplus::RectF{row_x,
                row_start + (row_height + 2.0F * s) * static_cast<float>(slot),
                row_width, row_height};
        };
        const std::optional<double> temperature = optional(&telemetry::Sample::temperature_c);
        const std::wstring temperature_maximum = maximum_temperature
            ? localization::Format(L"最高 {:.0f} °C", L"max {:.0f} °C",
                                   *maximum_temperature) : L"";
        DrawMetricLane(graphics, samples, start, end,
            lane_row(compact::Metric::Temperature),
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
            lane_row(compact::Metric::Power),
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
            lane_row(compact::Metric::Vram),
            L"VRAM %", vram_value,
            Gdiplus::Color(255, 255, 173, 69), 0.0, 100.0,
            &telemetry::Sample::vram_utilization_percent, label_font, value_font,
            presentation_gaps, false,
            std::nullopt, vram_maximum, stale);
        DrawMetricLane(graphics, samples, start, end,
            lane_row(compact::Metric::Gpu),
            L"GPU %", FormatMetric(optional(&telemetry::Sample::gpu_utilization_percent), L"%"),
            Gdiplus::Color(255, 75, 214, 139), 0.0, 100.0,
            &telemetry::Sample::gpu_utilization_percent, label_font, value_font,
            presentation_gaps, false,
            std::nullopt, {}, stale);
        DrawMetricLane(graphics, samples, start, end,
            lane_row(compact::Metric::Cpu),
            L"CPU %", FormatMetric(optional(&telemetry::Sample::cpu_utilization_percent), L"%"),
            Gdiplus::Color(255, 183, 121, 255), 0.0, 100.0,
            &telemetry::Sample::cpu_utilization_percent, label_font, value_font,
            presentation_gaps, false,
            std::nullopt, {}, stale);

        if (ram_enabled_) {
            const Gdiplus::RectF ram_row = lane_row(compact::Metric::Ram);
            const Gdiplus::Color ram_color(255, 255, 122, 196);
            const Gdiplus::Color commit_color(255, 228, 196, 76);
            // The readout is one coherent live reading; the history drives the
            // curve only. Mixing a stored percentage with a live capacity can
            // disagree, so both numbers come from the same query.
            const auto live = sysmem::Query();
            std::wstring ram_value = L"—";
            if (live.physical_percent && live.physical_used_gib) {
                ram_value = std::format(L"{:.0f}% ({:.1f} GiB)",
                                        *live.physical_percent,
                                        *live.physical_used_gib);
            }
            // Same derived scale as DrawMetricLane, not the raw DPI scale.
            const float ram_scale = compact::LaneScale(ram_row.Height);
            const auto ram_label = compact::LaneLabelSpan(ram_row.X, ram_scale);
            const auto ram_graph =
                compact::LaneGraphSpan(ram_row.X, ram_row.Width, ram_scale);
            Gdiplus::SolidBrush background(Gdiplus::Color(12, 255, 255, 255));
            graphics.FillRectangle(&background, ram_row);
            DrawText(graphics, L"RAM",
                {ram_label.x, ram_row.Y, ram_label.width, ram_row.Height},
                label_font, ram_color, Gdiplus::StringAlignmentNear);
            DrawText(graphics, ram_value,
                {ram_row.GetRight() - 140.0F * ram_scale, ram_row.Y,
                 132.0F * ram_scale, 22.0F * ram_scale},
                value_font, Gdiplus::Color(255, 245, 248, 252),
                Gdiplus::StringAlignmentFar);
            if (ram_history_ != nullptr)
                DrawMemorySparkline(graphics, ram_history_->Samples(), start, end,
                    {ram_graph.x, ram_row.Y + 23.0F * ram_scale,
                     ram_graph.width, ram_row.Height - 30.0F * ram_scale},
                    Gdiplus::Color(235, ram_color.GetR(), ram_color.GetG(),
                                   ram_color.GetB()),
                    Gdiplus::Color(200, commit_color.GetR(), commit_color.GetG(),
                                   commit_color.GetB()),
                    ram_scale);
        }
        if (fps_enabled_) {
            const Gdiplus::RectF fps_row = lane_row(compact::Metric::Fps);
            // Same derived scale as DrawMetricLane, not the raw DPI scale.
            const float fps_scale = compact::LaneScale(fps_row.Height);
            const auto fps_label = compact::LaneLabelSpan(fps_row.X, fps_scale);
            const auto fps_graph =
                compact::LaneGraphSpan(fps_row.X, fps_row.Width, fps_scale);
            Gdiplus::SolidBrush background(Gdiplus::Color(12, 255, 255, 255));
            graphics.FillRectangle(&background, fps_row);
            DrawText(graphics, L"FPS",
                {fps_label.x, fps_row.Y, fps_label.width, fps_row.Height},
                label_font, Gdiplus::Color(255, 92, 220, 215),
                Gdiplus::StringAlignmentNear);
            DrawText(graphics, fps_value,
                {fps_row.GetRight() - 140.0F * fps_scale, fps_row.Y,
                 132.0F * fps_scale, 22.0F * fps_scale},
                value_font, Gdiplus::Color(255, 245, 248, 252),
                Gdiplus::StringAlignmentFar);
            if (fps_history_ != nullptr)
                DrawFpsSparkline(graphics, fps_history_->Samples(), start, end,
                    {fps_graph.x, fps_row.Y + 23.0F * fps_scale,
                     fps_graph.width, fps_row.Height - 30.0F * fps_scale},
                    Gdiplus::Color(235, 92, 220, 215), fps_scale);
        }

        }
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
