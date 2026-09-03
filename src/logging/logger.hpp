#pragma once

#include <filesystem>
#include <string_view>

namespace gtg::logging {

enum class Role {
    Tray,
    Service,
};

// Initializes a process-wide UTF-8 text journal. The logger first tries the
// executable directory, then falls back to the conventional per-user/per-
// machine application-data location.
[[nodiscard]] bool Initialize(Role role) noexcept;
void Shutdown() noexcept;

void Info(std::wstring_view message) noexcept;
void Warning(std::wstring_view message) noexcept;
void Error(std::wstring_view message) noexcept;

[[nodiscard]] std::filesystem::path Path() noexcept;

}  // namespace gtg::logging
