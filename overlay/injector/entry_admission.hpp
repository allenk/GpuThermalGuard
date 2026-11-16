#pragma once
#include <cstddef>
#include <cstdint>

// The admission clauses that more than one side of this product evaluates, and
// which sites each one applies to.
//
// They live here because they were being evaluated twice. The rule over a
// prologue's shape was rewritten to state its invariant -- the five bytes
// written must lie inside one instruction and inside one aligned quadword --
// and the preflight in the executable, which runs before the DLL is loaded,
// went on applying the proxy that rule had stopped using. So the widened rule
// was true of the function and false of the product: an entry at a quadword
// offset of one, two or three was refused upstream and never reached it.
//
// Deliberately free of any splice header. The predicates below need no
// prologue facts, and the executable that holds the preflight does not link
// splice; it should not start including a library's headers to compare an
// address. The clause that does need facts, and a decoder to produce them,
// stays in `install_policy.hpp` where the DLL evaluates it.

namespace gtg::research {

// What the engine actually writes at an inline-patched entry, and why five is
// a fact on the path this product uses rather than a hope.
//
// The inline group installs through the coordinated tier, and that tier plans
// every site with `PreparedStrictPatch::prepare`. That function refuses an
// entry at a quadword offset above three, and refuses outright when
// `prepare_atomic_jmp_rel32` cannot form a five-byte relative jump to a
// destination it can reach -- whether because the near allocation failed or
// because what it got is out of rel32 range. It contains no fourteen-byte
// branch and no `memcpy` of a longer form. A site it cannot patch in five
// bytes is a site it declines, which is what makes this constant the length
// that will be written if anything is written at all.
//
// The fourteen-byte absolute form belongs to the library's ordinary installer,
// which plans `within_rel32(target, jump_dest) ? 5 : 14`. This product does
// not call it for these sites. That distinction is the whole content of the
// note here, and it was got wrong twice before it was looked up: first by a
// comment asserting the long form "does not arise" with no reason given, then
// by one asserting that it could arise here. Neither had read which tier the
// install goes through. The library's own gap -- that the facts a consumer
// admits on are computed for five bytes whichever installer it later uses --
// is real for a consumer on the ordinary path and is filed in its backlog.
//
// The displaced *window* can be longer than this -- that is `patch_bytes` --
// and the two are not the same number, which is the distinction the
// one-instruction clause turns on.
inline constexpr unsigned kEntryPatchBytes = 5;

// Sites, in report order: the order the DLL assembles its address array in and
// the order `entry_shape` is indexed by. Ordinals within the inline group are
// NOT these numbers -- `Signal` left that group -- and keeping the two apart
// is why they are named here instead of recomputed.
inline constexpr std::size_t kPresentSite = 0;
inline constexpr std::size_t kSignalSite = 1;
inline constexpr std::size_t kSiteCount = 7;

// Whether a site is installed by writing its function. `Signal` is installed
// by swapping its vtable cell, so nothing is written at its entry and no rule
// about that entry applies to it.
inline constexpr bool PatchedInline(std::size_t site) noexcept {
    return site != kSignalSite;
}

// One atomic store: the bytes written must lie within one aligned quadword.
// That is an offset of three or less, not an offset of zero. splice made this
// same correction inside its own atomic install and records the reason where it
// lives -- the aligned-quadword guarantee is the hardware rule, and demanding
// a zero offset cost the atomic path on every function a compiler had packed.
inline constexpr bool EntryStoreFitsOneQuadword(std::uintptr_t address) noexcept {
    return (address & 7u) + kEntryPatchBytes <= 8u;
}

// The preflight's rule for a product method, evaluated in the executable from
// the sixteen bytes the helper published and the address they were read at.
// There is no decoder on that side, so the one-instruction clause is not
// evaluated here and this does not pretend to: what it can decide is whether
// the store fits one quadword, and whether the site is written at all.
inline constexpr bool ProductEntryAdmitted(std::uintptr_t address,
                                           bool patched_inline) noexcept {
    if (address == 0) return false;

    // A site installed by swapping its vtable cell is not written at all, so
    // nothing about its entry address is required. The preflight used to
    // demand eight-byte alignment of this method too -- a code-generation
    // dependency of exactly the kind the cell swap was chosen to escape, which
    // passed only because the entry happened to be aligned.
    if (!patched_inline) return true;

    // The five product methods that are patched inline keep the stricter zero
    // offset, and this asymmetry with `Present` is deliberate. `Present` is
    // the one site the DLL also holds to the one-instruction clause; these
    // five have no such clause anywhere, so this is the only thing between
    // them and a patch spanning two quadwords. Widening it for symmetry would
    // remove a real guard and put nothing in its place. Their entry shapes are
    // measured on every attach now, so revisiting it is a decision on data.
    return (address & 7u) == 0u;
}

}  // namespace gtg::research
