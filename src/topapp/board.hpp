#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

namespace gtg::topapp {

// The four records a name can be attached to.
enum class Record : std::uint8_t { Gpu = 0, Vram = 1, Cpu = 2, Ram = 3 };
inline constexpr std::size_t kRecordCount = 4;

[[nodiscard]] constexpr std::uint32_t Bit(const Record record) noexcept {
    return 1U << static_cast<unsigned>(record);
}

// A name on screen, and when it appeared: the marquee is a function of the
// time since then, so a changed name starts its loop from the beginning.
struct Shown {
    std::wstring name;
    std::uint64_t since_ms{};

    [[nodiscard]] bool operator==(const Shown&) const = default;
};

// What the OSD draws: per record, the name to float over its curve, if any.
using Board = std::array<std::optional<Shown>, kRecordCount>;

// Carries a newly published name onto the board. The time a name first
// appeared is kept while it stays the same, and a changed name starts over.
inline void Carry(std::optional<Shown>& slot, const std::optional<std::wstring>& name,
                  const std::uint64_t now_ms) {
    if (!name) {
        slot.reset();
    } else if (!slot || slot->name != *name) {
        slot = Shown{*name, now_ms};
    }
}

}  // namespace gtg::topapp
