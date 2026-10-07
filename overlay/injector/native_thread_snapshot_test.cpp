#include "native_thread_snapshot.hpp"
#include "snapshot_retry.hpp"
#include <array>
#include <cstdio>
#include <atomic>
#include <cstring>

namespace {
DWORD WINAPI Waiter(void* event) { return WaitForSingleObject(event, 20000) == WAIT_OBJECT_0 ? 0 : 1; }
void Check(bool ok, const char* message) {
    if (!ok) { std::fprintf(stderr, "FAIL %s error=%lu\n", message, GetLastError()); ExitProcess(1); }
}
struct ChurnState {
    HANDLE stop{};
    std::atomic<unsigned> created{};
};
DWORD WINAPI BriefWorker(void* stop) { WaitForSingleObject(stop, 2); return 0; }
DWORD WINAPI Churner(void* argument) {
    auto& state = *static_cast<ChurnState*>(argument);
    while (WaitForSingleObject(state.stop, 0) == WAIT_TIMEOUT) {
        HANDLE thread = CreateThread(nullptr, 0, BriefWorker, state.stop, 0, nullptr);
        if (!thread) return 1;
        state.created.fetch_add(1);
        const bool joined = WaitForSingleObject(thread, 3000) == WAIT_OBJECT_0;
        const bool closed = CloseHandle(thread) != FALSE;
        if (!joined || !closed) return 2;
    }
    return 0;
}
void CheckChurn(const std::array<DWORD, 4>& ids, HANDLE gate) {
    // Deterministic limitation: a fresh thread is not retroactively enlisted.
    gtg::research::NativeThreadSnapshot prior;
    Check(prior.Collect(4096), "pre-creation snapshot");
    DWORD added_id{};
    HANDLE added = CreateThread(nullptr, 0, Waiter, gate, 0, &added_id);
    Check(added != nullptr, "create post-snapshot thread");
    for (auto handle : prior.Handles())
        Check(GetThreadId(handle) != added_id, "new thread absent from old snapshot");
    Check(prior.Close(), "close stale snapshot");
    Check(prior.Collect(4096), "fresh collection includes new thread");
    unsigned found = 0;
    for (auto handle : prior.Handles()) if (GetThreadId(handle) == added_id) ++found;
    Check(found == 1, "new thread found exactly once");
    Check(prior.Close(), "close fresh snapshot");
    // Release this worker independently without releasing the four stable workers.
    // Its gate is separate from the stable-worker event supplied by the caller.
    Check(SetEvent(gate), "release added thread");
    Check(WaitForSingleObject(added, 3000) == WAIT_OBJECT_0, "join added thread");
    Check(CloseHandle(added), "close added thread");

    ChurnState state{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    Check(state.stop != nullptr, "churn stop");
    HANDLE churner = CreateThread(nullptr, 0, Churner, &state, 0, nullptr);
    Check(churner != nullptr, "start churn");
    unsigned accepted = 0, refused = 0;
    const auto deadline = GetTickCount64() + 3000;
    while (state.created.load() == 0) {
        Check(GetTickCount64() < deadline, "churn started");
        Sleep(1);
    }
    for (unsigned sample = 0; sample < 32; ++sample) {
        gtg::research::NativeThreadSnapshot snapshot;
        if (snapshot.Collect(4096)) {
            ++accepted;
            for (auto id : ids) {
                unsigned matches = 0;
                for (auto handle : snapshot.Handles()) if (GetThreadId(handle) == id) ++matches;
                Check(matches == 1, "stable workers covered during churn");
            }
        } else ++refused; // Exit/open races may legitimately refuse.
        Check(snapshot.Close(), "close churn sample");
        Sleep(1);
    }
    Check(SetEvent(state.stop), "stop churn");
    Check(WaitForSingleObject(churner, 3000) == WAIT_OBJECT_0, "join churn");
    DWORD result{};
    Check(GetExitCodeThread(churner, &result) && result == 0, "churn result");
    Check(CloseHandle(churner) && CloseHandle(state.stop), "close churn resources");
    // All racing samples may refuse; verify recovery once churn has stopped.
    gtg::research::NativeThreadSnapshot stable;
    Check(stable.Collect(4096), "collect after churn stops");
    for (auto id : ids) {
        unsigned matches = 0;
        for (auto handle : stable.Handles()) if (GetThreadId(handle) == id) ++matches;
        Check(matches == 1, "stable workers covered after churn");
    }
    Check(stable.Close(), "close post-churn collection");
    std::printf("churn samples=32 accepted=%u refused=%u created=%u new_thread_omitted=1 no_patch=1\n",
                accepted, refused, state.created.load());
}
}
int main(int argc, char** argv) {
    struct Retry {
        unsigned calls{}, closes{}, proofs{}, fail_count{1};
        bool missing{true}, close_ok{true};
        gtg::research::InstallOperation operation{gtg::research::InstallOperation::OpenThread};
        DWORD error{ERROR_INVALID_PARAMETER};
        bool Collect(gtg::research::InstallSite& site) noexcept {
            ++calls;
            if (calls > fail_count) { site.threads = 4; return true; }
            gtg::research::RecordInstallError(site.primary, operation, 0, error, true, 123);
            return false;
        }
        bool Close(gtg::research::InstallSite& site) noexcept {
            ++closes;
            if (!close_ok) gtg::research::RecordInstallError(site.cleanup, gtg::research::InstallOperation::Close);
            return close_ok;
        }
        bool Absent(DWORD tid, gtg::research::InstallSite&) noexcept { ++proofs; return tid == 123 && missing; }
    } retry;
    gtg::research::InstallSite diagnostic{};
    Check(gtg::research::CollectStableSnapshot(retry, &diagnostic) && retry.calls == 2 && retry.closes == 1 &&
        retry.proofs == 1 && diagnostic.threads == 4 && diagnostic.primary.operation == gtg::research::InstallOperation::None,
        "confirmed vanished thread recollects full snapshot before patch");
    retry = {}; retry.missing = false;
    Check(!gtg::research::CollectStableSnapshot(retry, &diagnostic) && retry.calls == 1, "present or reused TID cannot be ignored");
    retry = {}; retry.error = ERROR_ACCESS_DENIED;
    Check(!gtg::research::CollectStableSnapshot(retry, &diagnostic) && retry.calls == 1 && retry.proofs == 0,
        "access denied never retries or skips a thread");
    retry = {}; retry.close_ok = false;
    Check(!gtg::research::CollectStableSnapshot(retry, &diagnostic) && retry.calls == 1 && retry.proofs == 0,
        "cleanup failure prevents any recollection");
    retry = {}; retry.fail_count = 99;
    Check(!gtg::research::CollectStableSnapshot(retry, &diagnostic) && retry.calls == 3 && retry.proofs == 2,
        "churn refusal is bounded to three complete attempts");
    retry = {}; retry.operation = gtg::research::InstallOperation::ProcessIdentity;
    Check(!gtg::research::CollectStableSnapshot(retry, &diagnostic) && retry.calls == 1 && retry.proofs == 0,
        "identity failure remains terminal");
    retry = {}; diagnostic = {}; diagnostic.transaction_entered = 1;
    Check(!gtg::research::CollectStableSnapshot(retry, &diagnostic) && retry.calls == 0,
        "already entered patch transaction can never recollect");
    const bool churn = argc == 2 && std::strcmp(argv[1], "--churn") == 0;
    HANDLE event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    Check(event != nullptr, "create event");
    std::array<HANDLE, 4> workers{};
    std::array<DWORD, 4> ids{};
    for (std::size_t i = 0; i < workers.size(); ++i) {
        workers[i] = CreateThread(nullptr, 0, Waiter, event, 0, &ids[i]);
        Check(workers[i] != nullptr, "create worker");
    }
    DWORD before{}, after{};
    Check(GetProcessHandleCount(GetCurrentProcess(), &before), "baseline handles before absence proofs");
    gtg::research::InstallSite proof{};
    Check(!gtg::research::ThreadAbsentFromFreshSnapshot(ids[0], proof), "native live TID cannot authorize recollection");
    HANDLE exit_gate = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    Check(exit_gate != nullptr, "create exit-race gate");
    DWORD exited_id{};
    HANDLE exiting = CreateThread(nullptr, 0, Waiter, exit_gate, 0, &exited_id);
    Check(exiting != nullptr && SetEvent(exit_gate) && WaitForSingleObject(exiting, 3000) == WAIT_OBJECT_0,
        "own thread exits normally before vanished proof");
    Check(CloseHandle(exiting) && CloseHandle(exit_gate), "release exit-race handles");
    Check(gtg::research::ThreadAbsentFromFreshSnapshot(exited_id, proof), "native exited TID confirmed absent");
    struct NativeRetry {
        gtg::research::NativeThreadSnapshot snapshot;
        DWORD exited{};
        unsigned calls{};
        bool Collect(gtg::research::InstallSite& attempt) noexcept {
            if (++calls == 1) {
                gtg::research::RecordInstallError(attempt.primary, gtg::research::InstallOperation::OpenThread,
                    0, ERROR_INVALID_PARAMETER, true, exited);
                return false;
            }
            return snapshot.Collect(4096, &attempt);
        }
        bool Close(gtg::research::InstallSite& attempt) noexcept { return snapshot.Close(&attempt); }
        bool Absent(DWORD tid, gtg::research::InstallSite& attempt) noexcept { return gtg::research::ThreadAbsentFromFreshSnapshot(tid, attempt); }
    } native;
    native.exited = exited_id;
    Check(gtg::research::CollectStableSnapshot(native, &proof) && native.calls == 2,
        "native absence proof followed by complete real recollection");
    for (auto id : ids) {
        unsigned found{};
        for (auto handle : native.snapshot.Handles()) if (GetThreadId(handle) == id) ++found;
        Check(found == 1, "recollected real snapshot includes every stable worker exactly once");
    }
    Check(native.snapshot.Close(), "close native recollection");
    {
        gtg::research::NativeThreadSnapshot snapshot;
        Check(snapshot.Collect(4096), "collect owned process");
        for (auto id : ids) {
            unsigned matches = 0;
            for (auto handle : snapshot.Handles()) {
                Check(GetThreadId(handle) != GetCurrentThreadId(), "installer excluded");
                Check(GetProcessIdOfThread(handle) == GetCurrentProcessId(), "process identity");
                if (GetThreadId(handle) == id) ++matches;
            }
            Check(matches == 1, "all owned workers exactly once");
        }
        Check(snapshot.Close(), "explicit close");
        Check(snapshot.Close(), "idempotent close");
        Check(!snapshot.Collect(2), "capacity overflow refuses");
        Check(snapshot.Handles().empty(), "overflow cleans handles");
        Check(snapshot.Collect(4096), "reuse after refusal");
        // Scope exit exercises cleanup without an explicit Close.
    }
    if (churn) {
        HANDLE added_gate = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        Check(added_gate != nullptr, "added worker gate");
        CheckChurn(ids, added_gate);
        Check(CloseHandle(added_gate), "close added gate");
    }
    Check(GetProcessHandleCount(GetCurrentProcess(), &after) && before == after, "no handle leak");
    Check(SetEvent(event), "release workers");
    for (auto worker : workers) {
        Check(WaitForSingleObject(worker, 3000) == WAIT_OBJECT_0, "join");
        DWORD result{};
        Check(GetExitCodeThread(worker, &result) && result == 0, "worker remains live");
        Check(CloseHandle(worker), "close worker");
    }
    Check(CloseHandle(event), "close event");
    std::puts("PASS owned native snapshot, capacity refusal and scope cleanup");
    if (churn) std::puts("PASS churn=1 all cleanup and worker checks completed");
}
