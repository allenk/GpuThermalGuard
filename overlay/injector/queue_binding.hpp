#pragma once
#include <cstdint>
#include <atomic>
namespace gtg::research {
struct QueueObservation {
    void* queue{};
    void* chain{};
    std::uintptr_t queue_id{}, chain_id{}, queue_device_id{}, chain_device_id{};
    unsigned depth{};
    bool direct{};
};
enum class BindingResult { kIgnored, kBound, kAmbiguous };
class QueueBinding {
public:
    template<class Ops> BindingResult Offer(const QueueObservation& value, Ops& ops) noexcept {
        if (Ambiguous()) return BindingResult::kAmbiguous;
        if (!value.depth || !value.direct) return BindingResult::kIgnored;
        if (value.depth != 1 || !value.queue || !value.chain || !value.queue_id || !value.chain_id ||
            !value.queue_device_id || value.queue_device_id != value.chain_device_id) {
            MarkAmbiguous(); return BindingResult::kAmbiguous;
        }
        if (ready_.load(std::memory_order_acquire)) return Match(value);
        if (publishing_.test_and_set(std::memory_order_acquire)) {
            // Never wait or discard a potentially different one-shot identity.
            // Even matching contention conservatively disables future drawing.
            MarkAmbiguous(); return BindingResult::kAmbiguous;
        }
        if (!ready_.load(std::memory_order_acquire)) {
            // All immutable fields and references precede release publication.
            // The DLL intentionally retains these references until process exit.
            ops.Retain(value.queue, value.chain);
            value_ = value;
            ready_.store(true, std::memory_order_release);
        }
        publishing_.clear(std::memory_order_release);
        return Match(value);
    }
    bool Ready() const noexcept { return ready_.load(std::memory_order_acquire) && !Ambiguous(); }
    bool Ambiguous() const noexcept { return ambiguous_.load(std::memory_order_acquire); }
    void MarkAmbiguous() noexcept { ambiguous_.store(true, std::memory_order_release); }
    void* Queue() const noexcept { return Ready() ? value_.queue : nullptr; }
    std::uintptr_t ChainId() const noexcept { return Ready() ? value_.chain_id : 0; }
private:
    BindingResult Match(const QueueObservation& value) noexcept {
        if (Ambiguous()) return BindingResult::kAmbiguous;
        if (value.queue_id != value_.queue_id || value.chain_id != value_.chain_id ||
            value.queue_device_id != value_.queue_device_id) {
            MarkAmbiguous(); return BindingResult::kAmbiguous;
        }
        return BindingResult::kBound;
    }
    std::atomic_flag publishing_ = ATOMIC_FLAG_INIT;
    std::atomic<bool> ready_{false}, ambiguous_{false};
    QueueObservation value_{}; // Immutable once ready_; readers never take a lock.
};
}
