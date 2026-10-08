#include "d3d12_lifecycle_gate.hpp"
#include <cstdio>
#include <cstdlib>
#include <thread>
using namespace gtg::overlay::integration;

namespace {
void Check(bool value, const char* message) {
    if (!value) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        std::exit(1);
    }
}

struct Clock {
    std::uint64_t time = 0;
    D3d12LifecycleGate* gate = nullptr;
    bool release = false;
    bool fail_during_start = false;
    std::uint64_t pause_cost = 40;

    std::uint64_t Now() noexcept {
        if (fail_during_start) {
            fail_during_start = false;
            Clock prior{time, gate, false};
            Check(gate->BeginTransition(prior), "intervening transition fixture");
            gate->EndTransition(false);
        }
        return time;
    }

    void Pause() noexcept {
        Check(!gate->TryDraw(), "published transition rejects new draws while owner is busy");
        time += pause_cost;
        if (release) {
            gate->EndDraw();
            release = false;
        }
    }
};
}

int main() {
    D3d12LifecycleGate gate;
    Check(gate.TryDraw(), "idle draw admission");
    Check(!gate.TryDraw(), "recursive/competing draw skips immediately");
    Clock clock{0, &gate, true};
    Check(gate.BeginTransition(clock, 100), "transition waits only for active overlay owner");
    Check(gate.Remaining(clock) == 60, "admission consumes shared deadline");
    clock.time += 20;
    Check(gate.Remaining(clock) == 40, "pre-drain work also consumes shared deadline");
    Check(!gate.TryDraw(), "draw disabled during native resize/requalification");
    gate.EndTransition(true);
    Check(gate.TryDraw(), "qualified generation resumes");
    gate.EndDraw();
    Check(gate.BeginTransition(clock, 0) && gate.Remaining(clock) == 0,
          "zero-budget idle admission permits only completion poll");
    gate.EndTransition(false);
    Check(gate.Disabled() && !gate.TryDraw() && !gate.BeginTransition(clock),
          "failed generation never resumes");

    D3d12LifecycleGate busy;
    Check(busy.TryDraw(), "timeout fixture owner");
    Clock timeout{0, &busy, false};
    Check(!busy.BeginTransition(timeout, 100), "admission timeout does not steal active owner");
    Check(busy.Disabled() && !busy.TryDraw(), "timeout permanently prevents drawing");
    busy.EndDraw();
    Check(!busy.BeginTransition(timeout), "timeout cannot create replacement generation");

    D3d12LifecycleGate concurrent;
    Clock single{0, &concurrent, false};
    Check(concurrent.BeginTransition(single), "first transition owns state");
    std::thread competing([&] {
        Clock other{0, &concurrent, false};
        Check(!concurrent.BeginTransition(other), "second resize cannot steal transition state");
    });
    competing.join();
    concurrent.EndTransition(true);
    Check(concurrent.Disabled() && !concurrent.TryDraw(),
          "successful first resize cannot revive after conflicting transition");
    D3d12LifecycleGate delayed;
    Clock race{0, &delayed, false, true};
    Check(!delayed.BeginTransition(race),
          "delayed enabled check cannot admit after an intervening terminal failure");
    D3d12LifecycleGate late;
    Check(late.TryDraw(), "late release fixture");
    Clock expired{0, &late, true, false, 120};
    Check(!late.BeginTransition(expired, 100),
          "busy admission cannot acquire after its shared deadline");
    Check(late.Disabled() && !late.TryDraw(), "expired admission is terminal");
    std::puts("PASS draw try-admission and shared resize transition deadline");
}
