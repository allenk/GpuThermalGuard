#pragma once
#include <array>
#include <cstdint>
#include <optional>

namespace gtg::supervision {
constexpr bool IsFreshSample(std::uint64_t now, std::uint64_t completed) noexcept {
    return now >= completed && now - completed < 1000;
}

// A child is healthy while its heartbeat is fresh and it reports protection
// ready, or reports that this machine has no NVIDIA GPU. That counts only while
// this supervisor has pinned no GPU: after one was protected, "no GPU" is a
// driver being reset or reinstalled, and a fresh process is what brings
// protection back.
constexpr bool ChildHealthy(std::uint64_t now, std::uint64_t heartbeat, bool ready, bool no_gpu,
                            bool pinned) noexcept {
    const bool fresh = heartbeat != 0 && now >= heartbeat && now - heartbeat < 1500;
    return fresh && (ready || (no_gpu && !pinned));
}

// A child that monitored a machine without a GPU asks for a fresh start when
// one appears. With none pinned there is nothing to recover, so its
// replacement starts as on any start; anything else is a recovery.
constexpr bool RelaunchAsRecovery(bool fresh_start, bool pinned) noexcept {
    return !fresh_start || pinned;
}

class RecoveryPolicy {
public:
    unsigned NextDelayMs() noexcept {
        constexpr std::array<unsigned, 4> delays{2000, 5000, 10000, 30000};
        const auto result = delays[failures_];
        if (failures_ < delays.size() - 1) ++failures_;
        healthy_since_.reset();
        return result;
    }
    void Observe(std::uint64_t now, bool healthy) noexcept {
        if (!healthy) { healthy_since_.reset(); return; }
        if (!healthy_since_) healthy_since_ = now;
        if (now >= *healthy_since_ && now - *healthy_since_ >= 60000) failures_ = 0;
    }
private:
    std::size_t failures_{};
    std::optional<std::uint64_t> healthy_since_;
};

class Readiness {
public:
    bool Observe(std::uint64_t now, bool valid) noexcept {
        if (!valid || (last_ && (now <= *last_ || now - *last_ >= 1000))) count_ = 0;
        last_ = now;
        if (valid && count_ < 5) ++count_;
        return count_ >= 5;
    }
private:
    unsigned count_{};
    std::optional<std::uint64_t> last_;
};
}  // namespace gtg::supervision
