#include "tray/history_chart.hpp"
#include "localization/localization.hpp"

#include <windows.h>
#include <windowsx.h>
#include <gdiplus.h>

#include <algorithm>
#include <cmath>
#include <format>
#include <string>
#include <vector>

namespace gtg::tray {
namespace {

struct ScopedGdiObject {
    HGDIOBJ value{};
    ~ScopedGdiObject() { if (value != nullptr) DeleteObject(value); }
};

Gdiplus::Color SystemColor(const int index, const BYTE alpha = 255) {
    const COLORREF color = GetSysColor(index);
    return Gdiplus::Color(alpha, GetRValue(color), GetGValue(color), GetBValue(color));
}

double ClampFraction(const double value) {
    return std::clamp(value, 0.0, 1.0);
}

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

void DrawChartText(Gdiplus::Graphics& graphics, const std::wstring& text,
                   const Gdiplus::RectF& bounds, Gdiplus::Font& font,
                   const Gdiplus::Color color,
                   const Gdiplus::StringAlignment alignment) {
    Gdiplus::SolidBrush brush(color);
    Gdiplus::StringFormat format;
    format.SetAlignment(alignment);
    format.SetLineAlignment(Gdiplus::StringAlignmentCenter);
    format.SetFormatFlags(Gdiplus::StringFormatFlagsNoWrap);
    format.SetTrimming(Gdiplus::StringTrimmingEllipsisCharacter);
    graphics.DrawString(text.c_str(), -1, &font, bounds, &format, &brush);
}

void DrawOutlinedChartText(Gdiplus::Graphics& graphics, const std::wstring& text,
                           const Gdiplus::RectF& bounds, Gdiplus::Font& font,
                           const Gdiplus::Color color,
                           const Gdiplus::StringAlignment alignment,
                           const Gdiplus::REAL scale) {
    const Gdiplus::REAL offset = std::max(0.7F, 0.75F * scale);
    const Gdiplus::Color outline(225, 255, 255, 255);
    for (const auto [dx, dy] : {
             std::pair{-offset, 0.0F}, std::pair{offset, 0.0F},
             std::pair{0.0F, -offset}, std::pair{0.0F, offset}}) {
        DrawChartText(graphics, text,
                      {bounds.X + dx, bounds.Y + dy, bounds.Width, bounds.Height},
                      font, outline, alignment);
    }
    DrawChartText(graphics, text, bounds, font, color, alignment);
}

}  // namespace

void HistoryChart::NotifyDataChanged() {
    Invalidate(FALSE);
}

const std::deque<telemetry::Sample>& HistoryChart::Samples() const noexcept {
    static const std::deque<telemetry::Sample> empty;
    return history_ == nullptr ? empty : history_->Samples();
}

void HistoryChart::SetThresholds(const int trigger_temperature_c, const int safe_power_w,
                                 const std::optional<double> maximum_power_w) {
    trigger_temperature_c_ = trigger_temperature_c;
    safe_power_w_ = safe_power_w;
    maximum_power_w_ = maximum_power_w;
    Invalidate(FALSE);
}

LRESULT HistoryChart::OnPaint(UINT, WPARAM, LPARAM, BOOL&) {
    PAINTSTRUCT paint{};
    HDC dc = BeginPaint(&paint);
    RECT bounds{};
    GetClientRect(&bounds);
    Paint(dc, bounds);
    EndPaint(&paint);
    return 0;
}

LRESULT HistoryChart::OnPrintClient(UINT, WPARAM wparam, LPARAM, BOOL&) {
    RECT bounds{};
    GetClientRect(&bounds);
    Paint(reinterpret_cast<HDC>(wparam), bounds);
    return 0;
}

LRESULT HistoryChart::OnEraseBackground(UINT, WPARAM, LPARAM, BOOL&) { return 1; }

LRESULT HistoryChart::OnVisualChanged(UINT, WPARAM, LPARAM, BOOL&) {
    Invalidate(FALSE);
    return 0;
}

RECT HistoryChart::TimelineHitRect() const {
    RECT client{};
    ::GetClientRect(m_hWnd, &client);
    const int dpi = static_cast<int>(GetDpiForWindow(m_hWnd));
    const int label_width = MulDiv(40, dpi, 96);
    const int right_padding = MulDiv(6, dpi, 96);
    const int hit_height = MulDiv(11, dpi, 96);
    return {label_width, client.bottom - hit_height,
            client.right - right_padding, client.bottom};
}

std::uint64_t HistoryChart::EffectiveViewEnd(const std::uint64_t now) const {
    if (follow_live_ || !view_end_ms_) return now;
    const std::uint64_t scrollable = telemetry::History::kRetainedDurationMs - kVisibleHistoryMs;
    const std::uint64_t earliest = now > scrollable ? now - scrollable : 0;
    return std::clamp(*view_end_ms_, earliest, now);
}

void HistoryChart::UpdateViewFromThumbLeft(const int thumb_left,
                                           const std::uint64_t now) {
    const RECT track = TimelineHitRect();
    const int track_width = std::max(1L, track.right - track.left);
    const int dpi = static_cast<int>(GetDpiForWindow(m_hWnd));
    const int thumb_width = std::max(MulDiv(12, dpi, 96),
        static_cast<int>(std::lround(
            static_cast<double>(track_width) * kVisibleHistoryMs /
            telemetry::History::kRetainedDurationMs)));
    const int available = std::max(1, track_width - thumb_width);
    const double fraction = ClampFraction(
        static_cast<double>(thumb_left - track.left) / static_cast<double>(available));
    if (fraction >= 0.998) {
        follow_live_ = true;
        view_end_ms_.reset();
    } else {
        const std::uint64_t scrollable = telemetry::History::kRetainedDurationMs -
                                         kVisibleHistoryMs;
        const std::uint64_t earliest = now > scrollable ? now - scrollable : 0;
        view_end_ms_ = earliest + static_cast<std::uint64_t>(
            std::llround(fraction * static_cast<double>(scrollable)));
        follow_live_ = false;
    }
    Invalidate(FALSE);
}

LRESULT HistoryChart::OnLButtonDown(UINT, WPARAM, LPARAM lparam, BOOL&) {
    const int x = GET_X_LPARAM(lparam);
    const int y = GET_Y_LPARAM(lparam);
    const RECT track = TimelineHitRect();
    if (x < track.left || x > track.right || y < track.top || y > track.bottom) return 0;

    const std::uint64_t now = GetTickCount64();
    const int track_width = std::max(1L, track.right - track.left);
    const int dpi = static_cast<int>(GetDpiForWindow(m_hWnd));
    const int thumb_width = std::max(MulDiv(12, dpi, 96),
        static_cast<int>(std::lround(
            static_cast<double>(track_width) * kVisibleHistoryMs /
            telemetry::History::kRetainedDurationMs)));
    const int available = std::max(1, track_width - thumb_width);
    const std::uint64_t scrollable = telemetry::History::kRetainedDurationMs - kVisibleHistoryMs;
    const std::uint64_t earliest = now > scrollable ? now - scrollable : 0;
    const double fraction = scrollable == 0 ? 1.0 : ClampFraction(
        static_cast<double>(EffectiveViewEnd(now) - earliest) /
        static_cast<double>(scrollable));
    const int current_left = track.left +
        static_cast<int>(std::lround(fraction * static_cast<double>(available)));
    if (x >= current_left && x <= current_left + thumb_width) {
        timeline_drag_offset_px_ = x - current_left;
    } else {
        timeline_drag_offset_px_ = thumb_width / 2;
        UpdateViewFromThumbLeft(x - timeline_drag_offset_px_, now);
    }
    dragging_timeline_ = true;
    SetCapture();
    return 0;
}

LRESULT HistoryChart::OnMouseMove(UINT, WPARAM wparam, LPARAM lparam, BOOL&) {
    if (!dragging_timeline_ || (wparam & MK_LBUTTON) == 0) return 0;
    UpdateViewFromThumbLeft(GET_X_LPARAM(lparam) - timeline_drag_offset_px_,
                            GetTickCount64());
    return 0;
}

LRESULT HistoryChart::OnLButtonUp(UINT, WPARAM, LPARAM lparam, BOOL&) {
    if (!dragging_timeline_) return 0;
    UpdateViewFromThumbLeft(GET_X_LPARAM(lparam) - timeline_drag_offset_px_,
                            GetTickCount64());
    dragging_timeline_ = false;
    if (GetCapture() == m_hWnd) ReleaseCapture();
    return 0;
}

LRESULT HistoryChart::OnCaptureChanged(UINT, WPARAM, LPARAM, BOOL&) {
    dragging_timeline_ = false;
    return 0;
}

void HistoryChart::Paint(HDC target, const RECT& bounds) {
    const int width = bounds.right - bounds.left;
    const int height = bounds.bottom - bounds.top;
    if (width <= 20 || height <= 20) return;

    HDC memory = CreateCompatibleDC(target);
    if (memory == nullptr) return;
    ScopedGdiObject bitmap{CreateCompatibleBitmap(target, width, height)};
    if (bitmap.value == nullptr) {
        DeleteDC(memory);
        return;
    }
    const HGDIOBJ old_bitmap = SelectObject(memory, bitmap.value);
    RECT client{0, 0, width, height};
    ScopedGdiObject background_brush{CreateSolidBrush(GetSysColor(COLOR_WINDOW))};
    FillRect(memory, &client, static_cast<HBRUSH>(background_brush.value));

    Gdiplus::Graphics graphics(memory);
    if (graphics.GetLastStatus() != Gdiplus::Ok) {
        SetBkMode(memory, TRANSPARENT);
        SetTextColor(memory, GetSysColor(COLOR_GRAYTEXT));
        const std::wstring unavailable(localization::Select(
            L"歷史圖表暫時不可用", L"History chart unavailable"));
        DrawTextW(memory, unavailable.c_str(), -1, &client,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        BitBlt(target, 0, 0, width, height, memory, 0, 0, SRCCOPY);
        SelectObject(memory, old_bitmap);
        DeleteDC(memory);
        return;
    }

    graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
    graphics.SetCompositingMode(Gdiplus::CompositingModeSourceOver);
    graphics.SetTextRenderingHint(Gdiplus::TextRenderingHintClearTypeGridFit);

    const Gdiplus::REAL scale = graphics.GetDpiX() / 96.0F;
    const Gdiplus::REAL margin = 4.0F * scale;
    const Gdiplus::REAL label_width = 40.0F * scale;
    const Gdiplus::REAL right_padding = 6.0F * scale;
    const Gdiplus::REAL footer_height = 25.0F * scale;
    const Gdiplus::REAL gap = 4.0F * scale;
    const int plot_count = fps_history_ == nullptr ? 5 : 6;
    const Gdiplus::REAL plots_height = static_cast<Gdiplus::REAL>(height) -
        margin * 2.0F - footer_height - gap * (plot_count - 1);
    const Gdiplus::REAL plot_height = std::max(16.0F, plots_height / plot_count);
    const Gdiplus::REAL plot_width = static_cast<Gdiplus::REAL>(width) -
        label_width - right_padding;

    const Gdiplus::RectF temperature_plot{label_width, margin, plot_width, plot_height};
    const Gdiplus::RectF power_plot{
        label_width, temperature_plot.GetBottom() + gap, plot_width, plot_height};
    const Gdiplus::RectF vram_plot{
        label_width, power_plot.GetBottom() + gap, plot_width, plot_height};
    const Gdiplus::RectF gpu_plot{
        label_width, vram_plot.GetBottom() + gap, plot_width, plot_height};
    const Gdiplus::RectF cpu_plot{
        label_width, gpu_plot.GetBottom() + gap, plot_width, plot_height};
    const Gdiplus::RectF fps_plot{
        label_width, cpu_plot.GetBottom() + gap, plot_width, plot_height};

    Gdiplus::FontFamily font_family(L"Segoe UI");
    Gdiplus::Font label_font(&font_family, 7.7F, Gdiplus::FontStyleRegular,
                             Gdiplus::UnitPoint);
    Gdiplus::Font small_font(&font_family, 7.0F, Gdiplus::FontStyleRegular,
                             Gdiplus::UnitPoint);
    const auto muted_color = SystemColor(COLOR_GRAYTEXT);
    const Gdiplus::Color plot_background(255, 249, 250, 252);
    const Gdiplus::Color border_color(255, 205, 211, 219);
    const Gdiplus::Color grid_color(150, 211, 217, 225);
    const Gdiplus::Color temperature_color(255, 224, 73, 71);
    const Gdiplus::Color power_color(255, 47, 124, 211);
    const Gdiplus::Color vram_color(255, 213, 126, 38);
    const Gdiplus::Color gpu_color(255, 36, 153, 95);
    const Gdiplus::Color cpu_color(255, 132, 82, 190);
    const Gdiplus::Color fps_color(255, 40, 165, 160);
    Gdiplus::SolidBrush plot_brush(plot_background);
    Gdiplus::Pen border_pen(border_color, 1.0F);
    Gdiplus::Pen grid_pen(grid_color, 0.7F);

    for (const auto& plot : {temperature_plot, power_plot, vram_plot, gpu_plot, cpu_plot}) {
        Gdiplus::GraphicsPath rounded;
        AddRoundedRectangle(rounded, plot, 3.5F * scale);
        graphics.FillPath(&plot_brush, &rounded);
        graphics.DrawPath(&border_pen, &rounded);
        for (int i = 1; i < 4; ++i) {
            const Gdiplus::REAL x = plot.X + plot.Width * static_cast<Gdiplus::REAL>(i) / 4.0F;
            graphics.DrawLine(&grid_pen, x, plot.Y + 1.0F, x, plot.GetBottom() - 1.0F);
        }
        graphics.DrawLine(&grid_pen, plot.X + 1.0F, plot.Y + plot.Height / 2.0F,
                          plot.GetRight() - 1.0F, plot.Y + plot.Height / 2.0F);
    }
    if (fps_history_ != nullptr) {
        Gdiplus::GraphicsPath rounded;
        AddRoundedRectangle(rounded, fps_plot, 3.5F * scale);
        graphics.FillPath(&plot_brush, &rounded);
        graphics.DrawPath(&border_pen, &rounded);
        for (int i = 1; i < 4; ++i) {
            const Gdiplus::REAL x = fps_plot.X +
                fps_plot.Width * static_cast<Gdiplus::REAL>(i) / 4.0F;
            graphics.DrawLine(&grid_pen, x, fps_plot.Y + 1.0F,
                              x, fps_plot.GetBottom() - 1.0F);
        }
        graphics.DrawLine(&grid_pen, fps_plot.X + 1.0F,
                          fps_plot.Y + fps_plot.Height / 2.0F,
                          fps_plot.GetRight() - 1.0F,
                          fps_plot.Y + fps_plot.Height / 2.0F);
    }

    const auto draw_axis_label = [&](const std::wstring& label,
                                     const Gdiplus::RectF& plot,
                                     const Gdiplus::Color color) {
        DrawChartText(graphics, label,
            {2.0F, plot.Y, label_width - 6.0F, plot.Height},
            label_font, color, Gdiplus::StringAlignmentNear);
    };
    draw_axis_label(std::wstring(localization::Select(L"溫度", L"Temp")), temperature_plot,
                    temperature_color);
    draw_axis_label(std::wstring(localization::Select(L"功率", L"Power")), power_plot,
                    power_color);
    draw_axis_label(L"VRAM", vram_plot, vram_color);
    draw_axis_label(L"GPU %", gpu_plot, gpu_color);
    draw_axis_label(L"CPU %", cpu_plot, cpu_color);
    if (fps_history_ != nullptr) draw_axis_label(L"FPS", fps_plot, fps_color);

    const std::uint64_t live_now = GetTickCount64();
    const std::uint64_t view_end = EffectiveViewEnd(live_now);
    const std::uint64_t visible_start = view_end > kVisibleHistoryMs
        ? view_end - kVisibleHistoryMs : 0;
    const auto x_for = [&](const std::uint64_t time) {
        const double elapsed = static_cast<double>(time) - static_cast<double>(visible_start);
        const double fraction = ClampFraction(elapsed / static_cast<double>(kVisibleHistoryMs));
        return temperature_plot.X + static_cast<Gdiplus::REAL>(fraction) *
            (temperature_plot.Width - 1.0F);
    };
    const auto y_for = [](const Gdiplus::RectF& plot, const double value,
                          const double minimum, const double maximum) {
        const double fraction = ClampFraction((value - minimum) / (maximum - minimum));
        return plot.GetBottom() - 1.0F - static_cast<Gdiplus::REAL>(fraction) *
            (plot.Height - 2.0F);
    };

    const double power_max = std::max(600.0, maximum_power_w_.value_or(0.0));
    const Gdiplus::REAL trigger_y = y_for(temperature_plot, trigger_temperature_c_, 30.0, 100.0);
    const Gdiplus::REAL safe_y = y_for(power_plot, safe_power_w_, 0.0, power_max);
    Gdiplus::Pen trigger_pen(Gdiplus::Color(205, 224, 73, 71), 1.0F);
    trigger_pen.SetDashStyle(Gdiplus::DashStyleDash);
    Gdiplus::Pen safe_pen(Gdiplus::Color(190, 47, 124, 211), 1.0F);
    safe_pen.SetDashStyle(Gdiplus::DashStyleDot);
    graphics.DrawLine(&trigger_pen, temperature_plot.X, trigger_y,
                      temperature_plot.GetRight(), trigger_y);
    graphics.DrawLine(&safe_pen, power_plot.X, safe_y, power_plot.GetRight(), safe_y);

    enum class Series { Temperature, Power, Vram, Gpu, Cpu };
    const auto collect_segments = [&](const Series series) {
        std::vector<std::vector<Gdiplus::PointF>> segments;
        std::vector<Gdiplus::PointF> current;
        for (const auto& sample : Samples()) {
            if (sample.monotonic_ms < visible_start || sample.monotonic_ms > view_end) continue;
            const std::optional<double>* value = nullptr;
            const Gdiplus::RectF* plot = nullptr;
            double minimum = 0.0;
            double maximum = 100.0;
            switch (series) {
            case Series::Temperature:
                value = &sample.temperature_c;
                plot = &temperature_plot;
                minimum = 30.0;
                maximum = 100.0;
                break;
            case Series::Power:
                value = &sample.power_w;
                plot = &power_plot;
                maximum = power_max;
                break;
            case Series::Vram:
                value = &sample.vram_utilization_percent;
                plot = &vram_plot;
                break;
            case Series::Gpu:
                value = &sample.gpu_utilization_percent;
                plot = &gpu_plot;
                break;
            case Series::Cpu:
                value = &sample.cpu_utilization_percent;
                plot = &cpu_plot;
                break;
            }
            if (!value->has_value()) {
                if (!current.empty()) segments.push_back(std::move(current));
                current.clear();
                continue;
            }
            current.emplace_back(x_for(sample.monotonic_ms),
                                 y_for(*plot, **value, minimum, maximum));
        }
        if (!current.empty()) segments.push_back(std::move(current));
        return segments;
    };

    const auto draw_series = [&](const Gdiplus::RectF& plot,
                                 const std::vector<std::vector<Gdiplus::PointF>>& segments,
                                 const Gdiplus::Color color,
                                 const bool mark_all_singletons = false) {
        Gdiplus::LinearGradientBrush fill(
            Gdiplus::PointF(plot.X, plot.Y), Gdiplus::PointF(plot.X, plot.GetBottom()),
            Gdiplus::Color(62, color.GetR(), color.GetG(), color.GetB()),
            Gdiplus::Color(5, color.GetR(), color.GetG(), color.GetB()));
        Gdiplus::Pen line(color, std::max(1.05F, 0.95F * scale));
        line.SetLineJoin(Gdiplus::LineJoinRound);
        for (const auto& segment : segments) {
            if (segment.size() == 1 && mark_all_singletons) {
                const auto& point = segment.front();
                const Gdiplus::REAL radius = 1.8F * scale;
                Gdiplus::SolidBrush dot(color);
                graphics.FillEllipse(&dot, point.X - radius, point.Y - radius,
                                     radius * 2.0F, radius * 2.0F);
                continue;
            }
            if (segment.size() < 2) continue;
            Gdiplus::GraphicsPath area;
            area.AddLines(segment.data(), static_cast<INT>(segment.size()));
            area.AddLine(segment.back(), Gdiplus::PointF(segment.back().X, plot.GetBottom()));
            area.AddLine(Gdiplus::PointF(segment.back().X, plot.GetBottom()),
                         Gdiplus::PointF(segment.front().X, plot.GetBottom()));
            area.CloseFigure();
            graphics.FillPath(&fill, &area);
            graphics.DrawLines(&line, segment.data(), static_cast<INT>(segment.size()));
        }
        if (!segments.empty() &&
            (segments.back().size() >= 2 || !mark_all_singletons) &&
            !segments.back().empty()) {
            const auto& point = segments.back().back();
            const Gdiplus::REAL radius = 1.8F * scale;
            Gdiplus::SolidBrush dot(color);
            graphics.FillEllipse(&dot, point.X - radius, point.Y - radius,
                                 radius * 2.0F, radius * 2.0F);
        }
    };

    draw_series(temperature_plot, collect_segments(Series::Temperature), temperature_color);
    draw_series(power_plot, collect_segments(Series::Power), power_color);
    draw_series(vram_plot, collect_segments(Series::Vram), vram_color);
    draw_series(gpu_plot, collect_segments(Series::Gpu), gpu_color);
    draw_series(cpu_plot, collect_segments(Series::Cpu), cpu_color);
    if (fps_history_ != nullptr) {
        double fps_max = 60.0;
        for (const auto& sample : fps_history_->Samples()) {
            if (sample.monotonic_ms >= visible_start &&
                sample.monotonic_ms <= view_end && sample.displayed_fps)
                fps_max = std::max(fps_max, *sample.displayed_fps * 1.1);
        }
        // No-data sits at the waterline, but dashed styling distinguishes it
        // from a measured 0 FPS solid trace at the same height.
        Gdiplus::Pen no_data(Gdiplus::Color(125, fps_color.GetR(),
                                               fps_color.GetG(), fps_color.GetB()),
                             std::max(1.0F, 0.8F * scale));
        no_data.SetDashStyle(Gdiplus::DashStyleDot);
        const Gdiplus::REAL baseline = y_for(fps_plot, 0.0, 0.0, fps_max);
        graphics.DrawLine(&no_data, fps_plot.X, baseline,
                          fps_plot.GetRight(), baseline);
        std::vector<Gdiplus::PointF> points;
        const fps::HistorySample* last_valid = nullptr;
        const fps::HistorySample* last_sample = nullptr;
        for (const auto& sample : fps_history_->Samples()) {
            if (sample.monotonic_ms < visible_start ||
                sample.monotonic_ms > view_end) continue;
            last_sample = &sample;
            if (sample.displayed_fps) {
                points.emplace_back(x_for(sample.monotonic_ms),
                    y_for(fps_plot, *sample.displayed_fps, 0.0, fps_max));
                last_valid = &sample;
            }
        }
        if (last_valid != nullptr && last_sample != nullptr &&
            !last_sample->displayed_fps &&
            last_sample->monotonic_ms > last_valid->monotonic_ms)
            points.emplace_back(x_for(last_sample->monotonic_ms),
                y_for(fps_plot, *last_valid->displayed_fps, 0.0, fps_max));
        if (!points.empty()) {
            std::vector<std::vector<Gdiplus::PointF>> segments;
            segments.push_back(std::move(points));
            draw_series(fps_plot, segments, fps_color, true);
        }
        Gdiplus::SolidBrush gap_fill(Gdiplus::Color(95, 8, 19, 31));
        Gdiplus::Pen delayed(Gdiplus::Color(230,
            static_cast<BYTE>(fps_color.GetR() * 0.45F),
            static_cast<BYTE>(fps_color.GetG() * 0.55F),
            static_cast<BYTE>(fps_color.GetB() * 0.55F)),
            std::max(1.0F, 0.9F * scale));
        fps::ForEachHistoryStroke(fps_history_->Samples(), visible_start, view_end,
            [&](const fps::HistorySample& from, const fps::HistorySample& to,
                const bool dim) {
                if (!dim) return;
                const Gdiplus::PointF a(x_for(from.monotonic_ms),
                    y_for(fps_plot, *from.displayed_fps, 0.0, fps_max));
                const Gdiplus::PointF b(x_for(to.monotonic_ms),
                    to.displayed_fps
                        ? y_for(fps_plot, *to.displayed_fps, 0.0, fps_max) : a.Y);
                Gdiplus::GraphicsPath area;
                area.AddLine(a, b);
                area.AddLine(b, {b.X, baseline});
                area.AddLine(Gdiplus::PointF(b.X, baseline),
                             Gdiplus::PointF(a.X, baseline));
                area.CloseFigure();
                graphics.FillPath(&gap_fill, &area);
                graphics.DrawLine(&delayed, a, b);
            });
    }

    const auto draw_marker_label = [&](const std::wstring& label,
                                       const Gdiplus::RectF& plot,
                                       const Gdiplus::REAL y,
                                       const Gdiplus::Color color,
                                       const bool place_left) {
        const Gdiplus::REAL marker_width = plot.Width * 0.48F;
        const Gdiplus::REAL x = place_left
            ? plot.X + 4.0F * scale
            : plot.GetRight() - marker_width - 4.0F * scale;
        const Gdiplus::REAL marker_height = std::min(14.0F * scale, plot.Height);
        const Gdiplus::REAL marker_y = std::clamp(
            y - marker_height / 2.0F, plot.Y, plot.GetBottom() - marker_height);
        const Gdiplus::RectF rect(x, marker_y, marker_width, marker_height);
        DrawOutlinedChartText(graphics, label, rect, small_font, color,
                              place_left ? Gdiplus::StringAlignmentNear
                                         : Gdiplus::StringAlignmentFar,
                              scale);
    };
    draw_marker_label(localization::Format(L"觸發 {} °C", L"Trigger {} °C",
                                           trigger_temperature_c_),
                      temperature_plot, trigger_y, temperature_color, true);
    draw_marker_label(localization::Format(L"安全 {} W", L"Safe {} W", safe_power_w_),
                      power_plot, safe_y, power_color, true);

    std::optional<double> maximum_temperature;
    for (const auto& sample : Samples()) {
        if (sample.monotonic_ms >= visible_start && sample.monotonic_ms <= view_end &&
            sample.temperature_c &&
            (!maximum_temperature || *sample.temperature_c > *maximum_temperature)) {
            maximum_temperature = sample.temperature_c;
        }
    }
    if (maximum_temperature) {
        const Gdiplus::REAL badge_height = std::min(14.0F * scale, temperature_plot.Height);
        const Gdiplus::RectF badge(
            temperature_plot.GetRight() - 88.0F * scale,
            temperature_plot.Y, 84.0F * scale, badge_height);
        DrawOutlinedChartText(
            graphics, localization::Format(L"最高 {:.0f} °C", L"Max {:.0f} °C",
                                           *maximum_temperature),
            badge, small_font, temperature_color, Gdiplus::StringAlignmentFar, scale);
    }

    std::optional<double> maximum_vram_percent;
    std::optional<double> maximum_vram_used_gib;
    for (const auto& sample : Samples()) {
        if (sample.monotonic_ms >= visible_start && sample.monotonic_ms <= view_end &&
            sample.vram_utilization_percent &&
            (!maximum_vram_percent ||
             *sample.vram_utilization_percent > *maximum_vram_percent)) {
            maximum_vram_percent = sample.vram_utilization_percent;
            maximum_vram_used_gib = sample.vram_used_gib;
        }
    }
    if (maximum_vram_percent) {
        const Gdiplus::REAL badge_height = std::min(14.0F * scale, vram_plot.Height);
        const Gdiplus::RectF badge(
            vram_plot.GetRight() - 124.0F * scale,
            vram_plot.Y, 120.0F * scale, badge_height);
        const std::wstring text = maximum_vram_used_gib
            ? localization::Format(L"最高 {:.0f}% · {:.1f} GiB",
                                   L"Max {:.0f}% · {:.1f} GiB", *maximum_vram_percent,
                                   *maximum_vram_used_gib)
            : localization::Format(L"最高 {:.0f}%", L"Max {:.0f}%",
                                   *maximum_vram_percent);
        DrawOutlinedChartText(graphics, text, badge, small_font, vram_color,
                              Gdiplus::StringAlignmentFar, scale);
    }

    const Gdiplus::REAL timeline_y = static_cast<Gdiplus::REAL>(height) - footer_height;
    const auto time_label = [&](const std::uint64_t timestamp) {
        const double age_minutes = live_now >= timestamp
            ? static_cast<double>(live_now - timestamp) / 60'000.0 : 0.0;
        if (age_minutes < 0.05) {
            return std::wstring(localization::Select(L"現在", L"Now"));
        }
        const double rounded = std::round(age_minutes);
        return std::abs(age_minutes - rounded) < 0.05
            ? localization::Format(L"{:.0f} 分前", L"{:.0f} min ago", rounded)
            : localization::Format(L"{:.1f} 分前", L"{:.1f} min ago", age_minutes);
    };
    const std::uint64_t midpoint = visible_start + kVisibleHistoryMs / 2;
    const Gdiplus::REAL text_height = 15.0F * scale;
    DrawChartText(graphics, time_label(visible_start),
        {temperature_plot.X, timeline_y, temperature_plot.Width / 3.0F, text_height},
        small_font, muted_color, Gdiplus::StringAlignmentNear);
    DrawChartText(graphics, time_label(midpoint),
        {temperature_plot.X + temperature_plot.Width / 3.0F, timeline_y,
         temperature_plot.Width / 3.0F, text_height},
        small_font, muted_color, Gdiplus::StringAlignmentCenter);
    DrawChartText(graphics, time_label(view_end),
        {temperature_plot.X + temperature_plot.Width * 2.0F / 3.0F, timeline_y,
         temperature_plot.Width / 3.0F, text_height},
        small_font, muted_color, Gdiplus::StringAlignmentFar);

    const Gdiplus::REAL track_y = static_cast<Gdiplus::REAL>(height) - 5.0F * scale;
    const Gdiplus::RectF track_rect(
        temperature_plot.X, track_y, temperature_plot.Width, 3.0F * scale);
    Gdiplus::GraphicsPath track_path;
    AddRoundedRectangle(track_path, track_rect, 1.5F * scale);
    Gdiplus::SolidBrush track_brush(Gdiplus::Color(255, 214, 220, 228));
    graphics.FillPath(&track_brush, &track_path);

    const Gdiplus::REAL thumb_width = std::max(
        12.0F * scale, track_rect.Width * static_cast<Gdiplus::REAL>(kVisibleHistoryMs) /
                           static_cast<Gdiplus::REAL>(telemetry::History::kRetainedDurationMs));
    const std::uint64_t scrollable = telemetry::History::kRetainedDurationMs -
                                     kVisibleHistoryMs;
    const std::uint64_t earliest_end = live_now > scrollable ? live_now - scrollable : 0;
    const double thumb_fraction = scrollable == 0 ? 1.0 : ClampFraction(
        static_cast<double>(view_end - earliest_end) / static_cast<double>(scrollable));
    const Gdiplus::REAL thumb_x = track_rect.X +
        static_cast<Gdiplus::REAL>(thumb_fraction) * (track_rect.Width - thumb_width);
    const Gdiplus::RectF thumb_rect(
        thumb_x, track_y - 1.0F * scale, thumb_width, 5.0F * scale);
    Gdiplus::GraphicsPath thumb_path;
    AddRoundedRectangle(thumb_path, thumb_rect, 2.5F * scale);
    Gdiplus::SolidBrush thumb_brush(
        follow_live_ ? Gdiplus::Color(255, 47, 124, 211)
                     : Gdiplus::Color(255, 91, 104, 125));
    graphics.FillPath(&thumb_brush, &thumb_path);

    graphics.Flush(Gdiplus::FlushIntentionSync);
    BitBlt(target, 0, 0, width, height, memory, 0, 0, SRCCOPY);
    SelectObject(memory, old_bitmap);
    DeleteDC(memory);
}

}  // namespace gtg::tray
