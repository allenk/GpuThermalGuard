#pragma once
// Targets an attach failed on, remembered outside the four drawing slots.
//
// A failed session used to keep its slot until its process exited, so four
// long-lived programs that cannot take an overlay -- a terminal, a browser --
// left no slot for a game (owner, 2026-10-08). The identity is what has to be
// kept, not the slot: it is what stops a retry on the same process.
//
// Losing an entry is safe. A process the overlay attached to keeps the hook in
// its Present, so a later attempt fails the pre-load byte check before
// anything is loaded. The list bounds memory; it is not the only guard.
#include "session_policy.hpp"
#include <algorithm>
#include <cstddef>
#include <vector>

namespace gtg::overlay::integration {

class RefusedTargets {
public:
    static constexpr std::size_t kCapacity = 32;

    void Add(const Identity& target) {
        if (!target.pid || Contains(target)) return;
        if (targets_.size() == kCapacity) targets_.erase(targets_.begin());
        targets_.push_back(target);
    }
    [[nodiscard]] bool Contains(const Identity& target) const noexcept {
        return std::find(targets_.begin(), targets_.end(), target) != targets_.end();
    }
    // Drops every target `alive` says has exited.
    template <class Alive>
    void Prune(Alive alive) {
        targets_.erase(std::remove_if(targets_.begin(), targets_.end(),
                                      [&](const Identity& t) { return !alive(t); }),
                       targets_.end());
    }
    [[nodiscard]] std::size_t Size() const noexcept { return targets_.size(); }

private:
    std::vector<Identity> targets_;
};

}  // namespace gtg::overlay::integration
