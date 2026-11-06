#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>

#include "peer/protocol.hpp"

// How the paired hosts read on this machine's dashboard: whether each is
// there, whether it needs the reader, and which four are shown.
namespace gtg::peer {

// A host sends once a second. Three seconds of silence is worth showing --
// a datagram or two lost, a busy network -- but not yet worth an alert; ten is
// a host that is gone.
inline constexpr std::uint64_t kDimAfterMs = 3'000;
inline constexpr std::uint64_t kLostAfterMs = 10'000;
inline constexpr std::size_t kShownHosts = 4;
inline constexpr std::size_t kMaxPairs = 16;

enum class Presence {
    Online,
    Dim,        // silent for a few seconds; its last state is still drawn
    Lost,       // silent without a goodbye: a crash, a TDR, a power cut
    Offline,    // said goodbye: exit, sign-out, sleep
    NotHeard,   // nothing since this GTG started
};

// When this machine last heard each kind of datagram from a host, on its own
// monotonic clock.
struct Heard {
    std::optional<std::uint64_t> state_ms;
    std::optional<std::uint64_t> goodbye_ms;
};

[[nodiscard]] constexpr Presence PresenceAt(const Heard& heard,
                                            const std::uint64_t now_ms) noexcept {
    if (heard.goodbye_ms && (!heard.state_ms || *heard.goodbye_ms >= *heard.state_ms)) {
        return Presence::Offline;
    }
    // A host that has not been heard since this GTG started is not lost: it
    // may simply be switched off, and every machine that is off when this
    // one starts would otherwise be an alert.
    if (!heard.state_ms) return Presence::NotHeard;
    const std::uint64_t silent = now_ms > *heard.state_ms ? now_ms - *heard.state_ms : 0;
    if (silent > kLostAfterMs) return Presence::Lost;
    if (silent > kDimAfterMs) return Presence::Dim;
    return Presence::Online;
}

// Whether the host needs the reader: lost, or present with its protection not
// normal or any record raised by its own rules. The sender's conclusions are
// taken as sent, never recomputed here.
[[nodiscard]] inline bool InAlert(const Presence presence, const HostState* state) noexcept {
    if (presence == Presence::Lost) return true;
    if (presence != Presence::Online && presence != Presence::Dim) return false;
    if (state == nullptr) return false;
    if (state->protection == Protection::Warning || state->protection == Protection::SafePower ||
        state->protection == Protection::Fault) {
        return true;
    }
    return std::any_of(state->records.begin(), state->records.end(),
                       [](const RecordState& record) { return record.raised; });
}

struct Candidate {
    Presence presence{Presence::NotHeard};
    bool alert{};
};

struct Shown {
    std::array<std::size_t, kShownHosts> index{};
    std::size_t count{};
};

// Which hosts take the four places, from the pair list in the reader's order:
// hosts in alert first, then those present, then the rest. The chosen ones
// are then drawn in the reader's order, so a host does not move because
// another one changed state.
[[nodiscard]] inline Shown ChooseShown(const std::span<const Candidate> hosts) noexcept {
    const auto rank = [](const Candidate& host) {
        if (host.alert) return 0;
        if (host.presence == Presence::Online || host.presence == Presence::Dim) return 1;
        return 2;
    };
    Shown shown;
    for (int pass = 0; pass < 3 && shown.count < kShownHosts; ++pass) {
        for (std::size_t i = 0; i < hosts.size() && shown.count < kShownHosts; ++i) {
            if (rank(hosts[i]) == pass) shown.index[shown.count++] = i;
        }
    }
    std::sort(shown.index.begin(), shown.index.begin() + static_cast<std::ptrdiff_t>(shown.count));
    return shown;
}

// One of Tiny's four segments.
enum class Segment {
    Empty,     // no host in this place: nothing drawn
    Offline,   // the track only
    Online,    // a quiet fill
    Alert,     // raised: brightened, outlined, breathing
};

[[nodiscard]] constexpr Segment SegmentFor(const Candidate& host) noexcept {
    if (host.alert) return Segment::Alert;
    if (host.presence == Presence::Online || host.presence == Presence::Dim) return Segment::Online;
    return Segment::Offline;
}

// How long a lost host has been silent: 0:42, 12:05, 1:02:03.
[[nodiscard]] inline std::wstring SilenceText(const std::uint64_t silent_ms) {
    const std::uint64_t seconds = silent_ms / 1'000;
    const std::uint64_t hours = seconds / 3'600;
    const auto two = [](const std::uint64_t value) {
        std::wstring text = std::to_wstring(value);
        return value < 10 ? L"0" + text : text;
    };
    if (hours == 0) return std::to_wstring(seconds / 60) + L":" + two(seconds % 60);
    return std::to_wstring(hours) + L":" + two(seconds / 60 % 60) + L":" + two(seconds % 60);
}

}  // namespace gtg::peer
