#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

// SHA-256 for the updater: file identities, the release identity and the
// signer pin. CNG is loaded from System32 when a hasher is made, so the
// import table does not change and a machine without it simply cannot update.
namespace gtg::update {

using Digest = std::array<std::uint8_t, 32>;

// Upper-case hex, which is what the publish script writes.
[[nodiscard]] std::string ToHex(std::span<const std::uint8_t> bytes);

// Exactly 64 hex digits, either case; anything else is nothing.
[[nodiscard]] std::optional<Digest> ParseDigest(std::string_view hex) noexcept;
[[nodiscard]] std::optional<Digest> ParseDigest(std::wstring_view hex) noexcept;

class Sha256 {
public:
    Sha256() noexcept;
    ~Sha256();

    Sha256(const Sha256&) = delete;
    Sha256& operator=(const Sha256&) = delete;

    [[nodiscard]] bool Available() const noexcept { return algorithm_ != nullptr; }

    [[nodiscard]] std::optional<Digest> Of(std::span<const std::uint8_t> bytes) const noexcept;

    [[nodiscard]] std::optional<Digest> Of(std::string_view text) const noexcept {
        return Of(std::span(reinterpret_cast<const std::uint8_t*>(text.data()), text.size()));
    }

private:
    void* module_{};
    void* algorithm_{};
    void* hash_{};
    void* close_{};
};

}  // namespace gtg::update
