#include "color_choice.hpp"
#include "product_color_contract.hpp"
#include "consent_policy.hpp"
#include "../../src/settings/ui_preferences.hpp"
#include <cstdio>
#include <cstdlib>
using namespace gtg::overlay::integration;
void Check(bool value, const char* message) { if (!value) { std::fprintf(stderr, "FAIL %s\n", message); std::exit(1); } }
int main() {
    Check(gtg::settings::ResolveOverlayAutoSdr(false, 0), "missing Auto SDR defaults checked");
    Check(gtg::settings::ResolveOverlayAutoSdr(true, 1), "saved checked Auto SDR remains checked");
    Check(!gtg::settings::ResolveOverlayAutoSdr(true, 0), "saved unchecked Auto SDR remains unchecked");
    Check(gtg::settings::ResolveOverlayAutoSdr(true, 99), "invalid Auto SDR defaults checked");
    unsigned prompts{};
    for (const auto answer : {ColorConsent::Strict, ColorConsent::AssumeSdr, ColorConsent::Cancel}) {
        prompts = 0;
        const auto ask = [&]() { ++prompts; return answer; };
        Check(SelectColorConsent(true, ask) == ColorConsent::AssumeSdr && prompts == 0,
            "checked skips selector and chooses assumed SDR");
        Check(SelectColorConsent(false, ask) == answer && prompts == 1,
            "unchecked asks once and preserves strict SDR or cancel");
    }
    Check(DecodeConsent(kConsentStrict) == ColorConsent::Strict, "explicit/default strict stays strict");
    Check(DecodeConsent(kConsentSdr) == ColorConsent::AssumeSdr, "explicit SDR choice");
    Check(DecodeConsent(2) == ColorConsent::Cancel && DecodeConsent(0) == ColorConsent::Cancel,
        "cancel/error cannot grant permission");
    const Identity captured{123, 456};
    Check(AcceptConsent(ColorConsent::Strict, captured, captured, true, true), "captured live target accepted");
    Check(!AcceptConsent(ColorConsent::Cancel, captured, captured, true, true), "cancel allocates no session");
    Check(!AcceptConsent(ColorConsent::AssumeSdr, captured, {123, 457}, true, true), "PID reuse cannot inherit consent");
    Check(!AcceptConsent(ColorConsent::AssumeSdr, captured, {124, 456}, true, true), "focus change cannot retarget consent");
    Check(!AcceptConsent(ColorConsent::AssumeSdr, captured, captured, false, true), "dead target refused");
    Check(!AcceptConsent(ColorConsent::AssumeSdr, captured, captured, true, false), "disabled feature refused");
    const DWORD pid = GetCurrentProcessId();
    const auto created = ChoiceCreationTime(GetCurrentProcess());
    gtg::research::HookSection peer{};
    peer.header = {gtg::research::kHookMagic, gtg::research::kVersion, sizeof(peer), pid};
    peer.product_version = gtg::research::kProductVersion; peer.product_target_created = created;
    Check(ValidProductColorContract(peer, pid, created), "strict v2 identity contract");
    peer.product_version = 1; Check(!ValidProductColorContract(peer, pid, created), "old product peer refused");
    peer.product_version = 2; peer.header.size = 696; Check(!ValidProductColorContract(peer, pid, created), "old layout refused");
    peer.header.size = sizeof(peer); peer.product_color_policy = 99;
    Check(!ValidProductColorContract(peer, pid, created), "invalid worker choice refused");
    peer.product_color_policy = 1; Check(!ValidProductColorContract(peer, pid, created + 1), "worker creation mismatch refused");
    ColorPolicy policy{};
    Check(ReadColorChoice(pid, created, policy) == ChoiceRead::Missing && policy == ColorPolicy::Strict, "absent choice defaults strict");
    ColorChoice sample; sample.target_pid = pid; sample.target_created = created;
    sample.writer_pid = pid; sample.writer_created = created; sample.ready = 1;
    Check(ValidColorChoice(sample, pid, created), "complete identity choice valid");
    Check(!ValidColorChoice(sample, pid, created + 1), "PID reuse refused");
    sample.policy = 99; Check(!ValidColorChoice(sample, pid, created), "invalid choice enum refused");
    sample.policy = 1; sample.ready = 0; Check(!ValidColorChoice(sample, pid, created), "unpublished choice refused");
    sample.ready = 1;
    for (unsigned field = 0; field < 9; ++field) {
        auto bad = sample;
        switch (field) {
            case 0: bad.magic = 0; break; case 1: bad.version = 2; break; case 2: bad.size = 32; break;
            case 3: bad.target_pid = pid + 1; break; case 4: bad.target_created = created + 1; break;
            case 5: bad.writer_pid = 0; break; case 6: bad.writer_created = 0; break;
            case 7: bad.reserved = 1; break; case 8: bad.policy = 0; break;
        }
        Check(!ValidColorChoice(bad, pid, created), "malformed optional choice never grants permission");
    }
    {
        ColorChoiceMapping writer;
        Check(writer.Create(pid, created), "native choice mapping created");
        Check(ReadColorChoice(pid, created, policy) == ChoiceRead::Chosen && policy == ColorPolicy::ResearchAssumeSdr,
            "readonly choice reader checks live writer and creation");
        Check(ReadColorChoice(pid, created, policy, pid + 1, created) == ChoiceRead::Invalid && policy == ColorPolicy::Strict,
            "product helper cannot accept a different color-choice owner");
        Check(ReadColorChoice(pid, created, policy, pid, created) == ChoiceRead::Chosen,
            "product helper accepts its validated GTG parent writer");
        Check(ReadColorChoice(pid, created + 1, policy) == ChoiceRead::Invalid && policy == ColorPolicy::Strict,
            "mismatched native choice never grants permission");
        ColorChoiceMapping duplicate; Check(!duplicate.Create(pid, created), "duplicate choice cannot replace publisher");
    }
    Check(ReadColorChoice(pid, created, policy) == ChoiceRead::Missing, "choice lifetime ends with writer handle");
    Check(DecideSdrAdmission(DXGI_FORMAT_R10G10B10A2_UNORM, false, ColorPolicy::Strict) == ColorProvenance::WaitingUnknown,
        "strict unknown color stays pending");
    Check(DecideSdrAdmission(DXGI_FORMAT_R10G10B10A2_UNORM, false, ColorPolicy::ResearchAssumeSdr) == ColorProvenance::ResearchAssumedSdr,
        "explicit SDR choice allows R10 without fabricating observed color");
    Check(DecideSdrAdmission(DXGI_FORMAT_R10G10B10A2_UNORM, true, ColorPolicy::ResearchAssumeSdr) == ColorProvenance::ObservedSdr,
        "actual observation supersedes assumption");
    Check(DecideSdrAdmission(DXGI_FORMAT_R16G16B16A16_FLOAT, true, ColorPolicy::ResearchAssumeSdr) == ColorProvenance::None,
        "choice cannot admit FP16");
    Check(DecideSdrAdmission(DXGI_FORMAT_R10G10B10A2_UNORM, true, ColorPolicy::ResearchAssumeSdr, true) == ColorProvenance::None,
        "fatal always refuses");
    Check(DecideSdrAdmission(DXGI_FORMAT_R8G8B8A8_UNORM, false, static_cast<ColorPolicy>(99)) == ColorProvenance::None,
        "invalid policy cannot admit legacy format either");
    volatile LONG provenance{};
    RecordColorProvenance(provenance, ColorProvenance::ResearchAssumedSdr);
    RecordColorProvenance(provenance, ColorProvenance::ObservedSdr);
    Check(provenance == static_cast<LONG>(ColorProvenance::ObservedSdr), "provenance changes without changing evidence");
    StopColorProvenance(provenance); RecordColorProvenance(provenance, ColorProvenance::ResearchAssumedSdr);
    Check(provenance == (kColorFatalBit | static_cast<LONG>(ColorProvenance::ObservedSdr)), "fatal provenance is sticky");
    std::puts("PASS explicit SDR policy/provenance");
}
