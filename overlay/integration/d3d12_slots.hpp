#pragma once
#include <array>
#include <atomic>
#include <cstdint>
#include <limits>

namespace gtg::overlay::integration {
class D3d12Slots {
public:
    // Pure ownership policy, not a GPU resource owner. Call only while holding
    // D3d12Admission. Completion must come from this renderer's private fence.
    static constexpr int kSlots = 3;
    int Reserve(std::uint64_t completed) noexcept {
        if (!Observe(completed) || !enabled_) return -1;
        for (int i = 0; i < kSlots; ++i) {
            auto& slot = slots_[i];
            if (slot.state == State::Pending && completed >= slot.fence)
                slot.state = State::Idle;
            if (slot.state != State::Idle) continue;
            slot.state = State::Reserved;
            return i;
        }
        return -1;
    }
    // Call after ExecuteCommandLists, then the GPU-side Signal result. Failed
    // publication cannot be rolled back: submitted resources may still be live.
    bool Submitted(int index, std::uint64_t fence, bool signaled) noexcept {
        if (failed_ || index < 0 || index >= kSlots ||
            slots_[index].state != State::Reserved || !signaled ||
            fence <= last_fence_ || fence == kRemoved) {
            Fail();
            return false;
        }
        slots_[index] = {State::Pending, fence};
        last_fence_ = fence;
        return true;
    }
    // Valid only before ExecuteCommandLists; never cancels submitted work.
    bool Cancel(int index) noexcept {
        if (failed_ || index < 0 || index >= kSlots ||
            slots_[index].state != State::Reserved) return false;
        slots_[index].state = State::Idle;
        return true;
    }
    void Invalidate() noexcept {
        enabled_ = false;
        if (generation_ == kRemoved) { Fail(); return; }
        ++generation_;
    }
    bool Resume(std::uint64_t completed) noexcept {
        if (!Observe(completed)) return false;
        for (const auto& slot : slots_)
            if (slot.state == State::Reserved || slot.state == State::Quarantined ||
                (slot.state == State::Pending && completed < slot.fence)) return false;
        // Caller may retire only the resources proven complete here, then build
        // the new generation. This policy does not itself release COM objects.
        for (auto& slot : slots_) slot = {};
        enabled_ = true;
        return true;
    }
    bool Failed() const noexcept { return failed_; }
    unsigned Quarantined() const noexcept {
        unsigned count = 0;
        for (const auto& slot : slots_) if (slot.state == State::Quarantined) ++count;
        return count;
    }
    std::uint64_t Generation() const noexcept { return generation_; }

private:
    static constexpr auto kRemoved = std::numeric_limits<std::uint64_t>::max();
    enum class State { Idle, Reserved, Pending, Quarantined };
    struct Slot { State state = State::Idle; std::uint64_t fence = 0; };
    bool Observe(std::uint64_t completed) noexcept {
        if (completed == kRemoved) Fail();
        return !failed_;
    }
    void Fail() noexcept {
        enabled_ = false;
        failed_ = true;
        for (auto& slot : slots_)
            if (slot.state == State::Reserved || slot.state == State::Pending)
                slot.state = State::Quarantined;
    }
    std::array<Slot, kSlots> slots_{};
    std::uint64_t last_fence_ = 0;
    std::uint64_t generation_ = 1;
    bool enabled_ = true;
    bool failed_ = false;
};
class D3d12Admission {
public:
    explicit D3d12Admission(std::atomic_flag& flag) noexcept
        : flag_(flag), acquired_(!flag.test_and_set(std::memory_order_acquire)) {}
    ~D3d12Admission() noexcept {
        if (acquired_) flag_.clear(std::memory_order_release);
    }
    D3d12Admission(const D3d12Admission&) = delete;
    D3d12Admission& operator=(const D3d12Admission&) = delete;
    explicit operator bool() const noexcept { return acquired_; }
private:
    std::atomic_flag& flag_;
    bool acquired_;
};
}  // namespace gtg::overlay::integration
