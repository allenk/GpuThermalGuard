// Standalone hardware-free HWND integration test. Never initializes NVML/GTG.
#include <windows.h>
#include <objidl.h>
#include <gdiplus.h>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include "tray/osd_overlay.hpp"
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
            HWND foreground = GetForegroundWindow();
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
                Check(GetForegroundWindow() == foreground, "no foreground activation");
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
                    placed.row1, r.right - r.left, scale);
                const auto origin = gtg::tray::compact::CellOriginAt(grid, slot);
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
