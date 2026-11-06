#include "update/preference.hpp"

#include <windows.h>

#include <cstdint>
#include <vector>

#include "localization/localization.hpp"
#include "update/hosts_trigger.hpp"
#include "update/source.hpp"

namespace gtg::update {
namespace {

constexpr wchar_t kUpdatePath[] = L"SOFTWARE\\GpuThermalGuard\\Update";

std::optional<std::wstring> ReadText(const HKEY key, const wchar_t* name) {
    DWORD type = 0;
    DWORD bytes = 0;
    if (RegQueryValueExW(key, name, nullptr, &type, nullptr, &bytes) != ERROR_SUCCESS ||
        type != REG_SZ || bytes > 4096) {
        return std::nullopt;
    }
    std::vector<wchar_t> buffer(bytes / sizeof(wchar_t) + 1, L'\0');
    if (RegQueryValueExW(key, name, nullptr, &type, reinterpret_cast<BYTE*>(buffer.data()),
                         &bytes) != ERROR_SUCCESS) {
        return std::nullopt;
    }
    return std::wstring(buffer.data());
}

std::vector<std::uint8_t> ReadBinary(const HKEY key, const wchar_t* name, const DWORD limit) {
    DWORD type = 0;
    DWORD bytes = 0;
    if (RegQueryValueExW(key, name, nullptr, &type, nullptr, &bytes) != ERROR_SUCCESS ||
        type != REG_BINARY || bytes > limit) {
        return {};
    }
    std::vector<std::uint8_t> buffer(bytes);
    if (bytes != 0 &&
        RegQueryValueExW(key, name, nullptr, &type, buffer.data(), &bytes) != ERROR_SUCCESS) {
        return {};
    }
    buffer.resize(bytes);
    return buffer;
}

bool Write(const wchar_t* name, const std::optional<std::wstring>& text,
           const std::optional<DWORD> number, std::wstring& error,
           const std::optional<std::uint64_t> wide = std::nullopt,
           const std::optional<std::vector<std::uint8_t>>& binary = std::nullopt) {
    HKEY key = nullptr;
    const LSTATUS open = RegCreateKeyExW(HKEY_LOCAL_MACHINE, kUpdatePath, 0, nullptr, 0, KEY_WRITE,
                                         nullptr, &key, nullptr);
    if (open != ERROR_SUCCESS) {
        error = localization::Format(L"無法開啟更新設定（Win32 {}）",
                                     L"Unable to open the update settings (Win32 {})", open);
        return false;
    }
    LSTATUS status = ERROR_SUCCESS;
    if (number) {
        status = RegSetValueExW(key, name, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&*number),
                                sizeof(DWORD));
    } else if (wide) {
        status = RegSetValueExW(key, name, 0, REG_QWORD, reinterpret_cast<const BYTE*>(&*wide),
                                sizeof(std::uint64_t));
    } else if (binary) {
        status = RegSetValueExW(key, name, 0, REG_BINARY, binary->data(),
                                static_cast<DWORD>(binary->size()));
    } else if (text && !text->empty()) {
        status = RegSetValueExW(key, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(text->c_str()),
                                static_cast<DWORD>((text->size() + 1) * sizeof(wchar_t)));
    } else {
        status = RegDeleteValueW(key, name);
        if (status == ERROR_FILE_NOT_FOUND) status = ERROR_SUCCESS;
    }
    RegCloseKey(key);
    if (status == ERROR_SUCCESS) return true;
    error = localization::Format(L"無法寫入更新設定 {}（Win32 {}）",
                                 L"Unable to write update setting {} (Win32 {})", name, status);
    return false;
}

std::optional<std::uint64_t> ReadQword(const HKEY key, const wchar_t* name) {
    DWORD type = 0;
    std::uint64_t value = 0;
    DWORD size = sizeof(value);
    if (RegQueryValueExW(key, name, nullptr, &type, reinterpret_cast<BYTE*>(&value), &size) !=
            ERROR_SUCCESS ||
        type != REG_QWORD) {
        return std::nullopt;
    }
    return value;
}

}  // namespace

StoredSource ClassifySource(const bool present, const std::optional<std::wstring>& text) {
    StoredSource stored;
    if (!present || (text && text->empty())) return stored;
    if (text && IsValidSource(*text)) {
        stored.source = *text;
        return stored;
    }
    stored.invalid = true;
    stored.text = text.value_or(std::wstring());
    return stored;
}

UpdatePreference LoadUpdatePreference() noexcept {
    UpdatePreference preference;
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, kUpdatePath, 0, KEY_READ, &key) != ERROR_SUCCESS) {
        return preference;
    }
    try {
        DWORD source_type = 0;
        const bool present = RegQueryValueExW(key, L"Source", nullptr, &source_type, nullptr,
                                              nullptr) == ERROR_SUCCESS;
        StoredSource stored = ClassifySource(present, ReadText(key, L"Source"));
        preference.source = std::move(stored.source);
        preference.source_invalid = stored.invalid;
        preference.source_text = std::move(stored.text);
        DWORD type = 0;
        DWORD value = 0;
        DWORD size = sizeof(value);
        if (RegQueryValueExW(key, L"AutoCheck", nullptr, &type, reinterpret_cast<BYTE*>(&value),
                             &size) == ERROR_SUCCESS &&
            type == REG_DWORD) {
            preference.auto_check = value != 0;
        }
        size = sizeof(value);
        if (RegQueryValueExW(key, L"HostsTrigger", nullptr, &type, reinterpret_cast<BYTE*>(&value),
                             &size) == ERROR_SUCCESS &&
            type == REG_DWORD) {
            preference.hosts_trigger = value != 0;
        }
        if (const auto last = ReadQword(key, L"LastTrigger")) {
            preference.last_trigger_unix_ms = static_cast<std::int64_t>(*last);
        }
        preference.pending_trigger = ReadQword(key, L"PendingTrigger").value_or(0);
        preference.recent_triggers =
            KeptTriggers(ReadBinary(key, L"RecentTriggers", kKeptTriggers * 8));
        if (const auto pin = ReadText(key, L"SignerPin"); pin && !pin->empty()) {
            preference.pin = ParseDigest(std::wstring_view(*pin));
            preference.pin_invalid = !preference.pin.has_value();
        }
        if (const auto release = ReadText(key, L"NotifiedRelease")) {
            for (const wchar_t c : *release) {
                preference.notified_release.push_back(c < 0x80 ? static_cast<char>(c) : '?');
            }
        }
    } catch (...) {
        // Out of memory: what was read so far stands.
    }
    RegCloseKey(key);
    return preference;
}

bool SaveUpdateSource(const std::wstring_view source, std::wstring& error) {
    return Write(L"Source", std::wstring(source), std::nullopt, error);
}

bool SaveUpdateAutoCheck(const bool on, std::wstring& error) {
    return Write(L"AutoCheck", std::nullopt, on ? 1U : 0U, error);
}

bool SaveUpdatePin(const std::wstring_view hex, std::wstring& error) {
    return Write(L"SignerPin", std::wstring(hex), std::nullopt, error);
}

bool SaveNotifiedRelease(const std::string_view release, std::wstring& error) {
    return Write(L"NotifiedRelease", std::wstring(release.begin(), release.end()), std::nullopt,
                 error);
}

bool SaveHostsTrigger(const bool on, std::wstring& error) {
    return Write(L"HostsTrigger", std::nullopt, on ? 1U : 0U, error);
}

bool SaveLastTrigger(const std::int64_t unix_ms, std::wstring& error) {
    return Write(L"LastTrigger", std::nullopt, std::nullopt, error,
                 static_cast<std::uint64_t>(unix_ms));
}

bool SavePendingTrigger(const std::uint64_t trigger, std::wstring& error) {
    if (trigger == 0) return Write(L"PendingTrigger", std::nullopt, std::nullopt, error);
    return Write(L"PendingTrigger", std::nullopt, std::nullopt, error, trigger);
}

bool SaveRecentTriggers(const std::vector<std::uint64_t>& triggers, std::wstring& error) {
    return Write(L"RecentTriggers", std::nullopt, std::nullopt, error, std::nullopt,
                 StoredTriggers(triggers));
}

}  // namespace gtg::update
