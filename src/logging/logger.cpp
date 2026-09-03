#include "logging/logger.hpp"

#include <windows.h>
#include <knownfolders.h>
#include <shlobj.h>

#include <array>
#include <chrono>
#include <format>
#include <mutex>
#include <string>

namespace gtg::logging {
namespace {

constexpr std::uint64_t kRotateAtBytes = 4ULL * 1024ULL * 1024ULL;

std::mutex g_mutex;
HANDLE g_file = INVALID_HANDLE_VALUE;
std::filesystem::path g_path;

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

std::filesystem::path AppDataDirectory(const Role role) {
    PWSTR raw_path = nullptr;
    const KNOWNFOLDERID& folder = role == Role::Service
        ? FOLDERID_ProgramData : FOLDERID_LocalAppData;
    if (FAILED(SHGetKnownFolderPath(folder, KF_FLAG_CREATE, nullptr, &raw_path))) return {};
    std::filesystem::path path(raw_path);
    CoTaskMemFree(raw_path);
    return path / L"GpuThermalGuard" / L"logs";
}

std::string WideToUtf8(const std::wstring_view value) {
    if (value.empty()) return {};
    const int length = WideCharToMultiByte(CP_UTF8, 0, value.data(),
        static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (length <= 0) return "<invalid UTF-16>";
    std::string result(static_cast<std::size_t>(length), '\0');
    (void)WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
                             result.data(), length, nullptr, nullptr);
    return result;
}

bool OpenAt(const std::filesystem::path& directory, const wchar_t* filename) {
    if (directory.empty()) return false;
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    if (error) return false;

    const auto path = directory / filename;
    WIN32_FILE_ATTRIBUTE_DATA attributes{};
    if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &attributes) != FALSE) {
        ULARGE_INTEGER size{};
        size.HighPart = attributes.nFileSizeHigh;
        size.LowPart = attributes.nFileSizeLow;
        if (size.QuadPart >= kRotateAtBytes) {
            const auto previous = path.wstring() + L".1";
            (void)DeleteFileW(previous.c_str());
            (void)MoveFileExW(path.c_str(), previous.c_str(), MOVEFILE_REPLACE_EXISTING);
        }
    }

    HANDLE file = CreateFileW(path.c_str(), FILE_APPEND_DATA,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_ALWAYS,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    g_file = file;
    g_path = path;
    return true;
}

void Write(const wchar_t* level, const std::wstring_view message) noexcept {
    try {
        std::scoped_lock lock(g_mutex);
        if (g_file == INVALID_HANDLE_VALUE) return;

        SYSTEMTIME time{};
        GetLocalTime(&time);
        const std::wstring wide_line = std::format(
            L"{:04}-{:02}-{:02}T{:02}:{:02}:{:02}.{:03} {} pid={} tid={} {}\r\n",
            time.wYear, time.wMonth, time.wDay, time.wHour, time.wMinute, time.wSecond,
            time.wMilliseconds, level, GetCurrentProcessId(), GetCurrentThreadId(), message);
        const std::string line = WideToUtf8(wide_line);
        DWORD written = 0;
        (void)WriteFile(g_file, line.data(), static_cast<DWORD>(line.size()), &written, nullptr);
        OutputDebugStringW(wide_line.c_str());
    } catch (...) {
        // Diagnostics must never interfere with the thermal protection path.
    }
}

}  // namespace

bool Initialize(const Role role) noexcept {
    try {
        std::scoped_lock lock(g_mutex);
        if (g_file != INVALID_HANDLE_VALUE) return true;
        const wchar_t* filename = role == Role::Service
            ? L"GpuThermalGuard-service.log" : L"GpuThermalGuard-tray.log";
        if (OpenAt(ExecutableDirectory(), filename)) return true;
        return OpenAt(AppDataDirectory(role), filename);
    } catch (...) {
        return false;
    }
}

void Shutdown() noexcept {
    std::scoped_lock lock(g_mutex);
    if (g_file != INVALID_HANDLE_VALUE) CloseHandle(g_file);
    g_file = INVALID_HANDLE_VALUE;
    g_path.clear();
}

void Info(const std::wstring_view message) noexcept { Write(L"INFO ", message); }
void Warning(const std::wstring_view message) noexcept { Write(L"WARN ", message); }
void Error(const std::wstring_view message) noexcept { Write(L"ERROR", message); }

std::filesystem::path Path() noexcept {
    try {
        std::scoped_lock lock(g_mutex);
        return g_path;
    } catch (...) {
        return {};
    }
}

}  // namespace gtg::logging
