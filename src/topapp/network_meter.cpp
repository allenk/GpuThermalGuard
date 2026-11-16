#include "topapp/network_meter.hpp"

#include <windows.h>
#include <evntrace.h>
#include <evntcons.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cwchar>
#include <iterator>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

#include "topapp/app_name.hpp"
#include "topapp/details.hpp"

namespace gtg::topapp {

namespace {

// Room for the program's name in the session name, which is copied into a
// fixed buffer below.
constexpr std::size_t kMaxProgramName = 40;

// Microsoft-Windows-Kernel-Network.
constexpr GUID kKernelNetwork{
    0x7dd42a49, 0x5329, 0x4832, {0x8d, 0xfd, 0x43, 0xd9, 0x79, 0x15, 0x3a, 0x88}};
// KERNEL_NETWORK_KEYWORD_IPV4 | KERNEL_NETWORK_KEYWORD_IPV6.
constexpr ULONGLONG kIpKeywords = 0x10 | 0x20;

struct Properties {
    EVENT_TRACE_PROPERTIES properties{};
    wchar_t name[96]{};
};

// The buffers the spike ran with: 64 KB each, 4 to 32 of them. No event was
// lost at 42 k a second, and the memory exists only while Details is open.
void Initialize(Properties& buffer) noexcept {
    buffer = {};
    buffer.properties.Wnode.BufferSize = sizeof(buffer);
    buffer.properties.Wnode.Flags = WNODE_FLAG_TRACED_GUID;
    buffer.properties.Wnode.ClientContext = 1;  // QPC timestamps, the cheapest
    buffer.properties.LoggerNameOffset = offsetof(Properties, name);
    buffer.properties.BufferSize = 64;
    buffer.properties.MinimumBuffers = 4;
    buffer.properties.MaximumBuffers = 32;
    buffer.properties.FlushTimer = 1;
    buffer.properties.LogFileMode = EVENT_TRACE_REAL_TIME_MODE;
}

// Whether `handle` is still the session named `name`. A session handle is a
// logger number, which Windows gives to another session once this one stops:
// after a stop from outside, the handle may name a stranger's session.
bool IsSession(const TRACEHANDLE handle, const std::wstring& name, Properties& query) noexcept {
    Initialize(query);
    if (ControlTraceW(handle, nullptr, &query.properties, EVENT_TRACE_CONTROL_QUERY) !=
        ERROR_SUCCESS) {
        return false;
    }
    return name == std::wstring_view(query.name, wcsnlen(query.name, std::size(query.name)));
}

}  // namespace

std::wstring NetworkSessionName(const std::wstring_view module_path,
                                const std::uint32_t logon_session) {
    std::wstring_view program = ExecutableStem(module_path).substr(0, kMaxProgramName);
    if (program.empty()) program = L"GpuThermalGuard";
    return std::wstring(program) + L".Network.Session." + std::to_wstring(logon_session);
}

std::wstring NetworkMeter::SessionName() {
    std::wstring path(MAX_PATH, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    path.resize(length < path.size() ? length : 0);
    DWORD logon_session = 0;
    (void)ProcessIdToSessionId(GetCurrentProcessId(), &logon_session);
    return NetworkSessionName(path, logon_session);
}

struct NetworkMeter::State {
    std::wstring name;
    TRACEHANDLE session{};
    TRACEHANDLE consumer{INVALID_PROCESSTRACE_HANDLE};
    EVENT_TRACE_LOGFILEW logfile{};
    std::thread thread;
    std::mutex lock;
    std::unordered_map<std::uint32_t, std::uint64_t> bytes;
    ULONG lost_seen{};
    // Set by the consumer when ProcessTrace returns.
    std::atomic<bool> ended{false};
    std::atomic<std::uint32_t> end_status{0};

    // On the consumer thread, once an event: two numbers copied and added.
    static void WINAPI OnRecord(EVENT_RECORD* record) {
        if (record == nullptr || !IsEqualGUID(record->EventHeader.ProviderId, kKernelNetwork))
            return;
        const auto event = DecodeNetworkEvent(record->EventHeader.EventDescriptor.Id,
                                              record->UserData, record->UserDataLength);
        if (!event || event->bytes == 0) return;
        auto* state = static_cast<State*>(record->UserContext);
        try {
            std::lock_guard guard(state->lock);
            state->bytes[event->pid] += event->bytes;
        } catch (...) {
            // A table that cannot grow undercounts; it never stops the trace.
        }
    }

    // The session and its consumer, whichever exist, stopped and released.
    void Close() noexcept {
        if (consumer != INVALID_PROCESSTRACE_HANDLE) {
            (void)CloseTrace(consumer);
            consumer = INVALID_PROCESSTRACE_HANDLE;
        }
        if (session != 0) {
            // Through the handle, and only while it is still this session.
            Properties stop;
            if (IsSession(session, name, stop)) {
                Initialize(stop);
                (void)ControlTraceW(session, nullptr, &stop.properties, EVENT_TRACE_CONTROL_STOP);
            }
            session = 0;
        }
        if (thread.joinable()) thread.join();
    }
};

NetworkMeter::NetworkMeter() = default;

NetworkMeter::~NetworkMeter() {
    Stop();
}

bool NetworkMeter::Running() const noexcept {
    return state_ != nullptr && state_->thread.joinable();
}

bool NetworkMeter::Start() noexcept {
    if (Running()) return true;
    try {
        auto state = std::make_unique<State>();
        state->bytes.reserve(256);
        // One name per program and logon session: a GTG in another session
        // keeps its own, and a name left by a crashed GTG in this one is ours
        // to reclaim.
        state->name = SessionName();

        Properties properties;
        Initialize(properties);
        ULONG status = StartTraceW(&state->session, state->name.c_str(), &properties.properties);
        if (status == ERROR_ALREADY_EXISTS) {
            Properties orphan;
            Initialize(orphan);
            (void)ControlTraceW(0, state->name.c_str(), &orphan.properties,
                                EVENT_TRACE_CONTROL_STOP);
            Initialize(properties);
            status = StartTraceW(&state->session, state->name.c_str(), &properties.properties);
        }
        if (status != ERROR_SUCCESS) {
            state->session = 0;
            return false;
        }

        // Only the eight data events reach the session: connects, accepts,
        // retransmits and the rest are filtered out in the kernel.
        struct Filter {
            BOOLEAN filter_in{TRUE};
            UCHAR reserved{};
            USHORT count{static_cast<USHORT>(kNetworkDataEvents.size())};
            std::array<USHORT, kNetworkDataEvents.size()> events{};
        } filter;

        for (std::size_t i = 0; i < kNetworkDataEvents.size(); ++i)
            filter.events[i] = kNetworkDataEvents[i];
        EVENT_FILTER_DESCRIPTOR descriptor{};
        descriptor.Ptr = reinterpret_cast<ULONGLONG>(&filter);
        descriptor.Size = static_cast<ULONG>(sizeof(filter));
        descriptor.Type = EVENT_FILTER_TYPE_EVENT_ID;
        ENABLE_TRACE_PARAMETERS parameters{};
        parameters.Version = ENABLE_TRACE_PARAMETERS_VERSION_2;
        parameters.EnableFilterDesc = &descriptor;
        parameters.FilterDescCount = 1;
        if (EnableTraceEx2(state->session, &kKernelNetwork, EVENT_CONTROL_CODE_ENABLE_PROVIDER,
                           TRACE_LEVEL_INFORMATION, kIpKeywords, 0, 0,
                           &parameters) != ERROR_SUCCESS) {
            state->Close();
            return false;
        }

        state->logfile.LoggerName = state->name.data();
        state->logfile.ProcessTraceMode =
            PROCESS_TRACE_MODE_REAL_TIME | PROCESS_TRACE_MODE_EVENT_RECORD;
        state->logfile.EventRecordCallback = &State::OnRecord;
        state->logfile.Context = state.get();
        state->consumer = OpenTraceW(&state->logfile);
        if (state->consumer == INVALID_PROCESSTRACE_HANDLE) {
            state->Close();
            return false;
        }
        const TRACEHANDLE consumer = state->consumer;
        State* const shared = state.get();
        state->thread = std::thread([consumer, shared] {
            // The lowest CPU, I/O and memory priority, as the sampler's own
            // thread: a starved consumer loses events, which the column
            // reports, rather than taking time from anything else.
            (void)SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN);
            TRACEHANDLE handle = consumer;
            const ULONG status = ProcessTrace(&handle, 1, nullptr, nullptr);
            // Returned by Stop, or on its own: Ended tells the two apart,
            // since after Stop nobody asks.
            shared->end_status.store(status, std::memory_order_relaxed);
            shared->ended.store(true, std::memory_order_release);
            (void)SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_END);
        });
        state_ = std::move(state);
        return true;
    } catch (...) {
        return false;
    }
}

std::optional<NetworkMeter::End> NetworkMeter::Ended() const noexcept {
    if (state_ == nullptr || !state_->ended.load(std::memory_order_acquire)) return std::nullopt;
    End end;
    end.status = state_->end_status.load(std::memory_order_relaxed);
    Properties query;
    Initialize(query);
    end.session_exists = ControlTraceW(0, state_->name.c_str(), &query.properties,
                                       EVENT_TRACE_CONTROL_QUERY) == ERROR_SUCCESS;
    return end;
}

void NetworkMeter::Stop() noexcept {
    if (state_ == nullptr) return;
    state_->Close();
    state_.reset();
}

NetworkMeter::Taken NetworkMeter::Take() {
    Taken taken;
    if (state_ == nullptr) return taken;
    std::unordered_map<std::uint32_t, std::uint64_t> counted;
    counted.reserve(256);
    {
        std::lock_guard guard(state_->lock);
        counted.swap(state_->bytes);
    }
    taken.bytes_by_pid.assign(counted.begin(), counted.end());
    Properties query;
    if (IsSession(state_->session, state_->name, query)) {
        const ULONG lost = query.properties.EventsLost;
        taken.lost = lost >= state_->lost_seen ? lost - state_->lost_seen : lost;
        state_->lost_seen = lost;
    }
    return taken;
}

}  // namespace gtg::topapp
