#pragma once

#include <cstdint>

namespace gtg::ipc {

inline constexpr wchar_t kPipeName[] = L"\\\\.\\pipe\\GpuThermalGuard.v2";
inline constexpr std::uint32_t kMagic = 0x47544731;  // GTG1
inline constexpr std::uint32_t kVersion = 2;

enum class Command : std::uint32_t { Query = 1, RestoreNormalPower = 2 };
enum ResponseFlags : std::uint32_t {
    SafeLatched = 1U << 0,
    ReadyToRestore = 1U << 1,
    LastCommandSucceeded = 1U << 2,
};

struct Request {
    std::uint32_t magic{kMagic};
    std::uint32_t version{kVersion};
    Command command{Command::Query};
};

struct Response {
    std::uint32_t magic{kMagic};
    std::uint32_t version{kVersion};
    std::uint32_t protection_state{};
    std::uint32_t flags{};
    std::int32_t temperature_c{-1};
    std::int32_t normal_power_w{};
    std::int32_t safe_power_w{};
    std::int32_t trigger_temperature_c{};
    std::uint32_t trigger_count{};
    std::uint32_t win32_error{};
};

static_assert(sizeof(Request) == 12);
static_assert(sizeof(Response) == 40);

[[nodiscard]] inline bool IsValid(const Request& request) noexcept {
    return request.magic == kMagic && request.version == kVersion;
}

[[nodiscard]] inline bool IsValid(const Response& response) noexcept {
    return response.magic == kMagic && response.version == kVersion;
}

}  // namespace gtg::ipc
