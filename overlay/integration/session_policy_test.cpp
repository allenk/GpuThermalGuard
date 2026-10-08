#include "session_policy.hpp"
#include <cstdio>
#include <cstdlib>
using namespace gtg::overlay::integration;

void Check(bool b, const char* why) {
    if (!b) {
        std::fprintf(stderr, "FAIL %s\n", why);
        std::exit(1);
    }
}

int main() {
    SessionPolicy s;
    const Identity a{5, 10}, reused{5, 11};
    Check(s.Press(false, a) == Action::None, "disabled has no side effects");
    Check(s.Press(true, {}) == Action::Refuse, "missing identity refused");
    Check(s.Press(true, a) == Action::Start, "first press installs");
    Check(s.Press(true, a) == Action::None, "inflight deduplicated");
    Check(s.Press(true, reused) == Action::Refuse, "PID reuse never toggles old session");
    s.Ready(true);
    Check(s.Current() == State::Visible && s.Press(true, a) == Action::Hide, "second press hides");
    Check(s.Press(true, a) == Action::Show, "third press shows without injection");
    s.Ready(false);
    Check(s.Current() == State::Visible, "late result ignored");
    s.Retire();
    Check(s.Press(true, reused) == Action::Start, "new lifetime admitted after retire");
    s.Ready(false);
    Check(s.Current() == State::Failed && s.Press(true, reused) == Action::Refuse,
          "failure no blind reinjection");
    Check(ValidHotkey(1, 0x79) && !ValidHotkey(1, 0x7B) && ValidHotkey(8, 0x79) &&
              !ValidHotkey(0, 0x79),
          "hotkey validation");
    Check(!ValidHotkey(1, VK_SHIFT), "modifier-only key refused");
    s.Retire();
    (void)s.Press(true, a);
    s.Disable();
    s.Ready(true);
    Check(s.Current() == State::Hidden, "disable during install stays hidden on success");
    Check(s.Press(true, a) == Action::Show, "explicit press shows after canceled visibility");
    s.Fail();
    Check(s.Current() == State::Failed && s.Target() == a, "helper failure retains identity");
    std::puts("PASS session lifecycle and hotkey policy");
}
