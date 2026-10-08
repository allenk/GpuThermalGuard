// A failed attach keeps its identity, not its slot.
#include "refused_targets.hpp"

#include <cstdio>
#include <cstdlib>

namespace {
int failures = 0;
void Check(const bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL %s\n", message);
        ++failures;
    }
}
}  // namespace

int main() {
    using namespace gtg::overlay::integration;
    RefusedTargets refused;
    const Identity terminal{13096, 77};
    refused.Add(terminal);
    Check(refused.Contains(terminal), "a failed target is remembered");
    Check(!refused.Contains(Identity{13096, 78}), "a reused pid is a different process");
    refused.Add(terminal);
    Check(refused.Size() == 1, "remembered once");
    refused.Add(Identity{});
    Check(refused.Size() == 1, "no identity, nothing to remember");

    for (std::uint32_t pid = 1; pid <= RefusedTargets::kCapacity; ++pid)
        refused.Add(Identity{pid, 1});
    Check(refused.Size() == RefusedTargets::kCapacity, "bounded");
    Check(!refused.Contains(terminal), "the oldest goes first when full");
    Check(refused.Contains(Identity{RefusedTargets::kCapacity, 1}), "the newest stays");

    refused.Prune([](const Identity& t) { return t.pid % 2 == 0; });
    Check(refused.Size() == RefusedTargets::kCapacity / 2, "exited processes are dropped");
    Check(!refused.Contains(Identity{1, 1}) && refused.Contains(Identity{2, 1}),
          "only the exited ones");

    if (failures == 0) std::puts("PASS refused targets");
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
