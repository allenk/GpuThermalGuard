#pragma once

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

[[nodiscard]] LoadResult Load();
[[nodiscard]] bool Save(const ProtectionConfig& config, std::wstring& error);
[[nodiscard]] bool LoadSafeLatch() noexcept;
[[nodiscard]] bool SaveSafeLatch(bool latched, std::wstring& error);
[[nodiscard]] unsigned int LoadTriggerCount() noexcept;
[[nodiscard]] bool SaveTriggerCount(unsigned int count, std::wstring& error);
[[nodiscard]] bool LoadCloseToTrayPreference() noexcept;
[[nodiscard]] bool SaveCloseToTrayPreference(bool enabled, std::wstring& error);
[[nodiscard]] OsdPreference LoadOsdPreference() noexcept;
[[nodiscard]] bool SaveOsdEnabled(bool enabled, std::wstring& error);
[[nodiscard]] bool SaveOsdPosition(int x, int y, std::wstring& error);
[[nodiscard]] localization::UiLanguage LoadUiLanguagePreference() noexcept;
[[nodiscard]] bool SaveUiLanguagePreference(localization::UiLanguage language,
                                            std::wstring& error);

}  // namespace gtg::settings
