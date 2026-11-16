#pragma once
#include "entry_admission.hpp"
#include <splice/engine.h>
// For std::uintptr_t, which this header's own signature names. It had been
// relying on whatever included it having pulled the type in first, which is
// true of the one production caller and was not true of anything else.
#include <cstdint>

namespace gtg::research {
// This product refuses to inline-patch its hottest target unless the bytes
// being replaced lie inside a single instruction, so that no thread can be
// *inside* them: it is either at the entry or past all of them.
//
// That is stricter than splice requires -- the coordinated tier maps an
// interior program counter through the site's instruction-boundary map rather
// than refusing it -- and the stricter rule stays here on purpose. Relaxing a
// safe-admission rule is a decision recorded on this side, and a library that
// answered "may I patch this?" with this product's policy baked in would be
// deciding it in the wrong repository.
//
// splice reports the facts; this is the policy over them.

// `kEntryPatchBytes`, the five bytes written, is in `entry_admission.hpp` with
// the clause both sides evaluate over it. The comment that used to stand here
// claimed the fourteen-byte absolute form "does not arise"; that is a premise
// about the relay and it is stated as one where the constant now lives.

// Rewritten 2026-11-15, from a proxy to the invariant it was reaching for.
//
// It used to read:
//
//     if (facts.patch_bytes != 5) return false;
//     if ((address & 7) != 0) return false;
//     return facts.interior_boundaries == 0;
//
// Both of the first two lines stood in for something else, and both were
// stricter than the thing they stood for:
//
//   * `patch_bytes != 5` stood in for "the window is one instruction". It
//     refuses a first instruction of six or seven bytes, where the five-byte
//     patch lies entirely *inside* that one instruction -- a single-instruction
//     window, which is exactly what this rule wants to admit. `D3D12Core`'s
//     `Signal` taught this project that a prologue's encoding is code
//     generation and changes under it; `dxgi.dll`'s `Present` is on the same
//     footing and this was the line that would have refused it for being safe
//     in an unexpected way.
//   * `(address & 7) != 0` stood in for "the five bytes fit in one aligned
//     quadword", which is `(address & 7) + 5 <= 8`, i.e. an offset of three or
//     less. splice made exactly this correction inside its own atomic install
//     and says so where it lives: the aligned-quadword guarantee is the
//     hardware rule, and demanding a zero offset cost the atomic path on every
//     function a compiler had packed. Asking for more than the hardware rule
//     here does not buy safety; it refuses installs that are atomic.
//
// What it does NOT do is weaken the invariant. Every shape that straddles an
// instruction boundary is still refused -- `Signal`'s three-byte entry among
// them, which is why that site moved to a vtable cell swap instead. The rule
// now states the reason instead of a proxy for it.
inline bool OneInstructionEntry(const splice_prologue_facts& facts,
                                std::uintptr_t address) noexcept {
    // One atomic store: the five bytes must lie within one aligned quadword.
    // The same predicate the preflight calls, so that what this rule admits is
    // what the product admits. It was this clause, written out separately on
    // each side, that made the widened rule true of the function and false of
    // the product.
    if (!EntryStoreFitsOneQuadword(address)) return false;

    // The invariant. The patch must not cross an instruction boundary, so the
    // only reachable program point among the replaced bytes is the entry.
    if (facts.first_instruction < kEntryPatchBytes) return false;

    // Implied by the line above -- when the first instruction is at least the
    // patch length, the displaced window is that one instruction and has no
    // interior starts. Kept anyway, and not as decoration: these are two facts
    // the library measured separately, and two measurements that disagree are
    // a reason to refuse rather than an invitation to pick one.
    return facts.interior_boundaries == 0;
}

// RecoverEnlisted is gone. It drove a Sleep(1) retry loop over a transaction
// that had failed part-way and left threads suspended, and it existed because
// the primitive underneath could end in that state with no way back. The
// coordinated transaction reports `recovered` -- the writes undone before any
// thread resumed -- or `lost`, which is the one outcome a caller must decide
// something about and which no loop here can repair. Neither needs a caller
// side recovery pass.
}  // namespace gtg::research
