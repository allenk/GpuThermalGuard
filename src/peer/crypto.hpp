#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <span>
#include <vector>

// The cryptography Hosts needs, from Windows itself: CNG for randomness,
// HMAC-SHA256, ECDH P-256 and AES-256-GCM, and DPAPI for keys at rest.
//
// bcrypt.dll and crypt32.dll are loaded from System32 at run time, and only
// while Hosts is on, so the EXE's import table does not change and a GTG that
// never turns Hosts on never loads them. A missing export leaves Hosts
// unavailable rather than half working.
namespace gtg::peer {

inline constexpr std::size_t kKeyBytes = 32;
inline constexpr std::size_t kNonceBytes = 12;
using Key = std::array<std::uint8_t, kKeyBytes>;
using Digest = std::array<std::uint8_t, 32>;
using Bytes = std::span<const std::uint8_t>;

class Crypto;

// One side of an ECDH agreement, used once and destroyed.
class EphemeralKey {
public:
    EphemeralKey() = default;

    ~EphemeralKey();

    EphemeralKey(EphemeralKey&& other) noexcept;

    EphemeralKey& operator=(EphemeralKey&& other) noexcept;

    EphemeralKey(const EphemeralKey&) = delete;
    EphemeralKey& operator=(const EphemeralKey&) = delete;

    // X then Y, big-endian, as it goes on the wire.
    [[nodiscard]] const std::array<std::uint8_t, 64>& PublicKey() const noexcept { return public_; }

    [[nodiscard]] bool Valid() const noexcept { return handle_ != nullptr; }

private:
    friend class Crypto;
    const Crypto* owner_{};
    void* handle_{};
    std::array<std::uint8_t, 64> public_{};
};

class Crypto {
public:
    Crypto() = default;

    ~Crypto();

    Crypto(const Crypto&) = delete;
    Crypto& operator=(const Crypto&) = delete;

    // Loads both DLLs and opens the algorithm providers. False if anything is
    // missing.
    [[nodiscard]] bool Load() noexcept;

    [[nodiscard]] bool Loaded() const noexcept { return loaded_; }

    [[nodiscard]] bool Random(std::span<std::uint8_t> out) const noexcept;

    // HMAC-SHA256 over the parts in order, as if they were one message.
    [[nodiscard]] std::optional<Digest> Hmac(Bytes key, std::initializer_list<Bytes> parts) const;

    // HKDF-SHA256 (RFC 5869), the first 32 bytes of output.
    [[nodiscard]] std::optional<Key> Hkdf(Bytes input, Bytes salt, Bytes info) const;

    [[nodiscard]] std::optional<EphemeralKey> GenerateEphemeral() const;

    // The shared secret with the other side's public key: the X coordinate,
    // big-endian. Nothing if the key is not a point on the curve.
    [[nodiscard]] std::optional<Key> Agree(const EphemeralKey& mine, Bytes theirs) const;

    // AES-256-GCM. `out` is the plaintext's size; the tag is 16 bytes.
    [[nodiscard]] bool Seal(const Key& key, const std::array<std::uint8_t, kNonceBytes>& nonce,
                            Bytes associated, Bytes plaintext, std::span<std::uint8_t> out,
                            std::span<std::uint8_t, 16> tag) const;

    // False, and `out` zeroed, unless the tag verifies. Nothing in `out` may be
    // read before this returns true.
    [[nodiscard]] bool Open(const Key& key, const std::array<std::uint8_t, kNonceBytes>& nonce,
                            Bytes associated, Bytes ciphertext,
                            std::span<const std::uint8_t, 16> tag,
                            std::span<std::uint8_t> out) const;

    // DPAPI for the current user, with an entropy of GTG's own, so another
    // program running as the same user cannot unprotect the blob by asking
    // DPAPI alone.
    [[nodiscard]] std::optional<std::vector<std::uint8_t>> Protect(Bytes secret) const;

    [[nodiscard]] std::optional<std::vector<std::uint8_t>> Unprotect(Bytes blob) const;

    // For known-answer tests: an ECDH key from its private scalar and public
    // point, big-endian.
    [[nodiscard]] std::optional<EphemeralKey> ImportEphemeral(Bytes private_key, Bytes x,
                                                              Bytes y) const;

private:
    friend class EphemeralKey;
    struct Api;

    void DestroyKey(void* handle) const noexcept;
    std::optional<EphemeralKey> Finish(void* handle) const;

    bool loaded_{};
    void* bcrypt_{};
    void* crypt32_{};
    Api* api_{};
    void* aes_{};
    void* hmac_{};
    void* ecdh_{};
};

}  // namespace gtg::peer
