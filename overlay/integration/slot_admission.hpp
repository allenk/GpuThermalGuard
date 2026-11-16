#pragma once
#include <windows.h>
#include <cstdint>

namespace gtg::overlay::integration {
// Admission for a site installed by swapping its vtable cell rather than by
// patching its function.
//
// `OneInstructionEntry` is the rule for a site whose prologue gets written:
// the bytes being replaced must be one instruction, so no thread can be inside
// them. A cell swap writes no prologue, so that rule has nothing to say about
// it -- and the hazard it guards against does not arise, because one
// pointer-sized aligned store has no interior for a thread to stand in.
//
// What takes its place is an agreement between two facts obtained by different
// routes. The helper resolved the method in its own process, published the
// address with its owner image's identity, and the attach side rebased it and
// read back sixteen prologue bytes from the target to confirm it. Separately
// it published the address of the cell. **The cell's address is arithmetic
// over an offset**, and a wrong offset would make the install a pointer-sized
// write to somewhere it had no business being. So the cell is required to hold
// exactly the method that was verified by its own bytes.
//
// Agreement is not proof. It is two measurements having to meet before
// anything is written, and a cell holding anything else -- a wrong offset,
// another overlay's hook, a table built at run time -- is a refusal that
// writes nothing and does not try to chain.
//
// The evidence is gathered by the caller and the verdict is taken here, for
// the same reason the prologue rule is shaped that way: a rule over
// measurements can be tested against states a real runtime cannot be talked
// into producing.
struct VtableCell {
    std::uint64_t slot{};              // the cell's address in this process
    std::uint64_t method{};            // the function address the helper published
    std::uint64_t held{};              // what the cell holds right now
    std::uint64_t cell_region_base{};  // the cell's committed region
    std::uint64_t cell_region_size{};
    std::uint64_t cell_image_base{};   // the mapped image the cell belongs to
    std::uint64_t method_image_base{}; // the mapped image the method belongs to
    DWORD cell_state{};
    DWORD cell_type{};
    DWORD cell_protect{};
};

// Everything that must hold before the cell may be *read*, which is every
// clause except the one that needs its contents. Split out because the caller
// has to dereference the slot to fill `held`, and that dereference is only
// defined once this has passed: `VirtualQuery` returning a region is not a
// readable page -- it succeeds for MEM_RESERVE and MEM_FREE, and for
// PAGE_NOACCESS and PAGE_GUARD -- so a slot address that is merely reserved,
// or guarded, or eight bytes short of the end of its region, would have
// faulted inside the target process during the install.
//
// Nothing here looks at `held`. A test asserts that by setting it to a value
// that would satisfy the identity check and watching each clause refuse anyway.
inline bool VtableCellMemoryAdmitted(const VtableCell& cell) noexcept {
    // Nothing published means the helper found no cell it could vouch for.
    // That is not a cell to examine; it is the absence of one.
    if (cell.slot == 0 || cell.method == 0) return false;

    // An unaligned store of a pointer is not single-copy atomic, and the whole
    // reason to prefer this route is that one aligned store has no torn state.
    if (cell.slot % sizeof(void*) != 0) return false;

    // A vtable the helper can name by an offset into an image lives in that
    // image's constant data. Writable or private memory would be a table built
    // at run time -- a real shape, and not one an RVA can address.
    if (cell.cell_state != MEM_COMMIT) return false;
    if (cell.cell_type != MEM_IMAGE) return false;
    if (cell.cell_protect != PAGE_READONLY) return false;

    // Eight bytes entirely inside the committed region. The size test comes
    // first because the subtraction below is unsigned.
    if (cell.cell_region_size < sizeof(void*)) return false;
    if (cell.slot < cell.cell_region_base) return false;
    if (cell.slot - cell.cell_region_base > cell.cell_region_size - sizeof(void*)) return false;

    // One image for both, which is what makes a single rebasing of two offsets
    // sound. The report refuses to emit a pair that spans two modules; this is
    // the same requirement checked again on this side, against the mapping
    // actually present rather than against the one that was described.
    if (cell.cell_image_base == 0 || cell.method_image_base == 0) return false;
    if (cell.cell_image_base != cell.method_image_base) return false;

    return true;
}

inline bool VtableCellAdmitted(const VtableCell& cell) noexcept {
    // The memory clauses again -- they are integer comparisons on a struct the
    // caller already filled, so re-running them costs nothing and keeps one
    // function as the verdict the tests cover -- and then the agreement, which
    // is the reason for all of the above.
    if (!VtableCellMemoryAdmitted(cell)) return false;
    return cell.held == cell.method;
}
}  // namespace gtg::overlay::integration
