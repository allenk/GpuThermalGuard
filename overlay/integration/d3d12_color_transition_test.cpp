#include "d3d12_color_transition.hpp"
#include <cstdio>
#include <cstdlib>
using namespace gtg::overlay::integration;

namespace {
void Check(bool value, const char* message) {
    if (!value) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        std::exit(1);
    }
}

struct Clock {
    D3d12LifecycleGate& gate;
    UINT64 time{};
    bool release_draw = true;

    UINT64 Now() noexcept { return time; }

    void Pause() noexcept {
        Check(!gate.TryDraw(), "reserved color transition rejects competing Present");
        time += 40;
        if (release_draw) {
            gate.EndDraw();
            release_draw = false;
        }
    }
};

struct Ops {
    D3d12LifecycleGate& gate;
    bool drain_ok = true, resume_ok = true;
    unsigned drains{}, resumes{};
    DWORD budget{};
    LONG reason{-202};

    void RefuseColor(LONG value) noexcept {
        if (reason == -202) reason = value;
    }

    bool BeforeColor(DWORD remaining) noexcept {
        ++drains;
        budget = remaining;
        Check(!gate.TryDraw(), "GPU drain owns drawing exclusion");
        return drain_ok;
    }

    bool AfterColor(HRESULT result) noexcept {
        ++resumes;
        Check(SUCCEEDED(result) && !gate.TryDraw(), "resume is exclusive and original succeeded");
        if (!resume_ok) RefuseColor(-272);
        return resume_ok;
    }

    void AbortColor() noexcept {}
};
}

int main() {
    D3d12LifecycleGate gate;
    Check(gate.TryDraw(), "already admitted Present fixture");
    Clock clock{gate};
    Ops ops{gate};
    unsigned calls{};
    const auto success = ForwardColorTransition(gate, clock, ops, [&]() noexcept {
        ++calls;
        Check(ops.drains == 1 && ops.budget == 210,
              "drain precedes original with remaining shared budget");
        Check(!gate.TryDraw(), "original color setter excludes Present");
        return S_FALSE;
    });
    Check(calls == 1 && success.original_result == S_FALSE && success.qualified && ops.resumes == 1,
          "qualified color preserves exact original HRESULT");
    Check(gate.TryDraw(), "successful color transition resumes drawing");
    gate.EndDraw();

    D3d12LifecycleGate timeout_gate;
    Check(timeout_gate.TryDraw(), "timeout active Present");
    Clock timeout{timeout_gate, 0, false};
    Ops untouched{timeout_gate};
    const auto expired = ForwardColorTransition(timeout_gate, timeout, untouched, [&]() noexcept {
        ++calls;
        return S_OK;
    });
    Check(expired.original_result == S_OK && !expired.qualified && untouched.drains == 0 &&
              untouched.resumes == 0,
          "timeout forwards original once without touching an unowned renderer");
    Check(untouched.reason == -251,
          "unowned color admission records timeout before forwarding original");
    timeout_gate.EndDraw();
    Check(timeout_gate.Disabled() && !timeout_gate.TryDraw(),
          "timeout cannot revive future Present");

    for (unsigned failure = 0; failure < 3; ++failure) {
        D3d12LifecycleGate failed_gate;
        Clock idle{failed_gate};
        Ops failed{failed_gate};
        failed.drain_ok = failure != 0;
        failed.resume_ok = failure != 2;
        const HRESULT original = failure == 1 ? E_INVALIDARG : S_OK;
        const auto rejected = ForwardColorTransition(failed_gate, idle, failed, [&]() noexcept {
            ++calls;
            return original;
        });
        Check(!rejected.qualified && rejected.original_result == original && failed_gate.Disabled(),
              "drain/setter/resume failures are terminal");
        Check(failed.resumes == (failure == 2 ? 1u : 0u),
              "unproven drain/failed setter never resume");
        const LONG reasons[] = {-251, -270, -272};
        Check(failed.reason == reasons[failure],
              "actual color forwarding records drain/original/rebuild refusal");
    }
    D3d12LifecycleGate blocked_gate;
    Clock blocked_clock{blocked_gate};
    Ops blocked{blocked_gate};
    const auto stopped =
        ForwardColorTransition(blocked_gate, blocked_clock, blocked, [&]() noexcept {
            ++calls;
            Clock competitor{blocked_gate};
            Check(!blocked_gate.BeginTransition(competitor),
                  "competing transition disables active generation");
            return S_OK;
        });
    Check(!stopped.qualified && stopped.original_result == S_OK && blocked.reason == -271 &&
              blocked.resumes == 0,
          "disabled gate short-circuit records blocked reason without touching rebuild");
    Check(calls == 6, "each original setter forwarded exactly once");
    std::puts("PASS color transition admission, drain, HRESULT and timeout contracts");
}
