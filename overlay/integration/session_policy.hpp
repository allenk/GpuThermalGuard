#pragma once
#include <cstdint>
#include "../../src/settings/ui_preferences.hpp"
namespace gtg::overlay::integration {
struct Identity {
    std::uint32_t pid{};
    std::uint64_t created{};
    bool operator==(const Identity&) const = default;
};
enum class State { Empty, Installing, Visible, Hidden, Failed };
enum class Action { None, Start, Show, Hide, Refuse };
class SessionPolicy {
public:
    Action Press(bool enabled, Identity target) noexcept {
        if (!enabled) return Action::None;
        if (!target.pid || !target.created) return Action::Refuse;
        if (state_ == State::Empty) {
            target_ = target;
            state_ = State::Installing;
            want_visible_ = true;
            return Action::Start;
        }
        if (!(target == target_)) return Action::Refuse;
        if (state_ == State::Installing) return Action::None;
        if (state_ == State::Visible) {
            state_ = State::Hidden;
            return Action::Hide;
        }
        if (state_ == State::Hidden) {
            state_ = State::Visible;
            return Action::Show;
        }
        return Action::Refuse;
    }
    void Ready(bool success) noexcept {
        if (state_ == State::Installing)
            state_ = success ? (want_visible_ ? State::Visible : State::Hidden) : State::Failed;
    }
    void Retire() noexcept {
        state_ = State::Empty;
        target_ = {};
    }
    void Disable() noexcept {
        want_visible_ = false;
        if (state_ == State::Visible) state_ = State::Hidden;
    }
    void Fail() noexcept {
        if (state_ != State::Empty) state_ = State::Failed;
    }
    State Current() const noexcept { return state_; }
    Identity Target() const noexcept { return target_; }

private:
    State state_{};
    Identity target_{};
    bool want_visible_{};
};
constexpr bool ValidHotkey(unsigned modifiers, unsigned key) noexcept {
    return settings::IsAcceptableOverlayHotkey({modifiers, key});
}
}  // namespace gtg::overlay::integration
