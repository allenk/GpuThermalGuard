#pragma once
#include <atomic>
#include <cstddef>

namespace gtg::research {
enum class SiteResult { kInstalled, kRefused, kRecoveryRequired };
enum class AttachResult { kActive, kRefused, kPartialResident, kRecoveryRequired };
// installed counts only previously confirmed successful sites on recovery;
// the uncertain current site may ALSO be committed. Never use this count to
// decide whether unloading is safe: only kRefused permits clean unload.
struct AttachOutcome { AttachResult result; std::size_t installed; };

// Consumer policy, not a multi-site atomic transaction. Ops owns all storage
// and preserves uncertain state. Every installed hook calls through while the
// gate is closed. The caller must retain the DLL for every non-refusal result.
template<class Ops>
AttachOutcome AttachStaged(std::size_t count, std::atomic<bool>& gate, Ops& ops) {
    if (gate.load(std::memory_order_acquire)) return {AttachResult::kRecoveryRequired, 0};
    if (count == 0) return {AttachResult::kRefused, 0};
    for (std::size_t i = 0; i < count; ++i) {
        if (!ops.Prepare(i)) return {AttachResult::kRefused, 0};
    }
    for (std::size_t i = 0; i < count; ++i) {
        const auto result = ops.Install(i);
        if (result == SiteResult::kRecoveryRequired) return {AttachResult::kRecoveryRequired, i};
        if (result == SiteResult::kRefused)
            return {i ? AttachResult::kPartialResident : AttachResult::kRefused, i};
    }
    gate.store(true, std::memory_order_release);
    return {AttachResult::kActive, count};
}
}  // namespace gtg::research
