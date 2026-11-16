// The inline-patch admission rule, over states a real prologue cannot be
// talked into producing.
//
// This file used to test a one-argument `OneInstructionEntry(Plan)` and a
// `RecoverEnlisted` that the header says is gone, and it compiled against
// neither: `overlay/injector/CMakeLists.txt`, which registered it, is added by
// no preset, so **nothing built it and nothing ran it.** The most
// safety-critical decision in this consumer had no test in any build. It is
// registered from `overlay/integration/CMakeLists.txt` now, beside the vtable
// cell rule, which is where the builds that run live.
//
// Every refusal is paired with the admitting case, because a test that only
// asserts "refused" is also satisfied by a rule that refuses everything.
#include "entry_admission.hpp"
#include "install_policy.hpp"
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <cstdlib>

using gtg::research::EntryStoreFitsOneQuadword;
using gtg::research::kEntryPatchBytes;
using gtg::research::kPresentSite;
using gtg::research::kSignalSite;
using gtg::research::OneInstructionEntry;
using gtg::research::PatchedInline;
using gtg::research::ProductEntryAdmitted;

namespace {

int failures = 0;

// Read the preflight and require that it reaches the shared clauses by name.
// The file is named rather than globbed: a detector that scans a directory
// passes when the directory is empty, which this project has paid for.
void CheckSource();

int source_cases = 0;

void Check(bool ok, const char* why) {
    if (!ok) {
        std::fprintf(stderr, "FAIL %s\n", why);
        ++failures;
    }
}

// Same, for a case inside a loop: a failure that does not say which input
// produced it sends the reader back to the arithmetic to guess.
void CheckAt(bool ok, const char* why, unsigned offset) {
    if (!ok) {
        std::fprintf(stderr, "FAIL %s (offset %u)\n", why, offset);
        ++failures;
    }
}

// The facts splice reports for a prologue whose first instruction is `first`
// bytes long. When that instruction is at least the patch length the displaced
// window is exactly it, with no interior starts; when it is shorter the walk
// continues into the next instruction and leaves a boundary behind, which is
// what `interior_mask` records the position of.
splice_prologue_facts Facts(unsigned first, unsigned window, unsigned mask) {
    splice_prologue_facts facts{};
    facts.patch_bytes = window;
    facts.first_instruction = first;
    facts.interior_mask = mask;
    facts.interior_boundaries = 0;
    for (unsigned bit = 0; bit < 32; ++bit)
        if (mask & (1u << bit)) ++facts.interior_boundaries;
    return facts;
}

// A whole instruction covering the patch: window == first, no boundaries.
splice_prologue_facts Whole(unsigned first) { return Facts(first, first, 0); }

constexpr std::uintptr_t kAligned = 0x7ff000001000ull;

void CheckSource() {
    const char* const path = GTG_SOURCE_ROOT "/overlay/injector/target_attach.cpp";
    std::ifstream file(path, std::ios::binary);
    Check(file.is_open(), "the preflight source is where this test says it is");
    if (!file.is_open()) return;
    const std::string text((std::istreambuf_iterator<char>(file)),
                           std::istreambuf_iterator<char>());

    Check(text.size() > 10000, "the preflight source was read, not merely opened");
    Check(text.find("EntryStoreFitsOneQuadword(present)") != std::string::npos,
          "the preflight admits Present through the shared clause");
    Check(text.find("ProductEntryAdmitted(address, slot_out == nullptr)") != std::string::npos,
          "the preflight admits a product method through the shared clause");
    // The two forms it used to be written in. Either reappearing means the
    // sides have drifted again, which is the defect and not the style.
    Check(text.find("(present & 7)") == std::string::npos,
          "no open-coded alignment test for Present");
    Check(text.find("!(address & 7)") == std::string::npos,
          "no open-coded alignment test for a product method");

    // The same, for the ordering the DLL applies: a site installed by swapping
    // its cell is admitted whether or not its prologue decodes. Putting the
    // old gate back breaks no behavioural case here, because nothing in this
    // binary drives `Prepare`, so this is the detector that exists. A real one
    // needs a seam in the install, which is worth more than this and is not
    // what today's finding buys.
    const char* const dll = GTG_SOURCE_ROOT "/overlay/integration/product_overlay_hook.cpp";
    std::ifstream second(dll, std::ios::binary);
    Check(second.is_open(), "the install source is where this test says it is");
    if (!second.is_open()) return;
    const std::string install((std::istreambuf_iterator<char>(second)),
                              std::istreambuf_iterator<char>());
    Check(install.size() > 10000, "the install source was read, not merely opened");
    Check(install.find("if (!profiled && PatchedInline(i))") != std::string::npos,
          "only a site whose function is written refuses on a failed profile");
    Check(install.find("if (splice_prologue_profile(target, &facts) != 0)") == std::string::npos,
          "the profile is no longer a precondition of reaching the rule");

    ++source_cases;
}

} // namespace

int main() {
    // `dxgi.dll`'s `Present` as measured today: one five-byte instruction at a
    // sixteen-byte-aligned entry. This is the shape the old rule was written
    // around, and it must keep passing.
    Check(OneInstructionEntry(Whole(5), kAligned), "five bytes, one instruction, aligned");

    // The case the old rule refused for being safe in an unexpected way. The
    // five bytes written lie entirely inside one longer instruction, so the
    // window is still one instruction and no thread can be inside the replaced
    // bytes. `patch_bytes != 5` rejected every one of these.
    for (unsigned first = 6; first <= 15; ++first)
        Check(OneInstructionEntry(Whole(first), kAligned),
              "a patch wholly inside one longer instruction is admitted");

    // `ID3D12CommandQueue::Signal` on the newer runtime: a three-byte entry
    // instruction, so a five-byte patch displaces two and leaves a boundary at
    // offset three. Refused, which is why that site went to a vtable cell swap
    // rather than being admitted here.
    Check(!OneInstructionEntry(Facts(3, 7, 0x8), kAligned),
          "a three-byte entry with a boundary at offset three is refused");

    // And every entry instruction shorter than the patch, boundary or not: the
    // invariant is about the patch crossing a boundary, not about the mask.
    for (unsigned first = 1; first < kEntryPatchBytes; ++first) {
        Check(!OneInstructionEntry(Facts(first, 8, 1u << first), kAligned),
              "an entry instruction shorter than the patch is refused");
        Check(!OneInstructionEntry(Whole(first), kAligned),
              "a short entry is refused even when no boundary is reported");
    }

    // Two facts that disagree are a refusal, not an invitation to pick one. A
    // long first instruction with a boundary reported inside the window cannot
    // both be true; the rule declines rather than trusting the half it likes.
    Check(!OneInstructionEntry(Facts(8, 8, 0x4), kAligned),
          "a reported boundary inside a single-instruction window is refused");

    // Alignment: the five bytes must lie in one aligned quadword, which is an
    // offset of three or less -- the hardware rule, not the stricter zero the
    // old rule asked for. Offsets one through three are atomic and used to be
    // refused.
    for (unsigned offset = 0; offset <= 3; ++offset)
        Check(OneInstructionEntry(Whole(5), kAligned + offset),
              "an entry at offset three or less is one atomic store");
    for (unsigned offset = 4; offset <= 7; ++offset)
        Check(!OneInstructionEntry(Whole(5), kAligned + offset),
              "an entry whose patch would span two quadwords is refused");

    // The two tests are independent: a well-aligned entry that is too short is
    // refused, and a long-enough entry at a bad offset is refused. Neither
    // clause can stand in for the other.
    Check(!OneInstructionEntry(Whole(3), kAligned), "short entry, good offset");
    Check(!OneInstructionEntry(Whole(5), kAligned + 5), "good entry, bad offset");

    // ---- The two sides have to agree, and they did not ----------------------
    //
    // A review on 2026-11-15 read the preflight in the executable and found it
    // still applying `(address & 7) == 0` while this rule had moved to the
    // quadword invariant. That made the widened rule unreachable in the
    // product: an entry at offset one, two or three is refused before the DLL
    // is loaded. The claim was read, not run, so it is run here. Both sides
    // call one predicate now; with the old body restored this fails at exactly
    // those three offsets, which is the claim measured.
    for (unsigned offset = 0; offset <= 7; ++offset) {
        const auto address = kAligned + offset;
        CheckAt(EntryStoreFitsOneQuadword(address) ==
                    OneInstructionEntry(Whole(kEntryPatchBytes), address),
                "the preflight and the rule admit the same offsets", offset);
    }

    // The preflight cannot evaluate the one-instruction clause -- it holds
    // sixteen bytes and no decoder -- so agreement is asserted only where the
    // rule's other clause is satisfied. Where it is not, the rule is the
    // stricter of the two, which is the direction that is safe.
    Check(EntryStoreFitsOneQuadword(kAligned) &&
              !OneInstructionEntry(Whole(3), kAligned),
          "the DLL may refuse what the preflight admits, never the reverse");

    // ---- A site that is not written has no rule about its entry -------------
    //
    // `Signal` is installed by swapping its vtable cell. The preflight went on
    // requiring that method's entry to be eight-byte aligned, which is a
    // code-generation dependency of exactly the kind the cell swap was chosen
    // to escape, and which passed only because the entry happened to be
    // aligned. Requiring it again fails this at every offset but zero.
    Check(!PatchedInline(kSignalSite), "the cell-swapped site is not patched");
    Check(PatchedInline(kPresentSite), "Present is patched inline");
    for (unsigned offset = 0; offset <= 7; ++offset)
        CheckAt(ProductEntryAdmitted(kAligned + offset, PatchedInline(kSignalSite)),
                "a site installed by cell swap is admitted at any entry offset", offset);

    // Not an invitation to admit anything: a site with no address is still
    // refused, and an inline-patched site still has to fit one quadword.
    Check(!ProductEntryAdmitted(0, PatchedInline(kSignalSite)),
          "a cell-swapped site with no published method is refused");
    Check(!ProductEntryAdmitted(0, true), "an inline site with no address is refused");

    // The five product methods patched inline keep the stricter zero offset,
    // and that asymmetry with `Present` is the finding's decision rather than
    // an oversight: `Present` is the one inline site the DLL also holds to the
    // one-instruction clause, and these five have no clause over their shape
    // anywhere, so the preflight's demand is all they have. Asserted here so
    // that widening it has to come with a reason and a measurement.
    Check(ProductEntryAdmitted(kAligned, true), "an aligned inline product site is admitted");
    for (unsigned offset = 1; offset <= 7; ++offset)
        CheckAt(!ProductEntryAdmitted(kAligned + offset, true),
                "an inline product site keeps the zero-offset requirement", offset);
    Check(EntryStoreFitsOneQuadword(kAligned + 3) && !ProductEntryAdmitted(kAligned + 3, true),
          "Present's clause is wider than the five's, deliberately");

    // ---- The two sides cannot drift apart again in silence -----------------
    //
    // The clause is one predicate now, but nothing stops a future edit from
    // writing the arithmetic out inline again, which is how it drifted the
    // first time. This is the detector for that, and it names what it reads.
    CheckSource();

    // The detector has to have run, or its five cases prove nothing by being
    // absent. This is the shape of failure a source-reading test is prone to.
    Check(source_cases == 1, "the source detector ran");

    if (failures != 0) {
        std::fprintf(stderr, "%d case(s) failed\n", failures);
        return 1;
    }
    std::puts("PASS inline entry admission");
    return 0;
}
