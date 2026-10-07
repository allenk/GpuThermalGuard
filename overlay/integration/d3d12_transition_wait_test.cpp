#include "d3d12_transition_wait.hpp"
#include <cstdio>
#include <cstdlib>
using namespace gtg::overlay::integration;
namespace {
void Check(bool value, const char* message) {
    if (!value) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); }
}
struct Ops {
    std::uint64_t completed = 0, now = 0, notify_cost = 0, completion_after_wait = 9;
    HRESULT notification = S_OK;
    DWORD result = WAIT_OBJECT_0, waited = 0;
    int notifies = 0, waits = 0;
    std::uint64_t Completed() noexcept { return completed; }
    std::uint64_t Now() noexcept { return now; }
    HRESULT Notify(std::uint64_t value) noexcept {
        Check(value == 9, "private target forwarded unchanged");
        ++notifies; now += notify_cost; return notification;
    }
    DWORD Wait(DWORD ms) noexcept {
        ++waits; waited = ms; now += ms; completed = completion_after_wait; return result;
    }
};
}
int main() {
    Ops done; done.completed = 9;
    Check(DrainPrivateFence(done, 9, 250) == DrainResult::Complete,
          "already complete private fence does not wait");
    Check(done.notifies == 0 && done.waits == 0, "completed fast path has no event operation");
    Ops pending;
    Check(DrainPrivateFence(pending, 9, 250) == DrainResult::Complete,
          "transition wait requires independent completion proof");
    Check(pending.notifies == 1 && pending.waits == 1 && pending.waited == 250,
          "one notification and one finite wait");
    Ops capped;
    Check(DrainPrivateFence(capped, 9, INFINITE) == DrainResult::Complete && capped.waited == 250,
          "caller cannot turn transition wait into INFINITE");
    Ops spent; spent.notify_cost = 40;
    Check(DrainPrivateFence(spent, 9, 100) == DrainResult::Complete && spent.waited == 60,
          "notification consumes shared transition deadline");
    Ops expired; expired.notify_cost = 251;
    Check(DrainPrivateFence(expired, 9, 250) == DrainResult::Timeout && expired.waits == 0,
          "no new wait after deadline expired");
    Ops zero;
    Check(DrainPrivateFence(zero, 9, 0) == DrainResult::Timeout && zero.notifies == 0,
          "zero remaining budget cannot register more asynchronous work");
    Ops removed; removed.completed = std::numeric_limits<std::uint64_t>::max();
    Check(DrainPrivateFence(removed, 9, 250) == DrainResult::Removed && removed.waits == 0,
          "device removal is not fence completion");
    Ops failed_notify; failed_notify.notification = E_FAIL;
    Check(DrainPrivateFence(failed_notify, 9, 250) == DrainResult::NotifyFailed && failed_notify.waits == 0,
          "notification failure refuses wait");
    Ops timeout; timeout.result = WAIT_TIMEOUT; timeout.completion_after_wait = 0;
    Check(DrainPrivateFence(timeout, 9, 250) == DrainResult::Timeout && timeout.waits == 1,
          "timeout does not spin or retry");
    Ops failed_wait; failed_wait.result = WAIT_FAILED;
    Check(DrainPrivateFence(failed_wait, 9, 250) == DrainResult::WaitFailed,
          "failed event wait is not completion");
    Ops stale; stale.completion_after_wait = 8;
    Check(DrainPrivateFence(stale, 9, 250) == DrainResult::Unproven,
          "event wake alone cannot prove GPU completion");
    Ops lost; lost.completion_after_wait = std::numeric_limits<std::uint64_t>::max();
    Check(DrainPrivateFence(lost, 9, 250) == DrainResult::Removed,
          "device removal after wake is refused");
    std::puts("PASS resize-only private fence deadline and failure contracts");
}
