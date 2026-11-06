#pragma once

#include <cstdint>
#include <iterator>
#include <map>
#include <optional>

namespace gtg::topapp {

// A process, as opposed to a process id: Windows reuses ids, and a delta taken
// across two different processes that happened to share one is a CPU figure
// no process ever produced.
struct Identity {
    std::uint32_t pid{};
    std::uint64_t created{};   // FILETIME of creation, as one number

    [[nodiscard]] constexpr bool operator==(const Identity&) const noexcept = default;

    [[nodiscard]] constexpr bool operator<(const Identity& other) const noexcept {
        return pid != other.pid ? pid < other.pid : created < other.created;
    }
};

// Per-process CPU usage between two sweeps, the way Task Manager reports it:
// busy time over wall time over logical processors, so 100 means every
// processor of the machine.
class CpuLedger {
public:
    // Starts a sweep at `now_ms` (a monotonic clock in milliseconds).
    void BeginSweep(const std::uint64_t now_ms) noexcept {
        wall_ms_ = has_sweep_ && now_ms > sweep_ms_ ? now_ms - sweep_ms_ : 0;
        sweep_ms_ = now_ms;
        has_sweep_ = true;
        ++generation_;
    }

    // `busy_100ns` is the process's kernel plus user time. Nothing on first
    // sight, across a reused id, or if the counter went backwards.
    std::optional<double> Update(const Identity identity, const std::uint64_t busy_100ns,
                                 const unsigned logical_processors) {
        auto& entry = entries_[identity];
        const bool known = entry.generation != 0 && entry.generation + 1 == generation_;
        const std::uint64_t before = entry.busy_100ns;
        entry.busy_100ns = busy_100ns;
        entry.generation = generation_;
        if (!known || busy_100ns < before || wall_ms_ == 0 || logical_processors == 0) {
            return std::nullopt;
        }
        const double busy_ms = static_cast<double>(busy_100ns - before) / 10'000.0;
        return 100.0 * busy_ms /
               (static_cast<double>(wall_ms_) * static_cast<double>(logical_processors));
    }

    // Forgets every process this sweep did not see.
    void EndSweep() {
        for (auto it = entries_.begin(); it != entries_.end();) {
            it = it->second.generation == generation_ ? std::next(it) : entries_.erase(it);
        }
    }

    void Reset() noexcept {
        entries_.clear();
        has_sweep_ = false;
        wall_ms_ = 0;
    }

private:
    struct Entry {
        std::uint64_t busy_100ns{};
        std::uint64_t generation{};
    };

    std::map<Identity, Entry> entries_;
    std::uint64_t sweep_ms_{};
    std::uint64_t wall_ms_{};
    std::uint64_t generation_{};
    bool has_sweep_{};
};

}  // namespace gtg::topapp
