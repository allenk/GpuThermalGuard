#include "snapshot/ui_snapshot.hpp"

#include <objidl.h>
#include <gdiplus.h>
#include <knownfolders.h>
#include <shlobj.h>

#include <format>
#include <cstddef>
#include <cwchar>
#include <exception>
#include <memory>
#include <optional>
#include <system_error>
#include <type_traits>
#include <vector>

namespace gtg::snapshot {
namespace {

struct DeleteDc {
    void operator()(HDC dc) const noexcept { if (dc != nullptr) DeleteDC(dc); }
};

struct DeleteBitmap {
    void operator()(HBITMAP bitmap) const noexcept {
        if (bitmap != nullptr) DeleteObject(bitmap);
    }
};

std::wstring Win32Error(const wchar_t* operation, const DWORD error = GetLastError()) {
    return std::format(L"{} failed (Win32 {})", operation, error);
}

std::filesystem::path ExecutableDirectory() {
    std::wstring buffer(512, L'\0');
    for (;;) {
        const DWORD length = GetModuleFileNameW(nullptr, buffer.data(),
                                                static_cast<DWORD>(buffer.size()));
        if (length == 0) return {};
        if (length < buffer.size() - 1) {
            buffer.resize(length);
            return std::filesystem::path(buffer).parent_path();
        }
        buffer.resize(buffer.size() * 2);
    }
}

std::filesystem::path PreferredSnapshotDirectory() {
    PWSTR raw_path = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_CREATE,
                                       nullptr, &raw_path))) {
        std::filesystem::path path(raw_path);
        CoTaskMemFree(raw_path);
        return path / L"GpuThermalGuard" / L"snapshots";
    }
    return {};
}

bool EnsureDirectory(const std::filesystem::path& directory) {
    if (directory.empty()) return false;
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    return !error;
}

std::optional<CLSID> PngEncoder() {
    UINT count = 0;
    UINT bytes = 0;
    if (Gdiplus::GetImageEncodersSize(&count, &bytes) != Gdiplus::Ok || bytes == 0) {
        return std::nullopt;
    }
    std::vector<std::byte> storage(bytes);
    auto* encoders = reinterpret_cast<Gdiplus::ImageCodecInfo*>(storage.data());
    if (Gdiplus::GetImageEncoders(count, bytes, encoders) != Gdiplus::Ok) {
        return std::nullopt;
    }
    for (UINT index = 0; index < count; ++index) {
        if (encoders[index].MimeType != nullptr &&
            wcscmp(encoders[index].MimeType, L"image/png") == 0) {
            return encoders[index].Clsid;
        }
    }
    return std::nullopt;
}

std::filesystem::path UniquePath(const std::filesystem::path& directory,
                                 const std::wstring& filename) {
    std::filesystem::path candidate = directory / filename;
    std::error_code error;
    if (!std::filesystem::exists(candidate, error)) return candidate;
    const auto stem = candidate.stem().wstring();
    for (unsigned int suffix = 2; suffix < 10'000; ++suffix) {
        candidate = directory / std::format(L"{}-{}.png", stem, suffix);
        error.clear();
        if (!std::filesystem::exists(candidate, error)) return candidate;
    }
    return directory / std::format(L"{}-{}.png", stem, GetTickCount64());
}

std::optional<SIZE> CaptureClientSize(HWND window) {
    RECT client{};
    if (IsIconic(window) == FALSE && GetClientRect(window, &client) != FALSE &&
        client.right > client.left && client.bottom > client.top) {
        return SIZE{client.right - client.left, client.bottom - client.top};
    }

    WINDOWPLACEMENT placement{sizeof(placement)};
    if (GetWindowPlacement(window, &placement) == FALSE) return std::nullopt;
    const int outer_width = placement.rcNormalPosition.right - placement.rcNormalPosition.left;
    const int outer_height = placement.rcNormalPosition.bottom - placement.rcNormalPosition.top;
    RECT frame{};
    const DWORD style = static_cast<DWORD>(GetWindowLongPtrW(window, GWL_STYLE)) &
                        ~(WS_MINIMIZE | WS_MAXIMIZE);
    const DWORD extended_style = static_cast<DWORD>(GetWindowLongPtrW(window, GWL_EXSTYLE));
    if (AdjustWindowRectExForDpi(&frame, style, GetMenu(window) != nullptr,
                                 extended_style, GetDpiForWindow(window)) == FALSE) {
        return std::nullopt;
    }
    const int width = outer_width - (frame.right - frame.left);
    const int height = outer_height - (frame.bottom - frame.top);
    if (width <= 0 || height <= 0) return std::nullopt;
    return SIZE{width, height};
}

void PaintChildWindows(HWND root, HDC target) {
    std::vector<HWND> children;
    for (HWND child = GetWindow(root, GW_CHILD); child != nullptr;
         child = GetWindow(child, GW_HWNDNEXT)) {
        // IsWindowVisible also tests every ancestor. A tray-hidden dialog would
        // therefore make all otherwise-visible controls look invisible here.
        // Only the child's own resource/style visibility is relevant to this
        // off-screen composition.
        const auto style = static_cast<DWORD>(GetWindowLongPtrW(child, GWL_STYLE));
        if ((style & WS_VISIBLE) != 0) children.push_back(child);
    }

    // Compose bottom-to-top so labels and edits are not overwritten by group
    // boxes that occupy the same client region.
    for (auto iterator = children.rbegin(); iterator != children.rend(); ++iterator) {
        HWND child = *iterator;
        RECT bounds{};
        if (GetWindowRect(child, &bounds) == FALSE) continue;
        POINT origin{};
        SetLastError(ERROR_SUCCESS);
        if (MapWindowPoints(child, root, &origin, 1) == 0 && GetLastError() != ERROR_SUCCESS) {
            continue;
        }
        const int width = bounds.right - bounds.left;
        const int height = bounds.bottom - bounds.top;
        if (width <= 0 || height <= 0) continue;

        const int saved = SaveDC(target);
        IntersectClipRect(target, origin.x, origin.y, origin.x + width, origin.y + height);
        SetViewportOrgEx(target, origin.x, origin.y, nullptr);
        SendMessageW(child, WM_PRINT, reinterpret_cast<WPARAM>(target),
                     PRF_NONCLIENT | PRF_CLIENT | PRF_CHILDREN | PRF_ERASEBKGND);
        RestoreDC(target, saved);
    }
}

}  // namespace

WallTime LocalWallTime() noexcept {
    SYSTEMTIME value{};
    GetLocalTime(&value);
    return {value.wYear, value.wMonth, value.wDay, value.wHour, value.wMinute,
            value.wSecond, value.wMilliseconds};
}

std::wstring FormatDisplayTime(const WallTime& time) {
    return std::format(L"{:04}-{:02}-{:02}  {:02}:{:02}:{:02}", time.year, time.month,
                       time.day, time.hour, time.minute, time.second);
}

std::wstring FormatFileName(const Reason reason, const WallTime& time) {
    const wchar_t* reason_text = L"manual";
    if (reason == Reason::ThermalTrigger) reason_text = L"thermal-trigger";
    if (reason == Reason::ServiceTrigger) reason_text = L"service-trigger";
    return std::format(L"GpuThermalGuard-{}-{:04}{:02}{:02}-{:02}{:02}{:02}-{:03}.png",
                       reason_text, time.year, time.month, time.day, time.hour, time.minute,
                       time.second, time.millisecond);
}

CaptureResult CaptureWindowToPng(HWND window, const std::filesystem::path& path) noexcept {
    try {
        if (!IsWindow(window)) return {{}, L"snapshot target window is invalid"};
        const auto size = CaptureClientSize(window);
        if (!size) return {{}, L"snapshot target has no restorable client area"};
        const int width = size->cx;
        const int height = size->cy;

        HDC window_dc = GetDC(window);
        if (window_dc == nullptr) return {{}, Win32Error(L"GetDC")};
        std::unique_ptr<std::remove_pointer_t<HDC>, DeleteDc> memory_dc(
            CreateCompatibleDC(window_dc));
        HBITMAP raw_bitmap = CreateCompatibleBitmap(window_dc, width, height);
        ReleaseDC(window, window_dc);
        std::unique_ptr<std::remove_pointer_t<HBITMAP>, DeleteBitmap> bitmap(raw_bitmap);
        if (!memory_dc || !bitmap) return {{}, Win32Error(L"CreateCompatibleBitmap")};

        HGDIOBJ previous = SelectObject(memory_dc.get(), bitmap.get());
        RECT image_bounds{0, 0, width, height};
        FillRect(memory_dc.get(), &image_bounds, GetSysColorBrush(COLOR_BTNFACE));
        SendMessageW(window, WM_PRINTCLIENT, reinterpret_cast<WPARAM>(memory_dc.get()),
                     PRF_CLIENT | PRF_ERASEBKGND);
        PaintChildWindows(window, memory_dc.get());
        SelectObject(memory_dc.get(), previous);

        const auto encoder = PngEncoder();
        if (!encoder) return {{}, L"Windows PNG encoder is unavailable"};
        Gdiplus::Bitmap image(bitmap.get(), nullptr);
        if (image.GetLastStatus() != Gdiplus::Ok) return {{}, L"GDI+ bitmap creation failed"};
        const Gdiplus::Status status = image.Save(path.c_str(), &*encoder, nullptr);
        if (status != Gdiplus::Ok) {
            return {{}, std::format(L"PNG save failed (GDI+ status {})",
                                    static_cast<int>(status))};
        }
        return {path, {}};
    } catch (const std::exception& error) {
        const std::string message(error.what());
        return {{}, std::wstring(message.begin(), message.end())};
    } catch (...) {
        return {{}, L"unexpected snapshot failure"};
    }
}

CaptureResult CaptureIncidentSnapshot(HWND window, const Reason reason,
                                      const WallTime& time) noexcept {
    try {
        std::filesystem::path directory = PreferredSnapshotDirectory();
        if (!EnsureDirectory(directory)) {
            directory = ExecutableDirectory() / L"snapshots";
        }
        if (!EnsureDirectory(directory)) return {{}, L"cannot create snapshot directory"};
        return CaptureWindowToPng(window, UniquePath(directory, FormatFileName(reason, time)));
    } catch (...) {
        return {{}, L"cannot prepare snapshot path"};
    }
}

}  // namespace gtg::snapshot
