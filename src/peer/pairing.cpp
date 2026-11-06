#include "peer/pairing.hpp"

#include <cstring>

#include "peer/keys.hpp"

namespace gtg::peer {
namespace {

// What an accept's MAC covers: the accept up to its MAC, then the joiner's
// public key, so an accept answers one request and no other.
std::optional<Digest> AcceptMac(const Crypto& crypto, const Key& code_key,
                                const std::span<const std::uint8_t> accept,
                                const std::array<std::uint8_t, kPublicKeyBytes>& joiner_public) {
    return crypto.Hmac(code_key, {MacInput(accept), joiner_public});
}

}  // namespace

std::optional<Invitation> Invitation::Create(const Crypto& crypto,
                                             const std::array<std::uint8_t, 4>& address,
                                             const std::uint16_t port, const std::uint64_t now_ms) {
    Invitation invitation;
    invitation.code_.address = address;
    invitation.code_.port = port;
    if (!crypto.Random(invitation.code_.secret)) return std::nullopt;
    const auto code_key = CodeKey(crypto, invitation.code_.secret);
    if (!code_key) return std::nullopt;
    const auto code_id = CodeId(crypto, *code_key);
    if (!code_id) return std::nullopt;
    invitation.code_key_ = *code_key;
    invitation.code_id_ = *code_id;
    invitation.created_ms_ = now_ms;
    return invitation;
}

Invitation::Status Invitation::StatusAt(const std::uint64_t now_ms) const noexcept {
    if (spent_) return Status::Spent;
    if (bad_macs_ >= kBadMacLimit) return Status::Burned;
    if (now_ms - created_ms_ >= kCodeLifetimeMs) return Status::Expired;
    return Status::Live;
}

std::optional<std::array<std::uint8_t, kPairAcceptBytes>> Invitation::Refuse(
    const Crypto& crypto, const Self& self, const PairRequest& request,
    const AcceptStatus status) const {
    PairAccept refusal;
    refusal.issuer = self.id;
    refusal.status = status;
    refusal.joiner = request.joiner;
    auto bytes = WritePairAccept(refusal);
    const auto mac = AcceptMac(crypto, code_key_, bytes, request.public_key);
    if (!mac) return std::nullopt;
    std::memcpy(bytes.data() + kPairAcceptBytes - kMacBytes, mac->data(), kMacBytes);
    return bytes;
}

Invitation::Answer Invitation::Handle(const Crypto& crypto, const Self& self,
                                      const PairRequest& request,
                                      const std::span<const std::uint8_t> datagram,
                                      const std::uint64_t now_ms) {
    if (request.code_id != code_id_ || datagram.size() != kPairRequestBytes) return {};
    const auto mac = crypto.Hmac(code_key_, {MacInput(datagram)});
    if (!mac || !SameBytes(*mac, request.mac)) {
        if (StatusAt(now_ms) == Status::Live) ++bad_macs_;
        return {};
    }

    // From here the sender holds the code.
    switch (StatusAt(now_ms)) {
        case Status::Spent:
            // The joiner did not hear the accept and asks again: the same
            // answer. Anyone else is told the code is used.
            if (SameBytes(request.public_key, joiner_public_)) return {accept_, std::nullopt};
            return {Refuse(crypto, self, request, AcceptStatus::Used), std::nullopt};
        case Status::Burned:
            return {Refuse(crypto, self, request, AcceptStatus::Used), std::nullopt};
        case Status::Expired:
            return {Refuse(crypto, self, request, AcceptStatus::Expired), std::nullopt};
        case Status::Live:
            break;
    }
    if (request.joiner == self.id) return {};
    const auto name = ReadNameField(request.name);
    if (!name) return {};

    auto ephemeral = crypto.GenerateEphemeral();
    if (!ephemeral) return {};
    const auto shared = crypto.Agree(*ephemeral, request.public_key);
    if (!shared) return {};
    const auto pair_key = PairKey(crypto, *shared, code_.secret, request.public_key,
                                  ephemeral->PublicKey(), request.joiner, self.id);
    if (!pair_key) return {};

    PairAccept accept;
    accept.issuer = self.id;
    accept.status = AcceptStatus::Accepted;
    accept.joiner = request.joiner;
    accept.public_key = ephemeral->PublicKey();
    accept.name = NameField(self.name);
    auto bytes = WritePairAccept(accept);
    const auto accept_mac = AcceptMac(crypto, code_key_, bytes, request.public_key);
    if (!accept_mac) return {};
    std::memcpy(bytes.data() + kPairAcceptBytes - kMacBytes, accept_mac->data(), kMacBytes);

    spent_ = true;
    joiner_public_ = request.public_key;
    accept_ = bytes;
    joined_by_ = *name;
    return {bytes, NewPair{request.joiner, *name, *pair_key}};
}

std::optional<Joining> Joining::Start(const Crypto& crypto, const Self& self,
                                      const PairingCode& code) {
    Joining joining;
    joining.code_ = code;
    const auto code_key = CodeKey(crypto, code.secret);
    if (!code_key) return std::nullopt;
    const auto code_id = CodeId(crypto, *code_key);
    if (!code_id) return std::nullopt;
    auto ephemeral = crypto.GenerateEphemeral();
    if (!ephemeral) return std::nullopt;
    joining.code_key_ = *code_key;
    joining.ephemeral_ = std::move(*ephemeral);

    PairRequest request;
    request.joiner = self.id;
    request.code_id = *code_id;
    request.public_key = joining.ephemeral_.PublicKey();
    request.name = NameField(self.name);
    joining.request_ = WritePairRequest(request);
    const auto mac = crypto.Hmac(joining.code_key_, {MacInput(joining.request_)});
    if (!mac) return std::nullopt;
    std::memcpy(joining.request_.data() + kPairRequestBytes - kMacBytes, mac->data(), kMacBytes);
    return joining;
}

Joining::Result Joining::Handle(const Crypto& crypto, const Self& self, const PairAccept& accept,
                                const std::span<const std::uint8_t> datagram) const {
    if (accept.joiner != self.id || datagram.size() != kPairAcceptBytes) return {};
    const auto mac = AcceptMac(crypto, code_key_, datagram, ephemeral_.PublicKey());
    if (!mac || !SameBytes(*mac, accept.mac)) return {};

    switch (accept.status) {
        case AcceptStatus::Used:
            return {Outcome::Used, std::nullopt};
        case AcceptStatus::Expired:
            return {Outcome::Expired, std::nullopt};
        case AcceptStatus::Accepted:
            break;
    }
    if (accept.issuer == self.id) return {Outcome::Self, std::nullopt};
    const auto name = ReadNameField(accept.name);
    if (!name) return {};
    const auto shared = crypto.Agree(ephemeral_, accept.public_key);
    if (!shared) return {};
    const auto pair_key = PairKey(crypto, *shared, code_.secret, ephemeral_.PublicKey(),
                                  accept.public_key, self.id, accept.issuer);
    if (!pair_key) return {};
    return {Outcome::Paired, NewPair{accept.issuer, *name, *pair_key}};
}

}  // namespace gtg::peer
