#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>

#include "peer/crypto.hpp"
#include "peer/pairing_code.hpp"
#include "peer/protocol.hpp"

// Pairing: a code shown on one GTG (the issuer) and pasted into another (the
// joiner) becomes a pair key both of them keep.
//
//   joiner -> issuer  pair-request: ephemeral public key, id, name; MAC under
//                     the code key
//   issuer -> joiner  pair-accept: ephemeral public key, id, name; MAC under
//                     the code key over the accept and the joiner's key
//
// Each side derives the pair key from the ECDH secret, salted by the code's
// secret: someone who later finds the code in a clipboard history cannot
// derive it without an ephemeral private key, and someone who has only the
// network traffic cannot without the code.
//
// A code is spent by the first good request, after ten minutes, or after five
// bad MACs. A later request that proves it holds the code is told why it
// failed, so a code someone else redeemed first is noticed rather than
// silently failing.
namespace gtg::peer {

inline constexpr std::uint64_t kCodeLifetimeMs = 10ULL * 60 * 1'000;
inline constexpr int kBadMacLimit = 5;

// Who this GTG is to its pairs.
struct Self {
    std::uint64_t id{};
    std::wstring name;
};

// The outcome of a pairing, on either side.
struct NewPair {
    std::uint64_t peer{};
    std::wstring name;
    Key key{};
};

// A code this GTG has shown, and what has been done with it.
class Invitation {
public:
    enum class Status { Live, Spent, Expired, Burned };

    // A fresh code for `address`:`port`. Nothing if randomness or the key
    // derivation failed.
    [[nodiscard]] static std::optional<Invitation> Create(
        const Crypto& crypto, const std::array<std::uint8_t, 4>& address, std::uint16_t port,
        std::uint64_t now_ms);

    struct Answer {
        // What to send back, if anything. Nothing for a request that does not
        // prove it holds this code: no one learns anything by guessing.
        std::optional<std::array<std::uint8_t, kPairAcceptBytes>> reply;
        // Set once, for the request that spent the code.
        std::optional<NewPair> paired;
    };

    // `datagram` is the request as received, of which `request` is the parse.
    [[nodiscard]] Answer Handle(const Crypto& crypto, const Self& self, const PairRequest& request,
                                std::span<const std::uint8_t> datagram, std::uint64_t now_ms);

    [[nodiscard]] Status StatusAt(std::uint64_t now_ms) const noexcept;

    [[nodiscard]] const PairingCode& Code() const noexcept { return code_; }

    [[nodiscard]] std::uint64_t ExpiresMs() const noexcept { return created_ms_ + kCodeLifetimeMs; }

    [[nodiscard]] int BadMacs() const noexcept { return bad_macs_; }

    // The joiner's name once the code is spent.
    [[nodiscard]] const std::wstring& JoinedBy() const noexcept { return joined_by_; }

private:
    [[nodiscard]] std::optional<std::array<std::uint8_t, kPairAcceptBytes>> Refuse(
        const Crypto& crypto, const Self& self, const PairRequest& request,
        AcceptStatus status) const;

    PairingCode code_;
    Key code_key_{};
    std::array<std::uint8_t, kCodeIdBytes> code_id_{};
    std::uint64_t created_ms_{};
    int bad_macs_{};
    bool spent_{};
    std::array<std::uint8_t, kPublicKeyBytes> joiner_public_{};
    std::array<std::uint8_t, kPairAcceptBytes> accept_{};
    std::wstring joined_by_;
};

// A code this GTG is redeeming.
class Joining {
public:
    // Nothing if the code is this GTG's own (a GTG does not pair with itself
    // -- caught here by id once the accept names the issuer) or a key could
    // not be made.
    [[nodiscard]] static std::optional<Joining> Start(const Crypto& crypto, const Self& self,
                                                      const PairingCode& code);

    // Sent until an answer comes, once a second.
    [[nodiscard]] const std::array<std::uint8_t, kPairRequestBytes>& Request() const noexcept {
        return request_;
    }

    [[nodiscard]] const PairingCode& Code() const noexcept { return code_; }

    enum class Outcome { Ignored, Paired, Used, Expired, Self };

    struct Result {
        Outcome outcome{Outcome::Ignored};
        std::optional<NewPair> paired;
    };

    [[nodiscard]] Result Handle(const Crypto& crypto, const Self& self, const PairAccept& accept,
                                std::span<const std::uint8_t> datagram) const;

private:
    PairingCode code_;
    Key code_key_{};
    EphemeralKey ephemeral_;
    std::array<std::uint8_t, kPairRequestBytes> request_{};
};

}  // namespace gtg::peer
