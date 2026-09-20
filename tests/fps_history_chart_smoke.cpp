// Hardware-free chart render check. No NVML, GTG singleton, or protection work.
#include <windows.h>
#include <objidl.h>
#include <gdiplus.h>

#include <filesystem>
#include <iostream>
#include <stdexcept>

#include "fps/fps_history.hpp"
#include "telemetry/telemetry_history.hpp"
#include "tray/history_chart.hpp"

WTL::CAppModule _Module;

namespace {

void Check(const bool good, const char* message) {
    if (!good) throw std::runtime_error(message);
}

void SaveChart(HWND chart, const std::filesystem::path& path) {
    constexpr int kWidth = 460;
    constexpr int kHeight = 300;
    HDC screen = GetDC(nullptr);
    Check(screen != nullptr, "screen DC");
    HDC memory = CreateCompatibleDC(screen);
    Check(memory != nullptr, "memory DC");
    HBITMAP bitmap = CreateCompatibleBitmap(screen, kWidth, kHeight);
    Check(bitmap != nullptr, "bitmap");
    const HGDIOBJ previous = SelectObject(memory, bitmap);
    RECT area{0, 0, kWidth, kHeight};
    FillRect(memory, &area, static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));
    SendMessageW(chart, WM_PRINTCLIENT, reinterpret_cast<WPARAM>(memory), PRF_CLIENT);
    SelectObject(memory, previous);
    const CLSID png{0x557cf406, 0x1a04, 0x11d3,
                    {0x9a, 0x73, 0x00, 0x00, 0xf8, 0x1e, 0xf3, 0x2e}};
    bool saved = false;
    {
        Gdiplus::Bitmap image(bitmap, nullptr);
        saved = image.Save(path.c_str(), &png) == Gdiplus::Ok;
    }
    DeleteObject(bitmap);
    DeleteDC(memory);
    ReleaseDC(nullptr, screen);
    Check(saved, "chart PNG saved");
}

}  // namespace

int main(int argc, char** argv) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    Gdiplus::GdiplusStartupInput startup{};
    ULONG_PTR token{};
    if (Gdiplus::GdiplusStartup(&token, &startup, nullptr) != Gdiplus::Ok) return 2;
    _Module.Init(nullptr, GetModuleHandleW(nullptr));
    int result = 0;
    try {
        const std::filesystem::path output = argc > 1 ? argv[1] : "out/fps-chart-smoke";
        std::filesystem::create_directories(output);
        const HWND parent = CreateWindowExW(0, L"STATIC", L"", WS_OVERLAPPED,
                                            0, 0, 470, 310, nullptr, nullptr,
                                            GetModuleHandleW(nullptr), nullptr);
        Check(parent != nullptr, "parent HWND");
        gtg::tray::HistoryChart chart;
        RECT bounds{0, 0, 460, 300};
        Check(chart.Create(parent, bounds, nullptr, WS_CHILD | WS_VISIBLE) != nullptr,
              "chart HWND");
        gtg::telemetry::History thermal;
        gtg::fps::History fps;
        const gtg::fps::Identity game{77, 1234};
        const auto now = GetTickCount64();
        const auto start_ms = now > 300'000 ? now - 300'000 : 0;
        for (unsigned i = 0; i <= 600; ++i) {
            const auto t = start_ms + i * 500;
            thermal.AddSample({t, 55.0 + i % 10, 120.0 + i % 20,
                               25.0, 8.0, 45.0, 15.0});
            const auto status = i >= 220 && i < 245
                ? gtg::fps::Status::Warmup : gtg::fps::Status::Ready;
            fps.Record(t, game, {status, 55.0 + i % 25, game});
        }
        chart.SetHistory(&thermal);
        chart.SetThresholds(85, 300, 600.0);
        chart.SetFpsHistory(&fps);
        SaveChart(chart, output / "six-lanes.png");
        chart.SetFpsHistory(nullptr);
        SaveChart(chart, output / "five-lanes.png");
        chart.DestroyWindow();
        DestroyWindow(parent);
        Check(GetModuleHandleW(L"nvml.dll") == nullptr, "no NVML loaded");
        std::cout << "PASS: six/five lane production chart rendering without NVML\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        result = 1;
    }
    _Module.Term();
    Gdiplus::GdiplusShutdown(token);
    return result;
}
