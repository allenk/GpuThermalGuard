// DPI legibility bench for the Main UI history chart.
//
// Two render paths:
//
//   1. PRODUCTION  - the real gtg::tray::HistoryChart, rendered offscreen at a
//                    chosen DPI at the true dialog-unit size of IDC_HISTORY.
//                    This is the authority for what ships today (5 or 6
//                    records).
//   2. MOCK        - a local reproduction of history_chart.cpp's layout
//                    arithmetic, used only to preview record counts and colours
//                    that are not implemented yet (7 records, RAM accent
//                    candidates). Clearly not the product code path.
//
// Also emits magnified crops of the axis-label gutter so low-DPI truncation is
// visible rather than argued about.
//
// Hardware-free: no NVML, no GTG singleton, no protection work, no registry.
//
// Usage: dpi_legibility_probe <output-dir> [dpi]
//   dpi omitted or 96 -> process runs DPI-unaware, chart renders at scale 1.0
//   any other value   -> process runs Per-Monitor V2

#include <windows.h>
#include <objidl.h>
#include <gdiplus.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include "fps/fps_history.hpp"
#include "telemetry/telemetry_history.hpp"
#include "tray/history_chart.hpp"

WTL::CAppModule _Module;

namespace {

// IDC_HISTORY as declared in GpuThermalGuard.rc.
constexpr int kCurrentWidthDlu = 202;
constexpr int kCurrentHeightDlu = 135;
// Candidate enlargement into padding already inside the history group box.
constexpr int kProposedWidthDlu = 206;
constexpr int kProposedHeightDlu = 150;

// history_chart.cpp layout constants. Kept in sync by hand; the production
// render above is what validates them.
constexpr float kMarginDip = 4.0F;
constexpr float kLabelWidthDip = 40.0F;
constexpr float kRightPaddingDip = 6.0F;
constexpr float kFooterDip = 25.0F;
constexpr float kGapDip = 4.0F;
constexpr float kLaneFloorPx = 16.0F;
constexpr float kLabelInsetPx = 6.0F;   // NOT scaled in production code
constexpr float kLabelOriginPx = 2.0F;  // NOT scaled in production code

void Check(const bool good, const char* message) {
    if (!good) throw std::runtime_error(message);
}

const CLSID kPngEncoder{0x557cf406, 0x1a04, 0x11d3,
                        {0x9a, 0x73, 0x00, 0x00, 0xf8, 0x1e, 0xf3, 0x2e}};

struct BaseUnits {
    int x{};
    int y{};
};

// The documented MapDialogRect equivalent for the dialog's own font
// (FONT 9, "Segoe UI"), which GetDialogBaseUnits does not describe.
BaseUnits MeasureDialogBaseUnits(const int dpi) {
    const HDC screen = GetDC(nullptr);
    Check(screen != nullptr, "screen DC for base units");
    const HDC memory = CreateCompatibleDC(screen);
    Check(memory != nullptr, "memory DC for base units");
    const HFONT font = CreateFontW(-MulDiv(9, dpi, 72), 0, 0, 0, FW_NORMAL,
                                   FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                   OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                   CLEARTYPE_QUALITY, DEFAULT_PITCH,
                                   L"Segoe UI");
    Check(font != nullptr, "Segoe UI 9pt");
    const HGDIOBJ previous = SelectObject(memory, font);
    TEXTMETRICW metrics{};
    GetTextMetricsW(memory, &metrics);
    SIZE extent{};
    const wchar_t* alphabet =
        L"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
    GetTextExtentPoint32W(memory, alphabet, 52, &extent);
    const BaseUnits units{static_cast<int>((extent.cx / 26 + 1) / 2),
                          static_cast<int>(metrics.tmHeight)};
    SelectObject(memory, previous);
    DeleteObject(font);
    DeleteDC(memory);
    ReleaseDC(nullptr, screen);
    return units;
}

// GDI+ resolves UnitPoint against the device DPI; the chart declares point
// sizes and never multiplies them by its own scale factor.
constexpr double PointFontEmPixels(const double points, const int dpi) {
    return points * dpi / 72.0;
}

double LaneHeightPx(const int lanes, const double control_height_px,
                    const double scale) {
    const double plots = control_height_px - kMarginDip * 2.0 * scale -
                         kFooterDip * scale - kGapDip * scale * (lanes - 1);
    return plots / lanes;
}

bool SaveBitmap(HBITMAP bitmap, const std::filesystem::path& path) {
    Gdiplus::Bitmap image(bitmap, nullptr);
    return image.Save(path.c_str(), &kPngEncoder) == Gdiplus::Ok;
}

// ---------------------------------------------------------------------------
// Magnified crop, so a 34 px text box can actually be inspected.
// ---------------------------------------------------------------------------

void SaveMagnifiedCrop(const std::filesystem::path& source,
                       const std::filesystem::path& destination,
                       const int x, const int y, const int w, const int h,
                       const int factor) {
    Gdiplus::Bitmap original(source.c_str());
    Check(original.GetLastStatus() == Gdiplus::Ok, "crop source decodable");
    const int cw = std::min(w, static_cast<int>(original.GetWidth()) - x);
    const int ch = std::min(h, static_cast<int>(original.GetHeight()) - y);
    Check(cw > 0 && ch > 0, "crop inside source");
    Gdiplus::Bitmap magnified(cw * factor, ch * factor,
                              PixelFormat32bppARGB);
    {
        Gdiplus::Graphics graphics(&magnified);
        graphics.SetInterpolationMode(Gdiplus::InterpolationModeNearestNeighbor);
        graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
        graphics.SetSmoothingMode(Gdiplus::SmoothingModeNone);
        const Gdiplus::RectF destination_rect(
            0.0F, 0.0F, static_cast<Gdiplus::REAL>(cw * factor),
            static_cast<Gdiplus::REAL>(ch * factor));
        graphics.DrawImage(&original, destination_rect,
                           static_cast<Gdiplus::REAL>(x),
                           static_cast<Gdiplus::REAL>(y),
                           static_cast<Gdiplus::REAL>(cw),
                           static_cast<Gdiplus::REAL>(ch),
                           Gdiplus::UnitPixel);
    }
    Check(magnified.Save(destination.c_str(), &kPngEncoder) == Gdiplus::Ok,
          "magnified crop saved");
}

// ---------------------------------------------------------------------------
// MOCK renderer. Mirrors history_chart.cpp geometry so unimplemented record
// counts and accent colours can be previewed. NOT the product code path.
// ---------------------------------------------------------------------------

// Virtual Commit sits BELOW physical RAM in practice: its denominator is the
// commit limit (physical + page file), which is larger than physical total, so
// commit % is normally the lower of the two. It is therefore drawn as its own
// secondary curve under the primary RAM curve, not as a wash behind it.
// RAM and Virtual Commit are independent measures with different denominators,
// so they are overlaid area series, never stacked: stacking would imply
// RAM = commit + something, which is false.
enum class CommitStyle {
    None,
    LineOnly,       // RAM gradient + RAM stroke + bare secondary stroke
    FillUnder,      // secondary gradient drawn BEFORE the RAM gradient
    FillOver,       // secondary gradient drawn AFTER the RAM gradient
    ThickLineOnly,  // LineOnly with a heavier secondary stroke
};

struct MockLane {
    std::wstring label;
    Gdiplus::Color color;
    CommitStyle commit{CommitStyle::None};
    Gdiplus::Color secondary{Gdiplus::Color(0, 0, 0, 0)};
};

void AddRoundedRectangle(Gdiplus::GraphicsPath& path,
                         const Gdiplus::RectF& rect, const float radius) {
    const float d = radius * 2.0F;
    path.AddArc(rect.X, rect.Y, d, d, 180.0F, 90.0F);
    path.AddArc(rect.GetRight() - d, rect.Y, d, d, 270.0F, 90.0F);
    path.AddArc(rect.GetRight() - d, rect.GetBottom() - d, d, d, 0.0F, 90.0F);
    path.AddArc(rect.X, rect.GetBottom() - d, d, d, 90.0F, 90.0F);
    path.CloseFigure();
}

void DrawMockText(Gdiplus::Graphics& graphics, const std::wstring& text,
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

// series index -> value in [0,1]
double MockValue(const int lane, const int i, const int count) {
    const double t = static_cast<double>(i) / count;
    switch (lane % 7) {
        case 0: return 0.45 + 0.06 * std::sin(t * 19.0);
        case 1: return 0.30 + 0.10 * std::sin(t * 7.0 + 1.0);
        case 2: return 0.22 + 0.03 * std::sin(t * 11.0);
        case 3: return 0.55 + 0.30 * std::sin(t * 23.0 + 2.0);
        case 4: return 0.40 + 0.34 * std::sin(t * 31.0);
        case 5: return 0.62 + 0.08 * std::sin(t * 5.0);        // RAM physical
        default: return 0.50 + 0.26 * std::sin(t * 13.0 + 0.5);  // FPS
    }
}

// Virtual commit: normally BELOW physical because its denominator is the
// commit limit (physical + page file). Slower moving, and allowed to step when
// a system-managed page file grows the limit.
double MockCommitValue(const int i, const int count) {
    const double t = static_cast<double>(i) / count;
    const double step = t > 0.62 ? 0.04 : 0.0;  // a commit-limit growth step
    return 0.41 + 0.04 * std::sin(t * 3.0) - step;
}

void RenderMockChart(const std::filesystem::path& path, const int width,
                     const int height, const float scale,
                     const std::vector<MockLane>& lanes,
                     const float label_inset_px,
                     const float label_width_dip = kLabelWidthDip) {
    const HDC screen = GetDC(nullptr);
    Check(screen != nullptr, "screen DC");
    const HDC memory = CreateCompatibleDC(screen);
    const HBITMAP bitmap = CreateCompatibleBitmap(screen, width, height);
    const HGDIOBJ previous = SelectObject(memory, bitmap);
    RECT area{0, 0, width, height};
    FillRect(memory, &area, static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));

    {
        Gdiplus::Graphics graphics(memory);
        graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
        graphics.SetCompositingMode(Gdiplus::CompositingModeSourceOver);
        graphics.SetTextRenderingHint(Gdiplus::TextRenderingHintClearTypeGridFit);

        const float margin = kMarginDip * scale;
        const float label_width = label_width_dip * scale;
        const float right_padding = kRightPaddingDip * scale;
        const float footer_height = kFooterDip * scale;
        const float gap = kGapDip * scale;
        const int count = static_cast<int>(lanes.size());
        const float plots_height = static_cast<float>(height) - margin * 2.0F -
                                   footer_height - gap * (count - 1);
        const float plot_height =
            std::max(kLaneFloorPx * scale, plots_height / count);
        const float plot_width =
            static_cast<float>(width) - label_width - right_padding;

        Gdiplus::FontFamily family(L"Segoe UI");
        Gdiplus::Font label_font(&family, 7.7F, Gdiplus::FontStyleRegular,
                                 Gdiplus::UnitPoint);
        Gdiplus::Font small_font(&family, 7.0F, Gdiplus::FontStyleRegular,
                                 Gdiplus::UnitPoint);

        const Gdiplus::Color plot_background(255, 249, 250, 252);
        const Gdiplus::Color border_color(255, 205, 211, 219);
        const Gdiplus::Color grid_color(150, 211, 217, 225);
        Gdiplus::SolidBrush plot_brush(plot_background);
        Gdiplus::Pen border_pen(border_color, 1.0F);
        Gdiplus::Pen grid_pen(grid_color, 0.7F);

        const int samples = 220;
        for (int lane = 0; lane < count; ++lane) {
            const Gdiplus::RectF plot{
                label_width, margin + (plot_height + gap) * lane, plot_width,
                plot_height};

            Gdiplus::GraphicsPath rounded;
            AddRoundedRectangle(rounded, plot, 3.5F * scale);
            graphics.FillPath(&plot_brush, &rounded);
            graphics.DrawPath(&border_pen, &rounded);
            for (int i = 1; i < 4; ++i) {
                const float x = plot.X + plot.Width * i / 4.0F;
                graphics.DrawLine(&grid_pen, x, plot.Y + 1.0F, x,
                                  plot.GetBottom() - 1.0F);
            }
            graphics.DrawLine(&grid_pen, plot.X + 1.0F,
                              plot.Y + plot.Height / 2.0F,
                              plot.GetRight() - 1.0F,
                              plot.Y + plot.Height / 2.0F);

            const auto point_at = [&](const int i, const double value) {
                const float x =
                    plot.X + plot.Width * i / static_cast<float>(samples);
                const float y = plot.GetBottom() - 1.0F -
                                static_cast<float>(value) * (plot.Height - 2.0F);
                return Gdiplus::PointF(x, y);
            };

            const Gdiplus::Color color = lanes[lane].color;

            const CommitStyle commit = lanes[lane].commit;
            const Gdiplus::Color second = lanes[lane].secondary;

            std::vector<Gdiplus::PointF> line;
            line.reserve(samples + 1);
            for (int i = 0; i <= samples; ++i)
                line.push_back(point_at(i, MockValue(lane, i, samples)));

            std::vector<Gdiplus::PointF> commit_line;
            if (commit != CommitStyle::None) {
                commit_line.reserve(samples + 1);
                for (int i = 0; i <= samples; ++i)
                    commit_line.push_back(
                        point_at(i, MockCommitValue(i, samples)));
            }

            const auto fill_area = [&](const std::vector<Gdiplus::PointF>& curve,
                                       const Gdiplus::Color hue,
                                       const BYTE top_alpha,
                                       const BYTE bottom_alpha) {
                std::vector<Gdiplus::PointF> area_points(curve);
                area_points.push_back({plot.GetRight(), plot.GetBottom() - 1.0F});
                area_points.push_back({plot.X, plot.GetBottom() - 1.0F});
                Gdiplus::GraphicsPath fill;
                fill.AddPolygon(area_points.data(),
                                static_cast<INT>(area_points.size()));
                Gdiplus::LinearGradientBrush gradient(
                    Gdiplus::RectF(plot.X, plot.Y, plot.Width, plot.Height + 1.0F),
                    Gdiplus::Color(top_alpha, hue.GetR(), hue.GetG(), hue.GetB()),
                    Gdiplus::Color(bottom_alpha, hue.GetR(), hue.GetG(), hue.GetB()),
                    Gdiplus::LinearGradientModeVertical);
                graphics.FillPath(&gradient, &fill);
            };

            if (commit == CommitStyle::FillUnder)
                fill_area(commit_line, second, 70, 8);

            // Primary gradient fill, matching every other record.
            fill_area(line, color, 62, 5);

            if (commit == CommitStyle::FillOver)
                fill_area(commit_line, second, 70, 8);

            // Secondary curve, thinner, in its own hue.
            if (commit != CommitStyle::None) {
                Gdiplus::Pen secondary_pen(
                    Gdiplus::Color(215, second.GetR(), second.GetG(),
                                   second.GetB()),
                    (commit == CommitStyle::ThickLineOnly ? 1.6F : 0.9F) * scale);
                graphics.DrawLines(&secondary_pen, commit_line.data(),
                                   static_cast<INT>(commit_line.size()));
            }

            // Primary stroke last so RAM always reads as the lane's subject.
            Gdiplus::Pen pen(color, 1.0F * scale);
            graphics.DrawLines(&pen, line.data(),
                               static_cast<INT>(line.size()));

            DrawMockText(graphics, lanes[lane].label,
                         {kLabelOriginPx, plot.Y,
                          label_width - label_inset_px, plot.Height},
                         label_font, color, Gdiplus::StringAlignmentNear);
        }

        const Gdiplus::Color muted(255, 128, 133, 142);
        const float footer_y = static_cast<float>(height) - footer_height;
        DrawMockText(graphics, L"5 min ago",
                     {label_width, footer_y, plot_width / 3.0F,
                      footer_height / 2.0F},
                     small_font, muted, Gdiplus::StringAlignmentNear);
        DrawMockText(graphics, L"2.5 min ago",
                     {label_width + plot_width / 3.0F, footer_y,
                      plot_width / 3.0F, footer_height / 2.0F},
                     small_font, muted, Gdiplus::StringAlignmentCenter);
        DrawMockText(graphics, L"Now",
                     {label_width + plot_width * 2.0F / 3.0F, footer_y,
                      plot_width / 3.0F, footer_height / 2.0F},
                     small_font, muted, Gdiplus::StringAlignmentFar);
    }

    SelectObject(memory, previous);
    Check(SaveBitmap(bitmap, path), "mock chart PNG saved");
    DeleteObject(bitmap);
    DeleteDC(memory);
    ReleaseDC(nullptr, screen);
}

// ---------------------------------------------------------------------------

void FillSyntheticHistory(gtg::telemetry::History& thermal,
                          gtg::fps::History& fps) {
    const gtg::fps::Identity game{77, 1234};
    const auto now = GetTickCount64();
    const auto start_ms = now > 300'000 ? now - 300'000 : 0;
    for (unsigned i = 0; i <= 600; ++i) {
        const auto t = start_ms + i * 500;
        thermal.AddSample({t, 55.0 + (i % 17) * 0.9, 120.0 + (i % 23) * 3.0,
                           25.0 + (i % 11) * 0.7, 8.0 + (i % 11) * 0.2,
                           45.0 + (i % 29) * 1.2, 15.0 + (i % 31) * 1.5});
        const auto status = i >= 220 && i < 245 ? gtg::fps::Status::Warmup
                                                : gtg::fps::Status::Ready;
        fps.Record(t, game, {status, 55.0 + i % 25, game});
    }
}

void SaveProductionChart(const HWND chart, const int width, const int height,
                         const std::filesystem::path& path) {
    const HDC screen = GetDC(nullptr);
    const HDC memory = CreateCompatibleDC(screen);
    const HBITMAP bitmap = CreateCompatibleBitmap(screen, width, height);
    const HGDIOBJ previous = SelectObject(memory, bitmap);
    RECT area{0, 0, width, height};
    FillRect(memory, &area, static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));
    SendMessageW(chart, WM_PRINTCLIENT, reinterpret_cast<WPARAM>(memory),
                 PRF_CLIENT);
    SelectObject(memory, previous);
    Check(SaveBitmap(bitmap, path), "production chart PNG saved");
    DeleteObject(bitmap);
    DeleteDC(memory);
    ReleaseDC(nullptr, screen);
}

// Candidate RAM accents. Occupied hues: red 0, orange 33, green 150,
// teal 177, blue 211, purple 272. Only ~60-120 and ~290-350 are free.
struct AccentCandidate {
    const wchar_t* name;
    Gdiplus::Color color;
    int hue;
};

const std::array<AccentCandidate, 3> kAccents{{
    {L"a-magenta", Gdiplus::Color(255, 198, 58, 140), 325},
    {L"b-rose", Gdiplus::Color(255, 214, 69, 96), 347},
    {L"c-olive", Gdiplus::Color(255, 146, 158, 36), 67},
}};

// Secondary hue for Virtual Commit, paired with magenta; the candidates are
// CMYK's C and Y. Pure process yellow is nearly invisible on the light plot
// background, so a darkened variant is included alongside it.
const std::array<AccentCandidate, 3> kSecondaries{{
    {L"cyan", Gdiplus::Color(255, 0, 158, 216), 196},
    {L"yellow", Gdiplus::Color(255, 240, 200, 0), 50},
    {L"yellow-dark", Gdiplus::Color(255, 186, 148, 0), 48},
}};

std::vector<MockLane> BuildLanes(const Gdiplus::Color ram, const bool with_ram,
                                 const bool with_fps, const CommitStyle commit,
                                 const Gdiplus::Color secondary) {
    std::vector<MockLane> lanes{
        {L"Temp", Gdiplus::Color(255, 224, 73, 71), CommitStyle::None, {}},
        {L"Power", Gdiplus::Color(255, 47, 124, 211), CommitStyle::None, {}},
        {L"VRAM", Gdiplus::Color(255, 213, 126, 38), CommitStyle::None, {}},
        {L"GPU %", Gdiplus::Color(255, 36, 153, 95), CommitStyle::None, {}},
        {L"CPU %", Gdiplus::Color(255, 132, 82, 190), CommitStyle::None, {}},
    };
    if (with_ram) lanes.push_back({L"RAM", ram, commit, secondary});
    if (with_fps)
        lanes.push_back({L"FPS", Gdiplus::Color(255, 40, 165, 160),
                         CommitStyle::None, {}});
    return lanes;
}

int RenderAt(const int dpi, const std::filesystem::path& output) {
    const BaseUnits units = MeasureDialogBaseUnits(dpi);
    const float scale = static_cast<float>(dpi) / 96.0F;
    const auto px_x = [&](const int dlu) { return MulDiv(dlu, units.x, 4); };
    const auto px_y = [&](const int dlu) { return MulDiv(dlu, units.y, 8); };

    const int current_w = px_x(kCurrentWidthDlu);
    const int current_h = px_y(kCurrentHeightDlu);
    const int proposed_w = px_x(kProposedWidthDlu);
    const int proposed_h = px_y(kProposedHeightDlu);

    std::cout << "\n=== DPI " << dpi << " (" << dpi * 100 / 96 << "%) ===\n"
              << "base units x=" << units.x << " y=" << units.y << '\n'
              << "current  " << kCurrentWidthDlu << "x" << kCurrentHeightDlu
              << " DLU -> " << current_w << "x" << current_h << " px\n"
              << "proposed " << kProposedWidthDlu << "x" << kProposedHeightDlu
              << " DLU -> " << proposed_w << "x" << proposed_h << " px\n"
              << "font em px: label 7.7pt=" << PointFontEmPixels(7.7, dpi)
              << " annotation 7.0pt=" << PointFontEmPixels(7.0, dpi) << '\n'
              << "axis label text box: " << kLabelWidthDip * scale << " - "
              << kLabelInsetPx << " = "
              << kLabelWidthDip * scale - kLabelInsetPx << " px ("
              << static_cast<int>(100.0F *
                                  (kLabelWidthDip * scale - kLabelInsetPx) /
                                  (kLabelWidthDip * scale))
              << "% of gutter)\n"
              << "lane heights px:\n";
    for (const int lanes : {5, 6, 7})
        std::cout << "  " << lanes << ": current "
                  << LaneHeightPx(lanes, current_h, scale) << ", proposed "
                  << LaneHeightPx(lanes, proposed_h, scale) << '\n';

    const std::wstring tag = std::to_wstring(dpi);

    // ---- production render (what ships today) ----
    const HWND parent =
        CreateWindowExW(0, L"STATIC", L"", WS_OVERLAPPED, 0, 0,
                        proposed_w + 20, proposed_h + 20, nullptr, nullptr,
                        GetModuleHandleW(nullptr), nullptr);
    Check(parent != nullptr, "parent HWND");
    gtg::telemetry::History thermal;
    gtg::fps::History fps;
    gtg::sysmem::History ram;
    FillSyntheticHistory(thermal, fps);
    {
        // Physical above commit, with a commit-limit growth step part way.
        const auto now = GetTickCount64();
        const auto start_ms = now > 300'000 ? now - 300'000 : 0;
        for (unsigned i = 0; i <= 600; ++i) {
            const double t = static_cast<double>(i) / 600.0;
            const auto physical = static_cast<std::uint64_t>(
                (0.60 + 0.03 * std::sin(t * 9.0)) * 128.0 * 1024.0) *
                1024ULL * 1024ULL;
            const std::uint64_t limit =
                (t > 0.62 ? 192ULL : 160ULL) * 1024ULL * 1024ULL * 1024ULL;
            const auto charge = static_cast<std::uint64_t>(
                (0.41 + 0.03 * std::sin(t * 4.0)) * 160.0 * 1024.0) *
                1024ULL * 1024ULL;
            ram.Record(start_ms + i * 500,
                       gtg::sysmem::Compute(128 * 1024ULL * 1024ULL * 1024ULL,
                                            128 * 1024ULL * 1024ULL * 1024ULL -
                                                physical,
                                            limit, limit - charge));
        }
    }

    gtg::tray::HistoryChart chart;
    RECT bounds{0, 0, proposed_w, proposed_h};
    Check(chart.Create(parent, bounds, nullptr, WS_CHILD | WS_VISIBLE) != nullptr,
          "chart HWND");
    chart.SetHistory(&thermal);
    chart.SetThresholds(85, 300, 600.0);

    const auto shoot = [&](const bool with_ram, const bool with_fps,
                           const std::wstring& name) {
        chart.SetRamHistory(with_ram ? &ram : nullptr);
        chart.SetFpsHistory(with_fps ? &fps : nullptr);
        const auto path = output / name;
        SaveProductionChart(chart, proposed_w, proposed_h, path);
        std::wcout << L"  wrote " << name << L'\n';
        return path;
    };
    const auto production =
        shoot(true, true, L"dpi" + tag + L"-PRODUCTION-7-lanes.png");
    (void)shoot(false, false, L"dpi" + tag + L"-PRODUCTION-5-lanes.png");
    (void)shoot(true, false, L"dpi" + tag + L"-PRODUCTION-6-ram.png");
    chart.DestroyWindow();
    DestroyWindow(parent);

    // Magnified gutter crop: is "GPU %" still truncated at this DPI?
    const auto crop = output / (L"dpi" + tag + L"-GUTTER-x4.png");
    SaveMagnifiedCrop(production, crop, 0, 0,
                      static_cast<int>(48.0F * scale), proposed_h, 4);
    std::wcout << L"  wrote " << crop.filename().wstring()
               << L"  <- inspect axis labels here\n";

    // ---- mock renders ----
    // The primary accent is magenta; these renders are what settled it.
    const Gdiplus::Color kRam = kAccents[0].color;

    // Label gutter widths, to settle the 100 % "GPU..." truncation.
    for (const float gutter : {40.0F, 44.0F, 48.0F}) {
        const std::wstring name = L"dpi" + tag + L"-MOCK-GUTTER-" +
                                  std::to_wstring(static_cast<int>(gutter)) +
                                  L"dlu.png";
        RenderMockChart(output / name, proposed_w, proposed_h, scale,
                        BuildLanes(kRam, true, true,
                                   CommitStyle::FillOver,
                                   kSecondaries[0].color),
                        kLabelInsetPx, gutter);
        std::wcout << L"  wrote MOCK gutter " << gutter << L" DLU (text box "
                   << gutter * scale - kLabelInsetPx << L" px)\n";
    }

    // Virtual Commit treatments. Dark yellow was chosen, but the
    // secondary reads as a bare line, because only RAM carries a fill.
    const std::array<std::pair<const wchar_t*, CommitStyle>, 4> vc_styles{{
        {L"1-line", CommitStyle::LineOnly},
        {L"2-thickline", CommitStyle::ThickLineOnly},
        {L"3-fillunder", CommitStyle::FillUnder},
        {L"4-fillover", CommitStyle::FillOver},
    }};
    for (const auto& second : kSecondaries) {
        for (const auto& [suffix, style] : vc_styles) {
            const std::wstring name = L"dpi" + tag + L"-MOCK-VC-" +
                                      second.name + L"-" + suffix + L".png";
            RenderMockChart(output / name, proposed_w, proposed_h, scale,
                            BuildLanes(kRam, true, true, style, second.color),
                            kLabelInsetPx, 44.0F);
            std::wcout << L"  wrote MOCK VC " << second.name << L"/" << suffix
                       << L'\n';
        }
    }

    // Five lanes in the enlarged container, for the both-off case.
    RenderMockChart(output / (L"dpi" + tag + L"-MOCK-5-proposed.png"),
                    proposed_w, proposed_h, scale,
                    BuildLanes(kRam, false, false, CommitStyle::None, {}),
                    kLabelInsetPx, 46.0F);
    std::wcout << L"  wrote MOCK 5-lane variant\n";
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    const int dpi = argc > 2 ? std::atoi(argv[2]) : 96;
    // DPI-unaware reports 96 from GetDeviceCaps, which is what the chart reads
    // through Graphics::GetDpiX. That is the only way to render the true 100 %
    // layout on a machine whose desktop is scaled.
    SetProcessDpiAwarenessContext(
        dpi == 96 ? DPI_AWARENESS_CONTEXT_UNAWARE
                  : DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    Gdiplus::GdiplusStartupInput startup{};
    ULONG_PTR token{};
    if (Gdiplus::GdiplusStartup(&token, &startup, nullptr) != Gdiplus::Ok)
        return 2;
    _Module.Init(nullptr, GetModuleHandleW(nullptr));

    int result = 0;
    try {
        const std::filesystem::path output =
            argc > 1 ? argv[1] : "out/dpi-legibility";
        std::filesystem::create_directories(output);
        result = RenderAt(dpi, output);
        Check(GetModuleHandleW(L"nvml.dll") == nullptr, "no NVML loaded");
        std::cout << "PASS: rendered without NVML\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        result = 1;
    }

    _Module.Term();
    Gdiplus::GdiplusShutdown(token);
    return result;
}
