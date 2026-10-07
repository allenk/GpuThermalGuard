#pragma once
#include <windows.h>
#include <cstdint>
#include <format>
#include <string_view>
#include "logging/logger.hpp"

namespace gtg::timing {
inline constexpr std::uint64_t kSlowUiPhaseMs = 100;
[[nodiscard]] constexpr bool IsSlowUiPhase(std::uint64_t start, std::uint64_t end) noexcept {
    return end >= start && end - start >= kSlowUiPhaseMs;
}

// UI-only attribution, through the existing bounded deferred logger. Never
// installed on the dedicated protection worker or game Present callbacks.
class SlowUiScope final {
public:
    explicit SlowUiScope(std::wstring_view stage) noexcept : stage_(stage), start_(GetTickCount64()) {}
    ~SlowUiScope() noexcept {
        const auto saved_error = GetLastError();
        const auto end = GetTickCount64();
        if (IsSlowUiPhase(start_, end)) {
            try {
                (void)logging::TryWarning(std::format(
                    L"UI slow phase: stage={} elapsed_ms={} start_tick_ms={}", stage_, end - start_, start_));
            } catch (...) {}
        }
        SetLastError(saved_error);
    }
    SlowUiScope(const SlowUiScope&) = delete;
    SlowUiScope& operator=(const SlowUiScope&) = delete;
private:
    std::wstring_view stage_;
    std::uint64_t start_;
};
}
