#include "tray/osd_overlay.hpp"
#include "localization/localization.hpp"
#include "logging/logger.hpp"
#include "sysmem/reclaim_win32.hpp"
#include "tray/osd_animation.hpp"

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

// One line drawn in pieces, so a piece can differ in colour.
//
// Typographic format throughout, because the default StringFormat pads every
// run by roughly a sixth of an em on each side. That is invisible when a line
// is drawn once and accumulates into a visible gap when the same line is
// drawn in five pieces.
struct TextRun {
    std::wstring text;
    Gdiplus::Color color;
};

void DrawRuns(Gdiplus::Graphics& graphics, const std::vector<TextRun>& runs,
              const Gdiplus::RectF& bounds, Gdiplus::Font& font,
              const Gdiplus::StringAlignment alignment) {
    Gdiplus::StringFormat format(Gdiplus::StringFormat::GenericTypographic());
    format.SetFormatFlags(format.GetFormatFlags() |
                          Gdiplus::StringFormatFlagsNoWrap |
                          Gdiplus::StringFormatFlagsMeasureTrailingSpaces);
    std::vector<float> widths;
    widths.reserve(runs.size());
    float total = 0.0F;
    float height = 0.0F;
    for (const auto& run : runs) {
        Gdiplus::RectF measured;
        graphics.MeasureString(run.text.c_str(), -1, &font,
                               Gdiplus::PointF(0.0F, 0.0F), &format, &measured);
        widths.push_back(measured.Width);
        total += measured.Width;
        if (measured.Height > height) height = measured.Height;
    }
    float cursor = bounds.X;
    if (alignment == Gdiplus::StringAlignmentFar) {
        cursor = bounds.GetRight() - total;
    } else if (alignment == Gdiplus::StringAlignmentCenter) {
        cursor = bounds.X + (bounds.Width - total) / 2.0F;
    }
    // Centred the way DrawText centres its single run, so a line that gains
    // pieces does not shift on its row.
    const float top = bounds.Y + (bounds.Height - height) / 2.0F;
    for (std::size_t i = 0; i < runs.size(); ++i) {
        Gdiplus::SolidBrush brush(runs[i].color);
        graphics.DrawString(runs[i].text.c_str(), -1, &font,
                            Gdiplus::PointF(cursor, top), &format, &brush);
        cursor += widths[i];
    }
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

// One accent per record, named once. These eighteen literals used to be
// repeated across the compact cells, the narrow two-column fallback and the
// expanded lanes, so a record could be recoloured in one view and not the
// others. Accent() is the only place that maps a record to its colour.
const Gdiplus::Color kAccentTemperature(255, 255, 91, 94);
const Gdiplus::Color kAccentPower(255, 77, 160, 255);
const Gdiplus::Color kAccentVram(255, 255, 173, 69);
const Gdiplus::Color kAccentGpu(255, 75, 214, 139);
const Gdiplus::Color kAccentCpu(255, 183, 121, 255);
const Gdiplus::Color kAccentRam(255, 255, 122, 196);
const Gdiplus::Color kAccentFps(255, 92, 220, 215);
// Virtual commit rides behind RAM and is deliberately not a record of its own:
// it has no cell, no lane and no slot in the arrangement.
const Gdiplus::Color kAccentCommit(255, 228, 196, 76);
// Network is one record with one accent, like every other. Receive carries it;
// transmit is drawn in a neutral that claims to be nobody -- the same shape as
// commit riding behind RAM, and for the same reason: a second hue here would
// read as a second record.
//
// Lime is the one clear gap in the palette. The seven accents sit at roughly
// 359, 213, 35, 150, 271, 328 and 178 degrees; 75 collides with none of them.
const Gdiplus::Color kAccentNetwork(255, 200, 232, 92);
// Bright enough to be told apart from lime where the two cross, and cool
// enough not to be mistaken for a second lime. Against the main window's
// near-white chart this same role needs a much darker colour; see
// `net_sent_color` in history_chart.cpp.
const Gdiplus::Color kNetworkSent(255, 122, 186, 255);
// The colour every record draws its reading in, and the tint a direction
// mark takes beside it: near enough the record accent to belong to it, far
// enough from the white to sit behind the number rather than beside it.
const Gdiplus::Color kValuePrimary(255, 245, 248, 252);
const Gdiplus::Color kNetworkMark(235, 200, 232, 92);

// The figure the reader sees after a reclaim. GiB rather than a percentage,
// deliberately: the cell already shows a percentage, and a delta in the same
// unit would be read as the reading itself.
std::wstring FormatReclaimResult(const sysmem::reclaim::Outcome& outcome) {
    switch (sysmem::reclaim::ToneOf(outcome)) {
        case sysmem::reclaim::ResultTone::Gain: {
            const double gib = sysmem::reclaim::FreedGib(outcome);
            return std::format(L"+{:.{}f} GB", gib,
                               sysmem::reclaim::FreedDecimals(gib));
        }
        case sysmem::reclaim::ResultTone::Loss: {
            const double gib = sysmem::reclaim::FreedGib(outcome);
            return std::format(L"{:.{}f} GB", gib,
                               sysmem::reclaim::FreedDecimals(gib));
        }
        case sysmem::reclaim::ResultTone::Neutral:
            break;
    }
    // Something was done, but the machine moved less than the noise floor.
    // Saying "+0.0 GB" would claim more than is known.
    return L"--";
}

std::uint32_t ReclaimResultTint(const sysmem::reclaim::Outcome& outcome) noexcept {
    // Green already means healthy in this palette, so a gain borrows it
    // instead of introducing a colour the reader has to learn.
    switch (sysmem::reclaim::ToneOf(outcome)) {
        case sysmem::reclaim::ResultTone::Gain:   return 0xFF4BD68BU;
        case sysmem::reclaim::ResultTone::Loss:   return 0xFFFF5B5EU;
        case sysmem::reclaim::ResultTone::Neutral: break;
    }
    return 0xB4CDD6E2U;
}

Gdiplus::Color Accent(const compact::Metric metric) noexcept {
    switch (metric) {
        case compact::Metric::Temperature: return kAccentTemperature;
        case compact::Metric::Power: return kAccentPower;
        case compact::Metric::Vram: return kAccentVram;
        case compact::Metric::Gpu: return kAccentGpu;
        case compact::Metric::Cpu: return kAccentCpu;
        case compact::Metric::Ram: return kAccentRam;
        case compact::Metric::Fps: return kAccentFps;
        case compact::Metric::Network: return kAccentNetwork;
    }
    return kAccentTemperature;
}

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
// Both directions of a network record, on one axis.
//
// The shared span is the point. Scaled independently, 12 MB/s of receive and
// 800 B/s of send would be drawn the same height, and the cell would be
// telling its reader the opposite of what is happening. `peak` is the largest
// rate in either direction across the window, so the quieter direction is
// drawn quiet.
//
// Each curve is a line over a translucent area down to the baseline, which is
// the shape every other sparkline here already uses -- the two differ by
// colour and by the receive curve being drawn last, so it stays legible where
// they cross.
void DrawNetworkSparkline(Gdiplus::Graphics& graphics,
                          const std::deque<net::HistorySample>& samples,
                          const std::uint64_t start, const std::uint64_t end,
                          const Gdiplus::RectF& bounds, const double peak,
                          const Gdiplus::Color received_color,
                          const Gdiplus::Color sent_color, const float scale) {
    const double duration = static_cast<double>(std::max<std::uint64_t>(1, end - start));
    // A flat pair of curves on an idle link is correct; dividing by it is not.
    const double span = peak > 0.0 ? peak : 1.0;

    const auto stroke = [&](const bool sent, const Gdiplus::Color color) {
        std::vector<Gdiplus::PointF> points;
        for (const auto& sample : samples) {
            if (sample.monotonic_ms < start || sample.monotonic_ms > end) continue;
            const auto& value = sent ? sample.sent_bytes_per_second
                                     : sample.received_bytes_per_second;
            // A refused reading leaves a hole rather than a line drawn across
            // it. The link stopped reporting; joining the ends would invent
            // the traffic that was never measured.
            if (!value) {
                if (points.size() >= 2) {
                    Gdiplus::GraphicsPath area;
                    area.AddLines(points.data(), static_cast<INT>(points.size()));
                    area.AddLine(points.back(),
                                 {points.back().X, bounds.GetBottom()});
                    area.AddLine(Gdiplus::PointF(points.back().X, bounds.GetBottom()),
                                 Gdiplus::PointF(points.front().X, bounds.GetBottom()));
                    area.CloseFigure();
                    Gdiplus::SolidBrush fill(Gdiplus::Color(
                        40, color.GetR(), color.GetG(), color.GetB()));
                    graphics.FillPath(&fill, &area);
                    Gdiplus::Pen line(color, std::max(1.0F, 1.0F * scale));
                    line.SetLineJoin(Gdiplus::LineJoinRound);
                    graphics.DrawLines(&line, points.data(),
                                       static_cast<INT>(points.size()));
                }
                points.clear();
                continue;
            }
            const double height = static_cast<double>(*value) / span;
            points.emplace_back(
                bounds.X + static_cast<float>((sample.monotonic_ms - start) / duration) *
                               bounds.Width,
                bounds.GetBottom() -
                    static_cast<float>(std::clamp(height, 0.0, 1.0)) * bounds.Height);
        }
        if (points.size() < 2) return;
        Gdiplus::GraphicsPath area;
        area.AddLines(points.data(), static_cast<INT>(points.size()));
        area.AddLine(points.back(), {points.back().X, bounds.GetBottom()});
        area.AddLine(Gdiplus::PointF(points.back().X, bounds.GetBottom()),
                     Gdiplus::PointF(points.front().X, bounds.GetBottom()));
        area.CloseFigure();
        Gdiplus::SolidBrush fill(Gdiplus::Color(
            40, color.GetR(), color.GetG(), color.GetB()));
        graphics.FillPath(&fill, &area);
        Gdiplus::Pen line(color, std::max(1.0F, 1.0F * scale));
        line.SetLineJoin(Gdiplus::LineJoinRound);
        graphics.DrawLines(&line, points.data(), static_cast<INT>(points.size()));
    };

    // Sent first, so received is on top where they overlap: it is the larger
    // of the two almost always, and the one the reader is looking for.
    stroke(true, sent_color);
    stroke(false, received_color);
}

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
    // How much of the top band the annotation actually takes, measured rather
    // than reserved. The reservation was a flat 240 dip whatever was written
    // there, which left the value a fixed 132 dip -- and "349.5 W (350 W)"
    // does not fit in 132 dip, so a reader on a 350 W card saw
    // "349.5 W (350..." with the limit cut off. A truncated number is a
    // different number, which is the one thing this project does not let a
    // figure become.
    Gdiplus::StringFormat measure;
    measure.SetFormatFlags(Gdiplus::StringFormatFlagsNoWrap);
    const auto width_of = [&](const std::wstring& text, Gdiplus::Font& font) {
        if (text.empty()) return 0.0F;
        Gdiplus::RectF measured;
        graphics.MeasureString(text.c_str(), -1, &font,
                               Gdiplus::PointF(0.0F, 0.0F), &measure, &measured);
        return measured.Width;
    };

    float annotation_width = 0.0F;
    if (!annotation.empty()) {
        const std::wstring annotation_text(annotation);
        const Gdiplus::RectF annotation_rect(graph_span.x, row.Y,
                                              row.Width - 240.0F * scale,
                                              22.0F * scale);
        DrawText(graphics, annotation_text, annotation_rect, label_font,
                 Gdiplus::Color(230, color.GetR(), color.GetG(), color.GetB()),
                 Gdiplus::StringAlignmentNear);
        // The annotation is drawn first and keeps what it needs; the value
        // takes the rest. They can no longer collide, and neither is sized by
        // a constant that has to be revisited when a string changes.
        annotation_width = std::min(width_of(annotation_text, label_font),
                                    annotation_rect.Width);
    }

    const auto value_span =
        compact::LaneValueSpan(row.X, row.Width, annotation_width, scale);
    const Gdiplus::RectF value_rect(value_span.x, row.Y, value_span.width,
                                    22.0F * scale);
    // Everything free is usually enough. When it is not -- a wide value under
    // a wide annotation -- step the face down rather than cut the figure, the
    // same ladder the compact cell walks.
    Gdiplus::FontFamily value_family;
    value_font.GetFamily(&value_family);
    const float base_size = value_font.GetSize();
    float value_size = base_size;
    for (; value_size > base_size * 0.72F; value_size -= 0.5F) {
        Gdiplus::Font probe(&value_family, value_size, Gdiplus::FontStyleBold,
                            Gdiplus::UnitPixel);
        if (width_of(current, probe) <= value_rect.Width) break;
    }
    Gdiplus::Font fitted(&value_family, value_size, Gdiplus::FontStyleBold,
                         Gdiplus::UnitPixel);
    DrawText(graphics, current, value_rect,
             value_size < base_size ? fitted : value_font,
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
        EndAction();
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

void OsdOverlay::SetNetEnabled(const bool enabled) noexcept {
    if (net_enabled_ == enabled) return;
    net_enabled_ = enabled;
    // The arrangement gains or loses a cell, so the window changes width.
    const POINT position = Position();
    const auto layout = CurrentLayout();
    POINT next = position;
    (void)FitToWorkArea(next, layout);
    ApplyPosition(next, layout);
    RenderLatest();
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
    const auto placed = CurrentPlacement();
    // A release outside the rows, or more than one band below the last one,
    // changes nothing -- so say so before the reader commits to it.
    const bool droppable =
        compact::DropTargetAt(grid, placed, cursor.x, cursor.y).row >= 0;
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

// What is known about the frame source, in the fewest characters that stay
// true: an API name when the modules could be read, a size when one could be
// established, and nothing at all for either when it could not.
//
// The size is what the program PRESENTED, not what its engine rendered
// internally: a game upscaling with DLSS or FSR resolves into its own
// full-size swapchain and that texture never leaves the process. Reading it
// would need code inside the game.
std::wstring FpsBackendText(const fps::Snapshot& snapshot) {
    if (snapshot.status != fps::Status::Ready) return {};
    return fps::BackendLabel(snapshot.backend);
}

std::wstring FpsResolutionText(const fps::Snapshot& snapshot) {
    if (snapshot.status != fps::Status::Ready ||
        !snapshot.resolution.Known()) return {};
    return std::format(L"{}\u00d7{}", snapshot.resolution.width,
                       snapshot.resolution.height);
}

// Both, for the expanded lane, which has the width for one line.
std::wstring FpsSource(const fps::Snapshot& snapshot) {
    std::wstring text = FpsBackendText(snapshot);
    const std::wstring resolution = FpsResolutionText(snapshot);
    if (!resolution.empty()) {
        if (!text.empty()) text += L" \u00b7 ";
        text += resolution;
    }
    return text;
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
        case compact::Metric::Network:
            return std::wstring(localization::Select(L"網路", L"NET"));
    }
    return L"";
}

}  // namespace

bool OsdBusyIndicator::Begin(const HWND owner, const RECT& screen_rect,
                             const std::uint32_t accent, const float scale,
                             std::wstring label,
                             const animation::Effect effect) noexcept {
    try {
        End();
        accent_ = accent;
        effect_ = effect;
        scale_ = scale > 0.1F ? scale : 1.0F;
        label_ = std::move(label);
        started_ms_ = GetTickCount64();
        // Owned by the overlay so Windows keeps it above its owner, and
        // NOACTIVATE so pressing it never steals focus from the game. No
        // WS_EX_TRANSPARENT: swallowing the pointer is the point.
        const DWORD ex_style = WS_EX_LAYERED | WS_EX_NOACTIVATE |
                               WS_EX_TOOLWINDOW | WS_EX_TOPMOST;
        RECT bounds = screen_rect;   // _U_RECT wants a non-const pointer
        if (Create(owner, &bounds, nullptr, WS_POPUP, ex_style) == nullptr) {
            return false;
        }
        SetWindowPos(HWND_TOPMOST, screen_rect.left, screen_rect.top,
                     screen_rect.right - screen_rect.left,
                     screen_rect.bottom - screen_rect.top,
                     SWP_NOACTIVATE | SWP_SHOWWINDOW);
        Paint();
        // Its own cadence. The overlay keeps rendering at 500 ms throughout.
        (void)SetTimer(kAnimationTimerId, kAnimationIntervalMs);
        return true;
    } catch (...) {
        End();
        return false;
    }
}

void OsdBusyIndicator::ShowResult(std::wstring text,
                                  const std::uint32_t tint,
                                  const int bars) noexcept {
    if (m_hWnd == nullptr) return;
    try {
        result_ = std::move(text);
    } catch (...) {
        result_.clear();
    }
    result_tint_ = tint;
    result_bars_ = bars;
    showing_result_ = true;
    // The sweep has nothing left to say, so the timer stops and the figure is
    // painted once. A still frame also costs the game nothing.
    KillTimer(kAnimationTimerId);
    Paint();
}

void OsdBusyIndicator::End() noexcept {
    if (m_hWnd == nullptr) return;
    KillTimer(kAnimationTimerId);
    DestroyWindow();
    label_.clear();
    result_.clear();
    result_bars_ = -1;
    showing_result_ = false;
}

LRESULT OsdBusyIndicator::OnTimer(UINT, const WPARAM id, LPARAM, BOOL& handled) {
    if (id != kAnimationTimerId) {
        handled = FALSE;
        return 0;
    }
    Paint();
    return 0;
}

void OsdBusyIndicator::Paint() noexcept {
    if (m_hWnd == nullptr) return;
    RECT rect{};
    if (GetWindowRect(&rect) == FALSE) return;
    const int width = rect.right - rect.left;
    const int height = rect.bottom - rect.top;
    if (width <= 0 || height <= 0) return;

    const HDC screen = ::GetDC(nullptr);
    if (screen == nullptr) return;
    const HDC memory = CreateCompatibleDC(screen);
    if (memory == nullptr) {
        ::ReleaseDC(nullptr, screen);
        return;
    }
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(info.bmiHeader);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    const HBITMAP bitmap =
        CreateDIBSection(screen, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (bitmap == nullptr) {
        DeleteDC(memory);
        ::ReleaseDC(nullptr, screen);
        return;
    }
    const HGDIOBJ previous = SelectObject(memory, bitmap);

    {
        Gdiplus::Graphics graphics(memory);
        graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        graphics.SetTextRenderingHint(Gdiplus::TextRenderingHintClearTypeGridFit);
        graphics.Clear(Gdiplus::Color(0, 0, 0, 0));

        const float s = scale_;
        const Gdiplus::Color accent(
            static_cast<BYTE>((accent_ >> 24) & 0xFF),
            static_cast<BYTE>((accent_ >> 16) & 0xFF),
            static_cast<BYTE>((accent_ >> 8) & 0xFF),
            static_cast<BYTE>(accent_ & 0xFF));

        // The cell keeps its own shape and accent, so the reader sees the
        // record they clicked rather than a foreign panel appearing on top.
        //
        // Inset by half the pen. A stroke straddles its path, so a rectangle
        // flush with the bitmap loses the outer half of its right and bottom
        // edges to the edge of the surface -- the frame came out open on two
        // sides. The dashboard's own cells do not show this because they are
        // drawn inside a larger bitmap with room for the stroke to spill; this
        // window is exactly one cell, so it has none.
        const float pen_width = s;
        Gdiplus::GraphicsPath path;
        AddRoundedRectangle(
            path, {pen_width * 0.5F, pen_width * 0.5F,
                   static_cast<Gdiplus::REAL>(width) - pen_width,
                   static_cast<Gdiplus::REAL>(height) - pen_width}, 3.0F * s);
        Gdiplus::SolidBrush base(Gdiplus::Color(
            232, 22, 27, 36));
        graphics.FillPath(&base, &path);
        Gdiplus::Pen border(Gdiplus::Color(
            210, accent.GetR(), accent.GetG(), accent.GetB()), pen_width);
        graphics.DrawPath(&border, &path);

        Gdiplus::FontFamily family(L"Segoe UI");
        Gdiplus::Font label_font(&family, 9.0F * s, Gdiplus::FontStyleRegular,
                                 Gdiplus::UnitPixel);
        DrawText(graphics, label_,
                 {4.0F * s, 1.0F * s, static_cast<Gdiplus::REAL>(width) - 8.0F * s,
                  14.0F * s},
                 label_font, accent, Gdiplus::StringAlignmentNear);

        if (showing_result_) {
            // The figure sits where the value normally does, so the eye lands
            // in the same place it already reads this cell. One frame, no
            // timer: a still result costs a game nothing.
            // Adaptive precision keeps the common cases at full size, but a
            // three-digit figure on a very large machine still would not fit.
            // Step down until it does rather than truncate: an ellipsis in the
            // middle of a number turns it into a different number. Measured on
            // this palette, "+128 GB" needs one step and "+128.0 GB" needs
            // four, which is why precision is reduced first.
            // The mark, when the action has one. Three bars, `n` filled --
            // drawn rather than typeset, because an emoji at this size is a
            // smudge and because the palette here is already vector. The
            // unfilled bars stay visible at low alpha so the reader sees one
            // of three rather than a lone bar meaning nothing.
            float text_left = 4.0F * s;
            if (result_bars_ >= 0) {
                const float bar_width = 2.5F * s;
                const float gap = 1.5F * s;
                const float bar_base = 26.0F * s;   // bars grow upward from here
                const Gdiplus::Color mark(
                    static_cast<BYTE>((result_tint_ >> 24) & 0xFF),
                    static_cast<BYTE>((result_tint_ >> 16) & 0xFF),
                    static_cast<BYTE>((result_tint_ >> 8) & 0xFF),
                    static_cast<BYTE>(result_tint_ & 0xFF));
                for (int i = 0; i < 3; ++i) {
                    const float bar_height = (3.0F + 3.0F * static_cast<float>(i)) * s;
                    const bool filled = i < result_bars_;
                    Gdiplus::SolidBrush brush(Gdiplus::Color(
                        static_cast<BYTE>(filled ? mark.GetA() : 60),
                        mark.GetR(), mark.GetG(), mark.GetB()));
                    graphics.FillRectangle(
                        &brush, text_left + static_cast<float>(i) * (bar_width + gap),
                        bar_base - bar_height, bar_width, bar_height);
                }
                text_left += 3.0F * bar_width + 2.0F * gap + 3.0F * s;
            }
            const float box_width =
                static_cast<Gdiplus::REAL>(width) - 4.0F * s - text_left;
            Gdiplus::StringFormat measure;
            measure.SetFormatFlags(Gdiplus::StringFormatFlagsNoWrap);
            float point_size = 15.0F;
            for (; point_size > 10.0F; point_size -= 1.0F) {
                Gdiplus::Font probe(&family, point_size * s,
                                    Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
                Gdiplus::RectF measured;
                graphics.MeasureString(result_.c_str(), -1, &probe,
                                       Gdiplus::PointF(0.0F, 0.0F), &measure,
                                       &measured);
                if (measured.Width <= box_width) break;
            }
            Gdiplus::Font result_font(&family, point_size * s,
                                      Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
            const Gdiplus::Color tint(
                static_cast<BYTE>((result_tint_ >> 24) & 0xFF),
                static_cast<BYTE>((result_tint_ >> 16) & 0xFF),
                static_cast<BYTE>((result_tint_ >> 8) & 0xFF),
                static_cast<BYTE>(result_tint_ & 0xFF));
            DrawText(graphics, result_,
                     {text_left, 14.0F * s, box_width, 22.0F * s},
                     result_font, tint, Gdiplus::StringAlignmentNear);
        } else {
            // The animation is arithmetic, drawn here and decided in
            // osd_animation.hpp. Nothing here is a progress bar: how long a
            // reclaim takes is set by the processes being trimmed, so a bar
            // filling at an invented rate would be a lie told smoothly.
            //
            // Blocks rather than glyphs. A cell is 72 x 51 dip, and characters
            // at that size are texture rather than symbols; four-dip blocks
            // read as falling at a glance.
            const std::uint64_t elapsed = GetTickCount64() - started_ms_;
            const float block = animation::kBlockDip * s;
            const float field_left = 4.0F * s;
            const float field_top = 16.0F * s;
            const int columns =
                animation::BlocksAcross(static_cast<float>(width) / s - 8.0F);
            const int rows = animation::BlocksAcross(
                static_cast<float>(height) / s - 19.0F);
            // One brush recoloured per block rather than one per block: at 33
            // ms a cell can light a hundred blocks a frame, and a GDI+ object
            // each would be churn paid for by the game.
            Gdiplus::SolidBrush brush(accent);
            for (int cx = 0; cx < columns; ++cx) {
                for (int cy = 0; cy < rows; ++cy) {
                    const float level = animation::IntensityAt(
                        effect_, cx, cy, columns, rows, elapsed);
                    if (level <= 0.0F) continue;
                    // The record's own accent, so the cell keeps its identity
                    // while it works. Squared, so the trail falls away quickly
                    // and the head stays the thing the eye lands on.
                    const auto alpha =
                        static_cast<BYTE>(25.0F + 220.0F * level * level);
                    brush.SetColor(Gdiplus::Color(alpha, accent.GetR(),
                                                  accent.GetG(),
                                                  accent.GetB()));
                    graphics.FillRectangle(
                        &brush, field_left + static_cast<float>(cx) * block,
                        field_top + static_cast<float>(cy) * block,
                        block - 1.0F * s, block - 1.0F * s);
                }
            }
        }
    }

    POINT source{0, 0};
    POINT position{rect.left, rect.top};
    SIZE size{width, height};
    BLENDFUNCTION blend{};
    blend.BlendOp = AC_SRC_OVER;
    blend.SourceConstantAlpha = 255;
    blend.AlphaFormat = AC_SRC_ALPHA;
    (void)UpdateLayeredWindow(m_hWnd, screen, &position, &size, memory, &source,
                              0, &blend, ULW_ALPHA);

    SelectObject(memory, previous);
    DeleteObject(bitmap);
    DeleteDC(memory);
    ::ReleaseDC(nullptr, screen);
}

// A double-click on a cell activates that record. Only while the dashboard is
// locked: unlocked is arrange mode and keeps the drag gesture it already has,
// so the two can never be confused with one another.
LRESULT OsdOverlay::OnCellActivate(UINT, WPARAM, const LPARAM point, BOOL& handled) {
    if (!collapsed_ || !compact_locked_ || !ShowsLock()) {
        handled = FALSE;
        return 0;
    }
    // The indicator's existence is the guard. No second boolean to fall out of
    // step with what the reader can see.
    if (busy_indicator_.Active()) return 0;

    const auto geometry = CurrentLayout();
    const auto grid = compact::MakeCellGrid(geometry.columns, geometry.width,
                                            geometry.scale);
    const auto placed = CurrentPlacement();
    const POINT cursor{GET_X_LPARAM(point), GET_Y_LPARAM(point)};
    const int slot = compact::CellSlotAt(grid, placed, cursor.x, cursor.y);
    if (slot < 0) return 0;
    // Two records have an action. Every other one ignores the gesture rather
    // than pretending to do something.
    switch (placed.cells[static_cast<std::size_t>(slot)]) {
        case compact::Metric::Ram: BeginReclaim(slot); break;
        case compact::Metric::Network: BeginNetworkProbe(slot); break;
        default: break;
    }
    return 0;
}

bool OsdOverlay::BeginAction(const int slot,
                             const compact::Metric metric) noexcept {
    try {
        if (busy_indicator_.Active()) return false;
        if (action_worker_.joinable()) action_worker_.join();

        const auto geometry = CurrentLayout();
        const auto grid = compact::MakeCellGrid(geometry.columns, geometry.width,
                                                geometry.scale);
        const auto placed = CurrentPlacement();
        const auto origin = compact::CellOriginAt(grid, placed, slot);
        RECT window_rect{};
        if (GetWindowRect(&window_rect) == FALSE) return false;
        // Round, do not truncate. A cell is 51 dip tall, which is 127.5 px at
        // 250 % scaling; truncating made the indicator a pixel short and left
        // the cell's own border showing along its bottom and right, which is
        // exactly what the owner saw. Truncation always loses on the far edge,
        // never the near one, so the artefact only ever appeared on two sides.
        const auto round_to_px = [](const float value) {
            return static_cast<int>(value + 0.5F);
        };
        const int left = window_rect.left + round_to_px(origin.x);
        const int top = window_rect.top + round_to_px(origin.y);
        const RECT cell{
            left, top,
            left + round_to_px(grid.cell_width_dip * geometry.scale),
            top + round_to_px(compact::kCellHeightDip * geometry.scale)};

        const Gdiplus::Color accent = Accent(metric);
        const std::uint32_t argb =
            (static_cast<std::uint32_t>(accent.GetA()) << 24) |
            (static_cast<std::uint32_t>(accent.GetR()) << 16) |
            (static_cast<std::uint32_t>(accent.GetG()) << 8) |
            static_cast<std::uint32_t>(accent.GetB());
        // The house effect. Neither action has progress to report -- a
        // reclaim's length is set by the processes being trimmed and the
        // probe's by a fixed window -- so whatever is drawn here must not look
        // like it is counting down to anything.
        if (!busy_indicator_.Begin(m_hWnd, cell, argb, geometry.scale,
                                   CompactLabel(metric),
                                   animation::kDefaultEffect)) {
            return false;
        }

        action_started_ms_ = GetTickCount64();
        action_visible_until_ms_ = 0;
        action_result_until_ms_ = 0;
        action_done_.store(false, std::memory_order_release);

        // The teardown poll is deliberately coarse: it only has to notice that
        // the work finished and that the minimum has elapsed. The animation
        // runs on the indicator's own timer, not this one.
        (void)SetTimer(kActionTimerId, kActionPollIntervalMs);
        return true;
    } catch (...) {
        EndAction();
        return false;
    }
}

void OsdOverlay::BeginReclaim(const int slot) noexcept {
    reclaim_outcome_ = sysmem::reclaim::Outcome{};
    if (!BeginAction(slot, compact::Metric::Ram)) return;
    active_action_ = CellAction::Reclaim;
    try {

        // Its own thread. Never the protection worker, and never this one: on
        // the message loop the trim would stall the loop, which would also
        // stop the indicator animating. The animation is not decoration, it is
        // evidence that the work is somewhere else.
        const sysmem::reclaim::Policy policy = reclaim_policy_;
        action_worker_ = std::thread([this, policy]() noexcept {
            sysmem::reclaim::Outcome outcome;
            try {
                outcome = sysmem::reclaim::Run(policy);
            } catch (...) {
                // A reclaim that throws must still take its indicator down.
            }
            reclaim_outcome_ = outcome;
            action_done_.store(true, std::memory_order_release);
        });
    } catch (...) {
        EndAction();
    }
}

// Measure the foreground program's path and report what the stack already
// knows about it.
//
// Note what is NOT here: nothing is sent, nothing is opened, nothing is
// resolved for the purpose of timing it. Every figure comes from counters the
// TCP stack kept whether or not this program asked. That is why the action can
// be honest about improving nothing -- it never claimed to be a test.
void OsdOverlay::BeginNetworkProbe(const int slot) noexcept {
    net_verdict_ = net::Verdict{};
    net_status_ = net::ProbeStatus::NoLibrary;
    if (!BeginAction(slot, compact::Metric::Network)) return;
    active_action_ = CellAction::Network;
    try {
        // Its own thread, for the same reason the reclaim has one -- but here
        // the thread also sleeps out the measurement window, and doing that on
        // the message loop would stop the very animation that tells the reader
        // the work is somewhere else.
        action_worker_ = std::thread([this]() noexcept {
            net::Verdict verdict;
            net::ProbeStatus status = net::ProbeStatus::NoLibrary;
            if (net_stub_) {
                verdict = net_stub_->first;
                status = net_stub_->second;
                net_verdict_ = verdict;
                net_status_ = status;
                action_done_.store(true, std::memory_order_release);
                return;
            }
            try {
                net::Probe probe;
                if (probe.Begin()) {
                    probe.Attempt();
                    Sleep(static_cast<DWORD>(net::action::kMeasureWindowMs));
                    verdict = probe.Finish();
                }
                status = probe.status();
            } catch (...) {
                // A probe that throws must still take its indicator down.
            }
            net_verdict_ = verdict;
            net_status_ = status;
            action_done_.store(true, std::memory_order_release);
        });
    } catch (...) {
        EndAction();
    }
}

void OsdOverlay::EndAction() noexcept {
    if (m_hWnd != nullptr) KillTimer(kActionTimerId);
    busy_indicator_.End();
    if (action_worker_.joinable()) {
        // Every worker is bounded -- the reclaim by reclaim::BudgetSpent, the
        // probe by its measurement window -- so this join cannot wait
        // indefinitely. Without that, a topmost window that swallows input
        // would be stuck over the reader's screen.
        action_worker_.join();
    }
    active_action_ = CellAction::None;
    action_started_ms_ = 0;
    action_visible_until_ms_ = 0;
    action_result_until_ms_ = 0;
    action_done_.store(false, std::memory_order_release);
    if (m_hWnd != nullptr) RequestRefresh();
}

LRESULT OsdOverlay::OnOverlayTimer(UINT, const WPARAM id, LPARAM, BOOL& handled) {
    if (id != kActionTimerId) {
        handled = FALSE;
        return 0;
    }
    const std::uint64_t now = GetTickCount64();
    if (!busy_indicator_.Active() || active_action_ == CellAction::None) {
        KillTimer(kActionTimerId);
        return 0;
    }
    // How long this particular action is allowed to take, and how long its
    // answer stays up. The rest of this function is identical for both, which
    // is why only the numbers are chosen here.
    const bool is_network = active_action_ == CellAction::Network;
    const std::uint64_t minimum_visible_ms =
        is_network ? net::action::kMinimumVisibleMs
                   : sysmem::reclaim::kMinimumVisibleMs;
    const std::uint64_t result_hold_ms =
        is_network ? net::action::kResultHoldMs : sysmem::reclaim::kResultHoldMs;
    const std::uint64_t budget_ms =
        is_network ? net::action::kWorkBudgetMs : sysmem::reclaim::kWorkBudgetMs;
    // Written once. "Is this deadline behind us" is not a rule belonging to
    // either action, and giving each its own copy is how the two would drift.
    const auto elapsed = [now](const std::uint64_t deadline) {
        return deadline != 0 && now >= deadline;
    };

    if (action_done_.load(std::memory_order_acquire)) {
        if (action_visible_until_ms_ == 0) {
            // Work can finish faster than a reader can perceive. The floor is
            // what stops the indicator flashing past and inviting a second
            // click at the very moment the first one is still settling.
            action_visible_until_ms_ =
                action_started_ms_ + minimum_visible_ms;
        }
        if (!elapsed(action_visible_until_ms_)) return 0;
        // The rain has run its course. Say what happened, in the cell, for a
        // moment. Nothing pops up and nothing takes focus: the reader is in a
        // game and an interruption would cost more than the answer is worth.
        if (action_result_until_ms_ == 0) {
            if (!ReportActionResult(now)) {
                EndAction();
                return 0;
            }
            action_result_until_ms_ = now + result_hold_ms;
            return 0;
        }
        if (elapsed(action_result_until_ms_)) EndAction();
        return 0;
    }
    // Each worker has its own budget, but if one were ever to overrun even
    // that, the indicator must still come down rather than live on the
    // reader's screen. Twice the budget is generous and finite.
    if (now > action_started_ms_ && now - action_started_ms_ > budget_ms * 2) {
        EndAction();
    }
    return 0;
}

// What the finished action puts in the cell. False means there is nothing
// worth showing and the indicator should simply go away.
bool OsdOverlay::ReportActionResult(const std::uint64_t now) noexcept {
    switch (active_action_) {
        case CellAction::Reclaim: {
            // One record per invocation. Unlike the RAM telemetry beside it,
            // this is a user-initiated system-level action: it changed other
            // processes, and someone asking later why a machine behaved oddly
            // deserves to find out that it happened. Written after the fact,
            // on the message loop, through the try-only admission -- a full
            // queue drops it rather than waiting.
            (void)logging::TryInfo(std::format(
                L"host memory reclaim: considered={} trimmed={} would_trim={} "
                L"failed={} skipped={} cache_flushed={} timed_out={} dry_run={} "
                L"freed_gib={:.2f} elapsed_ms={}",
                reclaim_outcome_.considered, reclaim_outcome_.trimmed,
                reclaim_outcome_.would_trim,
                reclaim_outcome_.failed, reclaim_outcome_.skipped,
                reclaim_outcome_.cache_flushed ? 1 : 0,
                reclaim_outcome_.timed_out ? 1 : 0,
                reclaim_policy_.dry_run ? 1 : 0,
                sysmem::reclaim::FreedGib(reclaim_outcome_),
                now > action_started_ms_ ? now - action_started_ms_ : 0));
            if (!sysmem::reclaim::HasResultToShow(reclaim_outcome_,
                                                  reclaim_policy_.dry_run)) {
                return false;
            }
            busy_indicator_.ShowResult(FormatReclaimResult(reclaim_outcome_),
                                       ReclaimResultTint(reclaim_outcome_));
            return true;
        }
        case CellAction::Network: {
            // Recorded the same way and for a weaker reason: this action
            // changed nothing on the machine. It is logged so that a verdict
            // the reader disputes can be checked against what was measured,
            // rather than argued from memory.
            (void)logging::TryInfo(std::format(
                L"network verdict: status={} grade={} rtt_ms={} connections={} "
                L"elapsed_ms={}",
                static_cast<int>(net_status_),
                static_cast<int>(net_verdict_.grade), net_verdict_.rtt_ms,
                net_verdict_.connections,
                now > action_started_ms_ ? now - action_started_ms_ : 0));
            // Always shown, including every way it can fail. A reclaim that
            // freed nothing has nothing to say; a probe that measured nothing
            // has something specific to say, and "idle" or "denied" is the
            // answer to the question the reader just asked.
            busy_indicator_.ShowResult(
                net::action::ResultText(net_verdict_, net_status_),
                net::action::TintFor(net_verdict_.grade),
                net::action::BarsFor(net_verdict_.grade));
            return true;
        }
        case CellAction::None: break;
    }
    return false;
}

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
    const auto placed = CurrentPlacement();
    const auto grid = compact::MakeCellGrid(geometry.columns, geometry.width,
                                            geometry.scale);
    // Read against the dragged image's origin, not the cursor: the reader is
    // aiming a block they can see, not a point they cannot.
    const POINT origin{point.x - drag_hotspot_.x, point.y - drag_hotspot_.y};
    const auto plan = compact::PlanDrop(grid, placed, origin.x, origin.y);
    const auto next = compact::ApplyPlan(layout_, placed, from_slot, plan);
    if (next.order == layout_.order && next.breaks == layout_.breaks) return;
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
                CurrentPlacement();
            const int slot = compact::CellSlotAt(
                compact::MakeCellGrid(geometry.columns, geometry.width,
                                      geometry.scale),
                placed, cursor.x, cursor.y);
            if (slot >= 0) {
                drag_from_ = slot;
                ::SetCapture(input_window);
                const auto grid = compact::MakeCellGrid(
                    geometry.columns, geometry.width, geometry.scale);
                const auto origin = compact::CellOriginAt(grid, placed, slot);
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
    EndAction();
    if (message == WM_CANCELMODE && ::GetCapture() == input_window)
        ::ReleaseCapture();
    return 0;
}

void OsdOverlay::ToggleCollapsed() noexcept {
    // The expanded view has no cells, so an indicator pinned to one would be
    // left floating over a lane.
    EndAction();
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
    // The cell this indicator was pinned over has just moved or changed size,
    // so it would sit over the wrong place, or over nothing.
    EndAction();
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
        EndAction();
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
        collapsed_, CurrentPlacement(),
        work_width_dip, work_height_dip);
    return {MulDiv(footprint.width, static_cast<int>(dpi), 96),
            MulDiv(footprint.height, static_cast<int>(dpi), 96),
            MulDiv(kDragHeightDip, static_cast<int>(dpi), 96),
            static_cast<float>(dpi) / 96.0F, footprint.columns, footprint.rows};
}

bool OsdOverlay::FitToWorkArea(POINT& point, const Layout& layout) const noexcept {
    RECT proposed{point.x, point.y, point.x + layout.width, point.y + layout.height};
    const bool repaired = MonitorFromRect(&proposed, MONITOR_DEFAULTTONULL) == nullptr;
    // The monitor is decided by where the window already is, not by where a
    // resize would spill it to.
    //
    // MonitorFromRect answers with the monitor the rectangle overlaps most.
    // Expanding an 84-dip column docked on a right edge widens the proposed
    // rectangle across the neighbouring screen, which then wins that test, and
    // the clamp below pins the window to *its* left edge. To the reader the
    // overlay vanishes and reappears on another screen -- it reads as losing
    // the window, not as a repair.
    //
    // The anchor cannot move the window between screens, so expanding keeps it
    // where it was and slides it left only as far as fitting requires.
    HMONITOR monitor = MonitorFromPoint(point, MONITOR_DEFAULTTONULL);
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
        // At one cell across the header is 84 dip and the two buttons take 50
        // of it. The label and the dot both sit under them at their usual
        // places, so the label goes and the dot moves to the left margin --
        // which is also what leaves 34 x 25 dip of grabbable drag strip.
        const int widest_columns =
            layout.columns < 0 ? -layout.columns : layout.columns;
        const bool show_title = !collapsed_ || compact::HeaderShowsTitle(widest_columns);
        if (show_title) {
            DrawText(graphics, L"GTG",
                     {10.0F * s, 2.0F * s, 30.0F * s, 22.0F * s}, title_font,
                     Gdiplus::Color(222, 237, 243, 251), Gdiplus::StringAlignmentNear);
        }
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
        const float dot_left = (show_title ? 46.0F : 11.0F) * s;
        graphics.FillEllipse(&dot, dot_left, 9.0F * s, 6.0F * s, 6.0F * s);
        const float status_left = 58.0F * s;
        const float header_buttons =
            static_cast<float>(compact::HeaderButtonWidth(layout.drag_height)) *
            (ShowsLock() ? 2.0F : 1.0F);
        // Two cells across leaves 48 dip for the status, which fits only the
        // shortest Chinese string. Rather than truncate a warning into an
        // ellipsis, the header keeps the dot and drops the words: the colour
        // already says which state this is, and the detail lives in the main
        // window. Above two cells there is room for every string in either
        // language, so nothing changes there.
        const bool show_status_text =
            !collapsed_ || compact::HeaderShowsStatusText(widest_columns);
        if (show_status_text) {
            DrawText(graphics, displayed_status,
                     {status_left, 2.0F * s,
                      layout.width - header_buttons - status_left - 4.0F * s,
                      22.0F * s}, status_font,
                     status_color, Gdiplus::StringAlignmentNear);
        }
        // The hint line marks where the window can be grabbed, and under a
        // title it sits in the middle of the strip it describes. With no title
        // there is no middle to speak of: the line ended up under the status
        // dot, pointing at nothing in particular. The dot is already the only
        // thing in that corner, and the whole corner is draggable, so at one
        // cell across the line is simply not drawn.
        if (show_title) {
            Gdiplus::Pen handle(Gdiplus::Color(38, 203, 215, 231), 1.0F * s);
            graphics.DrawLine(&handle, layout.width / 2.0F - 10.0F * s, 23.0F * s,
                              layout.width / 2.0F + 10.0F * s, 23.0F * s);
        }
        // The buttons are square and narrower than the band is tall. At the
        // band's full 25 dip their 3-dip insets left a 6-dip gap that read as
        // two unrelated controls, and at one cell across the lock landed near
        // the middle of an 84-dip window. Nine tenths closes the gap, moves
        // both towards the corner they belong in, and widens the drag strip
        // from 34 to 40 dip in the process.
        const float button_w =
            static_cast<float>(compact::HeaderButtonWidth(layout.drag_height));
        // Everything inside a button is expressed against the band it used to
        // fill, so one factor rescales the whole glyph rather than a dozen
        // hand-adjusted offsets.
        const float bs = button_w / static_cast<float>(compact::kDragBandDip);
        const float button_y =
            (static_cast<float>(layout.drag_height) - button_w) / 2.0F;
        const float button_x = static_cast<float>(layout.width) - button_w;
        Gdiplus::SolidBrush button_fill(Gdiplus::Color(22, 203, 215, 231));
        graphics.FillRectangle(&button_fill, button_x + 3.0F * bs,
                               button_y + 3.0F * bs, 19.0F * bs, 19.0F * bs);
        Gdiplus::Pen chevron(Gdiplus::Color(235, 224, 235, 248), 1.5F * bs);
        chevron.SetLineJoin(Gdiplus::LineJoinRound);
        const float edge_y = button_y + (collapsed_ ? 10.0F : 14.0F) * bs;
        const float middle_y = button_y + (collapsed_ ? 14.0F : 10.0F) * bs;
        // The two plates are already adjacent, but each glyph is centred in
        // its own plate, so the gap the eye sees is the sum of two inner
        // margins -- wider than the seam between the plates. Nudging the
        // glyphs towards each other closes it without moving either plate or
        // its hit rectangle.
        const float chevron_nudge = 1.5F * bs;
        const Gdiplus::PointF arrow[]{
            {button_x + 8.0F * bs - chevron_nudge, edge_y},
            {button_x + 12.5F * bs - chevron_nudge, middle_y},
            {button_x + 17.0F * bs - chevron_nudge, edge_y}};
        graphics.DrawLines(&chevron, arrow, 3);

        // The lock is offered only where arranging is possible: the compact
        // dashboard, and only in builds whose body receives pointer input.
        if (ShowsLock()) {
            const float lock_x = button_x - button_w;
            graphics.FillRectangle(&button_fill, lock_x + 3.0F * bs,
                                   button_y + 3.0F * bs, 19.0F * bs, 19.0F * bs);
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
            Gdiplus::Pen lock_pen(lock_tint, 1.4F * bs);
            lock_pen.SetLineJoin(Gdiplus::LineJoinRound);
            // Closed: the shackle sits on the body. Open: it lifts and shifts.
            const float body_top = button_y + 12.0F * bs;
            const float lock_nudge = 1.0F * bs;
            const float arc_x =
                lock_x + (compact_locked_ ? 9.0F : 10.75F) * bs + lock_nudge;
            graphics.DrawArc(&lock_pen, arc_x, body_top - 5.25F * bs, 7.0F * bs,
                             7.0F * bs, 180.0F, 180.0F);
            const Gdiplus::RectF body(lock_x + 7.75F * bs + lock_nudge, body_top,
                                      9.5F * bs, 6.5F * bs);
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
            // Geometry comes from the shared grid, the same one hit testing
            // uses. Render used to derive cell origins from its own copy of
            // these numbers, and two copies of one layout is exactly how the
            // expanded lanes drifted apart. A second pointer gesture would
            // have made any drift land on the wrong cell.
            const auto grid = compact::MakeCellGrid(layout.columns, layout.width, s);
            const float cell_width = grid.cell_width_dip * s;

            // A record describes itself, then one block draws every record the
            // same way. RAM and FPS are rows in that description rather than
            // special cases appended after it: they differ only in where their
            // value and their sparkline come from.
            enum class Spark { None, Telemetry, Memory, Fps, Network };
            // One format for every measurement below; MeasureString wraps by
            // default and a wrapped measurement is not the width that will be
            // drawn.
            Gdiplus::StringFormat measure_format;
            measure_format.SetFormatFlags(Gdiplus::StringFormatFlagsNoWrap);
            struct CellContent {
                std::wstring label;
                // Drawn after the label in a smaller face, when a record has a
                // unit that changes. Only network and FPS do: every other
                // record's unit is part of its value and never moves.
                //
                // The FPS record right-aligns this, so its resolution stacks
                // on the same edge as the backend below it: both boxes end at
                // x + cell_width - 4 by construction, with no shared constant
                // to keep in step. Network keeps it beside the label -- `MB/s`
                // is too short to survive being stranded at the far edge, and
                // a unit belongs to the word it qualifies.
                std::wstring unit;
                // Drawn on the value row, right of the value. The two small
                // slots carry the FPS record's backend and resolution, and
                // which goes where is not arbitrary: the label row has the
                // more room, because `FPS` is three characters while a value
                // like `120` plus its gap is wider. So the long string --
                // `3840x2160` -- takes the label row and the short one --
                // `D12` -- takes the value row. Put the other way round the
                // resolution had to shrink to about five point to fit.
                std::wstring footnote;
                // The resolution is the one small text that has to be read
                // rather than merely noticed, so it is drawn in a colour of
                // its own instead of the record's accent.
                bool unit_highlight{};
                std::wstring value;
                // When non-empty, `value` is drawn in these pieces instead of
                // as one run, so a piece can take a colour of its own. The
                // pieces must concatenate to `value`, which stays the
                // authority for fitting: the line then occupies the width it
                // was measured and sized for.
                std::vector<TextRun> value_runs;
                Gdiplus::Color accent{};
                MetricMember member{nullptr};
                double minimum_span{};
                bool unavailable{};
                bool attention{};   // the reading itself is the warning
                Spark spark{Spark::None};
            };
            const auto describe = [&](const compact::Metric metric) {
                CellContent c;
                c.label = CompactLabel(metric);
                c.accent = Accent(metric);
                c.unavailable = stale;
                c.spark = Spark::Telemetry;
                switch (metric) {
                case compact::Metric::Temperature:
                    c.member = &telemetry::Sample::temperature_c;
                    c.value = FormatMetric(optional(c.member), L" °C");
                    c.minimum_span = 10.0;
                    break;
                case compact::Metric::Power:
                    c.member = &telemetry::Sample::power_w;
                    c.value = FormatMetric(optional(c.member), L" W", 1);
                    c.minimum_span = 100.0;
                    break;
                case compact::Metric::Vram:
                    c.member = &telemetry::Sample::vram_utilization_percent;
                    c.value = FormatMetric(optional(c.member), L"%");
                    c.minimum_span = 20.0;
                    break;
                case compact::Metric::Gpu:
                    c.member = &telemetry::Sample::gpu_utilization_percent;
                    c.value = FormatMetric(optional(c.member), L"%");
                    c.minimum_span = 20.0;
                    break;
                case compact::Metric::Cpu:
                    c.member = &telemetry::Sample::cpu_utilization_percent;
                    c.value = FormatMetric(optional(c.member), L"%");
                    c.minimum_span = 20.0;
                    break;
                case compact::Metric::Ram: {
                    // One coherent live reading, as in the expanded lane.
                    const auto live = sysmem::Query();
                    c.value = live.physical_percent
                        ? std::format(L"{:.0f}%", *live.physical_percent)
                        : L"-";
                    c.unavailable = !live.physical_percent;
                    c.attention = host_memory_low_ && live.physical_percent.has_value();
                    c.spark = ram_history_ != nullptr ? Spark::Memory : Spark::None;
                    break;
                }
                case compact::Metric::Network: {
                    // Both directions in one unit, chosen from the peak over
                    // the drawn window rather than the latest reading: the
                    // instantaneous value crosses unit boundaries constantly,
                    // and the label would flicker. The number and the curve
                    // beneath it are then scaled by the same quantity.
                    const auto* latest =
                        net_history_ == nullptr ? nullptr : net_history_->Latest();
                    // The curve still scales to the window peak -- the
                    // Spark::Network case below asks for it -- but the number
                    // no longer depends on it at all.
                    constexpr net::Unit unit = net::kDisplayUnit;
                    c.unit = net::UnitSuffix(unit);
                    const bool readable = latest != nullptr &&
                                          latest->received_bytes_per_second &&
                                          latest->sent_bytes_per_second;
                    if (readable) {
                        const int rx = net::TenthsIn(
                            *latest->received_bytes_per_second, unit);
                        const int tx = net::TenthsIn(
                            *latest->sent_bytes_per_second, unit);
                        // Whole units, padded to two digits so the pair keeps
                        // its shape as the numbers change -- a cell whose text
                        // jumps between two and three characters twitches at
                        // the edge of vision, which is the opposite of what a
                        // glanceable readout is for. The separator is the one
                        // VRAM already uses.
                        // One decimal below a hundred, none above.
                        //
                        // The decimal is what makes a fixed megabyte scale
                        // usable at all: ordinary traffic is a fraction of one
                        // -- this machine idles between 0.1 and 0.5 -- and
                        // whole units would print `0 · 0` for all of it.
                        //
                        // Above a hundred it is the decimal that breaks the
                        // row instead. Measured: a saturated 10 GbE link reads
                        // `1192.1 · 1190.3` at 95.8 dip and would need 7.5 pt
                        // type to fit a 64 dip row, which is past legible.
                        // Without the decimals the same pair is 74.0 and fits
                        // at 10.5. Nobody needs to know whether 1192 MB/s was
                        // really 1192.1.
                        // The larger figure decides for both, so the pair
                        // keeps one shape. Formatting each for its own
                        // magnitude gives `297 · 31.5`, which reads as two
                        // unrelated measurements rather than one reading.
                        const bool coarse = rx >= 1000 || tx >= 1000;
                        const auto figure = [coarse](const int tenths) {
                            return coarse
                                ? std::format(L"{}", tenths / 10)
                                : std::format(L"{}.{}", tenths / 10, tenths % 10);
                        };
                        // Direction marks rather than a separator. The dot
                        // carried no information at all: nothing on screen
                        // said which figure was down and which was up, so the
                        // reader had to remember.
                        //
                        // Arrows rather than solid triangles. A filled
                        // triangle carries as much ink as a digit, so beside
                        // one it reads as a second glyph competing for
                        // attention instead of as a mark qualifying the
                        // number. An arrow is mostly stroke, so it sits back.
                        // They are drawn at full size for the same reason the
                        // triangles were reduced: the weight has to match the
                        // digits, and for these two shapes that means
                        // opposite adjustments.
                        //
                        // They take the record's accent, not a red/blue pair.
                        // Red already means something on this strip: the
                        // temperature record turns red when it is in trouble.
                        // A second, unrelated red -- for upload traffic, which
                        // is not a problem at all -- would make the colour
                        // mean two contradictory things in one glance.
                        const std::wstring down = figure(rx);
                        const std::wstring up = figure(tx);
                        //
                        // Suffixed rather than prefixed: the numbers are what
                        // the cell is read for, so they start at the left edge
                        // where the other seven records' numbers start, and
                        // the mark trails each one the way a unit does.
                        c.value = std::format(L"{}↓ {}↑", down, up);
                        c.value_runs = {
                            {down, kValuePrimary}, {L"\u2193 ", kNetworkMark},
                            {up, kValuePrimary}, {L"\u2191", kNetworkMark}};
                    } else {
                        c.value = L"-";
                    }
                    c.unavailable = !readable;
                    c.spark = net_history_ != nullptr ? Spark::Network : Spark::None;
                    break;
                }
                case compact::Metric::Fps:
                    c.value = fps_snapshot_.status == fps::Status::Ready
                        ? std::format(L"{:.0f}", fps_snapshot_.displayed_fps)
                        : L"-";
                    // Beside the label, in the small face the NET cell already
                    // uses for its unit: it is context for the number, not a
                    // reading of its own, and it disappears with the number.
                    c.unit = FpsResolutionText(fps_snapshot_);
                    c.footnote = FpsBackendText(fps_snapshot_);
                    c.unit_highlight = true;
                    c.unavailable = fps_snapshot_.status != fps::Status::Ready;
                    c.spark = fps_history_ != nullptr ? Spark::Fps : Spark::None;
                    break;
                }
                return c;
            };

            const auto placed = CurrentPlacement();
            for (int slot = 0; slot < placed.count; ++slot) {
                const auto content =
                    describe(placed.cells[static_cast<std::size_t>(slot)]);
                const auto origin = compact::CellOriginAt(grid, placed, slot);
                const float x = origin.x;
                const float y = origin.y;
                const Gdiplus::Color accent = content.accent;
                // Attention raises the cell's own accent rather than replacing
                // it. A record that changes colour under warning stops being
                // recognisable at a glance, which is the one thing the compact
                // dashboard exists to preserve; a brighter fill and a solid
                // border read as urgency while the record stays itself.
                const BYTE fill_alpha = content.attention ? 46 : 15;
                const BYTE border_alpha = content.attention ? 190 : 42;
                Gdiplus::SolidBrush background(Gdiplus::Color(
                    fill_alpha, accent.GetR(), accent.GetG(), accent.GetB()));
                Gdiplus::Pen border(Gdiplus::Color(
                    border_alpha, accent.GetR(), accent.GetG(), accent.GetB()), s);
                Gdiplus::GraphicsPath path;
                AddRoundedRectangle(
                    path, {x, y, cell_width, compact::kCellHeightDip * s}, 3.0F * s);
                graphics.FillPath(&background, &path);
                graphics.DrawPath(&border, &path);
                DrawText(graphics, content.label,
                    {x + 4.0F * s, y + 1.0F * s, cell_width - 8.0F * s, 14.0F * s},
                    small_font, accent, Gdiplus::StringAlignmentNear);
                if (!content.unit.empty()) {
                    // A size below the label, so a record that has to show its
                    // unit does not shout louder than the seven that do not.
                    Gdiplus::RectF label_size;
                    graphics.MeasureString(content.label.c_str(), -1, &small_font,
                                           Gdiplus::PointF(0.0F, 0.0F),
                                           &measure_format, &label_size);
                    const float unit_room =
                        cell_width - 10.0F * s - label_size.Width;
                    Gdiplus::Font unit_font(&family, 7.5F * s,
                                            Gdiplus::FontStyleRegular,
                                            Gdiplus::UnitPixel);
                    // DrawText trims with an ellipsis, which for a resolution
                    // would quietly produce a different number. Measure first
                    // and say nothing rather than say something wrong; `MB/s`
                    // has never come close to this and is unaffected.
                    Gdiplus::RectF unit_measured;
                    graphics.MeasureString(content.unit.c_str(), -1, &unit_font,
                                           Gdiplus::PointF(0.0F, 0.0F),
                                           &measure_format, &unit_measured);
                    if (unit_measured.Width <= unit_room) {
                        DrawText(graphics, content.unit,
                            {x + 6.0F * s + label_size.Width, y + 2.5F * s,
                             unit_room, 12.0F * s},
                            unit_font,
                            content.unit_highlight
                                ? Gdiplus::Color(255, 198, 255, 110)
                                : Gdiplus::Color(190, accent.GetR(),
                                                 accent.GetG(), accent.GetB()),
                            content.unit_highlight
                                ? Gdiplus::StringAlignmentFar
                                : Gdiplus::StringAlignmentNear);
                    }
                }
                // The value steps down until it fits rather than being cut off
                // with an ellipsis. A truncated number is a different number,
                // and network throughput has no ceiling to design a width
                // against: measured, 999 · 999 fits at 12.5 and 1023 · 1023
                // needs 10.5.
                // The value is fitted against the WHOLE row. It does not give
                // ground to the footnote: the reading is what the record is
                // for, and the source annotation is a note about it. Measured
                // in a 72 dip cell, `120` at 12.5 and `3840x2160` at 7.5 come
                // to 66 dip against 64 available -- two dip short, which used
                // to cost the FPS number a whole size step. The footnote gives
                // up those two dip instead, below.
                const float value_row_width = cell_width - 8.0F * s;
                const float value_box = value_row_width;
                // Measured at the real font: `0.7 · 0.1` is 51.4 dip and
                // fits at full size, while both directions saturated on a
                // gigabit link -- `119.2 · 118.5` -- is 81.0 and needs 9.5.
                float value_size = 12.5F;
                Gdiplus::RectF value_measured;
                for (; value_size > 8.5F; value_size -= 1.0F) {
                    Gdiplus::Font probe(&family, value_size * s,
                                        Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
                    graphics.MeasureString(content.value.c_str(), -1, &probe,
                                           Gdiplus::PointF(0.0F, 0.0F),
                                           &measure_format, &value_measured);
                    if (value_measured.Width <= value_box) break;
                }
                Gdiplus::Font fitted_value(&family, value_size * s,
                                           Gdiplus::FontStyleBold,
                                           Gdiplus::UnitPixel);
                const Gdiplus::Color value_color =
                    content.unavailable ? Gdiplus::Color(180, 205, 214, 226)
                                        : Gdiplus::Color(255, 245, 248, 252);
                if (content.value_runs.empty()) {
                    DrawText(graphics, content.value,
                        {x + 4.0F * s, y + 14.0F * s, value_box, 22.0F * s},
                        fitted_value, value_color, Gdiplus::StringAlignmentNear);
                } else {
                    DrawRuns(graphics, content.value_runs,
                             {x + 4.0F * s, y + 14.0F * s, value_box, 22.0F * s},
                             fitted_value, Gdiplus::StringAlignmentNear);
                }
                // What the value did not use, rounded down by a small gap so
                // the two never touch. The footnote steps down until it fits
                // and is dropped entirely if it cannot -- an annotation that
                // has run out of room is worth less than a legible reading,
                // and a resolution with its digits cut off is a different
                // resolution.
                float footnote_size = 7.5F;
                bool footnote_fits = false;
                if (!content.footnote.empty()) {
                    const float room =
                        value_row_width - value_measured.Width - 3.0F * s;
                    Gdiplus::RectF footnote_measured;
                    for (; footnote_size >= 5.5F; footnote_size -= 0.5F) {
                        Gdiplus::Font probe(&family, footnote_size * s,
                                            Gdiplus::FontStyleRegular,
                                            Gdiplus::UnitPixel);
                        graphics.MeasureString(content.footnote.c_str(), -1,
                                               &probe, Gdiplus::PointF(0.0F, 0.0F),
                                               &measure_format, &footnote_measured);
                        if (footnote_measured.Width <= room) {
                            footnote_fits = true;
                            break;
                        }
                    }
                }
                if (footnote_fits) {
                    Gdiplus::Font footnote_font(&family, footnote_size * s,
                                                Gdiplus::FontStyleRegular,
                                                Gdiplus::UnitPixel);
                    // Right-aligned on the value row, nudged down towards the
                    // value's baseline.
                    //
                    // The record's accent, because this slot now carries the
                    // backend -- `D12` is context for the reading, not a
                    // reading, and three letters stay legible quiet. The
                    // resolution gets the loud colour, on the label row.
                    DrawText(graphics, content.footnote,
                        {x + 4.0F * s, y + 16.0F * s, value_row_width, 20.0F * s},
                        footnote_font,
                        Gdiplus::Color(content.unavailable ? 120 : 225,
                                       accent.GetR(), accent.GetG(), accent.GetB()),
                        Gdiplus::StringAlignmentFar);
                }
                const Gdiplus::RectF spark_bounds{
                    x + 5.0F * s, y + 37.0F * s, cell_width - 10.0F * s, 10.0F * s};
                switch (content.spark) {
                case Spark::None:
                    break;
                case Spark::Telemetry:
                    DrawCompactSparkline(graphics, samples, start, end,
                        content.member, spark_bounds,
                        Gdiplus::Color(content.unavailable ? 140 : 235,
                                       accent.GetR(), accent.GetG(), accent.GetB()),
                        content.minimum_span, s);
                    break;
                case Spark::Memory:
                    DrawMemorySparkline(graphics, ram_history_->Samples(),
                        start, end, spark_bounds,
                        Gdiplus::Color(235, kAccentRam.GetR(),
                                       kAccentRam.GetG(), kAccentRam.GetB()),
                        Gdiplus::Color(200, kAccentCommit.GetR(),
                                       kAccentCommit.GetG(), kAccentCommit.GetB()),
                        s);
                    break;
                case Spark::Network:
                    DrawNetworkSparkline(graphics, net_history_->Samples(),
                        start, end, spark_bounds,
                        net_history_->PeakWithin(start, end),
                        Gdiplus::Color(235, kAccentNetwork.GetR(),
                                       kAccentNetwork.GetG(), kAccentNetwork.GetB()),
                        Gdiplus::Color(200, kNetworkSent.GetR(),
                                       kNetworkSent.GetG(), kNetworkSent.GetB()),
                        s);
                    break;
                case Spark::Fps:
                    DrawFpsSparkline(graphics, fps_history_->Samples(),
                        start, end, spark_bounds,
                        Gdiplus::Color(235, kAccentFps.GetR(),
                                       kAccentFps.GetG(), kAccentFps.GetB()), s);
                    break;
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
                 kAccentTemperature);
            card(1, std::wstring(localization::Select(L"功率", L"Power")),
                 FormatMetric(optional(&telemetry::Sample::power_w), L" W", 1),
                 kAccentPower);
            card(2, L"VRAM %",
                 FormatMetric(optional(&telemetry::Sample::vram_utilization_percent), L"%"),
                 kAccentVram);
            card(3, L"GPU %",
                 FormatMetric(optional(&telemetry::Sample::gpu_utilization_percent), L"%"),
                 kAccentGpu);
            card(4, L"CPU %",
                 FormatMetric(optional(&telemetry::Sample::cpu_utilization_percent), L"%"),
                 kAccentCpu);
            card(5, L"FPS", fps_value, kAccentFps);
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

        // Lanes follow the same arrangement as the compact cells. Each record
        // asks where it sits rather than being placed by the order of the
        // calls below, so the two views cannot drift apart.
        const auto expanded_placed = CurrentPlacement();
        // The lane count is the placement's, not a sum of the optional flags.
        // It was written out as `5 + ram + fps`, which is a third copy of the
        // rule about which records exist -- it did not know about the eighth,
        // so the window divided its height by seven, every lane came out too
        // tall and the last one fell past the bottom edge. The record was
        // drawn; it was simply drawn off the window.
        const int expanded_lanes = expanded_placed.count;
        const float row_height =
            ((static_cast<float>(layout.height) / s - 31.0F) /
                 static_cast<float>(expanded_lanes) - 2.0F) * s;
        const float row_width = 376.0F * s;
        const float row_x = 6.0F * s;
        const float row_start = 31.0F * s;
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
            kAccentTemperature, 30.0, 105.0,
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
            kAccentPower, 0.0, power_maximum,
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
            kAccentVram, 0.0, 100.0,
            &telemetry::Sample::vram_utilization_percent, label_font, value_font,
            presentation_gaps, false,
            std::nullopt, vram_maximum, stale);
        DrawMetricLane(graphics, samples, start, end,
            lane_row(compact::Metric::Gpu),
            L"GPU %", FormatMetric(optional(&telemetry::Sample::gpu_utilization_percent), L"%"),
            kAccentGpu, 0.0, 100.0,
            &telemetry::Sample::gpu_utilization_percent, label_font, value_font,
            presentation_gaps, false,
            std::nullopt, {}, stale);
        DrawMetricLane(graphics, samples, start, end,
            lane_row(compact::Metric::Cpu),
            L"CPU %", FormatMetric(optional(&telemetry::Sample::cpu_utilization_percent), L"%"),
            kAccentCpu, 0.0, 100.0,
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
                {ram_row.GetRight() - 140.0F * s, ram_row.Y,
                 132.0F * s, 22.0F * ram_scale},
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
        if (net_enabled_) {
            const Gdiplus::RectF net_row = lane_row(compact::Metric::Network);
            const float net_scale = compact::LaneScale(net_row.Height);
            const auto net_label = compact::LaneLabelSpan(net_row.X, net_scale);
            const auto net_graph =
                compact::LaneGraphSpan(net_row.X, net_row.Width, net_scale);
            Gdiplus::SolidBrush background(Gdiplus::Color(12, 255, 255, 255));
            graphics.FillRectangle(&background, net_row);
            const double peak =
                net_history_ == nullptr ? 0.0 : net_history_->PeakWithin(start, end);
            // The same fixed unit as the cell: one number must mean one thing
            // in both views, or switching between them reads as a jump.
            constexpr net::Unit unit = net::kDisplayUnit;
            DrawText(graphics,
                std::wstring(localization::Select(L"網路", L"NET")),
                {net_label.x, net_row.Y, net_label.width, net_row.Height},
                label_font, kAccentNetwork, Gdiplus::StringAlignmentNear);
            const auto* net_latest =
                net_history_ == nullptr ? nullptr : net_history_->Latest();
            const bool readable = net_latest != nullptr &&
                                  net_latest->received_bytes_per_second &&
                                  net_latest->sent_bytes_per_second;
            const int rx = readable
                ? net::TenthsIn(*net_latest->received_bytes_per_second, unit) : 0;
            const int tx = readable
                ? net::TenthsIn(*net_latest->sent_bytes_per_second, unit) : 0;
            // The unit rides with the value here, not on the label: every
            // other lane reads `52 °C`, `110.0 W`, `32% (30.6 GiB)`, and the
            // lane has the width for it. The compact cell puts it on the
            // label only because 64 dip has no room for both.
            //
            // The direction marks are the compact cell's, in the same colour
            // and on the same side of their figure. One number has to mean
            // one thing in both views, and so does one mark: a reader who
            // learns the cell must not have to learn the lane again.
            const Gdiplus::RectF net_value_box{
                net_row.GetRight() - 140.0F * s, net_row.Y,
                132.0F * s, 22.0F * net_scale};
            if (readable) {
                DrawRuns(graphics,
                    {{std::format(L"{}.{}", rx / 10, rx % 10), kValuePrimary},
                     {L"↓ ", kNetworkMark},
                     {std::format(L"{}.{}", tx / 10, tx % 10), kValuePrimary},
                     {L"↑ ", kNetworkMark},
                     {std::wstring(net::UnitSuffix(unit)), kValuePrimary}},
                    net_value_box, value_font, Gdiplus::StringAlignmentFar);
            } else {
                DrawText(graphics, std::wstring(L"-"), net_value_box,
                         value_font, kValuePrimary,
                         Gdiplus::StringAlignmentFar);
            }
            if (net_history_ != nullptr)
                DrawNetworkSparkline(graphics, net_history_->Samples(), start, end,
                    {net_graph.x, net_row.Y + 23.0F * net_scale,
                     net_graph.width, net_row.Height - 30.0F * net_scale},
                    peak,
                    Gdiplus::Color(235, kAccentNetwork.GetR(),
                                   kAccentNetwork.GetG(), kAccentNetwork.GetB()),
                    Gdiplus::Color(215, kNetworkSent.GetR(), kNetworkSent.GetG(),
                                   kNetworkSent.GetB()),
                    net_scale);
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
                label_font, kAccentFps,
                Gdiplus::StringAlignmentNear);
            // The frame source, where the other lanes put their "max" note.
            const std::wstring fps_source = FpsSource(fps_snapshot_);
            float fps_annotation_width = 0.0F;
            if (!fps_source.empty()) {
                Gdiplus::StringFormat measure;
                measure.SetFormatFlags(Gdiplus::StringFormatFlagsNoWrap);
                Gdiplus::RectF measured;
                graphics.MeasureString(fps_source.c_str(), -1, &label_font,
                                       Gdiplus::PointF(0.0F, 0.0F), &measure,
                                       &measured);
                fps_annotation_width = measured.Width;
                DrawText(graphics, fps_source,
                    {fps_graph.x, fps_row.Y, fps_row.Width - 240.0F * fps_scale,
                     22.0F * fps_scale},
                    label_font,
                    Gdiplus::Color(230, kAccentFps.GetR(), kAccentFps.GetG(),
                                   kAccentFps.GetB()),
                    Gdiplus::StringAlignmentNear);
            }
            // The same measured span the other lanes use, rather than the
            // constant that cut "349.5 W (350 W)" before it was fixed there.
            const auto fps_value_span = compact::LaneValueSpan(
                fps_row.X, fps_row.Width, fps_annotation_width, fps_scale);
            DrawText(graphics, fps_value,
                {fps_value_span.x, fps_row.Y, fps_value_span.width,
                 22.0F * fps_scale},
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
