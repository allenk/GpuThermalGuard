#pragma once

#include <filesystem>
#include <optional>

#include "update/digest.hpp"

// The signer pin: an optional HKLM setting naming the one certificate (by the
// SHA-256 of its DER bytes) whose signature a staged EXE or DLL must carry.
//
// WinVerifyTrust checks the signature over the file's bytes whether or not the
// certificate chains to a trusted root; a changed byte fails it. So the pin
// carries the trust, and the certificate need not be imported anywhere. A
// signer compared without WinVerifyTrust would accept a changed file.
namespace gtg::update {

enum class SignerVerdict {
    Pinned,        // a valid signature by the pinned certificate
    Unsigned,      // no signature at all
    Invalid,       // a signature that does not verify (changed, malformed, ...)
    OtherSigner,   // valid, by some other certificate
    Unavailable,   // wintrust could not be loaded
};

struct SignerResult {
    SignerVerdict verdict{SignerVerdict::Unavailable};
    // WinVerifyTrust's answer.
    long status{};
    // The signer's certificate, when there was one.
    std::optional<Digest> signer;
};

// Flags as the overlay helper uses them: no UI, no revocation, cache-only
// retrieval, and the state closed on every path. Accepted: a status of 0, or
// CERT_E_UNTRUSTEDROOT for a self-signed certificate nobody imported -- and
// then only when the signer is the pinned one.
[[nodiscard]] SignerResult VerifySigner(const std::filesystem::path& file, const Digest& pin,
                                        const Sha256& sha);

}  // namespace gtg::update
