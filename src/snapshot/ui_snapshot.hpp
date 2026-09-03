#pragma once

#include <windows.h>

#include <filesystem>
#include <string>

namespace gtg::snapshot {

enum class Reason {
    Manual,
    ThermalTrigger,
    ServiceTrigger,
};

struct WallTime {
    int year{};
    int month{};
    int day{};
    int hour{};
    int minute{};
    int second{};
    int millisecond{};
};

struct CaptureResult {
    std::filesystem::path path;
    std::wstring error;

    [[nodiscard]] explicit operator bool() const noexcept { return error.empty(); }
};

[[nodiscard]] WallTime LocalWallTime() noexcept;
[[nodiscard]] std::wstring FormatDisplayTime(const WallTime& time);
[[nodiscard]] std::wstring FormatFileName(Reason reason, const WallTime& time);
[[nodiscard]] CaptureResult CaptureWindowToPng(HWND window,
                                               const std::filesystem::path& path) noexcept;
[[nodiscard]] CaptureResult CaptureIncidentSnapshot(HWND window, Reason reason,
                                                    const WallTime& time) noexcept;

}  // namespace gtg::snapshot
