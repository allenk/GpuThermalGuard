#pragma once
#include <splice/engine.h>

namespace gtg::research {
// This product refuses to patch its two hottest targets unless the patch
// window is a single instruction, so that no thread can be *inside* the bytes
// being replaced: it is either at the entry or past all of them.
//
// That is stricter than splice requires -- the coordinated tier maps an
// interior program counter through the site's instruction-boundary map rather
// than refusing it -- and the stricter rule stays here on purpose. Relaxing a
// safe-admission rule is a decision recorded on this side, and a library that
// answered "may I patch this?" with this product's policy baked in would be
// deciding it in the wrong repository.
//
// splice reports the facts; this is the policy over them.
inline bool OneInstructionEntry(const splice_prologue_facts& facts,
                                std::uintptr_t address) noexcept {
    if (facts.patch_bytes != 5) return false;
    if ((address & 7) != 0) return false;
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
