#pragma once

namespace gtg {

class RestorePromptGate {
public:
    [[nodiscard]] bool ShouldPrompt(const bool safe_latched, const bool ready_to_restore) noexcept {
        if (!safe_latched) {
            offered_for_latch_ = false;
            return false;
        }
        if (!ready_to_restore || offered_for_latch_) return false;
        offered_for_latch_ = true;
        return true;
    }

    void Reset() noexcept { offered_for_latch_ = false; }

private:
    bool offered_for_latch_{false};
};

}  // namespace gtg
