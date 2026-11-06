#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

// How many datagrams the peer thread reads at all. Paired hosts send one a
// second; anything beyond the limits below is dropped unread, so a flood of
// datagrams costs the receiver a receive call each and never a key derivation
// or a decryption.
namespace gtg::peer {

struct Endpoint {
    std::array<std::uint8_t, 4> address{};   // IPv4, network order
    std::uint16_t port{};

    friend bool operator==(const Endpoint&, const Endpoint&) = default;
};

class Admission {
public:
    static constexpr int kPerSourcePerSecond = 8;
    static constexpr int kTotalPerSecond = 64;

    [[nodiscard]] bool Admit(const Endpoint& source, const std::uint64_t now_ms) noexcept {
        if (now_ms - window_ms_ >= 1'000 || now_ms < window_ms_) {
            window_ms_ = now_ms;
            total_ = 0;
            sources_ = 0;
        }
        if (total_ >= kTotalPerSecond) return false;
        // Every admitted datagram adds at most one source, so the table cannot
        // fill before the total does.
        for (std::size_t i = 0; i < sources_; ++i) {
            if (seen_[i].source == source) {
                if (seen_[i].count >= kPerSourcePerSecond) return false;
                ++seen_[i].count;
                ++total_;
                return true;
            }
        }
        seen_[sources_++] = {source, 1};
        ++total_;
        return true;
    }

private:
    struct Seen {
        Endpoint source;
        int count{};
    };

    std::uint64_t window_ms_{};
    int total_{};
    std::size_t sources_{};
    std::array<Seen, kTotalPerSecond> seen_{};
};

}  // namespace gtg::peer
