// The vtable-cell admission rule, against states a real runtime cannot be
// talked into producing.
//
// The rule exists because the cell's address is arithmetic over an offset the
// helper published, while the method's address was confirmed by sixteen
// prologue bytes read out of the target. Requiring the cell to hold the method
// is two measurements taken by different routes having to meet. Everything
// else the rule checks bounds what the measurement can be about: eight aligned
// bytes of read-only image data belonging to the same mapping as the method.
//
// Every refusal below is paired with the agreeing case, because a test that
// only asserts "refused" is also satisfied by a rule that refuses everything.
#include "slot_admission.hpp"
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <cstdlib>
#include <initializer_list>

using namespace gtg::overlay::integration;

namespace {

void Check(bool value, const char* message) {
    if (!value) {
        std::fprintf(stderr, "FAIL %s\n", message);
        std::exit(1);
    }
}

// One image mapped at 0x7ff000000000 with a read-only region holding a table.
// The cell is the fifteenth entry -- slot fourteen, as the COM interface fixes
// it -- and holds the method the helper published.
constexpr std::uint64_t kImage = 0x00007ff000000000ull;
constexpr std::uint64_t kTable = kImage + 0x255260;
constexpr std::uint64_t kCell = kTable + 14 * sizeof(void*);
constexpr std::uint64_t kMethod = kImage + 0x0888e0;

VtableCell Agreeing() {
    VtableCell cell;
    cell.slot = kCell;
    cell.method = kMethod;
    cell.held = kMethod;
    cell.cell_region_base = kImage + 0x250000;
    cell.cell_region_size = 0x10000;
    cell.cell_image_base = kImage;
    cell.method_image_base = kImage;
    cell.cell_state = MEM_COMMIT;
    cell.cell_type = MEM_IMAGE;
    cell.cell_protect = PAGE_READONLY;
    return cell;
}

} // namespace

int main() {
    Check(VtableCellAdmitted(Agreeing()),
          "a cell in read-only image data holding the published method is admitted");

    // The one that matters. Anything else in the cell is a wrong offset or
    // another overlay's hook, and in both cases writing would be wrong: it
    // would evict them, and the original we publish would be the value from
    // before they arrived, so calling it would skip their hook.
    {
        VtableCell cell = Agreeing();
        cell.held = kMethod + 0x40;
        Check(!VtableCellAdmitted(cell), "a cell holding something else is refused");
        cell.held = 0;
        Check(!VtableCellAdmitted(cell), "an empty cell is refused");
    }

    // Nothing published is the absence of a cell, not a cell at zero.
    {
        VtableCell cell = Agreeing();
        cell.slot = 0;
        Check(!VtableCellAdmitted(cell), "no published cell is refused");
        cell = Agreeing();
        cell.method = 0;
        cell.held = 0;
        Check(!VtableCellAdmitted(cell),
              "no published method is refused even though the cell agrees with it");
    }

    // An unaligned pointer store is not single-copy atomic, and one atomic
    // store with no interior state is the property this whole route is chosen
    // for. Every misalignment within a pointer is refused, not just odd ones.
    for (std::uint64_t off = 1; off < sizeof(void*); ++off) {
        VtableCell cell = Agreeing();
        cell.slot = kCell + off;
        Check(!VtableCellAdmitted(cell), "a misaligned cell is refused");
    }

    // A table the helper can name by an offset into an image is in that
    // image's constant data. Writable, executable, private or uncommitted
    // memory is a different thing that an offset cannot address.
    for (const DWORD protect : {static_cast<DWORD>(PAGE_READWRITE),
                                static_cast<DWORD>(PAGE_WRITECOPY),
                                static_cast<DWORD>(PAGE_EXECUTE_READ),
                                static_cast<DWORD>(PAGE_NOACCESS),
                                static_cast<DWORD>(PAGE_READONLY | PAGE_GUARD)}) {
        VtableCell cell = Agreeing();
        cell.cell_protect = protect;
        Check(!VtableCellAdmitted(cell), "a cell outside read-only memory is refused");
    }
    {
        VtableCell cell = Agreeing();
        cell.cell_state = MEM_RESERVE;
        Check(!VtableCellAdmitted(cell), "an uncommitted cell is refused");
        cell = Agreeing();
        cell.cell_type = MEM_PRIVATE;
        Check(!VtableCellAdmitted(cell), "a table built at run time is refused");
        cell = Agreeing();
        cell.cell_type = MEM_MAPPED;
        Check(!VtableCellAdmitted(cell), "a cell in a mapped file is refused");
    }

    // Eight bytes entirely inside the committed region, including the case
    // where the region is smaller than a pointer -- which is where the bounds
    // arithmetic would wrap if it were done in the other order.
    {
        VtableCell cell = Agreeing();
        cell.cell_region_base = kCell + sizeof(void*);
        Check(!VtableCellAdmitted(cell), "a cell below its region is refused");

        cell = Agreeing();
        cell.cell_region_base = kCell;
        cell.cell_region_size = sizeof(void*);
        Check(VtableCellAdmitted(cell), "a cell exactly filling its region is admitted");

        cell.cell_region_size = sizeof(void*) - 1;
        Check(!VtableCellAdmitted(cell),
              "a region too small to hold a pointer is refused and does not wrap");

        cell = Agreeing();
        cell.cell_region_base = kCell - 1;
        cell.cell_region_size = sizeof(void*);
        Check(!VtableCellAdmitted(cell),
              "a cell whose last byte is outside the region is refused");
        cell.cell_region_size = 0;
        Check(!VtableCellAdmitted(cell), "an empty region is refused");
    }

    // One mapping for both offsets, which is what makes rebasing the pair by a
    // single base sound. The report refuses to emit a pair that spans two
    // modules; this is that requirement checked again here, against the
    // mapping actually present rather than the one that was described.
    {
        VtableCell cell = Agreeing();
        cell.method_image_base = kImage + 0x1000000;
        Check(!VtableCellAdmitted(cell),
              "a cell and a method in different images are refused");
        cell = Agreeing();
        cell.cell_image_base = 0;
        Check(!VtableCellAdmitted(cell), "a cell with no owning image is refused");
        cell = Agreeing();
        cell.method_image_base = 0;
        Check(!VtableCellAdmitted(cell), "a method with no owning image is refused");
    }

    // ---- The read has to come after the clauses that make it safe ----------
    //
    // An external review found the cell being dereferenced before the checks
    // that establish it is readable: `VirtualQuery` returning a region is not
    // a readable page, so a slot that is reserved, guarded, or eight bytes
    // short of its region's end would have faulted inside the target process
    // during the install. Two halves now, with the read between them.
    //
    // `held` is set to the value the identity check wants in every case below,
    // so a refusal can only come from a memory clause. That is what makes
    // these cases evidence that the first half does not depend on the
    // contents -- which is the whole point of being able to run it first.
    {
        VtableCell cell = Agreeing();
        Check(VtableCellMemoryAdmitted(cell), "a sound cell may be read");

        cell = Agreeing();
        cell.cell_state = MEM_RESERVE;
        Check(!VtableCellMemoryAdmitted(cell), "a reserved cell is not read");

        cell = Agreeing();
        cell.cell_protect = PAGE_NOACCESS;
        Check(!VtableCellMemoryAdmitted(cell), "a no-access cell is not read");

        cell = Agreeing();
        cell.cell_protect = PAGE_READONLY | PAGE_GUARD;
        Check(!VtableCellMemoryAdmitted(cell), "a guarded cell is not read");

        cell = Agreeing();
        cell.cell_type = MEM_PRIVATE;
        Check(!VtableCellMemoryAdmitted(cell), "a private cell is not read");

        cell = Agreeing();
        cell.slot += 1;
        cell.held = cell.method;
        Check(!VtableCellMemoryAdmitted(cell), "an unaligned cell is not read");

        // Eight bytes that would cross the end of the region. The read is what
        // would cross it, so this clause has to precede the read.
        cell = Agreeing();
        cell.cell_region_size = (cell.slot - cell.cell_region_base) + 4;
        Check(!VtableCellMemoryAdmitted(cell),
              "a cell whose eight bytes leave the region is not read");

        cell = Agreeing();
        cell.slot = 0;
        Check(!VtableCellMemoryAdmitted(cell), "an absent cell is not read");
    }

    // The two halves compose: the verdict is the memory clauses and then the
    // agreement, and neither half alone admits a cell the other refuses.
    {
        VtableCell cell = Agreeing();
        cell.held = cell.method + 0x20;
        Check(VtableCellMemoryAdmitted(cell) && !VtableCellAdmitted(cell),
              "a readable cell holding something else is refused by the verdict");
    }

    // And the install reads the cell after the first half, not before it. The
    // ordering is in the caller, where no case in this binary can reach it, so
    // this reads the caller. It names the file it reads.
    {
        const char* const path = GTG_SOURCE_ROOT "/overlay/integration/product_overlay_hook.cpp";
        std::ifstream file(path, std::ios::binary);
        Check(file.is_open(), "the install source is where this test says it is");
        if (file.is_open()) {
            const std::string text((std::istreambuf_iterator<char>(file)),
                                   std::istreambuf_iterator<char>());
            Check(text.size() > 10000, "the install source was read, not merely opened");
            const auto guard = text.find("VtableCellMemoryAdmitted(cell)");
            const auto read = text.find("cell.held = *reinterpret_cast");
            Check(guard != std::string::npos, "the install calls the memory clauses");
            Check(read != std::string::npos, "the install reads the cell");
            Check(guard != std::string::npos && read != std::string::npos && guard < read,
                  "the install decides the memory clauses before reading the cell");
        }
    }

    std::printf("PASS vtable cell admission\n");
    return 0;
}
