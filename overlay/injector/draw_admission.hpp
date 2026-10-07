#pragma once
#include <atomic>
#include <cstdint>
#include <type_traits>

namespace gtg::research {
struct DrawIdentity {
    std::uintptr_t chain{};
    std::uintptr_t device{};
    std::uint32_t thread{};
    bool operator==(const DrawIdentity&) const = default;
};
enum class DrawAdmission { kDisabled, kInvalid, kBusy, kMismatch, kExecuted };
class DrawAdmissionGate {
public:
    // Identity tokens must stay alive/stable for the renderer's lifetime.
    // This serializes our own drawing only, never the host's context users.
    template <typename Draw>
    DrawAdmission Run(bool enabled, DrawIdentity identity, Draw&& draw) noexcept {
        static_assert(std::is_nothrow_invocable_v<Draw&>);
        if (!enabled) return DrawAdmission::kDisabled;
        if (!identity.chain || !identity.device || !identity.thread)
            return DrawAdmission::kInvalid;
        if (busy_.test_and_set(std::memory_order_acquire))
            return DrawAdmission::kBusy;
        const Release release{busy_};
        if (!bound_) {
            owner_ = identity;
            bound_ = true;
        }
        if (!(identity == owner_)) return DrawAdmission::kMismatch;
        draw();
        return DrawAdmission::kExecuted;
    }
private:
    struct Release {
        std::atomic_flag& flag;
        ~Release() { flag.clear(std::memory_order_release); }
    };
    std::atomic_flag busy_ = ATOMIC_FLAG_INIT;
    DrawIdentity owner_{};
    bool bound_{};
};
}  // namespace gtg::research
