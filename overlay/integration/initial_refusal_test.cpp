#include "initial_refusal.hpp"
#include "startup_readiness.hpp"
#include "../injector/hook_protocol.hpp"
#include <cstdio>
#include <cstdlib>
using namespace gtg::overlay::integration;

void Check(bool value, const char* message) {
    if (!value) {
        std::fprintf(stderr, "FAIL %s\n", message);
        std::exit(1);
    }
}

int main() {
    InspectionEvidence evidence{static_cast<LONG>(gtg::research::HookState::kInstalled),
                                2,
                                1,
                                true,
                                false,
                                false,
                                true,
                                true,
                                true,
                                true,
                                true,
                                true,
                                false,
                                false};
    Check(ClassifyInspection(evidence) == InspectionState::Ready,
          "complete native D12 inspection is ready");

    struct Clock {
        UINT64 time{};
        unsigned pauses{};

        UINT64 Now() noexcept { return time; }

        void Pause(DWORD ms) noexcept {
            ++pauses;
            time += ms;
        }
    } clock;

    struct Ops {
        Clock& clock;
        UINT64 ready_at{};
        bool alive{true};
        InspectionState result{InspectionState::Ready};

        bool Continue() noexcept { return alive; }

        InspectionState Inspect() noexcept {
            return clock.time < ready_at ? InspectionState::Waiting : result;
        }
    } ops{clock};

    Check(WaitForInspection(clock, ops) && clock.time == 0 && clock.pauses == 0,
          "ready startup has no fixed ten-second delay");
    clock = {};
    ops.ready_at = 100;
    Check(WaitForInspection(clock, ops) && clock.time == 100,
          "pending startup resumes promptly when ready");
    clock = {};
    ops.ready_at = 20000;
    Check(!WaitForInspection(clock, ops) && clock.time == 10000 && clock.pauses <= 400,
          "pending inspection has bounded deadline and sleep polling");
    clock = {};
    ops.ready_at = 0;
    ops.alive = false;
    Check(!WaitForInspection(clock, ops) && clock.time == 0,
          "canceled startup never waits or activates");
    ops.alive = true;
    ops.result = InspectionState::Refused;
    Check(!WaitForInspection(clock, ops) && clock.time == 0, "terminal refusal exits immediately");
    for (unsigned field = 0; field < 9; ++field) {
        auto invalid = evidence;
        switch (field) {
            case 0:
                invalid.hook_state = static_cast<LONG>(gtg::research::HookState::kPartialResident);
                break;
            case 1:
                invalid.errors = true;
                break;
            case 2:
                invalid.ambiguous = true;
                break;
            case 3:
                invalid.signal = false;
                break;
            case 4:
                invalid.metadata_valid = false;
                break;
            case 5:
                invalid.window_owned = false;
                break;
            case 6:
                invalid.mismatch = true;
                break;
            case 7:
                invalid.staged_draw = true;
                break;
            case 8:
                invalid.ready = -261;
                break;
        }
        Check(ClassifyInspection(invalid) == InspectionState::Refused,
              "all previous refusal gates still prohibit fast activation");
    }
    static_assert(sizeof(gtg::research::HookSection) == 712, "approved product v2 layout");
    volatile LONG ready{};
    const LONG exact_reasons[] = {-260, -261, -262, -263, -264, -265, -270, -271, -272};
    for (const LONG reason : exact_reasons) {
        ready = 1;
        RecordRuntimeRefusal(ready, reason);
        RecordRuntimeRefusal(ready, -252);
        RecordRuntimeRefusal(ready, -299);
        ClearColorWait(ready);
        Check(ready == reason, "exact output/transition reason survives later abort/refusal");
    }
    ready = 0;
    RecordRuntimeRefusal(ready, -250);
    RecordRuntimeRefusal(ready, -252);
    RecordInitialReady(ready);
    Check(ready == -250,
          "first partial-presentation stop survives later transition failure/readiness");
    ready = 1;
    RecordRuntimeRefusal(ready, -251);
    RecordRuntimeRefusal(ready, -299);
    Check(ready == -251, "first resize admission failure cannot become unspecified");
    ready = 0;
    RecordInitialRefusal(ready, InitialRefusal::Format);
    Check(ready == -228, "first rejection records exact stable format reason");
    RecordInitialRefusal(ready, InitialRefusal::Signal);
    Check(ready == -228, "later rejection cannot overwrite first reason");
    RecordInitialReady(ready);
    Check(ready == -228, "in-flight readiness cannot overwrite rejection");
    RecordRuntimeRefusal(ready, -200);
    Check(ready == -228, "runtime rejection cannot overwrite initial rejection");
    ready = 0;
    RecordInitialReady(ready);
    Check(ready == 1, "healthy readiness is published");
    RecordRuntimeRefusal(ready, -201);
    Check(ready == -201, "qualified renderer failure publishes reason");
    ready = 1;
    RecordRuntimeRefusal(ready, -203);
    Check(ready == -203, "waiting color reason is published");
    RecordRuntimeRefusal(ready, -202);
    Check(ready == -202, "fatal runtime refusal overrides a pending color wait");
    ClearColorWait(ready);
    RecordColorWait(ready);
    Check(ready == -202, "pending and recovery cannot erase fatal refusal");
    ready = 1;
    RecordColorWait(ready);
    Check(ready == kWaitingSdrEvidence, "qualified metadata can wait without fatal refusal");
    ClearColorWait(ready);
    Check(ready == 1, "color qualification recovers pending state");
    ready = 0;
    RecordColorWait(ready);
    ClearColorWait(ready);
    Check(ready == 0, "pending operations cannot qualify an uninspected chain");
    const LONG initial_states[] = {1L, kWaitingSdrEvidence};
    for (const LONG initial : initial_states) {
        LONG concurrent = initial;
        unsigned calls{};
        PublishRuntimeRefusal(-202, [&](LONG desired, LONG expected) noexcept {
            const LONG seen = concurrent;
            if (seen == expected) concurrent = desired;
            if (++calls == 1) concurrent = initial == 1 ? kWaitingSdrEvidence : 1;
            return seen;
        });
        Check(concurrent == -202 && calls == 3,
              "fatal survives pending/qualified interleaving with bounded CAS");
    }
    LONG concurrent = 0;
    unsigned calls{};
    PublishRuntimeRefusal(-202, [&](LONG desired, LONG expected) noexcept {
        // Already admitted Present can publish initial readiness and a wait.
        if (calls == 0) concurrent = 1;
        if (calls == 1) concurrent = kWaitingSdrEvidence;
        ++calls;
        const LONG seen = concurrent;
        if (seen == expected) concurrent = desired;
        return seen;
    });
    Check(concurrent == -202 && calls == 3,
          "fatal covers both final publications from admitted Present");
    ready = 0;
    RecordRuntimeRefusal(ready, -200);
    RecordInitialReady(ready);
    Check(ready == -200, "runtime refusal before readiness stays visible");
    ready = 1;
    RecordInitialRefusal(ready, InitialRefusal::PresentMethod);
    Check(ready == 1, "initial diagnostics do not overwrite qualified state");
    ready = 0;
    RecordInitialRefusal(ready, InitialRefusal::Present1Method);
    Check(ready == -244, "method mismatch identifies individual vtable slot");
    std::puts("PASS initial refusal first-publication contracts");
}
