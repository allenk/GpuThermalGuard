#pragma once

#include <array>
#include <cstdint>

#include "peer/protocol.hpp"

// Which sealed datagrams a receiver accepts from one paired host, so a
// datagram recorded on the network cannot be shown twice.
//
// Each sender starts a session with a random salt and numbers its datagrams;
// the receiver keeps the session it accepted and the last number. Within it,
// only a higher number is new. A new session is accepted only when its
// sender's clock is within two minutes of the receiver's and later than the
// last datagram accepted -- otherwise anyone who recorded an old session could
// replay it after the host restarts.
//
// What remains is a replay inside those two minutes after the receiver itself
// restarts: old numbers, drawn as current, for at most two minutes. For state
// that is only displayed, that is accepted rather than paying for a handshake.
//
// Every check here runs on the header before the tag is verified, so it only
// ever drops a datagram; the session moves only through Accept, after the tag
// has verified.
namespace gtg::peer {

inline constexpr std::int64_t kSessionWindowMs = 120'000;

enum class Verdict {
    Current,      // the session in use, a new number: open with its key
    NewSession,   // a session not seen before, inside the window: derive its key
    Replayed,     // a number already seen in the session in use
    OutOfWindow,  // a new session whose clock is too far from this one
    Older,        // a new session from before the one in use
};

class ReplayGuard {
public:
    [[nodiscard]] Verdict Classify(const SealedHeader& header,
                                   const std::int64_t now_ms) const noexcept {
        if (valid_ && header.salt == salt_) {
            return header.sequence > sequence_ ? Verdict::Current : Verdict::Replayed;
        }
        const std::int64_t skew = header.sent_ms - now_ms;
        if (skew > kSessionWindowMs || skew < -kSessionWindowMs) return Verdict::OutOfWindow;
        if (valid_ && header.sent_ms <= sent_ms_) return Verdict::Older;
        return Verdict::NewSession;
    }

    // After the tag has verified.
    void Accept(const SealedHeader& header) noexcept {
        valid_ = true;
        salt_ = header.salt;
        sequence_ = header.sequence;
        sent_ms_ = header.sent_ms;
    }

    void Forget() noexcept { *this = {}; }

    [[nodiscard]] bool HasSession() const noexcept { return valid_; }

private:
    bool valid_{};
    std::array<std::uint8_t, kSaltBytes> salt_{};
    std::uint64_t sequence_{};
    std::int64_t sent_ms_{};
};

}  // namespace gtg::peer
