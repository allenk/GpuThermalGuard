#pragma once
#include <windows.h>

namespace gtg::research {
// Controller scope only; never used around the game's Present call.
class DrawingGateGuard {
public:
    explicit DrawingGateGuard(volatile LONG* gate) : gate_(gate) {}
    ~DrawingGateGuard() { if (gate_ && !complete_) InterlockedExchange(gate_, 1); }
    DrawingGateGuard(const DrawingGateGuard&) = delete;
    DrawingGateGuard& operator=(const DrawingGateGuard&) = delete;
    void Complete() noexcept { complete_ = true; }
private:
    volatile LONG* gate_;
    bool complete_{};
};
}  // namespace gtg::research
