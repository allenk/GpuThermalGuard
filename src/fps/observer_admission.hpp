#pragma once

#include <atomic>
#include <cstdint>

namespace gtg::fps {

enum class ModuleEvidence {
    DxgiModules,
    NoDxgiModules,
    AccessDenied,
    OtherFailure,
};

enum class ObserverAdmission { Reject, Normal, Probation };

[[nodiscard]] constexpr ObserverAdmission ChooseObserverAdmission(
    const ModuleEvidence evidence) noexcept {
    switch (evidence) {
    case ModuleEvidence::DxgiModules: return ObserverAdmission::Normal;
    case ModuleEvidence::AccessDenied: return ObserverAdmission::Probation;
    case ModuleEvidence::NoDxgiModules:
    case ModuleEvidence::OtherFailure: return ObserverAdmission::Reject;
    }
    return ObserverAdmission::Reject;
}

// Tray-observer-only admission timer. Evidence is released only by a
// correlated, actually displayed target frame, never by a module error or a
// raw DXGI Present. The ETW callback may call NoteDisplayed concurrently.
class ObserverProbation final {
public:
    static constexpr std::uint64_t kDurationMs = 5'000;
    static constexpr std::uint64_t kCooldownMs = 30'000;

    void Begin(const ObserverAdmission admission,
               const std::uint64_t now_ms) noexcept {
        started_ms_ = now_ms;
        probationary_ = admission == ObserverAdmission::Probation;
        displayed_.store(!probationary_, std::memory_order_release);
    }

    void NoteDisplayed() noexcept {
        displayed_.store(true, std::memory_order_release);
    }

    [[nodiscard]] bool CanPublish() const noexcept {
        return displayed_.load(std::memory_order_acquire);
    }

    [[nodiscard]] bool Expired(const std::uint64_t now_ms) const noexcept {
        return probationary_ && !CanPublish() && now_ms >= started_ms_ &&
            now_ms - started_ms_ >= kDurationMs;
    }

    [[nodiscard]] std::uint64_t RetryAt(const std::uint64_t now_ms) const noexcept {
        return now_ms + kCooldownMs;
    }

private:
    std::atomic<bool> displayed_{false};
    std::uint64_t started_ms_{};
    bool probationary_{};
};

}  // namespace gtg::fps
