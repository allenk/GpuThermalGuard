#pragma once
#include <array>
#include <cstdint>
#include <optional>

namespace gtg::supervision {
constexpr bool IsFreshSample(std::uint64_t now, std::uint64_t completed) noexcept {
    return now >= completed && now - completed < 1000;
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
