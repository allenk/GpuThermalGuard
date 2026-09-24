#pragma once

#include <cstdint>
#include <string>

#include "net/verdict.hpp"

// Reading a program's own connections, kept behind a declaration that names no
// Win32 type -- the same arrangement `throughput_win32.hpp` uses, and for the
// same reason: the include order `iphlpapi.h` needs is contained in one .cpp
// rather than imposed on the tray.
namespace gtg::net {

enum class ProbeStatus {
    Ready,
    NoLibrary,        // iphlpapi absent, or an export missing
    NoForeground,     // nothing in the foreground to ask about
    NoConnections,    // the foreground program holds no TCP connections
    NotPermitted,     // extended statistics refused; needs Administrator
};

// One run of the action: enable, baseline, attempt, wait, read, judge.
//
// Deliberately not a background sampler. Enabling extended statistics on a
// connection is a one-way door -- Microsoft advises never disabling them,
// because another monitor watching the same connection would be disrupted --
// so this runs when a reader asks for it, on the handful of connections
// belonging to the program they are looking at, and never on all of them.
class Probe final {
public:
    Probe() = default;
    ~Probe();

    Probe(const Probe&) = delete;
    Probe& operator=(const Probe&) = delete;

    // Finds the foreground program's TCP connections, enables path statistics
    // on them and records a baseline. False when there is nothing to measure,
    // which `status()` explains.
    [[nodiscard]] bool Begin() noexcept;

    // The same, for a named process rather than the foreground one. A seam,
    // not a feature: the foreground at the moment a test runs is the test
    // harness, which owns no connections, so without this the only path that
    // could ever be exercised is the one that finds nothing.
    [[nodiscard]] bool BeginFor(std::uint32_t process_id) noexcept;

    // The attempt: discard the cached path state and re-resolve the next hop.
    //
    // Measured against a control and found to be below the noise -- see
    // docs/network-latency-research-20260924.md. It is kept because it is
    // documented, permitted and undone by the stack re-discovering what it
    // discarded, and because the owner asked for an attempt. Nothing that
    // reads this class may report it as an improvement.
    void Attempt() noexcept;

    // Reads again and differences against the baseline.
    [[nodiscard]] Verdict Finish() noexcept;

    [[nodiscard]] ProbeStatus status() const noexcept { return status_; }
    // How many of the program's connections were enabled, whether or not they
    // went on to measure anything.
    [[nodiscard]] int watched() const noexcept { return watched_; }

private:
    struct State;
    State* state_{nullptr};
    ProbeStatus status_{ProbeStatus::NoLibrary};
    int watched_{};
};

}  // namespace gtg::net
