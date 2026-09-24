#include "settings/settings.hpp"

#include <windows.h>

#include <array>
#include <cstdint>
#include <format>

namespace gtg::settings {
namespace {

constexpr wchar_t kRegistryPath[] = L"SOFTWARE\\GpuThermalGuard";

bool ReadDword(HKEY key, const wchar_t* name, int& destination) {
    DWORD type = 0;
    DWORD value = 0;
    DWORD size = sizeof(value);
    if (RegQueryValueExW(key, name, nullptr, &type, reinterpret_cast<BYTE*>(&value), &size) !=
            ERROR_SUCCESS || type != REG_DWORD || value > static_cast<DWORD>(INT_MAX)) {
        return false;
    }
    destination = static_cast<int>(value);
    return true;
}

bool WriteDword(HKEY key, const wchar_t* name, int value, std::wstring& error) {
    const DWORD data = static_cast<DWORD>(value);
    const LSTATUS status = RegSetValueExW(key, name, 0, REG_DWORD,
        reinterpret_cast<const BYTE*>(&data), sizeof(data));
    if (status == ERROR_SUCCESS) return true;
    error = localization::Format(L"無法寫入設定 {}（Win32 {}）",
                                 L"Unable to write setting {} (Win32 {})", name, status);
    return false;
}

bool ReadSignedDword(HKEY key, const wchar_t* name, int& destination) {
    DWORD type = 0;
    DWORD value = 0;
    DWORD size = sizeof(value);
    if (RegQueryValueExW(key, name, nullptr, &type, reinterpret_cast<BYTE*>(&value), &size) !=
            ERROR_SUCCESS || type != REG_DWORD) {
        return false;
    }
    destination = static_cast<int>(static_cast<std::int32_t>(value));
    return true;
}

bool OpenUserSettingsForWrite(HKEY& key, std::wstring& error) {
    DWORD disposition = 0;
    const LSTATUS create = RegCreateKeyExW(HKEY_CURRENT_USER, kRegistryPath, 0, nullptr, 0,
        KEY_WRITE, nullptr, &key, &disposition);
    if (create == ERROR_SUCCESS) return true;
    error = localization::Format(L"無法開啟使用者設定（Win32 {}）",
                                 L"Unable to open user settings (Win32 {})", create);
    return false;
}

}  // namespace

LoadResult Load() {
    LoadResult result;
    HKEY key = nullptr;
    const LSTATUS open = RegOpenKeyExW(HKEY_LOCAL_MACHINE, kRegistryPath, 0, KEY_READ, &key);
    if (open == ERROR_FILE_NOT_FOUND) return result;
    if (open != ERROR_SUCCESS) {
        result.warning = localization::Format(
            L"無法讀取 HKLM 設定（Win32 {}），已使用預設值。",
            L"Unable to read HKLM settings (Win32 {}); defaults are in use.", open);
        return result;
    }

    ProtectionConfig candidate = result.config;
    const bool complete = ReadDword(key, L"NormalPowerWatts", candidate.normal_power_w) &&
        ReadDword(key, L"SafePowerWatts", candidate.safe_power_w) &&
        ReadDword(key, L"TriggerTemperatureC", candidate.trigger_temperature_c);
    int auto_restore = 0;
    if (ReadDword(key, L"AutoRestore", auto_restore)) {
        candidate.auto_restore = auto_restore != 0;
    }
    RegCloseKey(key);

    if (!complete) {
        result.warning = std::wstring(localization::Select(
            L"HKLM 設定不完整，已使用安全的內建預設值。",
            L"HKLM settings are incomplete; safe built-in defaults are in use."));
        return result;
    }
    if (const auto validation = ValidateConfig(candidate); validation.has_value()) {
        result.warning = std::wstring(localization::Select(
            L"HKLM 設定無效，已使用安全的內建預設值。",
            L"HKLM settings are invalid; safe built-in defaults are in use."));
        return result;
    }
    result.config = candidate;
    result.loaded = true;
    return result;
}

bool Save(const ProtectionConfig& config, std::wstring& error) {
    if (const auto validation = ValidateConfig(config); validation.has_value()) {
        error = std::wstring(localization::Select(L"拒絕保存無效設定。",
                                                  L"Invalid settings were not saved."));
        return false;
    }

    HKEY key = nullptr;
    DWORD disposition = 0;
    const LSTATUS create = RegCreateKeyExW(HKEY_LOCAL_MACHINE, kRegistryPath, 0, nullptr, 0,
        KEY_WRITE, nullptr, &key, &disposition);
    if (create != ERROR_SUCCESS) {
        error = localization::Format(L"無法開啟 HKLM 設定（Win32 {}）",
                                     L"Unable to open HKLM settings (Win32 {})", create);
        return false;
    }
    const bool success = WriteDword(key, L"NormalPowerWatts", config.normal_power_w, error) &&
        WriteDword(key, L"SafePowerWatts", config.safe_power_w, error) &&
        WriteDword(key, L"TriggerTemperatureC", config.trigger_temperature_c, error) &&
        WriteDword(key, L"AutoRestore", config.auto_restore ? 1 : 0, error);
    RegCloseKey(key);
    return success;
}

bool LoadSafeLatch() noexcept {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, kRegistryPath, 0, KEY_READ, &key) != ERROR_SUCCESS)
        return false;
    DWORD type = 0;
    DWORD value = 0;
    DWORD size = sizeof(value);
    const bool latched = RegQueryValueExW(key, L"SafeLatched", nullptr, &type,
        reinterpret_cast<BYTE*>(&value), &size) == ERROR_SUCCESS &&
        type == REG_DWORD && value != 0;
    RegCloseKey(key);
    return latched;
}

bool SaveSafeLatch(const bool latched, std::wstring& error) {
    HKEY key = nullptr;
    DWORD disposition = 0;
    const LSTATUS create = RegCreateKeyExW(HKEY_LOCAL_MACHINE, kRegistryPath, 0, nullptr, 0,
        KEY_WRITE, nullptr, &key, &disposition);
    if (create != ERROR_SUCCESS) {
        error = localization::Format(L"無法保存安全鎖定狀態（Win32 {}）",
                                     L"Unable to save safe-latch state (Win32 {})", create);
        return false;
    }
    const DWORD value = latched ? 1U : 0U;
    const LSTATUS write = RegSetValueExW(key, L"SafeLatched", 0, REG_DWORD,
        reinterpret_cast<const BYTE*>(&value), sizeof(value));
    RegCloseKey(key);
    if (write == ERROR_SUCCESS) return true;
    error = localization::Format(L"無法保存安全鎖定狀態（Win32 {}）",
                                 L"Unable to save safe-latch state (Win32 {})", write);
    return false;
}

unsigned int LoadTriggerCount() noexcept {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, kRegistryPath, 0, KEY_READ, &key) != ERROR_SUCCESS)
        return 0;
    DWORD type = 0;
    DWORD value = 0;
    DWORD size = sizeof(value);
    const bool loaded = RegQueryValueExW(key, L"TriggerCount", nullptr, &type,
        reinterpret_cast<BYTE*>(&value), &size) == ERROR_SUCCESS && type == REG_DWORD;
    RegCloseKey(key);
    return loaded ? static_cast<unsigned int>(value) : 0;
}

bool SaveTriggerCount(const unsigned int count, std::wstring& error) {
    HKEY key = nullptr;
    DWORD disposition = 0;
    const LSTATUS create = RegCreateKeyExW(HKEY_LOCAL_MACHINE, kRegistryPath, 0, nullptr, 0,
        KEY_WRITE, nullptr, &key, &disposition);
    if (create != ERROR_SUCCESS) {
        error = localization::Format(L"無法保存觸發次數（Win32 {}）",
                                     L"Unable to save trip count (Win32 {})", create);
        return false;
    }
    const DWORD value = static_cast<DWORD>(count);
    const LSTATUS write = RegSetValueExW(key, L"TriggerCount", 0, REG_DWORD,
        reinterpret_cast<const BYTE*>(&value), sizeof(value));
    RegCloseKey(key);
    if (write == ERROR_SUCCESS) return true;
    error = localization::Format(L"無法保存觸發次數（Win32 {}）",
                                 L"Unable to save trip count (Win32 {})", write);
    return false;
}

TriggerTestRun LoadTriggerTestRun() noexcept {
    TriggerTestRun result;
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRegistryPath, 0, KEY_READ, &key) != ERROR_SUCCESS) {
        return result;
    }

    DWORD baseline_type = 0;
    DWORD baseline = 0;
    DWORD baseline_size = sizeof(baseline);
    DWORD started_type = 0;
    ULONGLONG started = 0;
    DWORD started_size = sizeof(started);
    const bool baseline_loaded = RegQueryValueExW(
        key, L"TestRunBaseline", nullptr, &baseline_type,
        reinterpret_cast<BYTE*>(&baseline), &baseline_size) == ERROR_SUCCESS &&
        baseline_type == REG_DWORD;
    const bool started_loaded = RegQueryValueExW(
        key, L"TestRunStartedFileTime", nullptr, &started_type,
        reinterpret_cast<BYTE*>(&started), &started_size) == ERROR_SUCCESS &&
        started_type == REG_QWORD;
    RegCloseKey(key);

    if (baseline_loaded && started_loaded) {
        result.loaded = true;
        result.baseline = static_cast<std::uint32_t>(baseline);
        result.started_file_time = static_cast<std::uint64_t>(started);
    }
    return result;
}

bool SaveTriggerTestRun(const std::uint32_t baseline,
                        const std::uint64_t started_file_time,
                        std::wstring& error) {
    HKEY key = nullptr;
    if (!OpenUserSettingsForWrite(key, error)) return false;

    const DWORD baseline_data = static_cast<DWORD>(baseline);
    const ULONGLONG started_data = static_cast<ULONGLONG>(started_file_time);
    const LSTATUS baseline_write = RegSetValueExW(
        key, L"TestRunBaseline", 0, REG_DWORD,
        reinterpret_cast<const BYTE*>(&baseline_data), sizeof(baseline_data));
    const LSTATUS started_write = baseline_write == ERROR_SUCCESS
        ? RegSetValueExW(key, L"TestRunStartedFileTime", 0, REG_QWORD,
                        reinterpret_cast<const BYTE*>(&started_data), sizeof(started_data))
        : baseline_write;
    RegCloseKey(key);
    if (baseline_write == ERROR_SUCCESS && started_write == ERROR_SUCCESS) return true;

    const LSTATUS failure = baseline_write != ERROR_SUCCESS ? baseline_write : started_write;
    error = localization::Format(L"無法保存測試輪次（Win32 {}）",
                                 L"Unable to save test run (Win32 {})", failure);
    return false;
}

bool LoadCloseToTrayPreference() noexcept {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRegistryPath, 0, KEY_READ, &key) != ERROR_SUCCESS)
        return true;
    DWORD type = 0;
    DWORD value = 1;
    DWORD size = sizeof(value);
    const LSTATUS query = RegQueryValueExW(key, L"CloseToTray", nullptr, &type,
        reinterpret_cast<BYTE*>(&value), &size);
    RegCloseKey(key);
    return query != ERROR_SUCCESS || type != REG_DWORD ? true : value != 0;
}

bool SaveCloseToTrayPreference(const bool enabled, std::wstring& error) {
    HKEY key = nullptr;
    DWORD disposition = 0;
    const LSTATUS create = RegCreateKeyExW(HKEY_CURRENT_USER, kRegistryPath, 0, nullptr, 0,
        KEY_WRITE, nullptr, &key, &disposition);
    if (create != ERROR_SUCCESS) {
        error = localization::Format(L"無法保存目前使用者的 [X] 行為（Win32 {}）",
                                     L"Unable to save the current user's [X] behavior (Win32 {})",
                                     create);
        return false;
    }
    const DWORD value = enabled ? 1U : 0U;
    const LSTATUS write = RegSetValueExW(key, L"CloseToTray", 0, REG_DWORD,
        reinterpret_cast<const BYTE*>(&value), sizeof(value));
    RegCloseKey(key);
    if (write == ERROR_SUCCESS) return true;
    error = localization::Format(L"無法保存目前使用者的 [X] 行為（Win32 {}）",
                                 L"Unable to save the current user's [X] behavior (Win32 {})",
                                 write);
    return false;
}

OsdPreference LoadOsdPreference() noexcept {
    OsdPreference result;
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRegistryPath, 0, KEY_READ, &key) != ERROR_SUCCESS) {
        return result;
    }

    DWORD type = 0;
    DWORD enabled = 0;
    DWORD size = sizeof(enabled);
    const bool has_value =
        RegQueryValueExW(key, L"OsdEnabled", nullptr, &type,
            reinterpret_cast<BYTE*>(&enabled), &size) == ERROR_SUCCESS &&
        type == REG_DWORD;
    result.enabled = ResolveOsdPreference(has_value, enabled);
    int collapsed = 0;
    result.collapsed = ReadDword(key, L"OsdCollapsed", collapsed) && collapsed != 0;
    result.has_position = ReadSignedDword(key, L"OsdX", result.x) &&
                          ReadSignedDword(key, L"OsdY", result.y);
    RegCloseKey(key);
    return result;
}

bool SaveOsdEnabled(const bool enabled, std::wstring& error) {
    HKEY key = nullptr;
    if (!OpenUserSettingsForWrite(key, error)) return false;
    const bool success = WriteDword(key, L"OsdEnabled", enabled ? 1 : 0, error);
    RegCloseKey(key);
    return success;
}

bool SaveOsdCollapsed(const bool collapsed, std::wstring& error) {
    HKEY key = nullptr;
    if (!OpenUserSettingsForWrite(key, error)) return false;
    const bool success = WriteDword(key, L"OsdCollapsed", collapsed ? 1 : 0, error);
    RegCloseKey(key);
    return success;
}

bool SaveOsdPosition(const int x, const int y, std::wstring& error) {
    HKEY key = nullptr;
    if (!OpenUserSettingsForWrite(key, error)) return false;
    const bool success = WriteDword(key, L"OsdX", x, error) &&
                         WriteDword(key, L"OsdY", y, error);
    RegCloseKey(key);
    return success;
}

bool LoadFpsEnabled() noexcept {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRegistryPath, 0, KEY_READ, &key) != ERROR_SUCCESS)
        return true;
    DWORD type = 0;
    DWORD value = 0;
    DWORD size = sizeof(value);
    const bool valid = RegQueryValueExW(key, L"ShowFps", nullptr, &type,
        reinterpret_cast<BYTE*>(&value), &size) == ERROR_SUCCESS &&
        type == REG_DWORD && size == sizeof(value);
    RegCloseKey(key);
    return ResolveFpsPreference(valid, value);
}

bool SaveFpsEnabled(const bool enabled, std::wstring& error) {
    HKEY key = nullptr;
    if (!OpenUserSettingsForWrite(key, error)) return false;
    const bool success = WriteDword(key, L"ShowFps", enabled ? 1 : 0, error);
    RegCloseKey(key);
    return success;
}

bool LoadRamEnabled() noexcept {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRegistryPath, 0, KEY_READ, &key) != ERROR_SUCCESS)
        return true;
    DWORD type = 0;
    DWORD value = 0;
    DWORD size = sizeof(value);
    const bool valid = RegQueryValueExW(key, L"ShowRam", nullptr, &type,
        reinterpret_cast<BYTE*>(&value), &size) == ERROR_SUCCESS &&
        type == REG_DWORD && size == sizeof(value);
    RegCloseKey(key);
    return ResolveRamPreference(valid, value);
}

bool SaveRamEnabled(const bool enabled, std::wstring& error) {
    HKEY key = nullptr;
    if (!OpenUserSettingsForWrite(key, error)) return false;
    const bool success = WriteDword(key, L"ShowRam", enabled ? 1 : 0, error);
    RegCloseKey(key);
    return success;
}

bool LoadNetEnabled() noexcept {
    HKEY key = nullptr;
    // Absent means on, so an unreadable key is on too: the same answer RAM and
    // FPS give, and for the same reason -- a reader who has never touched the
    // setting should see the record, not have to find it.
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRegistryPath, 0, KEY_READ, &key) != ERROR_SUCCESS)
        return true;
    DWORD type = 0;
    DWORD value = 0;
    DWORD size = sizeof(value);
    const bool valid = RegQueryValueExW(key, L"ShowNet", nullptr, &type,
        reinterpret_cast<BYTE*>(&value), &size) == ERROR_SUCCESS &&
        type == REG_DWORD && size == sizeof(value);
    RegCloseKey(key);
    return valid ? value != 0 : true;
}

bool SaveNetEnabled(const bool enabled, std::wstring& error) {
    HKEY key = nullptr;
    if (!OpenUserSettingsForWrite(key, error)) return false;
    const bool success = WriteDword(key, L"ShowNet", enabled ? 1 : 0, error);
    RegCloseKey(key);
    return success;
}

std::uint32_t LoadCompactLayout() noexcept {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRegistryPath, 0, KEY_READ, &key) != ERROR_SUCCESS)
        return 0;
    DWORD type = 0;
    DWORD value = 0;
    DWORD size = sizeof(value);
    const bool valid = RegQueryValueExW(key, L"CompactLayout", nullptr, &type,
        reinterpret_cast<BYTE*>(&value), &size) == ERROR_SUCCESS &&
        type == REG_DWORD && size == sizeof(value);
    RegCloseKey(key);
    // Zero is the absent case and also sanitizes to the default arrangement,
    // so a missing value and a malformed one take the same safe path.
    return valid ? static_cast<std::uint32_t>(value) : 0;
}

bool SaveCompactLayout(const std::uint32_t packed, std::wstring& error) {
    HKEY key = nullptr;
    if (!OpenUserSettingsForWrite(key, error)) return false;
    const bool success = WriteDword(key, L"CompactLayout",
                                    static_cast<DWORD>(packed), error);
    RegCloseKey(key);
    return success;
}

bool LoadCompactLocked() noexcept {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRegistryPath, 0, KEY_READ, &key) != ERROR_SUCCESS)
        return true;
    DWORD type = 0;
    DWORD value = 0;
    DWORD size = sizeof(value);
    const bool valid = RegQueryValueExW(key, L"CompactLocked", nullptr, &type,
        reinterpret_cast<BYTE*>(&value), &size) == ERROR_SUCCESS &&
        type == REG_DWORD && size == sizeof(value);
    RegCloseKey(key);
    return ResolveCompactLockPreference(valid, value);
}

bool SaveCompactLocked(const bool locked, std::wstring& error) {
    HKEY key = nullptr;
    if (!OpenUserSettingsForWrite(key, error)) return false;
    const bool success = WriteDword(key, L"CompactLocked", locked ? 1 : 0, error);
    RegCloseKey(key);
    return success;
}

WindowPosition LoadMainWindowPosition() noexcept {
    WindowPosition result;
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRegistryPath, 0, KEY_READ, &key) != ERROR_SUCCESS) {
        return result;
    }
    result.has_position = ReadSignedDword(key, L"MainWindowX", result.x) &&
                          ReadSignedDword(key, L"MainWindowY", result.y);
    RegCloseKey(key);
    return result;
}

bool SaveMainWindowPosition(const int x, const int y, std::wstring& error) {
    HKEY key = nullptr;
    if (!OpenUserSettingsForWrite(key, error)) return false;
    const bool success = WriteDword(key, L"MainWindowX", x, error) &&
                         WriteDword(key, L"MainWindowY", y, error);
    RegCloseKey(key);
    return success;
}

localization::UiLanguage LoadUiLanguagePreference() noexcept {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRegistryPath, 0, KEY_READ, &key) != ERROR_SUCCESS) {
        return localization::UiLanguage::English;
    }
    std::array<wchar_t, 16> value{};
    DWORD type = 0;
    DWORD size = static_cast<DWORD>(value.size() * sizeof(wchar_t));
    const LSTATUS query = RegQueryValueExW(key, L"UiLanguage", nullptr, &type,
        reinterpret_cast<BYTE*>(value.data()), &size);
    RegCloseKey(key);
    if (query != ERROR_SUCCESS || type != REG_SZ) {
        return localization::UiLanguage::English;
    }
    return localization::FromRegistryValue(value.data());
}

bool SaveUiLanguagePreference(const localization::UiLanguage language, std::wstring& error) {
    HKEY key = nullptr;
    if (!OpenUserSettingsForWrite(key, error)) return false;
    const std::wstring_view value = localization::RegistryValue(language);
    const DWORD size = static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t));
    const LSTATUS write = RegSetValueExW(key, L"UiLanguage", 0, REG_SZ,
        reinterpret_cast<const BYTE*>(value.data()), size);
    RegCloseKey(key);
    if (write == ERROR_SUCCESS) return true;
    error = localization::Format(L"無法保存 UI 語言（Win32 {}）",
                                 L"Unable to save UI language (Win32 {})", write);
    return false;
}

}  // namespace gtg::settings
