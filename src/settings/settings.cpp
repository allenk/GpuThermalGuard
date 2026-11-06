#include "settings/settings.hpp"

#include <windows.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <format>
#include <optional>
#include <vector>

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
    int tiny = 0;
    result.tiny = ReadDword(key, L"CompactTiny", tiny) && tiny != 0;
    int card = 0;
    result.card = ReadDword(key, L"OsdCard", card) && card != 0;
    result.has_position = ReadSignedDword(key, L"OsdX", result.x) &&
                          ReadSignedDword(key, L"OsdY", result.y);
    result.has_compact_position = ReadSignedDword(key, L"CompactX", result.compact_x) &&
                                  ReadSignedDword(key, L"CompactY", result.compact_y);
    // Absent, malformed or out of range all mean the same thing here: follow
    // the display. EffectiveOsdScale makes that decision; this only carries
    // the number across.
    int scale = 0;
    result.scale_percent = ReadDword(key, L"OsdScalePercent", scale) ? scale : 0;
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

bool SaveCompactTiny(const bool tiny, std::wstring& error) {
    HKEY key = nullptr;
    if (!OpenUserSettingsForWrite(key, error)) return false;
    const bool success = WriteDword(key, L"CompactTiny", tiny ? 1 : 0, error);
    RegCloseKey(key);
    return success;
}

bool SaveOsdCard(const bool card, std::wstring& error) {
    HKEY key = nullptr;
    if (!OpenUserSettingsForWrite(key, error)) return false;
    const bool success = WriteDword(key, L"OsdCard", card ? 1 : 0, error);
    RegCloseKey(key);
    return success;
}

bool SaveOsdScale(const int scale_percent, std::wstring& error) {
    HKEY key = nullptr;
    if (!OpenUserSettingsForWrite(key, error)) return false;
    const bool success = WriteDword(
        key, L"OsdScalePercent",
        scale_percent >= kMinOsdScalePercent && scale_percent <= kMaxOsdScalePercent
            ? scale_percent : 0,
        error);
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

bool SaveCompactPosition(const int x, const int y, std::wstring& error) {
    HKEY key = nullptr;
    if (!OpenUserSettingsForWrite(key, error)) return false;
    const bool success = WriteDword(key, L"CompactX", x, error) &&
                         WriteDword(key, L"CompactY", y, error);
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

bool LoadTopAppEnabled() noexcept {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRegistryPath, 0, KEY_READ, &key) != ERROR_SUCCESS)
        return true;
    DWORD type = 0;
    DWORD value = 0;
    DWORD size = sizeof(value);
    const bool valid = RegQueryValueExW(key, L"ShowTopApp", nullptr, &type,
                                        reinterpret_cast<BYTE*>(&value), &size) == ERROR_SUCCESS &&
                       type == REG_DWORD && size == sizeof(value);
    RegCloseKey(key);
    return ResolveTopAppPreference(valid, value);
}

bool SaveTopAppEnabled(const bool enabled, std::wstring& error) {
    HKEY key = nullptr;
    if (!OpenUserSettingsForWrite(key, error)) return false;
    const bool success = WriteDword(key, L"ShowTopApp", enabled ? 1 : 0, error);
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

std::optional<std::uint64_t> LoadCompactArrangement() noexcept {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRegistryPath, 0, KEY_READ, &key) != ERROR_SUCCESS)
        return std::nullopt;
    DWORD type = 0;
    std::uint64_t value = 0;
    DWORD size = sizeof(value);
    const bool valid = RegQueryValueExW(key, L"CompactArrangement", nullptr, &type,
                                        reinterpret_cast<BYTE*>(&value), &size) == ERROR_SUCCESS &&
                       type == REG_QWORD && size == sizeof(value);
    RegCloseKey(key);
    if (!valid) return std::nullopt;
    return value;
}

bool SaveCompactArrangement(const std::uint64_t packed, std::wstring& error) {
    HKEY key = nullptr;
    if (!OpenUserSettingsForWrite(key, error)) return false;
    const LSTATUS status = RegSetValueExW(key, L"CompactArrangement", 0, REG_QWORD,
                                          reinterpret_cast<const BYTE*>(&packed), sizeof(packed));
    RegCloseKey(key);
    if (status == ERROR_SUCCESS) return true;
    error = localization::Format(L"無法寫入設定 {}（Win32 {}）",
                                 L"Unable to write setting {} (Win32 {})", L"CompactArrangement",
                                 status);
    return false;
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

namespace {

// A REG_DWORD that is present and the right size; anything else is absent.
bool ReadValidDword(const wchar_t* name, std::uint32_t& value) noexcept {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRegistryPath, 0, KEY_READ, &key) != ERROR_SUCCESS)
        return false;
    DWORD type = 0;
    DWORD stored = 0;
    DWORD size = sizeof(stored);
    const bool valid = RegQueryValueExW(key, name, nullptr, &type,
        reinterpret_cast<BYTE*>(&stored), &size) == ERROR_SUCCESS &&
        type == REG_DWORD && size == sizeof(stored);
    RegCloseKey(key);
    if (valid) value = stored;
    return valid;
}

}  // namespace

UiTheme LoadUiThemePreference() noexcept {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRegistryPath, 0, KEY_READ, &key) != ERROR_SUCCESS) {
        return UiTheme::Auto;
    }
    std::array<wchar_t, 16> value{};
    DWORD type = 0;
    // One short of the buffer, so the text is always terminated.
    DWORD size = static_cast<DWORD>((value.size() - 1) * sizeof(wchar_t));
    const LSTATUS query = RegQueryValueExW(key, L"UiTheme", nullptr, &type,
        reinterpret_cast<BYTE*>(value.data()), &size);
    RegCloseKey(key);
    if (query != ERROR_SUCCESS || type != REG_SZ) return UiTheme::Auto;
    return UiThemeFromRegistryValue(value.data());
}

bool SaveUiThemePreference(const UiTheme theme, std::wstring& error) {
    HKEY key = nullptr;
    if (!OpenUserSettingsForWrite(key, error)) return false;
    const std::wstring_view value = RegistryValue(theme);
    const DWORD size = static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t));
    const LSTATUS write = RegSetValueExW(key, L"UiTheme", 0, REG_SZ,
        reinterpret_cast<const BYTE*>(value.data()), size);
    RegCloseKey(key);
    if (write == ERROR_SUCCESS) return true;
    error = localization::Format(L"無法保存主題（Win32 {}）",
                                 L"Unable to save the theme (Win32 {})", write);
    return false;
}

bool LoadOverlayEnabled() noexcept {
    std::uint32_t value = 0;
    const bool valid = ReadValidDword(L"OverlayEnabled", value);
    return ResolveOverlayEnabled(valid, value);
}

bool LoadOverlayAutoSdr() noexcept {
    std::uint32_t value{};
    const bool valid = ReadValidDword(L"OverlayAutoSdr", value);
    return ResolveOverlayAutoSdr(valid, value);
}

bool SaveOverlayAutoSdr(const bool enabled, std::wstring& error) {
    HKEY key{};
    if (!OpenUserSettingsForWrite(key, error)) return false;
    const bool success = WriteDword(key, L"OverlayAutoSdr", enabled ? 1 : 0, error);
    RegCloseKey(key);
    return success;
}

bool LoadOverlayHidesOsd() noexcept {
    std::uint32_t value{};
    const bool valid = ReadValidDword(L"OverlayHidesOsd", value);
    return ResolveOverlayHidesOsd(valid, value);
}

bool SaveOverlayHidesOsd(const bool enabled, std::wstring& error) {
    HKEY key{};
    if (!OpenUserSettingsForWrite(key, error)) return false;
    const bool success = WriteDword(key, L"OverlayHidesOsd", enabled ? 1 : 0, error);
    RegCloseKey(key);
    return success;
}

bool LoadFeatureNoticeSerial(std::uint32_t& serial) noexcept {
    return ReadValidDword(L"FeatureNoticeSerial", serial);
}
bool SaveFeatureNoticeSerial(std::uint32_t serial, std::wstring& error) {
    HKEY key{};
    if (!OpenUserSettingsForWrite(key, error)) return false;
    const DWORD value = serial;
    const auto result = RegSetValueExW(key, L"FeatureNoticeSerial", 0, REG_DWORD,
        reinterpret_cast<const BYTE*>(&value), sizeof(value));
    RegCloseKey(key);
    if (result == ERROR_SUCCESS) return true;
    error = localization::Format(L"無法保存功能通知序列號（Win32 {}）",
                                 L"Unable to save feature notice serial (Win32 {})", result);
    return false;
}

bool SaveOverlayEnabled(const bool enabled, std::wstring& error) {
    HKEY key = nullptr;
    if (!OpenUserSettingsForWrite(key, error)) return false;
    const bool success = WriteDword(key, L"OverlayEnabled", enabled ? 1 : 0, error);
    RegCloseKey(key);
    return success;
}

OverlayHotkey LoadOverlayHotkey() noexcept {
    std::uint32_t value = 0;
    const bool valid = ReadValidDword(L"OverlayHotkey", value);
    return ResolveOverlayHotkey(valid, value);
}

bool SaveOverlayHotkey(const OverlayHotkey& hotkey, std::wstring& error) {
    HKEY key = nullptr;
    if (!OpenUserSettingsForWrite(key, error)) return false;
    const bool success = WriteDword(key, L"OverlayHotkey",
        static_cast<int>(PackOverlayHotkey(
            IsAcceptableOverlayHotkey(hotkey) ? hotkey : kDefaultOverlayHotkey)),
        error);
    RegCloseKey(key);
    return success;
}

// Hosts: HKCU\SOFTWARE\GpuThermalGuard\Hosts, with one subkey per pair under
// Pairs, named by the paired host's id. Pair keys are stored as the peer
// module hands them over -- already protected by DPAPI; this layer never sees
// a key in the clear.
namespace {

constexpr wchar_t kHostsPath[] = L"SOFTWARE\\GpuThermalGuard\\Hosts";
constexpr wchar_t kPairsPath[] = L"SOFTWARE\\GpuThermalGuard\\Hosts\\Pairs";
// Longer values are not GTG's: a name is at most 48 UTF-8 bytes on the wire,
// and a protected 32-byte key is a few hundred bytes.
constexpr DWORD kMaxNameChars = 128;
constexpr DWORD kMaxBlobBytes = 4'096;

std::wstring PairKeyName(const std::uint64_t peer) {
    return std::format(L"{:016x}", peer);
}

std::optional<std::uint64_t> ParsePairKeyName(const std::wstring_view name) noexcept {
    if (name.size() != 16) return std::nullopt;
    std::uint64_t value = 0;
    for (const wchar_t c : name) {
        value <<= 4;
        if (c >= L'0' && c <= L'9') {
            value |= static_cast<std::uint64_t>(c - L'0');
        } else if (c >= L'a' && c <= L'f') {
            value |= static_cast<std::uint64_t>(c - L'a' + 10);
        } else {
            return std::nullopt;
        }
    }
    if (value == 0) return std::nullopt;
    return value;
}

std::optional<DWORD> ReadKeyDword(HKEY key, const wchar_t* name) noexcept {
    DWORD type = 0;
    DWORD value = 0;
    DWORD size = sizeof(value);
    if (RegQueryValueExW(key, name, nullptr, &type, reinterpret_cast<BYTE*>(&value), &size) !=
            ERROR_SUCCESS ||
        type != REG_DWORD || size != sizeof(value)) {
        return std::nullopt;
    }
    return value;
}

std::optional<std::wstring> ReadKeyString(HKEY key, const wchar_t* name) {
    DWORD type = 0;
    DWORD size = 0;
    if (RegQueryValueExW(key, name, nullptr, &type, nullptr, &size) != ERROR_SUCCESS ||
        type != REG_SZ || size > (kMaxNameChars + 1) * sizeof(wchar_t)) {
        return std::nullopt;
    }
    std::wstring value(size / sizeof(wchar_t) + 1, L'\0');
    if (RegQueryValueExW(key, name, nullptr, &type, reinterpret_cast<BYTE*>(value.data()), &size) !=
            ERROR_SUCCESS ||
        type != REG_SZ) {
        return std::nullopt;
    }
    value.resize(wcsnlen(value.c_str(), value.size()));
    return value;
}

std::optional<std::vector<std::uint8_t>> ReadKeyBinary(HKEY key, const wchar_t* name) {
    DWORD type = 0;
    DWORD size = 0;
    if (RegQueryValueExW(key, name, nullptr, &type, nullptr, &size) != ERROR_SUCCESS ||
        type != REG_BINARY || size == 0 || size > kMaxBlobBytes) {
        return std::nullopt;
    }
    std::vector<std::uint8_t> value(size);
    if (RegQueryValueExW(key, name, nullptr, &type, value.data(), &size) != ERROR_SUCCESS ||
        type != REG_BINARY || size != value.size()) {
        return std::nullopt;
    }
    return value;
}

bool HostsWriteFailed(const wchar_t* what, const LSTATUS status, std::wstring& error) {
    error = localization::Format(L"無法保存主機設定 {}（Win32 {}）",
                                 L"Unable to save the Hosts setting {} (Win32 {})", what, status);
    return false;
}

bool WriteHostsValue(const wchar_t* path, const wchar_t* name, const DWORD type, const void* data,
                     const DWORD size, std::wstring& error) {
    HKEY key = nullptr;
    const LSTATUS create =
        RegCreateKeyExW(HKEY_CURRENT_USER, path, 0, nullptr, 0, KEY_WRITE, nullptr, &key, nullptr);
    if (create != ERROR_SUCCESS) return HostsWriteFailed(name, create, error);
    const LSTATUS status = RegSetValueExW(key, name, 0, type, static_cast<const BYTE*>(data), size);
    RegCloseKey(key);
    if (status != ERROR_SUCCESS) return HostsWriteFailed(name, status, error);
    return true;
}

}  // namespace

HostsPreference LoadHostsPreference() noexcept {
    HostsPreference result;
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kHostsPath, 0, KEY_READ, &key) != ERROR_SUCCESS) {
        return result;
    }
    try {
        if (const auto enabled = ReadKeyDword(key, L"Enabled")) result.enabled = *enabled != 0;
        if (const auto log = ReadKeyDword(key, L"Log")) result.log = *log != 0;
        if (const auto name = ReadKeyString(key, L"Name")) result.name = *name;
    } catch (...) {
    }
    if (const auto port = ReadKeyDword(key, L"Port"); port && IsValidHostsPort(*port)) {
        result.port = static_cast<int>(*port);
    }
    DWORD type = 0;
    std::uint64_t id = 0;
    DWORD size = sizeof(id);
    if (RegQueryValueExW(key, L"PeerId", nullptr, &type, reinterpret_cast<BYTE*>(&id), &size) ==
            ERROR_SUCCESS &&
        type == REG_QWORD && size == sizeof(id)) {
        result.peer_id = id;
    }
    RegCloseKey(key);
    return result;
}

bool SaveHostsEnabled(const bool enabled, std::wstring& error) {
    const DWORD value = enabled ? 1 : 0;
    return WriteHostsValue(kHostsPath, L"Enabled", REG_DWORD, &value, sizeof(value), error);
}

bool SaveHostsName(const std::wstring_view name, std::wstring& error) {
    const std::wstring value(name.substr(0, kMaxNameChars));
    return WriteHostsValue(kHostsPath, L"Name", REG_SZ, value.c_str(),
                           static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)), error);
}

bool SaveHostsPort(const int port, std::wstring& error) {
    if (port != 0 && !IsValidHostsPort(static_cast<std::uint32_t>(port))) {
        error = localization::Select(L"連接埠須介於 1024 與 65535 之間。",
                                     L"The port must be between 1024 and 65535.");
        return false;
    }
    const DWORD value = static_cast<DWORD>(port);
    return WriteHostsValue(kHostsPath, L"Port", REG_DWORD, &value, sizeof(value), error);
}

bool SaveHostsPeerId(const std::uint64_t id, std::wstring& error) {
    return WriteHostsValue(kHostsPath, L"PeerId", REG_QWORD, &id, sizeof(id), error);
}

bool SaveHostsLog(const bool on, std::wstring& error) {
    const DWORD value = on ? 1 : 0;
    return WriteHostsValue(kHostsPath, L"Log", REG_DWORD, &value, sizeof(value), error);
}

std::vector<StoredHost> LoadHosts() {
    std::vector<StoredHost> hosts;
    HKEY pairs = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kPairsPath, 0, KEY_READ, &pairs) != ERROR_SUCCESS) {
        return hosts;
    }
    for (DWORD index = 0; hosts.size() < kMaxStoredHosts * 4; ++index) {
        wchar_t name[64]{};
        DWORD length = static_cast<DWORD>(std::size(name));
        const LSTATUS status =
            RegEnumKeyExW(pairs, index, name, &length, nullptr, nullptr, nullptr, nullptr);
        if (status == ERROR_NO_MORE_ITEMS) break;
        if (status != ERROR_SUCCESS) continue;
        const auto peer = ParsePairKeyName(std::wstring_view(name, length));
        if (!peer) continue;
        HKEY entry = nullptr;
        if (RegOpenKeyExW(pairs, name, 0, KEY_READ, &entry) != ERROR_SUCCESS) continue;
        StoredHost host;
        host.peer = *peer;
        const auto key = ReadKeyBinary(entry, L"Key");
        const auto order = ReadKeyDword(entry, L"Order");
        host.name = ReadKeyString(entry, L"Name").value_or(std::wstring());
        host.issued = ReadKeyDword(entry, L"Issued").value_or(0) != 0;
        if (const auto endpoint = ReadKeyBinary(entry, L"Endpoint");
            endpoint && endpoint->size() == 6) {
            host.has_endpoint = true;
            std::memcpy(host.address.data(), endpoint->data(), 4);
            host.port = static_cast<std::uint16_t>(((*endpoint)[4] << 8) | (*endpoint)[5]);
        }
        RegCloseKey(entry);
        // Without a key there is no pair, whatever else the entry holds.
        if (!key) continue;
        host.protected_key = *key;
        host.order = order.value_or(0xFFFF'FFFF);
        hosts.push_back(std::move(host));
    }
    RegCloseKey(pairs);
    std::sort(hosts.begin(), hosts.end(), [](const StoredHost& a, const StoredHost& b) {
        return a.order != b.order ? a.order < b.order : a.peer < b.peer;
    });
    if (hosts.size() > kMaxStoredHosts) hosts.resize(kMaxStoredHosts);
    return hosts;
}

bool SaveHost(const StoredHost& host, std::wstring& error) {
    if (host.peer == 0 || host.protected_key.empty() || host.protected_key.size() > kMaxBlobBytes) {
        error =
            localization::Select(L"主機配對資料不完整。", L"The paired host's data is incomplete.");
        return false;
    }
    const std::wstring path = std::wstring(kPairsPath) + L"\\" + PairKeyName(host.peer);
    // A new pair goes last in the reader's order; a known one keeps its place.
    bool known = false;
    HKEY existing = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, path.c_str(), 0, KEY_READ, &existing) == ERROR_SUCCESS) {
        known = ReadKeyDword(existing, L"Order").has_value();
        RegCloseKey(existing);
    }
    if (!known) {
        DWORD next = 0;
        for (const StoredHost& other : LoadHosts()) {
            if (other.peer != host.peer && other.order != 0xFFFF'FFFF) {
                next = std::max<DWORD>(next, other.order + 1);
            }
        }
        if (!WriteHostsValue(path.c_str(), L"Order", REG_DWORD, &next, sizeof(next), error)) {
            return false;
        }
    }
    const std::wstring name = host.name.substr(0, kMaxNameChars);
    const DWORD issued = host.issued ? 1 : 0;
    if (!WriteHostsValue(path.c_str(), L"Name", REG_SZ, name.c_str(),
                         static_cast<DWORD>((name.size() + 1) * sizeof(wchar_t)), error) ||
        !WriteHostsValue(path.c_str(), L"Issued", REG_DWORD, &issued, sizeof(issued), error) ||
        !WriteHostsValue(path.c_str(), L"Key", REG_BINARY, host.protected_key.data(),
                         static_cast<DWORD>(host.protected_key.size()), error)) {
        return false;
    }
    if (host.has_endpoint) {
        const std::uint8_t endpoint[6]{host.address[0],
                                       host.address[1],
                                       host.address[2],
                                       host.address[3],
                                       static_cast<std::uint8_t>(host.port >> 8),
                                       static_cast<std::uint8_t>(host.port & 0xFF)};
        return WriteHostsValue(path.c_str(), L"Endpoint", REG_BINARY, endpoint, sizeof(endpoint),
                               error);
    }
    return true;
}

bool RemoveHost(const std::uint64_t peer, std::wstring& error) {
    HKEY pairs = nullptr;
    LSTATUS status = RegOpenKeyExW(HKEY_CURRENT_USER, kPairsPath, 0, KEY_WRITE | KEY_READ, &pairs);
    if (status == ERROR_FILE_NOT_FOUND) return true;
    if (status != ERROR_SUCCESS) return HostsWriteFailed(L"Pairs", status, error);
    status = RegDeleteTreeW(pairs, PairKeyName(peer).c_str());
    RegCloseKey(pairs);
    if (status != ERROR_SUCCESS && status != ERROR_FILE_NOT_FOUND) {
        return HostsWriteFailed(L"Pairs", status, error);
    }
    return true;
}

bool SaveHostsOrder(const std::vector<std::uint64_t>& order, std::wstring& error) {
    DWORD position = 0;
    for (const std::uint64_t peer : order) {
        const std::wstring path = std::wstring(kPairsPath) + L"\\" + PairKeyName(peer);
        HKEY entry = nullptr;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, path.c_str(), 0, KEY_READ, &entry) != ERROR_SUCCESS) {
            continue;
        }
        RegCloseKey(entry);
        if (!WriteHostsValue(path.c_str(), L"Order", REG_DWORD, &position, sizeof(position),
                             error)) {
            return false;
        }
        ++position;
    }
    return true;
}

}  // namespace gtg::settings
