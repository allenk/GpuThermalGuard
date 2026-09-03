#pragma once

#include <format>
#include <string>
#include <string_view>

namespace gtg::localization {

enum class UiLanguage {
    TraditionalChinese,
    English,
};

inline UiLanguage current_language = UiLanguage::English;

inline void SetCurrent(const UiLanguage language) noexcept {
    current_language = language;
}

[[nodiscard]] inline UiLanguage Current() noexcept {
    return current_language;
}

[[nodiscard]] inline std::wstring_view Select(const std::wstring_view traditional_chinese,
                                               const std::wstring_view english) noexcept {
    return current_language == UiLanguage::English ? english : traditional_chinese;
}

[[nodiscard]] inline std::wstring_view RegistryValue(const UiLanguage language) noexcept {
    return language == UiLanguage::English ? L"en-US" : L"zh-TW";
}

[[nodiscard]] inline UiLanguage FromRegistryValue(const std::wstring_view value) noexcept {
    return value == L"zh-TW" ? UiLanguage::TraditionalChinese : UiLanguage::English;
}

template <typename... Args>
[[nodiscard]] std::wstring Format(const std::wstring_view traditional_chinese,
                                  const std::wstring_view english, Args&&... args) {
    return std::vformat(Select(traditional_chinese, english),
                        std::make_wformat_args(args...));
}

}  // namespace gtg::localization
