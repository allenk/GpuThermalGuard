#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace gtg::topapp {

// The trace session's name: the program's own, then the logon session's
// number. One name per program and logon session, so a name left by a crashed
// copy of the same program is its to reclaim, while another program -- the
// test runner beside a running GTG -- never stops GTG's trace by reclaiming
// it. GTG's own name is `GpuThermalGuard.Network.Session.<n>`.
[[nodiscard]] std::wstring NetworkSessionName(std::wstring_view module_path,
                                              std::uint32_t logon_session);

// Bytes each process sends and receives, for Details' network column.
//
// Windows keeps no per-process network counter; Task Manager and Resource
// Monitor count the Microsoft-Windows-Kernel-Network trace's send and receive
// events, and so does this. A real-time ETW session and one consumer thread,
// in background mode, exist only between Start and Stop -- the sampler starts
// them when Details opens and stops them when it closes -- so a GTG with
// Details closed carries none of it.
//
// Measured before it was built, on a 32-thread machine: at 42 k events a
// second the consumer took at most about 0.2 % of one core, and a loopback
// transfer at 1.6 GB/s ran at the same speed and CPU cost with the trace as
// without it. The callback copies two numbers
// and adds them to a table; nothing is decoded through TDH.
//
// Enabling the provider needs Administrator, which the Tray has. Without it
// Start returns false and the column says it cannot be measured.
class NetworkMeter {
public:
    NetworkMeter();
    ~NetworkMeter();

    NetworkMeter(const NetworkMeter&) = delete;
    NetworkMeter& operator=(const NetworkMeter&) = delete;

    // Starts the session and its consumer. False, with nothing left running,
    // when Windows refuses or a resource is missing.
    [[nodiscard]] bool Start() noexcept;
    // Stops the session and joins the consumer: stopping the session is what
    // ends the consumer's ProcessTrace, so the join is bounded.
    void Stop() noexcept;

    [[nodiscard]] bool Running() const noexcept;

    // The consumer stopped on its own while the meter still runs: the session
    // was stopped from outside, or Windows ended its delivery. Nothing more is
    // counted until Stop and Start begin again.
    struct End {
        // What ProcessTrace returned.
        std::uint32_t status{};
        // Whether a session of this name still exists: if not, it was
        // stopped; if so, only the delivery ended.
        bool session_exists{};
    };

    [[nodiscard]] std::optional<End> Ended() const noexcept;

    // This process's session name.
    [[nodiscard]] static std::wstring SessionName();

    struct Taken {
        // Bytes sent and received together, per process id.
        std::vector<std::pair<std::uint32_t, std::uint64_t>> bytes_by_pid;
        // Events the session dropped since the previous Take.
        std::uint32_t lost{};
    };

    // Everything counted since the previous Take, which it clears.
    [[nodiscard]] Taken Take();

private:
    struct State;
    std::unique_ptr<State> state_;
};

}  // namespace gtg::topapp
