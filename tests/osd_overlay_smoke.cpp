// Standalone hardware-free HWND integration test. Never initializes NVML/GTG.
#include <windows.h>
#include <objidl.h>
#include <gdiplus.h>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include "net/history.hpp"
#include "tray/osd_overlay.hpp"
#include "logging/logger.hpp"

#include <fstream>
#include "localization/localization.hpp"

WTL::CAppModule _Module;
static std::filesystem::path frame_path;
static bool frame_saved = false;

// Observe the actual production-rendered DIB before composition; no desktop capture.
static BOOL WINAPI SmokeUpdateLayeredWindow(HWND window, HDC screen, POINT* destination,
    SIZE* size, HDC source, POINT* origin, COLORREF key, BLENDFUNCTION* blend, DWORD flags) {
    if (!frame_path.empty()) {
        DIBSECTION dib{};
        if (GetObjectW(GetCurrentObject(source, OBJ_BITMAP), sizeof(dib), &dib) == sizeof(dib)) {
            Gdiplus::Bitmap bitmap(size->cx, size->cy, dib.dsBm.bmWidthBytes,
                PixelFormat32bppPARGB, static_cast<BYTE*>(dib.dsBm.bmBits));
            const CLSID png{0x557cf406, 0x1a04, 0x11d3, {0x9a, 0x73, 0x00, 0x00, 0xf8, 0x1e, 0xf3, 0x2e}};
            frame_saved = bitmap.Save(frame_path.c_str(), &png) == Gdiplus::Ok;
        }
        frame_path.clear();
    }
    return ::UpdateLayeredWindow(window, screen, destination, size, source, origin, key, blend, flags);
}
#define UpdateLayeredWindow SmokeUpdateLayeredWindow
#include "../src/tray/osd_overlay.cpp"
#undef UpdateLayeredWindow

static void Check(bool success, const char* description) {
    if (!success) throw std::runtime_error(description);
}

static HWND InputWindow(HWND overlay) {
#if GTG_OSD_CLICK_THROUGH
    HWND helper = FindWindowW(L"GpuThermalGuard.OsdDragHandle.v1", nullptr);
    Check(helper != nullptr && GetWindow(helper, GW_OWNER) == overlay, "owned drag helper");
    return helper;
#else
    return overlay;
#endif
}

static void ClickToggle(HWND overlay, bool outside = false) {
    HWND input = InputWindow(overlay);
    RECT rect{};
    GetClientRect(input, &rect);
    const int header = MulDiv(25, static_cast<int>(GetDpiForWindow(input)), 96);
    const LPARAM point = MAKELPARAM(rect.right - header / 2, header / 2);
    POINT screen{rect.right - header / 2, header / 2};
    ClientToScreen(input, &screen);
    Check(SendMessageW(input, WM_NCHITTEST, 0, MAKELPARAM(screen.x, screen.y)) == HTCLIENT,
        "toggle excluded from caption drag");
    SendMessageW(input, WM_LBUTTONDOWN, MK_LBUTTON, point);
    SendMessageW(input, WM_LBUTTONUP, 0, outside ? MAKELPARAM(1, header + 10) : point);
}

int main(int argc, char** argv) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    Gdiplus::GdiplusStartupInput startup;
    ULONG_PTR token{};
    if (Gdiplus::GdiplusStartup(&token, &startup, nullptr) != Gdiplus::Ok) return 2;
    _Module.Init(nullptr, GetModuleHandleW(nullptr));
    // Without this the journal silently swallows everything, and a record
    // this test is meant to prove exists would never be written. It owns a
    // deferred writer, so it must be shut down as deliberately as the app
    // does it -- leaving it running past main crashed this test.
    (void)gtg::logging::Initialize(gtg::logging::Role::Tray);
    struct JournalLifetime {
        ~JournalLifetime() { gtg::logging::Shutdown(); }
    } journal_lifetime;
    int result = 0;
    {
        gtg::tray::OsdOverlay osd;
        try {
            const std::filesystem::path output = argc > 1 ? argv[1] : "out/osd-collapse-smoke";
            std::filesystem::create_directories(output);
            gtg::telemetry::History history;
            const auto now = GetTickCount64();
            for (unsigned i = 0; i <= 150; ++i) {
                history.AddSample({now - 30000 + i * 200, 52.0 + i % 5,
                    100.0 + i % 20, 32.0, 30.6, 60.0, 20.0});
            }
            bool repaired{};
            Check(osd.Initialize(nullptr, &history, true, {60, 60}, repaired), "initialize");
            osd.SetStatus(L"Monitoring", gtg::tray::OsdVisual::Armed);
            osd.SetVisible(true);
            auto check_size = [&](bool compact) {
                RECT r{};
                GetWindowRect(osd, &r);
                const int dpi = static_cast<int>(GetDpiForWindow(osd));
                Check(r.right - r.left == MulDiv(gtg::tray::compact::Width(compact), dpi, 96), "width");
                Check(r.bottom - r.top == MulDiv(gtg::tray::compact::Height(compact), dpi, 96), "height");
#if GTG_OSD_CLICK_THROUGH
                RECT helper{};
                GetWindowRect(InputWindow(osd), &helper);
                Check(helper.left == r.left && helper.top == r.top && helper.right == r.right &&
                    helper.bottom - helper.top == MulDiv(25, dpi, 96), "helper bounds");
#endif
                // What this actually has to prove is that *we* never steal
                // focus. Comparing against a window captured at startup also
                // fails whenever anything else on the desktop takes focus,
                // which on a live machine is often: measured at two failures
                // in five runs before any of this existed. A test that red-
                // lights a third of the time teaches people to re-run until
                // green, which is how a real regression gets waved through.
                const HWND active = GetForegroundWindow();
                DWORD active_pid = 0;
                if (active != nullptr) {
                    (void)GetWindowThreadProcessId(active, &active_pid);
                }
                Check(active == nullptr || active_pid != GetCurrentProcessId(),
                      "no window of ours ever takes the foreground");
            };
            check_size(false);
            ClickToggle(osd, true);
            check_size(false);
            RECT expanded_bounds{};
            GetWindowRect(osd, &expanded_bounds);
            ClickToggle(osd);
            check_size(true);
            RECT compact_bounds{};
            GetWindowRect(osd, &compact_bounds);
            Check(compact_bounds.left == expanded_bounds.left && compact_bounds.top == expanded_bounds.top &&
                compact_bounds.right == expanded_bounds.right, "header and toggle position stable");
            osd.SetVisible(false);
            osd.SetVisible(true);
            check_size(true);
            const auto history_count = history.Samples().size();
            const DWORD gdi_before = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
            const DWORD user_before = GetGuiResources(GetCurrentProcess(), GR_USEROBJECTS);
            for (int i = 0; i < 200; ++i) ClickToggle(osd);
            check_size(true);
            Check(history.Samples().size() == history_count, "history untouched");
            Check(GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS) <= gdi_before + 2, "no GDI growth");
            Check(GetGuiResources(GetCurrentProcess(), GR_USEROBJECTS) <= user_before + 2, "no USER growth");
            const auto screenshot = [&](const char* name) {
                history.AddSample({GetTickCount64(), 52.0, 110.0, 32.0, 30.6, 60.0, 20.0});
                frame_saved = false;
                frame_path = output / name;
                SendMessageW(osd, WM_TIMER, 1, 0);
                Check(frame_saved, "rendered frame saved");
            };
            screenshot("dashboard-normal.png");
            osd.SetStatus(L"ALERT · Safe power locked", gtg::tray::OsdVisual::Protected);
            screenshot("dashboard-alert.png");
            osd.SetStatus(L"Fault", gtg::tray::OsdVisual::Fault);
            screenshot("dashboard-fault.png");
            gtg::localization::SetCurrent(gtg::localization::UiLanguage::TraditionalChinese);
            screenshot("dashboard-fault-zh.png");
            ClickToggle(osd);
            check_size(false);
            screenshot("expanded.png");
            // The reading that was cut: a 350 W card idling just under its
            // limit writes the widest pair this dashboard produces. Rendered
            // so the fix is looked at, not only measured.
            {
                osd.SetCurrentPowerLimit(350.0);
                history.AddSample({GetTickCount64(), 88.0, 349.5, 15.0, 14.6, 96.0, 31.0});
                frame_saved = false;
                frame_path = output / "power-lane-at-limit.png";
                SendMessageW(osd, WM_TIMER, 1, 0);
                Check(frame_saved, "power lane frame saved");
                osd.SetCurrentPowerLimit(std::nullopt);
            }
            gtg::fps::History fps_history;
            const gtg::fps::Identity game{77, 1234};
            osd.SetFpsHistory(&fps_history);
            osd.SetFpsEnabled(true);
            const auto check_fps_size = [&](const bool compact,
                                            const bool with_ram = false) {
                RECT rect{};
                GetWindowRect(osd, &rect);
                MONITORINFO monitor_info{sizeof(monitor_info)};
                Check(GetMonitorInfoW(MonitorFromWindow(osd, MONITOR_DEFAULTTONEAREST),
                    &monitor_info) != FALSE, "monitor work area");
                const int dpi = static_cast<int>(GetDpiForWindow(osd));
                const auto footprint = gtg::tray::compact::ChooseFootprint(
                    compact, with_ram, true,
                    MulDiv(monitor_info.rcWork.right - monitor_info.rcWork.left, 96, dpi),
                    MulDiv(monitor_info.rcWork.bottom - monitor_info.rcWork.top, 96, dpi));
                Check(rect.right - rect.left == MulDiv(footprint.width, dpi, 96) &&
                    rect.bottom - rect.top == MulDiv(footprint.height, dpi, 96),
                    "FPS OSD fits its DPI-aware footprint");
            };
            check_fps_size(false);
            screenshot("fps-expanded-no-data.png");
            ClickToggle(osd);
            check_fps_size(true);
            screenshot("fps-compact-no-data.png");

            // RAM joins as the second-from-last record. The default
            // arrangement is one row, so the overlay gains exactly one cell
            // stride of width and no height.
            // Same 200 ms grid as the thermal telemetry above, because RAM is
            // sampled on the same tick and must span the same window.
            gtg::sysmem::History ram_history;
            {
                const auto ram_now = GetTickCount64();
                for (unsigned i = 0; i <= 150; ++i)
                    ram_history.Record(ram_now - 30'000 + i * 200,
                        gtg::sysmem::Compute(
                            128ULL << 30, (50ULL << 30) - (i << 20),
                            160ULL << 30, (94ULL << 30) - (i << 20)));
            }
            RECT before_ram{};
            GetWindowRect(osd, &before_ram);
            osd.SetRamHistory(&ram_history);
            osd.SetRamEnabled(true);
            check_fps_size(true, true);
            RECT after_ram{};
            GetWindowRect(osd, &after_ram);
            const int ram_dpi = static_cast<int>(GetDpiForWindow(osd));
            Check(after_ram.right - after_ram.left ==
                      before_ram.right - before_ram.left +
                          MulDiv(gtg::tray::compact::kCellStrideDip, ram_dpi, 96),
                  "enabling RAM widens the default row by one cell");
            Check(after_ram.bottom - after_ram.top ==
                      before_ram.bottom - before_ram.top,
                  "the seventh cell costs no extra height");
            screenshot("ram-compact.png");
            ClickToggle(osd);
            check_fps_size(false, true);
            screenshot("ram-expanded.png");

            // Regression: a RAM history shorter than the visible window
            // must still start where its data starts,
            // and must never be shifted right relative to the five telemetry
            // records that do span the window.
            gtg::sysmem::History partial;
            {
                const auto ram_now = GetTickCount64();
                for (unsigned i = 0; i < 40; ++i)
                    partial.Record(ram_now - 20'000 + i * 500,
                        gtg::sysmem::Compute(
                            128ULL << 30, (50ULL << 30) - (i << 20),
                            160ULL << 30, (94ULL << 30) - (i << 20)));
            }
            osd.SetRamHistory(&partial);
            osd.RequestRefresh();
            screenshot("ram-expanded-partial-history.png");
            osd.SetRamHistory(&ram_history);

            // Every expanded record must plot from the same left edge. The
            // geometry used to be duplicated per call site and drifted; this
            // measures the rendered pixels rather than trusting the constants.
            osd.RequestRefresh();
            screenshot("ram-expanded-aligned.png");
            {
                Gdiplus::Bitmap frame((output / "ram-expanded-aligned.png").c_str());
                Check(frame.GetLastStatus() == Gdiplus::Ok, "alignment frame decodable");
                // The axis label is painted in the record's own colour and a
                // wide glyph such as the "%" of "CPU %" can reach the label
                // box edge, so the scan starts at the plot area instead. No
                // curve can legitimately begin left of that.
                const float bitmap_scale =
                    static_cast<float>(frame.GetWidth()) / 388.0F;
                // Lane internals scale with the ROW HEIGHT, not the window DPI.
                // An earlier version of this scan used the DPI scale, put its
                // start to the right of where the telemetry curves actually
                // begin, and reported a false pass while the lanes were
                // visibly misaligned. Derive it the way the renderer does.
                namespace ct = gtg::tray::compact;
                const float height_dip =
                    static_cast<float>(frame.GetHeight()) / bitmap_scale;
                const float lanes =
                    static_cast<float>(ct::CellCount(true, true));
                const float row_height_dip =
                    (height_dip - 31.0F) / lanes - 2.0F;
                const float lane_scale = ct::LaneScale(row_height_dip);
                const UINT scan_from = static_cast<UINT>(
                    (ct::kLaneRowLeftDip +
                     ct::kLaneGraphLeftDip * lane_scale) * bitmap_scale);
                const auto leftmost = [&](const BYTE r, const BYTE g, const BYTE b) {
                    int best = INT_MAX;
                    for (UINT y = 0; y < frame.GetHeight(); ++y) {
                        for (UINT x = scan_from; x < frame.GetWidth(); ++x) {
                            Gdiplus::Color pixel;
                            if (frame.GetPixel(static_cast<INT>(x),
                                               static_cast<INT>(y), &pixel) != Gdiplus::Ok)
                                continue;
                            if (std::abs(static_cast<int>(pixel.GetR()) - r) < 34 &&
                                std::abs(static_cast<int>(pixel.GetG()) - g) < 34 &&
                                std::abs(static_cast<int>(pixel.GetB()) - b) < 34) {
                                best = std::min(best, static_cast<int>(x));
                                break;
                            }
                        }
                    }
                    return best;
                };
                const int ram_left = leftmost(255, 122, 196);
                const int cpu_left = leftmost(183, 121, 255);
                Check(ram_left != INT_MAX && cpu_left != INT_MAX,
                      "the RAM and CPU curves are present");
                std::cerr << "cpu_left=" << cpu_left
                          << " ram_left=" << ram_left << '\n';
                // The whole defect is this comparison: RAM must not begin
                // further right than a telemetry record sampled on the same
                // tick into the same window.
                Check(ram_left - cpu_left <= 3,
                      "RAM begins level with the telemetry records");
                const int graph_left = static_cast<int>(scan_from);
                // RAM used to start well right of the telemetry records.
                // Both must reach the plot's left edge, allowing one
                // 200 ms sampling interval of the 30 s window plus antialiasing.
                const int tolerance =
                    static_cast<int>(3.0F + 3.0F * bitmap_scale);
                Check(cpu_left - graph_left <= tolerance,
                      "a telemetry record reaches the left edge of its plot");
                Check(ram_left - graph_left <= tolerance,
                      "RAM reaches the left edge of its plot area");
            }
            ClickToggle(osd);

            // --- arranging the compact dashboard ---------------------------
            // The toggle above left the dashboard collapsed, which is the only
            // mode that offers the lock.
            // Go through WM_NCHITTEST exactly as Windows does. Sending button
            // messages straight to the window bypasses the caption band, which
            // is what let a control that could never be clicked pass its test.
            const auto press_release = [&](const POINT from, const POINT to) {
                POINT screen_from{from.x, from.y};
                ClientToScreen(osd, &screen_from);
                Check(SendMessageW(osd, WM_NCHITTEST, 0,
                                   MAKELPARAM(screen_from.x, screen_from.y)) == HTCLIENT,
                      "a press target must not be claimed by the caption band");
                SendMessageW(osd, WM_LBUTTONDOWN, MK_LBUTTON,
                             MAKELPARAM(from.x, from.y));
                SendMessageW(osd, WM_MOUSEMOVE, MK_LBUTTON,
                             MAKELPARAM(to.x, to.y));
                SendMessageW(osd, WM_LBUTTONUP, 0, MAKELPARAM(to.x, to.y));
            };
            const auto cell_centre = [&](const int slot) {
                const int dpi = static_cast<int>(GetDpiForWindow(osd));
                const float scale = static_cast<float>(dpi) / 96.0F;
                const auto placed = gtg::tray::compact::Resolve(
                    osd.compact_layout(), true, true);
                RECT r{};
                GetWindowRect(osd, &r);
                const auto grid = gtg::tray::compact::MakeCellGrid(
                    gtg::tray::compact::WidestRow(placed), r.right - r.left, scale);
                const auto origin =
                    gtg::tray::compact::CellOriginAt(grid, placed, slot);
                return POINT{
                    static_cast<LONG>(origin.x + grid.cell_width_dip * scale / 2),
                    static_cast<LONG>(origin.y +
                                      gtg::tray::compact::kCellHeightDip * scale / 2)};
            };

            const auto before = osd.compact_layout();
            Check(osd.compact_locked(), "the dashboard starts locked");
            press_release(cell_centre(0), cell_centre(3));
            Check(osd.compact_layout().order == before.order,
                  "a locked dashboard refuses to be rearranged");

            // Unlock through the header control, then rearrange.
            RECT bounds{};
            GetWindowRect(osd, &bounds);
            const int header = MulDiv(25, static_cast<int>(GetDpiForWindow(osd)), 96);
            const POINT lock_point{bounds.right - bounds.left - header * 3 / 2,
                                   header / 2};
            press_release(lock_point, lock_point);
            Check(!osd.compact_locked(), "the lock control unlocks the cells");

            RECT before_move{};
            GetWindowRect(osd, &before_move);
            press_release(cell_centre(0), cell_centre(3));
            const auto rearranged = osd.compact_layout();
            Check(rearranged.order != before.order,
                  "an unlocked dashboard rearranges on release");
            Check(gtg::tray::compact::IsValidLayout(rearranged),
                  "rearranging leaves every record present exactly once");
            RECT after_move{};
            GetWindowRect(osd, &after_move);
            Check(after_move.left == before_move.left &&
                      after_move.top == before_move.top,
                  "arranging a cell never moves the overlay itself");
            screenshot("arranged.png");

            // An abandoned drag changes nothing.
            const auto settled = osd.compact_layout();
            SendMessageW(osd, WM_LBUTTONDOWN, MK_LBUTTON,
                         MAKELPARAM(cell_centre(0).x, cell_centre(0).y));
            SendMessageW(osd, WM_CANCELMODE, 0, 0);
            Check(osd.compact_layout().order == settled.order,
                  "a cancelled drag leaves the arrangement untouched");
            Check(FindWindowW(L"GpuThermalGuard.OsdDragImage.v1", nullptr) == nullptr,
                  "a cancelled drag destroys the drag image");

            osd.SetCompactLayout(gtg::tray::compact::Layout{});
            osd.SetCompactLocked(true);
            osd.RequestRefresh();
            screenshot("locked.png");

            // Windows' low-memory signal raises the RAM cell's own accent
            // rather than recolouring it, so the record stays recognisable.
            osd.SetHostMemoryLow(true);
            screenshot("ram-low-memory.png");
            osd.SetHostMemoryLow(false);
            osd.RequestRefresh();

            // Every shape the new model allows, so a change to the geometry
            // moves a picture rather than only a number.
            {
                struct Shape { std::initializer_list<int> rows; const char* name; };
                const Shape shapes[] = {
                    {{7},          "rows-1x7.png"},
                    {{5, 2},       "rows-5-2.png"},
                    {{4, 3},       "rows-4-3.png"},
                    {{3, 2, 2},    "rows-3-2-2.png"},
                    {{2, 2, 2, 1}, "rows-2-2-2-1.png"},
                    {{5, 1, 1},    "rows-5-1-1.png"},
                    {{2, 1, 1, 1, 1, 1}, "rows-2-1x5.png"},
                    // The single column: the shape the old floor forbade, and
                    // the one the owner wants for docking against an edge.
                    {{1, 1, 1, 1, 1, 1, 1}, "rows-7x1.png"},
                };
                for (const auto& shape : shapes) {
                    std::array<int, gtg::tray::compact::kRecordCount> lengths{};
                    int n = 0;
                    for (const int length : shape.rows)
                        lengths[static_cast<std::size_t>(n++)] = length;
                    gtg::tray::compact::Layout arranged;
                    arranged.breaks = gtg::tray::compact::BreaksFromRows(lengths, n);
                    osd.SetCompactLayout(arranged);
                    osd.RequestRefresh();
                    screenshot(shape.name);
                }
                osd.SetCompactLayout(gtg::tray::compact::Layout{});
                osd.RequestRefresh();
            }

            // --- host memory reclaim: the busy indicator's lifecycle -------
            //
            // Dry run throughout: this exercises the window, not the trim, so
            // running the test never modifies the machine it runs on.
            {
                gtg::sysmem::reclaim::Policy dry;
                dry.dry_run = true;
                osd.SetReclaimPolicy(dry);
                osd.SetRamEnabled(true);
                osd.SetRamHistory(&ram_history);
                osd.SetCompactLayout(gtg::tray::compact::Layout{});
                osd.SetCompactLocked(true);
                osd.RequestRefresh();
                SendMessageW(osd, WM_TIMER, 1, 0);

                const auto find_indicator = [&]() {
                    return FindWindowExW(nullptr, nullptr,
                                         L"GpuThermalGuard.OsdBusyIndicator.v1",
                                         nullptr);
                };
                Check(find_indicator() == nullptr, "no indicator before activation");

                // Locate the RAM cell the same way the production code does.
                RECT osd_rect{};
                GetWindowRect(osd, &osd_rect);
                const int dpi = static_cast<int>(GetDpiForWindow(osd));
                const float scale = static_cast<float>(dpi) / 96.0F;
                const auto placed = gtg::tray::compact::Resolve(
                    gtg::tray::compact::Layout{}, true, true);
                const auto grid = gtg::tray::compact::MakeCellGrid(
                    gtg::tray::compact::WidestRow(placed),
                    osd_rect.right - osd_rect.left, scale);
                int ram_slot = -1;
                for (int i = 0; i < placed.count; ++i) {
                    if (placed.cells[static_cast<std::size_t>(i)] ==
                        gtg::tray::compact::Metric::Ram) {
                        ram_slot = i;
                        break;
                    }
                }
                Check(ram_slot >= 0, "the RAM cell is placed");
                const auto ram_origin =
                    gtg::tray::compact::CellOriginAt(grid, placed, ram_slot);
                const LPARAM ram_point = MAKELPARAM(
                    static_cast<int>(ram_origin.x) +
                        static_cast<int>(grid.cell_width_dip * scale) / 2,
                    static_cast<int>(ram_origin.y) +
                        static_cast<int>(gtg::tray::compact::kCellHeightDip * scale) / 2);

                // Unlocked is arrange mode; activation must not fire there, or
                // a drag and an action would compete for one gesture.
                osd.SetCompactLocked(false);
                SendMessageW(osd, WM_LBUTTONDBLCLK, 0, ram_point);
                Check(find_indicator() == nullptr,
                      "unlocked double-click starts no reclaim");

                osd.SetCompactLocked(true);
                SendMessageW(osd, WM_LBUTTONDBLCLK, 0, ram_point);
                const HWND indicator = find_indicator();
                Check(indicator != nullptr, "locked double-click raises the indicator");
                Check(GetWindow(indicator, GW_OWNER) == osd,
                      "the indicator is owned by the overlay");
                Check((GetWindowLongPtrW(indicator, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0,
                      "the indicator is topmost");
                // It must NOT be transparent to the mouse: swallowing further
                // clicks over the busy cell is half its purpose.
                Check((GetWindowLongPtrW(indicator, GWL_EXSTYLE) & WS_EX_TRANSPARENT) == 0,
                      "the indicator is not click-through");
                Check(SendMessageW(indicator, WM_NCHITTEST, 0, 0) == HTCLIENT,
                      "the indicator swallows the pointer");

                // A second activation while one is in flight must do nothing:
                // the window's existence is the guard.
                SendMessageW(osd, WM_LBUTTONDBLCLK, 0, ram_point);
                Check(find_indicator() == indicator,
                      "a second double-click does not start a second reclaim");

                // The floor keeps it up even though a dry run finishes at once.
                Sleep(120);
                SendMessageW(osd, WM_TIMER, 3, 0);
                Check(find_indicator() != nullptr,
                      "the indicator honours its minimum visible interval");

                screenshot("ram-reclaim-busy.png");

                // Past the floor it comes down on the next poll.
                Sleep(gtg::sysmem::reclaim::kMinimumVisibleMs);
                SendMessageW(osd, WM_TIMER, 3, 0);
                Check(find_indicator() == nullptr,
                      "the indicator is destroyed once the floor has elapsed");

                // Every abandonment path must take it down too: a topmost
                // window that swallows input is the worst thing to leak.
                SendMessageW(osd, WM_LBUTTONDBLCLK, 0, ram_point);
                Check(find_indicator() != nullptr, "raised again");
                // Through the real chevron, not a private method: the
                // production path is the one that has to tear it down.
                ClickToggle(osd);
                Check(find_indicator() == nullptr,
                      "switching to the expanded view tears the indicator down");
                ClickToggle(osd);

                SendMessageW(osd, WM_LBUTTONDBLCLK, 0, ram_point);
                Check(find_indicator() != nullptr, "raised again");
                osd.SetVisible(false);
                Check(find_indicator() == nullptr,
                      "hiding the overlay tears the indicator down");
                osd.SetVisible(true);

                // The result phase directly. A dry run never reaches it, so
                // without this the most dangerous teardown path -- the one
                // that would leave a topmost, input-swallowing window on the
                // reader's screen -- would ship untested.
                gtg::tray::OsdBusyIndicator standalone;
                const RECT probe{osd_rect.left + 20, osd_rect.top + 40,
                                 osd_rect.left + 120, osd_rect.top + 90};
                Check(standalone.Begin(osd, probe, 0xFFFF7AC4U, scale, L"RAM"),
                      "the indicator can be raised");
                Check(standalone.Active() && find_indicator() != nullptr,
                      "it is a real window and says so");
                standalone.ShowResult(L"+4.3 GB", 0xFF4BD68BU);
                Check(standalone.Active(), "showing a result keeps it up");
                // The widest figure a very large machine can produce. It must
                // render whole: truncating a number turns it into a different
                // number, which is worse than showing it small.
                standalone.ShowResult(L"+128 GB", 0xFF4BD68BU);
                Check(standalone.Active(), "a three-digit figure still renders");
                standalone.ShowResult(L"-128 GB", 0xFFFF5B5EU);
                Check(standalone.Active(), "so does a three-digit loss");
                Check(SendMessageW(find_indicator(), WM_NCHITTEST, 0, 0) == HTCLIENT,
                      "it still swallows the pointer while showing a result");
                standalone.End();
                Check(!standalone.Active() && find_indicator() == nullptr,
                      "nothing survives the result phase");

                // The network record, drawn with a history shaped like real
                // traffic: an asymmetric link, a burst, and a hole where the
                // adapter stopped reporting. A picture is the only way to
                // judge whether two curves on one span read at 10 dip tall.
                {
                    // Hundreds of megabytes, which is where the cell first
                    // ran out of room: the owner saw a real download push the
                    // value past the box and end in an ellipsis.
                    const auto fill = [](gtg::net::History& into,
                                         const double received_mb,
                                         const double sent_mb,
                                         const bool with_gap,
                                         const bool sustained = false) {
                        const std::uint64_t base = GetTickCount64() - 80'000;
                        std::uint64_t received = 0;
                        std::uint64_t sent = 0;
                        for (int step = 0; step < 400; ++step) {
                            const std::uint64_t at =
                                base + static_cast<std::uint64_t>(step) * 200;
                            // Sustained means the last sample is the full
                            // rate, which is the one the cell prints. An
                            // earlier draft put the burst in the middle, so
                            // the widest case never reached the value row and
                            // the test proved nothing about the font.
                            const double burst =
                                sustained ? 1.0
                                          : (step > 180 && step < 260 ? 1.0 : 0.35);
                            received += static_cast<std::uint64_t>(
                                received_mb * 1024.0 * 1024.0 * 0.2 * burst);
                            sent += static_cast<std::uint64_t>(
                                sent_mb * 1024.0 * 1024.0 * 0.2 * burst);
                            if (with_gap && step == 300) {
                                // The adapter goes away: the next difference
                                // would span two interfaces, so the curve must
                                // break rather than join across the hole.
                                into.Interrupt();
                                continue;
                            }
                            into.Record(at, {received, sent, at});
                        }
                    };

                    // An asymmetric link under a large download, which is the
                    // ordinary shape of the thing.
                    gtg::net::History traffic;
                    fill(traffic, 850.0, 90.0, true);
                    // Added on top of whatever is already shown rather than
                    // replacing it: every later block in this test depends on
                    // the records it set up, and an earlier draft of this one
                    // switched RAM off and never switched it back.
                    osd.SetNetHistory(&traffic);
                    osd.SetNetEnabled(true);
                    screenshot("net-cell.png");
                    // The expanded lane too. It was missing entirely from the
                    // first pass -- the cell worked and the lane did not, and
                    // nothing caught it because nothing rendered it.
                    ClickToggle(osd);
                    screenshot("net-lane.png");
                    ClickToggle(osd);

                    // The widest pair the unit ladder can produce: both
                    // directions just below the next unit, so the value is
                    // four digits either side of the separator. Measured at
                    // 74 dip against a 64 dip row, so this is the frame that
                    // proves the font steps down instead of truncating.
                    gtg::net::History saturated;
                    // Ten gigabit, saturated both ways: the widest pair the
                    // link-speed guard will let through on hardware that
                    // exists, and the frame that proves the row neither
                    // truncates nor shrinks past legibility.
                    fill(saturated, 1192.0, 1190.0, false, true);
                    osd.SetNetHistory(&saturated);
                    screenshot("net-cell-wide.png");

                    osd.SetNetEnabled(false);
                    osd.SetNetHistory(nullptr);
                }

                // A filmstrip of the animation itself, at 96 dip metrics and
                // through the real paint path. The frames are spaced by wall
                // clock rather than by frame count, because the effect is a
                // function of elapsed time and not of how often it was drawn.
                {
                    const RECT at{osd_rect.left + 40, osd_rect.top + 60,
                                  osd_rect.left + 40 + 72, osd_rect.top + 60 + 51};
                    gtg::tray::OsdBusyIndicator film;
                    Check(film.Begin(osd, at, 0xFFFF7AC4U, 1.0F, L"RAM",
                                     gtg::tray::animation::Effect::Rain),
                          "filmstrip indicator raised");
                    for (int frame = 0; frame < 24; ++frame) {
                        Sleep(80);
                        // Its animation runs on a timer, so the frames only
                        // advance if this thread pumps for it.
                        MSG message{};
                        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                            TranslateMessage(&message);
                            DispatchMessageW(&message);
                        }
                        const HDC desk = ::GetDC(nullptr);
                        const HDC mem = CreateCompatibleDC(desk);
                        const HBITMAP bits = CreateCompatibleBitmap(desk, 72, 51);
                        const HGDIOBJ old = SelectObject(mem, bits);
                        BitBlt(mem, 0, 0, 72, 51, desk, at.left, at.top,
                               SRCCOPY | CAPTUREBLT);
                        {
                            Gdiplus::Bitmap captured(bits, nullptr);
                            const CLSID png{0x557cf406, 0x1a04, 0x11d3,
                                            {0x9a, 0x73, 0x00, 0x00, 0xf8, 0x1e, 0xf3, 0x2e}};
                            wchar_t name[32]{};
                            swprintf_s(name, L"rain-%02d.png", frame);
                            const auto path = output / name;
                            Check(captured.Save(path.c_str(), &png) == Gdiplus::Ok,
                                  "filmstrip frame saved");
                        }
                        SelectObject(mem, old);
                        DeleteObject(bits);
                        DeleteDC(mem);
                        ::ReleaseDC(nullptr, desk);
                    }
                    film.End();
                }

                // What the figure looks like at 96 DPI, through the real paint
                // path rather than a replica of it: Begin() takes the scale, so
                // forcing 1.0 renders 100 % metrics whatever this desktop is
                // set to. Captured off the screen because the indicator is its
                // own layered window and the overlay's screenshot() cannot see
                // it.
                // The reading that was cut, at the scale it was actually cut
                // at. A lane's horizontal metrics are multiplied by
                // LaneScale(row.Height) -- a VERTICAL ratio, which shrinks as
                // records are added, because the lanes divide a fixed window
                // height between them. Eight lanes put it at 0.59, so the
                // value's old 132 dip box was really 78 dip, and
                // "349.5 W (350 W)" does not fit in 78 dip. Adding the NET
                // record is what pushed it over: at seven lanes the same box
                // was 89 dip.
                //
                // Measured with GDI+ rather than reasoned about, and derived
                // from the same constants the painter uses, so this keeps
                // holding when a ninth record arrives.
                {
                    namespace compact = gtg::tray::compact;
                    const HDC probe_dc = ::GetDC(nullptr);
                    Gdiplus::Graphics ruler(probe_dc);
                    Gdiplus::FontFamily family(L"Segoe UI");
                    Gdiplus::StringFormat measure;
                    measure.SetFormatFlags(Gdiplus::StringFormatFlagsNoWrap);
                    // The widest ordinary readings this dashboard produces:
                    // the power pair that failed, and the two behind it.
                    const wchar_t* readings[] = {
                        L"349.5 W (350 W)",
                        L"1234.5 W (1250 W)",
                        L"100% (1024.0 GiB)",
                    };
                    // Every record enabled is the worst case, and the one the
                    // owner was looking at.
                    const int lanes = compact::Resolve(compact::Layout{}, true,
                                                       true, true).count;
                    for (const float lane_dpi : {1.0F, 1.25F, 1.5F, 2.0F, 2.5F}) {
                        // The painter's own arithmetic: the expanded window
                        // is 330 dip tall, 31 of it header, and the lanes
                        // divide the rest with a 2 dip gap each.
                        const float row_height =
                            ((330.0F - 31.0F) /
                                 static_cast<float>(lanes) - 2.0F) * lane_dpi;
                        const float lane_scale = compact::LaneScale(row_height);
                        const float row_width = 376.0F * lane_dpi;
                        Gdiplus::Font value_font(&family, 12.5F * lane_dpi,
                                                 Gdiplus::FontStyleBold,
                                                 Gdiplus::UnitPixel);
                        const auto span = compact::LaneValueSpan(
                            0.0F, row_width, 0.0F, lane_scale);
                        for (const wchar_t* reading : readings) {
                            Gdiplus::RectF measured;
                            ruler.MeasureString(reading, -1, &value_font,
                                                Gdiplus::PointF(0.0F, 0.0F),
                                                &measure, &measured);
                            Check(measured.Width <= span.width,
                                  "a lane reading fits its box without being cut");
                        }
                    }
                    ::ReleaseDC(nullptr, probe_dc);
                }

                // The same gesture on the NET cell. The probe is stubbed:
                // exercising it for real would mean enabling extended
                // statistics on whatever program happens to be in front of
                // this test, on this machine. The probe is verified against a
                // real process by hand, elevated; what is verified here is
                // that the gesture reaches it and that the window always comes
                // down again.
                {
                    namespace compact = gtg::tray::compact;
                    osd.SetNetEnabled(true);
                    osd.SetCompactLocked(true);
                    // Enabling a record resizes the dashboard, so every
                    // coordinate above this line is stale. Recomputed rather
                    // than reused -- the cell that moved is the one being
                    // clicked.
                    RECT net_rect{};
                    GetWindowRect(osd, &net_rect);
                    const auto net_placed = osd.CurrentPlacement();
                    const auto net_grid = compact::MakeCellGrid(
                        compact::WidestRow(net_placed),
                        net_rect.right - net_rect.left, scale);
                    const auto point_at = [&](const compact::Metric metric) {
                        int slot = -1;
                        for (int i = 0; i < net_placed.count; ++i) {
                            if (net_placed.cells[static_cast<std::size_t>(i)] == metric) {
                                slot = i;
                                break;
                            }
                        }
                        Check(slot >= 0, "the cell is placed");
                        const auto origin =
                            compact::CellOriginAt(net_grid, net_placed, slot);
                        return MAKELPARAM(
                            static_cast<int>(origin.x) +
                                static_cast<int>(net_grid.cell_width_dip * scale) / 2,
                            static_cast<int>(origin.y) +
                                static_cast<int>(compact::kCellHeightDip * scale) / 2);
                    };
                    const LPARAM net_point = point_at(compact::Metric::Network);
                    const LPARAM ram_again = point_at(compact::Metric::Ram);

                    osd.StubNetworkProbe({gtg::net::Grade::Fair, 90, 5},
                                         gtg::net::ProbeStatus::Ready);

                    osd.SetCompactLocked(false);
                    SendMessageW(osd, WM_LBUTTONDBLCLK, 0, net_point);
                    Check(find_indicator() == nullptr,
                          "unlocked double-click starts no probe either");

                    osd.SetCompactLocked(true);
                    SendMessageW(osd, WM_LBUTTONDBLCLK, 0, net_point);
                    const HWND probing = find_indicator();
                    Check(probing != nullptr,
                          "locked double-click on NET raises the indicator");
                    SendMessageW(osd, WM_LBUTTONDBLCLK, 0, net_point);
                    Check(find_indicator() == probing,
                          "a second double-click does not start a second probe");
                    // Two actions, one indicator: the other record's gesture
                    // must not raise a second window over the dashboard.
                    SendMessageW(osd, WM_LBUTTONDBLCLK, 0, ram_again);
                    Check(find_indicator() == probing,
                          "a RAM activation during a probe raises nothing new");

                    // The result, through the real timer. A stub finishes at
                    // once, so the floor is what holds the rain up.
                    Sleep(gtg::net::action::kMinimumVisibleMs + 60);
                    SendMessageW(osd, WM_TIMER, 3, 0);
                    Check(find_indicator() != nullptr,
                          "a network verdict is always shown, never skipped");
                    screenshot("net-verdict-busy.png");
                    SendMessageW(osd, WM_TIMER, 3, 0);
                    Check(find_indicator() != nullptr,
                          "and it holds while the reader reads it");
                    Sleep(gtg::net::action::kResultHoldMs);
                    SendMessageW(osd, WM_TIMER, 3, 0);
                    Check(find_indicator() == nullptr,
                          "then the cell goes back to being a cell");

                    // Every abandonment path, as for the reclaim: a topmost
                    // window that swallows input is the worst thing to leak.
                    SendMessageW(osd, WM_LBUTTONDBLCLK, 0, net_point);
                    Check(find_indicator() != nullptr, "raised again");
                    osd.SetVisible(false);
                    Check(find_indicator() == nullptr,
                          "hiding the overlay tears a probe down too");
                    osd.SetVisible(true);
                    osd.SetNetEnabled(false);
                }

                // Captured off the screen: the indicator is its own layered
                // window and the overlay's screenshot() cannot see it.
                const auto capture_cell = [&](const RECT& at, const char* name) {
                    const HWND live = find_indicator();
                    Check(live != nullptr, "sample is on screen");
                    const int w = at.right - at.left;
                    const int h = at.bottom - at.top;
                    const HDC desk = ::GetDC(nullptr);
                    const HDC mem = CreateCompatibleDC(desk);
                    const HBITMAP shot_bitmap = CreateCompatibleBitmap(desk, w, h);
                    const HGDIOBJ old = SelectObject(mem, shot_bitmap);
                    // CAPTUREBLT, or a layered window is simply absent.
                    BitBlt(mem, 0, 0, w, h, desk, at.left, at.top,
                           SRCCOPY | CAPTUREBLT);
                    {
                        Gdiplus::Bitmap captured(shot_bitmap, nullptr);
                        const CLSID png{0x557cf406, 0x1a04, 0x11d3,
                                        {0x9a, 0x73, 0x00, 0x00, 0xf8, 0x1e, 0xf3, 0x2e}};
                        Check(captured.Save((output / name).c_str(), &png) == Gdiplus::Ok,
                              "sample saved");
                    }
                    SelectObject(mem, old);
                    DeleteObject(shot_bitmap);
                    DeleteDC(mem);
                    ::ReleaseDC(nullptr, desk);
                };

                const struct { const wchar_t* text; std::uint32_t tint; const char* name; }
                    widths[] = {
                        {L"+3.9 GB",  0xFF4BD68BU, "fit-96dpi-1-small.png"},
                        {L"+20 GB",   0xFF4BD68BU, "fit-96dpi-2-tens.png"},
                        {L"+128 GB",  0xFF4BD68BU, "fit-96dpi-3-hundreds.png"},
                        {L"+1024 GB", 0xFF4BD68BU, "fit-96dpi-4-terabyte.png"},
                        {L"-128 GB",  0xFFFF5B5EU, "fit-96dpi-5-loss.png"},
                        {L"--",       0xB4CDD6E2U, "fit-96dpi-6-noise.png"},
                    };
                for (const auto& sample : widths) {
                    const RECT at{osd_rect.left + 40, osd_rect.top + 60,
                                  osd_rect.left + 40 + 72, osd_rect.top + 60 + 51};
                    gtg::tray::OsdBusyIndicator shot;
                    Check(shot.Begin(osd, at, 0xFFFF7AC4U, 1.0F, L"RAM"),
                          "96 dpi sample raised");
                    shot.ShowResult(sample.text, sample.tint);
                    capture_cell(at, sample.name);
                    shot.End();
                    Check(find_indicator() == nullptr, "96 dpi sample torn down");
                }

                // The network verdict at 96 DPI, through the same real paint
                // path. Two shapes were rendered side by side and the owner
                // chose this one -- the grade as a three-bar mark beside the
                // latency -- over spelling the grade out as a word, which cost
                // the width that the figure and its unit needed. Every failure
                // the probe can report is rendered too: those are the reading
                // when there is no verdict, and they have to fit the same cell.
                {
                    using gtg::net::Grade;
                    using gtg::net::ProbeStatus;
                    using gtg::net::Verdict;
                    namespace action = gtg::net::action;
                    const struct {
                        Verdict verdict;
                        ProbeStatus status;
                        const char* name;
                    } verdicts[] = {
                        {{Grade::Good, 18, 4},   ProbeStatus::Ready,
                         "net-verdict-1-good.png"},
                        {{Grade::Fair, 90, 6},   ProbeStatus::Ready,
                         "net-verdict-2-fair.png"},
                        {{Grade::Poor, 210, 3},  ProbeStatus::Ready,
                         "net-verdict-3-poor.png"},
                        // The widest figure the stack can hand back, to prove
                        // the step-down ladder still has room beside the mark.
                        {{Grade::Poor, 1234, 1}, ProbeStatus::Ready,
                         "net-verdict-4-wide.png"},
                        {{}, ProbeStatus::NoConnections,
                         "net-verdict-5-no-tcp.png"},
                        {{}, ProbeStatus::NoForeground,
                         "net-verdict-6-no-app.png"},
                        {{}, ProbeStatus::NotPermitted,
                         "net-verdict-7-denied.png"},
                    };
                    const RECT at{osd_rect.left + 40, osd_rect.top + 60,
                                  osd_rect.left + 40 + 72, osd_rect.top + 60 + 51};
                    for (const auto& sample : verdicts) {
                        const std::uint32_t tint = action::TintFor(sample.verdict.grade);
                        const std::wstring figure =
                            action::ResultText(sample.verdict, sample.status);

                        gtg::tray::OsdBusyIndicator mark;
                        Check(mark.Begin(osd, at, 0xFFC8E85CU, 1.0F, L"NET"),
                              "verdict sample raised");
                        mark.ShowResult(figure, tint,
                                        action::BarsFor(sample.verdict.grade));
                        capture_cell(at, sample.name);
                        mark.End();
                        Check(find_indicator() == nullptr, "verdict sample torn down");
                    }
                }

                // A reclaim is a user-initiated action on other processes, so
                // it must leave a trace. Asserted against the journal on disk
                // rather than against the fact that a log call was written.
                //
                // Still dry. An earlier draft restored the live policy before
                // this block and the test then trimmed the machine running it
                // -- exactly what dry_run exists to prevent.
                SendMessageW(osd, WM_LBUTTONDBLCLK, 0, ram_point);
                Check(find_indicator() != nullptr, "raised for the journal check");
                Sleep(gtg::sysmem::reclaim::kMinimumVisibleMs + 120);
                for (int tick = 0; tick < 4 && find_indicator() != nullptr; ++tick) {
                    SendMessageW(osd, WM_TIMER, 3, 0);
                    Sleep(60);
                }
                Check(find_indicator() == nullptr, "and came down again");

                // Read the journal back off disk. Asserting that a log call
                // exists in the source proves nothing: the logger can be
                // uninitialised, the queue can be full, the write can fail.
                {
                    gtg::logging::Shutdown();   // flush before reading
                    bool found = false;
                    for (const auto& entry :
                         std::filesystem::directory_iterator(
                             std::filesystem::path(argv[0]).parent_path())) {
                        if (entry.path().extension() != ".log") continue;
                        std::wifstream journal(entry.path());
                        std::wstring line;
                        while (std::getline(journal, line)) {
                            if (line.find(L"host memory reclaim:") !=
                                std::wstring::npos) {
                                found = true;
                            }
                        }
                    }
                    Check(found, "the reclaim left a record in the journal");
                    (void)gtg::logging::Initialize(gtg::logging::Role::Tray);
                }
                standalone.End();
                Check(find_indicator() == nullptr, "a repeated teardown is safe");
            }

            osd.SetRamEnabled(false);
            osd.SetRamHistory(nullptr);
            check_fps_size(true);
            const auto fps_now = GetTickCount64();
            for (unsigned i = 0; i < 60; ++i) {
                const auto status = i >= 20 && i < 25
                    ? gtg::fps::Status::Warmup : gtg::fps::Status::Ready;
                fps_history.Record(fps_now - 30'000 + i * 500, game,
                    gtg::fps::Snapshot{status, 55.0 + i % 20, game});
            }
            osd.SetFpsSnapshot({gtg::fps::Status::Ready, 74.0, game});
            screenshot("fps-compact-trend.png");
            ClickToggle(osd);
            check_fps_size(false);
            screenshot("fps-expanded-trend.png");
            osd.SetFpsEnabled(false);
            osd.SetFpsHistory(nullptr);
            fps_history.Clear();
            check_size(false);
            Check(GetModuleHandleW(L"nvml.dll") == nullptr, "no hardware library loaded");
            std::cout << "PASS: actual HWND toggle, cancel, hide/show, FPS curves/footprint, helper, focus, 200 toggles, resources, rendered PNGs\n";
        } catch (const std::exception& error) {
            std::cerr << error.what() << '\n';
            result = 1;
        }
        osd.Shutdown();
    }
    _Module.Term();
    Gdiplus::GdiplusShutdown(token);
    return result;
}
