#pragma once

#include <cstdint>
#include <string>

#include "core/protection.hpp"
#include "localization/localization.hpp"

namespace gtg::settings {

struct LoadResult {
    ProtectionConfig config{};
    bool loaded{false};
    std::wstring warning;
};

struct OsdPreference {
    bool enabled{false};
    bool has_position{false};
    int x{};
    int y{};
};

struct WindowPosition {
    bool has_position{false};
    int x{};
    int y{};
};

struct TriggerTestRun {
    bool loaded{false};
    std::uint32_t baseline{};
    std::uint64_t started_file_time{};
};

[[nodiscard]] constexpr bool ResolveFpsPreference(
    bool has_valid_value, std::uint32_t value) noexcept {
    return !has_valid_value || value != 0;
}

// Same rule as FPS: an absent value enables the record.
[[nodiscard]] constexpr bool ResolveRamPreference(
    bool has_valid_value, std::uint32_t value) noexcept {
    return !has_valid_value || value != 0;
}

[[nodiscard]] LoadResult Load();
[[nodiscard]] bool Save(const ProtectionConfig& config, std::wstring& error);
[[nodiscard]] bool LoadSafeLatch() noexcept;
[[nodiscard]] bool SaveSafeLatch(bool latched, std::wstring& error);
[[nodiscard]] unsigned int LoadTriggerCount() noexcept;
[[nodiscard]] bool SaveTriggerCount(unsigned int count, std::wstring& error);
[[nodiscard]] TriggerTestRun LoadTriggerTestRun() noexcept;
[[nodiscard]] bool SaveTriggerTestRun(std::uint32_t baseline,
                                      std::uint64_t started_file_time,
                                      std::wstring& error);
[[nodiscard]] bool LoadCloseToTrayPreference() noexcept;
[[nodiscard]] bool SaveCloseToTrayPreference(bool enabled, std::wstring& error);
[[nodiscard]] OsdPreference LoadOsdPreference() noexcept;
[[nodiscard]] bool SaveOsdEnabled(bool enabled, std::wstring& error);
[[nodiscard]] bool SaveOsdPosition(int x, int y, std::wstring& error);
[[nodiscard]] bool LoadFpsEnabled() noexcept;
[[nodiscard]] bool SaveFpsEnabled(bool enabled, std::wstring& error);
[[nodiscard]] bool LoadRamEnabled() noexcept;
[[nodiscard]] bool SaveRamEnabled(bool enabled, std::wstring& error);
[[nodiscard]] WindowPosition LoadMainWindowPosition() noexcept;
[[nodiscard]] bool SaveMainWindowPosition(int x, int y, std::wstring& error);
[[nodiscard]] localization::UiLanguage LoadUiLanguagePreference() noexcept;
[[nodiscard]] bool SaveUiLanguagePreference(localization::UiLanguage language,
                                            std::wstring& error);

}  // namespace gtg::settings
