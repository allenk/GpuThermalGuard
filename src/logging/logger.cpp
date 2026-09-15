#include "logging/logger.hpp"

#include <windows.h>
#include <knownfolders.h>
#include <shlobj.h>

#include <array>
#include <atomic>
#include <chrono>
#include <format>
#include <mutex>
#include <string>
#include <thread>

#include "logging/deferred_queue.hpp"

namespace gtg::logging {
namespace {

constexpr std::uint64_t kRotateAtBytes = 4ULL * 1024ULL * 1024ULL;

std::mutex g_mutex;
HANDLE g_file = INVALID_HANDLE_VALUE;
std::filesystem::path g_path;
std::uint64_t g_written_bytes = 0;
ULONGLONG g_rotation_retry_after = 0;
constexpr std::size_t kDeferredQueueCapacity = 64;
constexpr std::size_t kDeferredMessageChars = 1024;
DeferredQueue<kDeferredQueueCapacity, kDeferredMessageChars> g_deferred_queue;
std::atomic<HANDLE> g_deferred_event{nullptr};
std::atomic<bool> g_deferred_stop{false};
std::atomic<std::uint64_t> g_deferred_unavailable_drops{0};
std::thread g_deferred_writer;

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
            (void)MoveFileExW(path.c_str(), previous.c_str(), MOVEFILE_REPLACE_EXISTING);
        }
    }

    HANDLE file = CreateFileW(path.c_str(), FILE_APPEND_DATA,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_ALWAYS,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    g_file = file;
    g_path = path;
    LARGE_INTEGER actual{};
    g_written_bytes = GetFileSizeEx(file, &actual) ? static_cast<std::uint64_t>(actual.QuadPart) : 0;
    return true;
}

// Called under g_mutex, outside the protection thread. If rotation fails,
// reopen the original log and suppress writes above the bound until retry.
bool RotateIfNeeded() {
    if (g_written_bytes < kRotateAtBytes) return true;
    const auto now = GetTickCount64();
    if (now < g_rotation_retry_after) return false;
    const auto previous = g_path.wstring() + L".1";
    CloseHandle(g_file);
    g_file = INVALID_HANDLE_VALUE;
    const bool moved = MoveFileExW(g_path.c_str(), previous.c_str(),
                                  MOVEFILE_REPLACE_EXISTING) != FALSE;
    g_file = CreateFileW(g_path.c_str(), FILE_APPEND_DATA,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_ALWAYS,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (moved) g_written_bytes = 0;
    g_rotation_retry_after = moved && g_file != INVALID_HANDLE_VALUE ? 0 : now + 60'000;
    return moved && g_file != INVALID_HANDLE_VALUE;
}

void Write(const wchar_t* level, const std::wstring_view message) noexcept {
    try {
        std::scoped_lock lock(g_mutex);
        if (g_file == INVALID_HANDLE_VALUE) {
            if (g_path.empty() || GetTickCount64() < g_rotation_retry_after) return;
            g_file = CreateFileW(g_path.c_str(), FILE_APPEND_DATA,
                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (g_file == INVALID_HANDLE_VALUE) {
                g_rotation_retry_after = GetTickCount64() + 60'000;
                return;
            }
            LARGE_INTEGER actual{};
            if (GetFileSizeEx(g_file, &actual)) {
                g_written_bytes = static_cast<std::uint64_t>(actual.QuadPart);
            }
        }
        if (!RotateIfNeeded()) return;

        SYSTEMTIME time{};
        GetLocalTime(&time);
        const std::wstring wide_line = std::format(
            L"{:04}-{:02}-{:02}T{:02}:{:02}:{:02}.{:03} {} pid={} tid={} {}\r\n",
            time.wYear, time.wMonth, time.wDay, time.wHour, time.wMinute, time.wSecond,
            time.wMilliseconds, level, GetCurrentProcessId(), GetCurrentThreadId(), message);
        const std::string line = WideToUtf8(wide_line);
        DWORD written = 0;
        (void)WriteFile(g_file, line.data(), static_cast<DWORD>(line.size()), &written, nullptr);
        g_written_bytes += written;
        OutputDebugStringW(wide_line.c_str());
    } catch (...) {
        // Diagnostics must never interfere with the thermal protection path.
    }
}

const wchar_t* LevelText(const DeferredLevel level) noexcept {
    switch (level) {
    case DeferredLevel::Warning: return L"WARN ";
    case DeferredLevel::Error: return L"ERROR";
    default: return L"INFO ";
    }
}

void DeferredWriter(const HANDLE event) noexcept {
    DeferredRecord<kDeferredMessageChars> record;
    for (;;) {
        const DWORD wait = WaitForSingleObject(event, INFINITE);
        if (wait != WAIT_OBJECT_0) break;
        while (g_deferred_queue.TryPop(record)) {
            Write(LevelText(record.level), record.View());
        }
        if (g_deferred_stop.load(std::memory_order_acquire) &&
            g_deferred_queue.Empty()) {
            break;
        }
    }
}

bool TryDeferred(const DeferredLevel level, const std::wstring_view message) noexcept {
    const HANDLE event = g_deferred_event.load(std::memory_order_acquire);
    if (event == nullptr || g_deferred_stop.load(std::memory_order_relaxed)) {
        g_deferred_unavailable_drops.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    if (!g_deferred_queue.TryPush(level, message)) return false;
    (void)SetEvent(event);
    return true;
}

}  // namespace

bool Initialize(const Role role) noexcept {
    try {
        {
            std::scoped_lock lock(g_mutex);
            if (g_file != INVALID_HANDLE_VALUE) return true;
            const wchar_t* filename = role == Role::Service
                ? L"GpuThermalGuard-service.log" : role == Role::Supervisor
                ? L"GpuThermalGuard-supervisor.log" : L"GpuThermalGuard-tray.log";
            if (!OpenAt(ExecutableDirectory(), filename) &&
                !OpenAt(AppDataDirectory(role), filename)) {
                return false;
            }
        }

        HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (event == nullptr) return true;
        g_deferred_stop.store(false, std::memory_order_release);
        try {
            g_deferred_writer = std::thread(DeferredWriter, event);
            g_deferred_event.store(event, std::memory_order_release);
        } catch (...) {
            CloseHandle(event);
        }
        return true;
    } catch (...) {
        return false;
    }
}

void Shutdown() noexcept {
    const HANDLE event = g_deferred_event.exchange(nullptr, std::memory_order_acq_rel);
    g_deferred_stop.store(true, std::memory_order_release);
    if (event != nullptr) SetEvent(event);
    if (g_deferred_writer.joinable()) g_deferred_writer.join();
    if (event != nullptr) CloseHandle(event);

    std::scoped_lock lock(g_mutex);
    if (g_file != INVALID_HANDLE_VALUE) CloseHandle(g_file);
    g_file = INVALID_HANDLE_VALUE;
    g_path.clear();
}

void Info(const std::wstring_view message) noexcept { Write(L"INFO ", message); }
void Warning(const std::wstring_view message) noexcept { Write(L"WARN ", message); }
void Error(const std::wstring_view message) noexcept { Write(L"ERROR", message); }
bool TryInfo(const std::wstring_view message) noexcept {
    return TryDeferred(DeferredLevel::Info, message);
}
bool TryWarning(const std::wstring_view message) noexcept {
    return TryDeferred(DeferredLevel::Warning, message);
}
bool TryError(const std::wstring_view message) noexcept {
    return TryDeferred(DeferredLevel::Error, message);
}
std::uint64_t DeferredDroppedCount() noexcept {
    return g_deferred_queue.Dropped() +
           g_deferred_unavailable_drops.load(std::memory_order_relaxed);
}

std::filesystem::path Path() noexcept {
    try {
        std::scoped_lock lock(g_mutex);
        return g_path;
    } catch (...) {
        return {};
    }
}

}  // namespace gtg::logging
