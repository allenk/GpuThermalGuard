#pragma once

#include <array>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <string_view>

#include "peer/crypto.hpp"
#include "peer/protocol.hpp"

// Which key protects what, and the sealing of state and goodbye datagrams.
//
//   code key       HKDF(code secret, "GTG hosts code v1"): MACs pairing
//   pair key       HKDF(ECDH secret, salt = code secret, both public keys and
//                  ids): stored under DPAPI, never sent
//   direction key  HKDF(pair key, salt = sender's session salt,
//                  "GTG hosts v1" || sender id || receiver id): one per
//                  sender, receiver and session
//
// A direction key encrypts with a nonce of 32 zero bits and the 64-bit
// sequence. The salt is fresh for every session and the sequence only rises
// within one, so no key ever sees a nonce twice.
namespace gtg::peer {

namespace key_detail {

inline constexpr std::string_view kCodeInfo = "GTG hosts code v1";
inline constexpr std::string_view kCodeIdInfo = "GTG hosts code id";
inline constexpr std::string_view kPairInfo = "GTG hosts pair v1";
inline constexpr std::string_view kDirectionInfo = "GTG hosts v1";

[[nodiscard]] inline Bytes Text(const std::string_view text) noexcept {
    return {reinterpret_cast<const std::uint8_t*>(text.data()), text.size()};
}

[[nodiscard]] inline std::array<std::uint8_t, 8> Id(const std::uint64_t id) noexcept {
    std::array<std::uint8_t, 8> bytes{};
    for (std::size_t i = 0; i < 8; ++i)
        bytes[i] = static_cast<std::uint8_t>(id >> (8 * i));
    return bytes;
}

}  // namespace key_detail

// Comparing secrets without stopping at the first difference.
[[nodiscard]] inline bool SameBytes(const Bytes a, const Bytes b) noexcept {
    if (a.size() != b.size()) return false;
    std::uint8_t difference = 0;
    for (std::size_t i = 0; i < a.size(); ++i)
        difference |= static_cast<std::uint8_t>(a[i] ^ b[i]);
    return difference == 0;
}

[[nodiscard]] inline std::optional<Key> CodeKey(const Crypto& crypto,
                                                const std::array<std::uint8_t, 16>& secret) {
    return crypto.Hkdf(secret, {}, key_detail::Text(key_detail::kCodeInfo));
}

// Names the code a request is for, so a request for an older code is not
// counted against the live one.
[[nodiscard]] inline std::optional<std::array<std::uint8_t, kCodeIdBytes>> CodeId(
    const Crypto& crypto, const Key& code_key) {
    const auto digest = crypto.Hmac(code_key, {key_detail::Text(key_detail::kCodeIdInfo)});
    if (!digest) return std::nullopt;
    std::array<std::uint8_t, kCodeIdBytes> id{};
    std::memcpy(id.data(), digest->data(), id.size());
    return id;
}

[[nodiscard]] inline std::optional<Key> PairKey(const Crypto& crypto, const Key& shared,
                                                const std::array<std::uint8_t, 16>& secret,
                                                const std::array<std::uint8_t, 64>& joiner_public,
                                                const std::array<std::uint8_t, 64>& issuer_public,
                                                const std::uint64_t joiner,
                                                const std::uint64_t issuer) {
    std::array<std::uint8_t, key_detail::kPairInfo.size() + 64 + 64 + 8 + 8> info{};
    std::size_t at = 0;
    const auto put = [&](const Bytes bytes) {
        std::memcpy(info.data() + at, bytes.data(), bytes.size());
        at += bytes.size();
    };
    put(key_detail::Text(key_detail::kPairInfo));
    put(joiner_public);
    put(issuer_public);
    put(key_detail::Id(joiner));
    put(key_detail::Id(issuer));
    return crypto.Hkdf(shared, secret, info);
}

[[nodiscard]] inline std::optional<Key> DirectionKey(
    const Crypto& crypto, const Key& pair_key, const std::array<std::uint8_t, kSaltBytes>& salt,
    const std::uint64_t sender, const std::uint64_t receiver) {
    std::array<std::uint8_t, key_detail::kDirectionInfo.size() + 16> info{};
    std::memcpy(info.data(), key_detail::kDirectionInfo.data(), key_detail::kDirectionInfo.size());
    const auto from = key_detail::Id(sender);
    const auto to = key_detail::Id(receiver);
    std::memcpy(info.data() + key_detail::kDirectionInfo.size(), from.data(), 8);
    std::memcpy(info.data() + key_detail::kDirectionInfo.size() + 8, to.data(), 8);
    return crypto.Hkdf(pair_key, salt, info);
}

[[nodiscard]] inline std::array<std::uint8_t, kNonceBytes> Nonce(
    const std::uint64_t sequence) noexcept {
    std::array<std::uint8_t, kNonceBytes> nonce{};
    for (std::size_t i = 0; i < 8; ++i)
        nonce[4 + i] = static_cast<std::uint8_t>(sequence >> (8 * i));
    return nonce;
}

// A state datagram, or nothing if sealing failed.
[[nodiscard]] inline std::optional<std::array<std::uint8_t, kStateBytes>> SealState(
    const Crypto& crypto, const Key& direction, SealedHeader header, const HostState& state) {
    header.type = Type::State;
    std::array<std::uint8_t, kStateBytes> datagram{};
    const auto head = WriteSealedHeader(header);
    std::memcpy(datagram.data(), head.data(), head.size());
    auto body = WriteStateBody(state);
    const auto out = std::span(datagram).subspan(kSealedHeaderBytes, kStateBodyBytes);
    const auto tag = std::span(datagram).subspan<kSealedHeaderBytes + kStateBodyBytes, kTagBytes>();
    const bool sealed = crypto.Seal(direction, Nonce(header.sequence), head, body, out, tag);
    std::memset(body.data(), 0, body.size());
    if (!sealed) return std::nullopt;
    return datagram;
}

[[nodiscard]] inline std::optional<std::array<std::uint8_t, kGoodbyeBytes>> SealGoodbye(
    const Crypto& crypto, const Key& direction, SealedHeader header) {
    header.type = Type::Goodbye;
    std::array<std::uint8_t, kGoodbyeBytes> datagram{};
    const auto head = WriteSealedHeader(header);
    std::memcpy(datagram.data(), head.data(), head.size());
    const auto tag = std::span(datagram).subspan<kSealedHeaderBytes, kTagBytes>();
    if (!crypto.Seal(direction, Nonce(header.sequence), head, {}, {}, tag)) return std::nullopt;
    return datagram;
}

#ifdef GTG_FEATURE_UPDATE
// An update datagram, or nothing if sealing failed. The body is the updater's;
// nothing here reads it.
template <std::size_t Bytes>
[[nodiscard]] inline std::optional<std::array<std::uint8_t, kSealedHeaderBytes + Bytes + kTagBytes>>
SealUpdate(const Crypto& crypto, const Key& direction, SealedHeader header, const Type type,
           const std::array<std::uint8_t, Bytes>& body) {
    header.type = type;
    std::array<std::uint8_t, kSealedHeaderBytes + Bytes + kTagBytes> datagram{};
    if (datagram.size() != DatagramBytes(type)) return std::nullopt;
    const auto head = WriteSealedHeader(header);
    std::memcpy(datagram.data(), head.data(), head.size());
    const auto out = std::span(datagram).subspan(kSealedHeaderBytes, Bytes);
    const auto tag = std::span(datagram).template subspan<kSealedHeaderBytes + Bytes, kTagBytes>();
    if (!crypto.Seal(direction, Nonce(header.sequence), head, body, out, tag)) return std::nullopt;
    return datagram;
}
#endif

// Whether a sealed datagram's tag verifies under `direction`, and its state
// if it is a state datagram. The body is not looked at until the tag has
// verified.
struct Opened {
    bool authentic{};
    std::optional<HostState> state;   // a state datagram whose fields all check
#ifdef GTG_FEATURE_UPDATE
    // An update datagram's body, unread.
    std::optional<update::hosts::StatusBody> update_status;
    std::optional<update::hosts::TriggerBody> update_trigger;
#endif
};

[[nodiscard]] inline Opened OpenSealed(const Crypto& crypto, const Key& direction,
                                       const SealedHeader& header,
                                       const std::span<const std::uint8_t> datagram) {
    if (datagram.size() != DatagramBytes(header.type)) return {};
    const std::size_t body_bytes = datagram.size() - kSealedHeaderBytes - kTagBytes;
    std::array<std::uint8_t, kStateBodyBytes> body{};
    const auto plain = std::span(body).first(body_bytes);
    const std::span<const std::uint8_t, kTagBytes> tag(
        datagram.data() + datagram.size() - kTagBytes, kTagBytes);
    if (!crypto.Open(direction, Nonce(header.sequence), datagram.first(kSealedHeaderBytes),
                     datagram.subspan(kSealedHeaderBytes, body_bytes), tag, plain)) {
        return {};
    }
    Opened opened;
    opened.authentic = true;
    if (header.type == Type::State) opened.state = ReadStateBody(plain);
#ifdef GTG_FEATURE_UPDATE
    if (header.type == Type::UpdateStatus) {
        update::hosts::StatusBody body_copy{};
        std::memcpy(body_copy.data(), plain.data(), body_copy.size());
        opened.update_status = body_copy;
    } else if (header.type == Type::UpdateTrigger) {
        update::hosts::TriggerBody body_copy{};
        std::memcpy(body_copy.data(), plain.data(), body_copy.size());
        opened.update_trigger = body_copy;
    }
#endif
    std::memset(body.data(), 0, body.size());
    return opened;
}

}  // namespace gtg::peer
