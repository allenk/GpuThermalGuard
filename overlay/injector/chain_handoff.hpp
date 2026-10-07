#pragma once
#include "draw_admission.hpp"
#include <limits>

namespace gtg::research {
struct ChainStamp {
    std::uint64_t session{}, generation{};
    std::uint32_t version{1}, reserved{};
    bool operator==(const ChainStamp&) const = default;
};
enum class StampState { kAbsent, kPresent, kError };
struct StampRead {
    StampState state{StampState::kError}; ChainStamp stamp{};
    std::int32_t result{}; std::uint32_t size{};
    bool operator==(const StampRead&) const = default;
};
struct ChainFacts {
    std::uintptr_t window{}; bool valid{}, fullscreen{};
    std::uint32_t width{}, height{}, format{};
    bool operator==(const ChainFacts&) const = default;
};
enum class HandoffResult { kDraw, kDisabled, kBusy, kInvalid, kIdentity,
    kFacts, kSameMode, kStamp, kWrite, kAmbiguous, kOverflow, kPending };
struct HandoffDecision {
    DrawIdentity owner{}, candidate{};
    ChainFacts facts{}; StampRead tag{};
    std::uint64_t generation{};
    HandoffResult result{};
};
constexpr std::uint64_t NextChainGeneration(std::uint64_t value) noexcept {
    return value == std::numeric_limits<std::uint64_t>::max() ? 0 : value + 1;
}

// IO owns COM references. This policy owns no graphics resources.
class ChainHandoffGate {
public:
    explicit ChainHandoffGate(std::uint64_t session) noexcept : session_(session) {}
    // Worker only, before any caller can enter Run.
    void SetSession(std::uint64_t session) noexcept { session_ = session; }
    // A COM identity query can fail before an IO candidate exists. Do not let
    // that callback count as an uninterrupted qualification sequence.
    template <typename IO>
    void CancelPending(IO& io) noexcept {
        static_assert(noexcept(io.ClearPending()));
        if (busy_.test_and_set(std::memory_order_acquire)) return;
        struct Release { std::atomic_flag& flag; ~Release() { flag.clear(std::memory_order_release); } } release{busy_};
        ResetPending(io);
    }
    template <typename IO>
    HandoffResult Run(bool enabled, bool allow_handoff, DrawIdentity id, IO& io) noexcept {
        static_assert(noexcept(io.Inspect()) && noexcept(io.ReadStamp()) &&
            noexcept(io.WriteStamp(ChainStamp{})) && noexcept(io.CommitOwner(false)) &&
            noexcept(io.Invalidate()) && noexcept(io.Draw()) && noexcept(io.ClearPending()) &&
            noexcept(io.HoldPending()) && noexcept(io.RecordDecision(HandoffDecision{})));
        if (busy_.test_and_set(std::memory_order_acquire)) return HandoffResult::kBusy;
        struct Release { std::atomic_flag& flag; ~Release() { flag.clear(std::memory_order_release); } } release{busy_};
        const auto refuse = [&](HandoffResult result) noexcept { ResetPending(io); return result; };
        if (!enabled) return refuse(HandoffResult::kDisabled);
        if (!session_ || !id.chain || !id.device || !id.thread) return refuse(HandoffResult::kInvalid);
        if (ambiguous_) return HandoffResult::kAmbiguous;
        const bool bound = generation_ != 0;
        const bool changed = bound && id.chain != owner_.chain;
        if (bound && (id.device != owner_.device || id.thread != owner_.thread))
            return refuse(HandoffResult::kIdentity);
        if (!allow_handoff) ResetPending(io);
        if (changed && !allow_handoff) return HandoffResult::kDisabled;
        const auto facts = io.Inspect();
        if (!facts.valid || !facts.window || (bound && facts.window != window_)) return refuse(HandoffResult::kFacts);
        if (!bound || changed) {
            auto tag = io.ReadStamp();
            const auto latch = [&](HandoffResult reason, ChainFacts observed, StampRead observed_tag) noexcept {
                ambiguous_ = true;
                io.RecordDecision({owner_,id,observed,observed_tag,generation_.load(),reason});
                return refuse(reason);
            };
            if (tag.state == StampState::kError) return refuse(HandoffResult::kStamp);
            if (!ValidTag(tag)) return latch(HandoffResult::kAmbiguous,facts,tag);
            if (changed) {
                if (!(pending_ == id) || !(pending_facts_ == facts) || !(pending_tag_ == tag)) {
                    ResetPending(io);
                    io.HoldPending();
                    pending_ = id; pending_facts_ = facts; pending_tag_ = tag; samples_ = 1;
                } else { ++samples_; }
                if (samples_ < 3) return HandoffResult::kPending;
                // Revalidate at commit, not just on the first probation callback.
                const auto final_facts = io.Inspect();
                const auto final_tag = io.ReadStamp();
                if (final_tag.state == StampState::kError) return refuse(HandoffResult::kStamp);
                if (!ValidTag(final_tag)) return latch(HandoffResult::kAmbiguous,final_facts,final_tag);
                if (!(final_facts == facts)) return refuse(HandoffResult::kFacts);
                if (!(final_tag == tag)) return refuse(HandoffResult::kStamp);
                tag = final_tag;
            }
            const auto next = NextChainGeneration(generation_);
            if (!next) return latch(HandoffResult::kOverflow,facts,tag);
            if (!io.WriteStamp({session_, next})) return refuse(HandoffResult::kWrite);
            // From successful stamp to owner publication: no allocation/API/failure.
            const auto previous = owner_;
            owner_ = id; window_ = facts.window; generation_ = next;
            fullscreen_ = facts.fullscreen;
            io.CommitOwner(changed);
            ResetPending(io);
            if (changed) io.RecordDecision({previous,id,facts,tag,next,HandoffResult::kDraw});
        } else {
            ResetPending(io);
            if (fullscreen_ != facts.fullscreen) io.Invalidate();
            fullscreen_ = facts.fullscreen;
        }
        io.Draw();
        return HandoffResult::kDraw;
    }
    std::uint64_t Generation() const noexcept { return generation_; }
private:
    bool ValidTag(const StampRead& tag) const noexcept {
        if (tag.state == StampState::kAbsent) return true;
        const auto& s = tag.stamp;
        return tag.state == StampState::kPresent && s.version == 1 && s.reserved == 0 &&
            s.session == session_ && s.generation > 0 && s.generation < generation_.load();
    }
    template <typename IO> void ResetPending(IO& io) noexcept {
        pending_ = {}; pending_facts_ = {}; pending_tag_ = {}; samples_ = 0;
        io.ClearPending();
    }
    std::atomic_flag busy_ = ATOMIC_FLAG_INIT;
    DrawIdentity owner_{};
    DrawIdentity pending_{};
    ChainFacts pending_facts_{};
    StampRead pending_tag_{};
    unsigned samples_{};
    std::uintptr_t window_{};
    bool fullscreen_{}, ambiguous_{};
    std::uint64_t session_{};
    std::atomic<std::uint64_t> generation_{};
};
} // namespace gtg::research
