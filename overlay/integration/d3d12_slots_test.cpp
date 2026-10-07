#include "d3d12_slots.hpp"
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <thread>

using namespace gtg::overlay::integration;
void Check(bool result, const char* message) {
    if (!result) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); }
}
int main() {
    constexpr auto removed = std::numeric_limits<std::uint64_t>::max();
    D3d12Slots slots;
    const auto first = slots.Reserve(0);
    Check(first == 0, "idle slot can be reserved without waiting");
    Check(slots.Submitted(first, 3, true), "successful GPU Signal records pending work");
    const auto second = slots.Reserve(0);
    Check(second == 1, "pending slot cannot be reused");
    Check(slots.Submitted(second, 4, true), "second private slot submits");
    const auto third = slots.Reserve(0);
    Check(third == 2, "fixed third slot");
    Check(slots.Submitted(third, 5, true), "third private slot submits");
    Check(slots.Reserve(2) == -1, "all GPU-busy slots skip drawing");
    Check(slots.Reserve(3) == first, "completed fence permits exact slot reuse");
    Check(!slots.Submitted(first, 3, true), "fence values must advance strictly");
    Check(slots.Failed(), "invalid completion publication is terminal");

    D3d12Slots resize;
    const auto old = resize.Reserve(0);
    Check(resize.Submitted(old, 9, true), "old generation submitted");
    const auto generation = resize.Generation();
    resize.Invalidate();
    Check(resize.Generation() == generation + 1, "resize changes generation");
    Check(resize.Reserve(9) == -1, "invalidation stops new submissions");
    Check(!resize.Resume(8), "pending work forbids old-resource retirement");
    Check(resize.Resume(9), "completed old work permits new generation");
    Check(resize.Reserve(9) == 0, "resumed generation has idle slots");

    D3d12Slots uncertain;
    const auto failed = uncertain.Reserve(0);
    Check(!uncertain.Submitted(failed, 1, false), "failed Signal cannot prove completion");
    Check(uncertain.Quarantined() == 1, "uncertain work is retained in bounded quarantine");
    Check(uncertain.Reserve(removed - 1) == -1, "failure stops all submissions");
    uncertain.Invalidate();
    Check(!uncertain.Resume(removed - 1), "later unrelated fence cannot clear uncertainty");

    D3d12Slots loss;
    Check(loss.Reserve(removed) == -1, "device removal is not fence completion");
    Check(loss.Failed(), "removed device becomes terminal");
    D3d12Slots submitted_loss;
    for (int i = 0; i < D3d12Slots::kSlots; ++i)
        Check(submitted_loss.Submitted(submitted_loss.Reserve(0), i + 1, true),
              "all slots submitted before device loss");
    Check(submitted_loss.Reserve(removed) == -1, "removed pending fence never frees a slot");
    Check(submitted_loss.Quarantined() == D3d12Slots::kSlots,
          "device loss quarantines every owned in-flight slot");
    Check(!submitted_loss.Resume(removed), "device loss cannot resume a generation");
    D3d12Slots overflow;
    Check(!overflow.Submitted(overflow.Reserve(0), removed, true), "reserved removal value refused");

    D3d12Slots invalid;
    Check(!invalid.Submitted(-1, 1, true), "invalid index safely refused");
    Check(invalid.Failed(), "contract misuse fails closed");

    D3d12Slots canceled;
    const auto reservation = canceled.Reserve(0);
    canceled.Invalidate();
    Check(!canceled.Resume(0), "outstanding CPU reservation prevents retirement");
    Check(canceled.Cancel(reservation), "unsubmitted reservation can be canceled");
    Check(canceled.Resume(0), "canceled reservation allows safe policy resume");
    Check(!canceled.Cancel(reservation), "idle slot cannot be canceled twice");
    const auto high = canceled.Reserve(0);
    Check(canceled.Submitted(high, removed - 1, true), "largest valid fence accepted");
    Check(!canceled.Cancel(high), "submitted slot cannot be canceled");
    canceled.Invalidate();
    Check(canceled.Resume(removed - 1), "largest ordinary completed value is valid");
    Check(!canceled.Submitted(canceled.Reserve(removed - 1), 1, true),
          "generation transition does not reset monotonic fence values");

    std::atomic_flag admission = ATOMIC_FLAG_INIT;
    {
        D3d12Admission owner(admission);
        Check(static_cast<bool>(owner), "first callback admitted");
        bool contender = true;
        std::thread worker([&] {
            D3d12Admission other(admission);
            contender = static_cast<bool>(other);
        });
        worker.join();
        Check(!contender, "other caller skips without waiting");
        D3d12Admission recursive(admission);
        Check(!recursive, "recursive callback skips");
    }
    D3d12Admission later(admission);
    Check(static_cast<bool>(later), "owner scope releases admission");
    std::puts("PASS D3D12 bounded nonblocking slot and admission contracts");
}
