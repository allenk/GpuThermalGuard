#include "fps/dxgi_observer.hpp"

#include <windows.h>
#include <evntrace.h>
#include <evntcons.h>
#include <tdh.h>
#include <tlhelp32.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <mutex>
#include <string>
#include <thread>

#include "fps/dxgi_event.hpp"
#include "fps/display_correlator.hpp"
#include "fps/observer_admission.hpp"

namespace gtg::fps {
namespace {

constexpr GUID kDxgiProvider{0xCA11C036, 0x0102, 0x4A2D,
                             {0xA6, 0xAD, 0xF0, 0x3C, 0xFE, 0xD5, 0xD3, 0xC9}};
constexpr std::uint64_t kDxgiEventsKeyword = 0x2;
constexpr GUID kWin32kProvider{0x8C416C79, 0xD49B, 0x4F01,
                               {0xA4, 0x67, 0xE5, 0x6D, 0x3A, 0xA8, 0x23, 0x4C}};
constexpr GUID kDwmProvider{0x9E9BBA3C, 0x2E38, 0x40CB,
                            {0x99, 0xF4, 0x9E, 0x82, 0x81, 0x42, 0x51, 0x64}};
constexpr GUID kDxgKrnlProvider{0x802EC45A, 0x1E99, 0x4B83,
                                {0x99, 0x20, 0x87, 0xC9, 0x82, 0x77, 0xBA, 0x9D}};

template <typename T>
bool ReadEtwProperty(EVENT_RECORD* record, const wchar_t* name,
                     T& value) noexcept {
    PROPERTY_DATA_DESCRIPTOR descriptor{};
    descriptor.PropertyName = reinterpret_cast<ULONGLONG>(name);
    descriptor.ArrayIndex = ULONG_MAX;
    ULONG size{};
    return TdhGetPropertySize(record, 0, nullptr, 1, &descriptor, &size) ==
            ERROR_SUCCESS && size == sizeof(T) &&
        TdhGetProperty(record, 0, nullptr, 1, &descriptor, size,
                       reinterpret_cast<BYTE*>(&value)) == ERROR_SUCCESS;
}

bool ReadTokenKey(EVENT_RECORD* record, const bool dwm,
                  TokenKey& key) noexcept {
    if (!ReadEtwProperty(record, dwm ? L"luidSurface" :
                                    L"CompositionSurfaceLuid", key.luid)) return false;
    if (!dwm && record->EventHeader.EventDescriptor.Id == 301) {
        std::uint32_t count{};
        if (!ReadEtwProperty(record, L"PresentCount", count)) return false;
        key.present_count = count;
    } else if (!ReadEtwProperty(record, L"PresentCount",
                                key.present_count)) return false;
    return ReadEtwProperty(record, dwm ? L"bindId" : L"BindId",
                           key.bind_id);
}

template <std::size_t N>
bool EnableEventIds(TRACEHANDLE handle, const GUID& provider,
                    std::uint64_t keywords,
                    const std::array<USHORT, N>& ids,
                    DWORD pid = 0) noexcept {
    struct Filter {
        BOOLEAN filter_in{TRUE};
        UCHAR reserved{};
        USHORT count{static_cast<USHORT>(N)};
        std::array<USHORT, N> events{};
    } filter;
    filter.events = ids;
    std::array<EVENT_FILTER_DESCRIPTOR, 2> descriptors{};
    descriptors[0].Ptr = reinterpret_cast<ULONGLONG>(&filter);
    descriptors[0].Size = static_cast<ULONG>(offsetof(Filter, events) +
                                            sizeof(filter.events));
    descriptors[0].Type = EVENT_FILTER_TYPE_EVENT_ID;
    if (pid != 0) {
        descriptors[1].Ptr = reinterpret_cast<ULONGLONG>(&pid);
        descriptors[1].Size = sizeof(pid);
        descriptors[1].Type = EVENT_FILTER_TYPE_PID;
    }
    ENABLE_TRACE_PARAMETERS parameters{};
    parameters.Version = ENABLE_TRACE_PARAMETERS_VERSION_2;
    parameters.EnableFilterDesc = descriptors.data();
    parameters.FilterDescCount = pid != 0 ? 2 : 1;
    return EnableTraceEx2(handle, &provider, EVENT_CONTROL_CODE_ENABLE_PROVIDER,
                          TRACE_LEVEL_VERBOSE, keywords, 0, 0,
                          &parameters) == ERROR_SUCCESS;
}

struct PropertiesBuffer {
    EVENT_TRACE_PROPERTIES properties{};
    wchar_t name[128]{};
};

void InitializeProperties(PropertiesBuffer& buffer) noexcept {
    buffer = {};
    buffer.properties.Wnode.BufferSize = sizeof(buffer);
    buffer.properties.Wnode.Flags = WNODE_FLAG_TRACED_GUID;
    buffer.properties.LoggerNameOffset = offsetof(PropertiesBuffer, name);
}

std::uint64_t QpcToMicroseconds(const std::uint64_t ticks,
                                const std::uint64_t frequency) noexcept {
    if (frequency == 0) return 0;
    return (ticks / frequency) * 1'000'000 +
        (ticks % frequency) * 1'000'000 / frequency;
}

std::uint64_t NowMicroseconds(const std::uint64_t frequency) noexcept {
    LARGE_INTEGER now{};
    if (!QueryPerformanceCounter(&now)) return 0;
    return QpcToMicroseconds(static_cast<std::uint64_t>(now.QuadPart), frequency);
}

// The API name, from the module list, as a label only.
//
// A module is a capability hint and not proof of the active renderer -- a
// launcher can load d3d11 while the game runs on Vulkan -- so where this and
// the events disagree, the events win and this is the weaker claim. It opens a
// snapshot handle and nothing else; when that is refused the answer is Unknown
// and the reading is untouched, which is the whole difference between a label
// and the gate this replaced.
// Which graphics runtimes are loaded. Deliberately NOT collapsed to a single
// answer here: a Vulkan program has d3d12.dll, dxgi.dll and opengl32.dll in its
// module list too, dragged in by the ICD -- measured, 84 modules in our own
// Vulkan generator. Any fixed priority over that set is wrong for somebody.
//
// The events decide the family; this only names it inside the family the
// events already established.
struct LoadedRuntimes {
    bool d3d9{}, d3d11{}, d3d12{}, vulkan{}, opengl{};
    bool readable{};   // false when the module list was refused
};

LoadedRuntimes BackendFromModules(const std::uint32_t pid) noexcept {
    HANDLE snapshot = INVALID_HANDLE_VALUE;
    for (int attempt = 0; attempt < 3; ++attempt) {
        snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, pid);
        if (snapshot != INVALID_HANDLE_VALUE) break;
        // Microsoft documents ERROR_BAD_LENGTH as a transient module-list race.
        if (GetLastError() != ERROR_BAD_LENGTH) return {};
    }
    if (snapshot == INVALID_HANDLE_VALUE) return {};
    LoadedRuntimes loaded{};
    MODULEENTRY32W module{};
    module.dwSize = sizeof(module);
    if (Module32FirstW(snapshot, &module) != FALSE) {
        loaded.readable = true;
        do {
            loaded.d3d12 |= _wcsicmp(module.szModule, L"d3d12.dll") == 0;
            loaded.d3d11 |= _wcsicmp(module.szModule, L"d3d11.dll") == 0;
            loaded.d3d9 |= _wcsicmp(module.szModule, L"d3d9.dll") == 0;
            loaded.vulkan |= _wcsicmp(module.szModule, L"vulkan-1.dll") == 0;
            loaded.opengl |= _wcsicmp(module.szModule, L"opengl32.dll") == 0;
        } while (Module32NextW(snapshot, &module) != FALSE);
    }
    CloseHandle(snapshot);
    return loaded;
}

}  // namespace

class DxgiObserver::Impl final {
public:
    Impl() {
        DWORD session_id{};
        (void)ProcessIdToSessionId(GetCurrentProcessId(), &session_id);
        session_.name = L"GpuThermalGuard.FPS.Session." + std::to_wstring(session_id);
#ifdef GTG_FPS_ISOLATED_SMOKE
        // The standalone observer must never reclaim the running GTG session.
        session_.name += L".Smoke." + std::to_wstring(GetCurrentProcessId());
#endif
        controller_ = std::thread([this] { ControlLoop(); });
    }

    ~Impl() { Stop(); }

    void Update(const std::optional<Identity> target) noexcept {
        {
            std::lock_guard lock(command_mutex_);
            const Identity next = target.value_or(Identity{});
            if (next == desired_) return;
            desired_ = next;
        }
        {
            std::lock_guard lock(published_mutex_);
            published_ = {};
        }
        command_cv_.notify_one();
    }

    [[nodiscard]] Snapshot TryRead() const noexcept {
        std::unique_lock lock(published_mutex_, std::try_to_lock);
        return lock.owns_lock() ? published_ : Snapshot{};
    }

    void Stop() noexcept {
        {
            std::lock_guard lock(command_mutex_);
            exiting_ = true;
            desired_ = {};
        }
        command_cv_.notify_one();
        if (controller_.joinable()) controller_.join();
    }

private:
    struct Session {
        Impl* owner{};
        Identity identity{};
        std::wstring name;
        TRACEHANDLE controller_handle{};
        PROCESSTRACE_HANDLE consumer_handle{INVALID_PROCESSTRACE_HANDLE};
        EVENT_TRACE_LOGFILEW logfile{};
        std::thread consumer;
        std::atomic<bool> consumer_alive{false};
        DisplayCorrelator correlator;
        ObserverProbation probation;
        std::atomic<bool> correlation_failed{false};
        std::uint32_t dwm_pid{};
        // What each side of the measurement actually delivered. Written on the
        // consumer thread, read once when the session stops, and journalled --
        // these event ids are not a documented contract, so "the reader saw a
        // dash" is not an acceptable way to find out that a Windows update
        // moved them.
        std::atomic<std::uint64_t> kernel_presents{0};
        std::atomic<std::uint64_t> displayed_frames{0};
        // The size the program last presented, from whichever provider saw it.
        // Written on the consumer thread, read under tracker_mutex_.
        std::atomic<std::uint32_t> presented_width{0};
        std::atomic<std::uint32_t> presented_height{0};
        // Whether the DXGI composition path was ever seen. It is what says the
        // program is a DXGI runtime, which no module list can prove.
        std::atomic<bool> saw_composition_token{false};
        LoadedRuntimes loaded_runtimes{};
        HWND window{nullptr};
        std::uint64_t qpc_frequency{};
        ULONG last_events_lost{};
        bool running{};
    } session_;

    // Guarded by tracker_mutex_, which the UI thread already takes to read.
    DxgiObserver::SessionCounts last_counts_{};

    static void WINAPI OnRecord(PEVENT_RECORD record) noexcept {
        if (record == nullptr) return;
        auto* session = static_cast<Session*>(record->UserContext);
        if (session == nullptr || session->owner == nullptr ||
            session->correlation_failed.load(std::memory_order_relaxed)) return;
        const auto& header = record->EventHeader;
        const auto id = header.EventDescriptor.Id;
        const auto timestamp = QpcToMicroseconds(
            static_cast<std::uint64_t>(header.TimeStamp.QuadPart),
            session->qpc_frequency);
        auto& correlation = session->correlator;
        bool valid = true;
        if (IsEqualGUID(header.ProviderId, kDxgiProvider) &&
            header.ProcessId == session->identity.pid) {
            if (id == 42) {
                if (header.EventDescriptor.Version != 0 ||
                    record->UserDataLength < 12) valid = false;
                else {
                    std::uint64_t surface{};
                    std::uint32_t flags{};
                    std::memcpy(&surface, record->UserData, sizeof(surface));
                    std::memcpy(&flags,
                        static_cast<const std::uint8_t*>(record->UserData) + 8,
                        sizeof(flags));
                    correlation.OnPresentStart(header.ThreadId, surface,
                                               flags, timestamp);
                }
            } else if (id == 43) {
                if (header.EventDescriptor.Version != 0 ||
                    record->UserDataLength < 4) valid = false;
                else {
                    std::int32_t result{};
                    std::memcpy(&result, record->UserData, sizeof(result));
                    correlation.OnPresentStop(header.ThreadId, result);
                }
            }
        } else if (IsEqualGUID(header.ProviderId, kWin32kProvider)) {
            if (id == 201 && header.ProcessId == session->identity.pid) {
                TokenKey key{};
                valid = header.EventDescriptor.Version == 1 &&
                    ReadTokenKey(record, false, key);
                if (valid) correlation.OnToken(header.ThreadId, key, timestamp);
                // The swapchain the program put up, not the window it landed
                // in: measured, buffers a quarter of the window make this
                // report the quarter.
                std::uint32_t width{}, height{};
                if (ReadEtwProperty(record, L"DestWidth", width) &&
                    ReadEtwProperty(record, L"DestHeight", height) &&
                    width != 0 && height != 0) {
                    session->presented_width.store(width, std::memory_order_relaxed);
                    session->presented_height.store(height, std::memory_order_relaxed);
                }
                session->saw_composition_token.store(true, std::memory_order_relaxed);
            } else if (id == 301) {
                TokenKey key{};
                std::uint32_t state{};
                valid = header.EventDescriptor.Version == 1 &&
                    ReadTokenKey(record, false, key) &&
                    ReadEtwProperty(record, L"NewState", state);
                if (valid && state == 3) correlation.OnInFrame(key);
                if (valid && state == 6) correlation.OnDiscard(key);
            }
        } else if (IsEqualGUID(header.ProviderId, kDwmProvider)) {
            if (id == 15) {
                valid = header.EventDescriptor.Version == 0;
                if (valid) {
                    session->dwm_pid = header.ProcessId;
                    correlation.SetDwmThread(header.ThreadId);
                }
            } else if (id == 196) {
                TokenKey key{};
                valid = header.EventDescriptor.Version == 0 &&
                    ReadTokenKey(record, true, key);
                if (valid) correlation.OnSurfaceUpdate(key);
            }
        } else if (IsEqualGUID(header.ProviderId, kDxgKrnlProvider)) {
            // The kernel's own flip submission, for the target itself. No pid
            // filter is possible here -- EVENT_FILTER_TYPE_PID is honoured by
            // the DXGI provider but not by this one, measured -- so every
            // process's flips arrive and all but the target's are dropped.
            // The non-DXGI path's rectangles. Source is what was presented;
            // Dest is where it landed. Only the size is taken -- the origins
            // differ by the window border and say nothing about resolution.
            if (id == 166 && header.ProcessId == session->identity.pid) {
                std::uint32_t left{}, right{}, top{}, bottom{};
                if (ReadEtwProperty(record, L"Source.Left", left) &&
                    ReadEtwProperty(record, L"Source.Right", right) &&
                    ReadEtwProperty(record, L"Source.Top", top) &&
                    ReadEtwProperty(record, L"Source.Bottom", bottom) &&
                    right > left && bottom > top) {
                    session->presented_width.store(right - left,
                                                   std::memory_order_relaxed);
                    session->presented_height.store(bottom - top,
                                                    std::memory_order_relaxed);
                }
            } else if (id == 184 && header.ProcessId == session->identity.pid) {
                std::uint64_t window{};
                valid = header.EventDescriptor.Version == 1 &&
                    ReadEtwProperty(record, L"hWindow", window);
                if (valid && window != 0) {
                    session->kernel_presents.fetch_add(1, std::memory_order_relaxed);
                    std::lock_guard lock(session->owner->tracker_mutex_);
                    session->owner->tracker_.RecordPresented(
                        session->identity, window, timestamp);
                }
            } else if (id == 252 && header.ProcessId == session->dwm_pid &&
                header.ThreadId != 0) {
                valid = header.EventDescriptor.Version == 0;
                if (valid) correlation.OnDwmFlip(header.ThreadId, timestamp);
            } else if (id == 178 && header.ProcessId == session->dwm_pid &&
                       header.ThreadId != 0) {
                std::uint32_t sequence{};
                std::uint32_t packet_type{};
                std::uint32_t is_present{};
                valid = header.EventDescriptor.Version == 1 &&
                    ReadEtwProperty(record, L"SubmitSequence", sequence) &&
                    ReadEtwProperty(record, L"PacketType", packet_type) &&
                    ReadEtwProperty(record, L"bPresent", is_present);
                if (valid && (packet_type == 3 || packet_type == 7 ||
                              is_present != 0))
                    correlation.OnDwmQueue(header.ThreadId, sequence);
            } else if (id == 273) {
                std::uint32_t count{};
                valid = header.EventDescriptor.Version == 4 &&
                    ReadEtwProperty(record, L"FlipEntryCount", count) &&
                    count <= 32;
                if (valid && count != 0) {
                    PROPERTY_DATA_DESCRIPTOR descriptor{};
                    descriptor.PropertyName = reinterpret_cast<ULONGLONG>(
                        L"FlipSubmitSequence");
                    descriptor.ArrayIndex = ULONG_MAX;
                    std::array<std::uint64_t, 32> sequences{};
                    valid = TdhGetProperty(record, 0, nullptr, 1, &descriptor,
                        count * sizeof(std::uint64_t),
                        reinterpret_cast<BYTE*>(sequences.data())) == ERROR_SUCCESS;
                    if (valid) for (std::uint32_t i = 0; i < count; ++i)
                        correlation.OnVSync(
                            static_cast<std::uint32_t>(sequences[i] >> 32),
                            timestamp);
                }
            }
        }
        if (!valid || !correlation.Healthy()) {
            correlation.MarkLost();
            session->correlation_failed.store(true, std::memory_order_release);
            return;
        }
        while (const auto shown = correlation.Pop()) {
            session->displayed_frames.fetch_add(1, std::memory_order_relaxed);
            session->probation.NoteDisplayed();
            std::lock_guard lock(session->owner->tracker_mutex_);
            session->owner->tracker_.RecordDisplayed(session->identity,
                shown->surface, shown->timestamp_us);
        }
    }

    bool StartSession(const Identity identity) noexcept {
        // No capability check. The submission side now comes from the
        // graphics kernel, which every presenting program reaches whatever API
        // it used, so there is nothing to look for in a module list -- and the
        // module list was the one step that could be refused: a real game
        // returned ERROR_ACCESS_DENIED for it on 2026-09-20 and the session
        // was rejected before it began. Evidence is what the events say, and
        // the RateTracker's own confidence gates decide when to publish.
        constexpr auto admission = ObserverAdmission::Normal;
        session_.identity = identity;
        // Both are labels, taken once at session start. Neither can stop the
        // session: a refused module list leaves the name Unknown, and a window
        // we cannot resolve leaves the fallback size unavailable.
        session_.loaded_runtimes = BackendFromModules(identity.pid);
        session_.window = GetForegroundWindow();
        session_.presented_width.store(0, std::memory_order_relaxed);
        session_.presented_height.store(0, std::memory_order_relaxed);
        session_.saw_composition_token.store(false, std::memory_order_relaxed);
        session_.owner = this;
        session_.probation.Begin(admission, GetTickCount64());
        session_.correlator.Reset();
        session_.correlation_failed.store(false, std::memory_order_release);
        session_.dwm_pid = 0;
        session_.last_events_lost = 0;
        LARGE_INTEGER frequency{};
        if (!QueryPerformanceFrequency(&frequency) || frequency.QuadPart <= 0)
            return false;
        session_.qpc_frequency = static_cast<std::uint64_t>(frequency.QuadPart);
        {
            std::lock_guard lock(tracker_mutex_);
            const auto now = NowMicroseconds(session_.qpc_frequency);
            tracker_.SetTarget(identity, now);
            tracker_.MarkLost(now);  // A restarted trace never reuses old frames.
        }

        PropertiesBuffer properties{};
        InitializeProperties(properties);
        properties.properties.Wnode.ClientContext = 1;  // QPC timestamps.
        properties.properties.BufferSize = 16;          // Small DXGI events.
        properties.properties.MinimumBuffers = 2;
        properties.properties.MaximumBuffers = 8;
        properties.properties.FlushTimer = 1;
        properties.properties.LogFileMode = EVENT_TRACE_REAL_TIME_MODE |
            EVENT_TRACE_NO_PER_PROCESSOR_BUFFERING;
        ULONG start = StartTraceW(&session_.controller_handle,
                                  session_.name.c_str(), &properties.properties);
        if (start == ERROR_ALREADY_EXISTS) {
            // Tray is per-session singleton. This deterministic name belongs
            // to a previous crashed GTG FPS observer; reclaim only that name.
            PropertiesBuffer orphan{};
            InitializeProperties(orphan);
            (void)ControlTraceW(0, session_.name.c_str(), &orphan.properties,
                                EVENT_TRACE_CONTROL_STOP);
            InitializeProperties(properties);
            properties.properties.Wnode.ClientContext = 1;
            properties.properties.BufferSize = 16;
            properties.properties.MinimumBuffers = 2;
            properties.properties.MaximumBuffers = 8;
            properties.properties.FlushTimer = 1;
            properties.properties.LogFileMode = EVENT_TRACE_REAL_TIME_MODE |
                EVENT_TRACE_NO_PER_PROCESSOR_BUFFERING;
            start = StartTraceW(&session_.controller_handle,
                                session_.name.c_str(), &properties.properties);
        }
        if (start != ERROR_SUCCESS) return false;
        if (!EnableEventIds(session_.controller_handle, kDxgiProvider,
                            kDxgiEventsKeyword, std::array<USHORT, 2>{42, 43},
                            identity.pid) ||
            !EnableEventIds(session_.controller_handle, kWin32kProvider,
                            0x8000000400001000ULL,
                            std::array<USHORT, 2>{201, 301}) ||
            !EnableEventIds(session_.controller_handle, kDwmProvider,
                            0x8000000000000080ULL,
                            std::array<USHORT, 2>{15, 196}) ||
            // 184 and 215 are the universal submission signal: measured
            // one-for-one against the present count under D3D11, D3D12, Vulkan
            // and OpenGL, and attributed to the presenting process. They are
            // what lets a Vulkan or OpenGL program have a number at all.
            !EnableEventIds(session_.controller_handle, kDxgKrnlProvider,
                            0x4000000008000001ULL,
                            std::array<USHORT, 6>{166, 178, 184, 215, 252, 273})) {
            StopSession();
            return false;
        }

        session_.logfile = {};
        session_.logfile.LoggerName = session_.name.data();
        session_.logfile.ProcessTraceMode = PROCESS_TRACE_MODE_REAL_TIME |
            PROCESS_TRACE_MODE_EVENT_RECORD | PROCESS_TRACE_MODE_RAW_TIMESTAMP;
        session_.logfile.EventRecordCallback = &OnRecord;
        session_.logfile.Context = &session_;
        session_.consumer_handle = OpenTraceW(&session_.logfile);
        if (session_.consumer_handle == INVALID_PROCESSTRACE_HANDLE) {
            StopSession();
            return false;
        }
        try {
            session_.consumer_alive.store(true, std::memory_order_release);
            session_.consumer = std::thread([this] {
                auto handle = session_.consumer_handle;
                (void)ProcessTrace(&handle, 1, nullptr, nullptr);
                session_.consumer_alive.store(false, std::memory_order_release);
                command_cv_.notify_one();
            });
        } catch (...) {
            session_.consumer_alive.store(false, std::memory_order_release);
            StopSession();
            return false;
        }
        session_.running = true;
        return true;
    }

    void StopSession() noexcept {
        session_.running = false;
        if (session_.consumer_handle != INVALID_PROCESSTRACE_HANDLE) {
            (void)CloseTrace(session_.consumer_handle);
            session_.consumer_handle = INVALID_PROCESSTRACE_HANDLE;
        }
        if (session_.controller_handle != 0) {
            PropertiesBuffer properties{};
            InitializeProperties(properties);
            (void)ControlTraceW(session_.controller_handle, session_.name.c_str(),
                                &properties.properties, EVENT_TRACE_CONTROL_STOP);
            session_.controller_handle = 0;
        }
        if (session_.consumer.joinable()) session_.consumer.join();
        session_.consumer_alive.store(false, std::memory_order_release);
        // The consumer has joined, so these are final.
        last_counts_ = {true,
                        session_.displayed_frames.load(std::memory_order_relaxed),
                        session_.kernel_presents.load(std::memory_order_relaxed)};
        session_.displayed_frames.store(0, std::memory_order_relaxed);
        session_.kernel_presents.store(0, std::memory_order_relaxed);
    }

public:
    DxgiObserver::SessionCounts TakeLastCounts() noexcept {
        std::lock_guard lock(tracker_mutex_);
        const auto counts = last_counts_;
        last_counts_ = {};
        return counts;
    }

private:

    bool SessionHealthy() noexcept {
        if (!session_.running ||
            !session_.consumer_alive.load(std::memory_order_acquire) ||
            session_.correlation_failed.load(std::memory_order_acquire))
            return false;
        PropertiesBuffer properties{};
        InitializeProperties(properties);
        if (ControlTraceW(session_.controller_handle, session_.name.c_str(),
                          &properties.properties, EVENT_TRACE_CONTROL_QUERY) !=
            ERROR_SUCCESS) return false;
        const ULONG lost = properties.properties.EventsLost +
            properties.properties.RealTimeBuffersLost;
        if (lost != session_.last_events_lost) {
            session_.last_events_lost = lost;
            std::lock_guard lock(tracker_mutex_);
            tracker_.MarkLost(NowMicroseconds(session_.qpc_frequency));
            return false;
        }
        return true;
    }

    void Publish(const bool healthy) noexcept {
        Snapshot current{};
        {
            std::lock_guard lock(tracker_mutex_);
            current = tracker_.Read(NowMicroseconds(session_.qpc_frequency),
                                    healthy && session_.probation.CanPublish());
        }
        // A label never appears without a number: "-" has to keep meaning
        // nothing was measured, and a lone API name beside a dash would read
        // as a half-answer.
        if (current.status == Status::Ready) {
            current.backend = ResolveBackend();
            current.resolution = ResolveResolution();
        }
        std::lock_guard lock(published_mutex_);
        published_ = current;
    }

    // The events decide the family; the modules only name it. Where they
    // disagree the events win, because a module list says what could be used
    // and the events say what was.
    [[nodiscard]] Backend ResolveBackend() const noexcept {
        const auto& loaded = session_.loaded_runtimes;
        if (!loaded.readable) {
            // The list was refused. The events still know the family, and
            // saying that much is better than saying nothing.
            return session_.saw_composition_token.load(std::memory_order_relaxed)
                       ? Backend::DXGI : Backend::Unknown;
        }
        if (session_.saw_composition_token.load(std::memory_order_relaxed)) {
            // A composition token means a DXGI runtime presented. Within that
            // family d3d12 outranks d3d11, because a D3D12 program commonly
            // keeps d3d11 loaded for an interop layer or an overlay.
            if (loaded.d3d12) return Backend::D3D12;
            if (loaded.d3d11) return Backend::D3D11;
            return Backend::DXGI;
        }
        // No composition token, so the DXGI modules are not the renderer --
        // whatever dragged them in, it was not what put this frame up. Vulkan
        // first: its ICD loads opengl32 as well, measured.
        if (loaded.vulkan) return Backend::Vulkan;
        if (loaded.opengl) return Backend::OpenGL;
        if (loaded.d3d9) return Backend::D3D9;
        return Backend::Unknown;
    }

    // Presented first, output second, nothing third -- the owner's rule.
    [[nodiscard]] Resolution ResolveResolution() const noexcept {
        const std::uint32_t width =
            session_.presented_width.load(std::memory_order_relaxed);
        const std::uint32_t height =
            session_.presented_height.load(std::memory_order_relaxed);
        if (width != 0 && height != 0)
            return {width, height, ResolutionSource::Presented};
        // The window the target is presenting into. A weaker claim -- it is
        // where the frame landed, not what was put up -- so it is labelled as
        // such rather than silently standing in for the other.
        RECT client{};
        if (session_.window != nullptr && IsWindow(session_.window) &&
            GetClientRect(session_.window, &client) != FALSE &&
            client.right > client.left && client.bottom > client.top) {
            return {static_cast<std::uint32_t>(client.right - client.left),
                    static_cast<std::uint32_t>(client.bottom - client.top),
                    ResolutionSource::Output};
        }
        return {};
    }

    void ControlLoop() noexcept {
        Identity active{};
        std::uint64_t retry_at_ms{};
        for (;;) {
            Identity desired{};
            {
                std::unique_lock lock(command_mutex_);
                command_cv_.wait_for(lock, std::chrono::milliseconds(500), [&] {
                    return exiting_ || desired_ != active;
                });
                if (exiting_) break;
                desired = desired_;
            }
            if (desired != active) {
                StopSession();
                active = desired;
                retry_at_ms = 0;
                Publish(false);
            }
            if (active.pid == 0 || active.creation_time == 0) continue;
            if (!session_.running && GetTickCount64() >= retry_at_ms) {
                if (!StartSession(active)) retry_at_ms = GetTickCount64() + 5'000;
            }
            const bool healthy = SessionHealthy();
            const auto now_ms = GetTickCount64();
            const bool probation_expired = healthy && session_.running &&
                session_.probation.Expired(now_ms);
            Publish(healthy && !probation_expired);
            if (session_.running && (!healthy || probation_expired)) {
                StopSession();
                {
                    std::lock_guard lock(tracker_mutex_);
                    tracker_.MarkLost(NowMicroseconds(session_.qpc_frequency));
                }
                retry_at_ms = probation_expired
                    ? session_.probation.RetryAt(now_ms) : now_ms + 5'000;
            }
        }
        StopSession();
        Publish(false);
    }

    mutable std::mutex command_mutex_;
    std::condition_variable command_cv_;
    Identity desired_{};
    bool exiting_{};
    std::thread controller_;
    std::mutex tracker_mutex_;
    RateTracker tracker_;
    mutable std::mutex published_mutex_;
    Snapshot published_{};
};

DxgiObserver::DxgiObserver() = default;
DxgiObserver::~DxgiObserver() { Stop(); }

void DxgiObserver::SetTarget(const std::optional<Identity> target) noexcept {
    if (!impl_ && target && target->pid != 0) {
        try {
            impl_ = std::make_unique<Impl>();
        } catch (...) {
            return;
        }
    }
    if (impl_) impl_->Update(target);
}

Snapshot DxgiObserver::TryRead() const noexcept {
    return impl_ ? impl_->TryRead() : Snapshot{};
}

DxgiObserver::SessionCounts DxgiObserver::TakeLastSessionCounts() noexcept {
    return impl_ ? impl_->TakeLastCounts() : SessionCounts{};
}

void DxgiObserver::Stop() noexcept {
    if (!impl_) return;
    impl_->Stop();
    impl_.reset();
}

namespace {

// A UWP or Store app does not own its own top-level window. The window belongs
// to ApplicationFrameHost, which hosts it and renders nothing; the app runs in
// its own process and presents from there. Resolving the foreground window to
// its owner therefore picks the wrong process, and the app's frames are never
// looked at.
//
// Measured on Microsoft Solitaire: ApplicationFrameHost produced not one event
// on any of the four providers in ten seconds, while the Solitaire process
// produced 1201 DXGI Presents and 1201 Win32k composition tokens -- a complete,
// working chain nobody was reading.
//
// The hosted app is the child window of class Windows.UI.Core.CoreWindow.
// Finding it costs three read-only calls: no process handle, no module list,
// nothing that can be refused.
struct HostedSearch {
    DWORD host_pid{};
    DWORD found_pid{};
};

BOOL CALLBACK FindHostedCoreWindow(HWND child, LPARAM parameter) noexcept {
    auto* search = reinterpret_cast<HostedSearch*>(parameter);
    DWORD pid{};
    if (GetWindowThreadProcessId(child, &pid) == 0 || pid == 0 ||
        pid == search->host_pid) return TRUE;
    wchar_t class_name[64]{};
    if (GetClassNameW(child, class_name, 64) == 0) return TRUE;
    if (std::wcscmp(class_name, L"Windows.UI.Core.CoreWindow") != 0) return TRUE;
    search->found_pid = pid;
    return FALSE;   // the first one is the app; stop
}

// Only ever applied to the frame host. A hop taken anywhere else would start
// reporting some other program's frames as the foreground program's, which is
// worse than reporting nothing.
DWORD HostedApplicationPid(const HWND window, const DWORD host_pid) noexcept {
    wchar_t class_name[64]{};
    if (GetClassNameW(window, class_name, 64) == 0) return 0;
    if (std::wcscmp(class_name, L"ApplicationFrameWindow") != 0) return 0;
    HostedSearch search{host_pid, 0};
    (void)EnumChildWindows(window, FindHostedCoreWindow,
                           reinterpret_cast<LPARAM>(&search));
    return search.found_pid;
}

}  // namespace

std::optional<Identity> ForegroundCandidate() noexcept {
    const HWND foreground = GetForegroundWindow();
    if (foreground == nullptr || foreground == GetShellWindow() ||
        IsWindowVisible(foreground) == FALSE) return std::nullopt;
    wchar_t class_name[64]{};
    if (GetClassNameW(foreground, class_name, 64) != 0 &&
        (std::wcscmp(class_name, L"Progman") == 0 ||
         std::wcscmp(class_name, L"WorkerW") == 0 ||
         std::wcscmp(class_name, L"Shell_TrayWnd") == 0 ||
         std::wcscmp(class_name, L"CabinetWClass") == 0)) return std::nullopt;
    DWORD pid{};
    if (GetWindowThreadProcessId(foreground, &pid) == 0 ||
        pid == 0 || pid == GetCurrentProcessId()) return std::nullopt;
    if (const DWORD hosted = HostedApplicationPid(foreground, pid);
        hosted != 0 && hosted != GetCurrentProcessId()) {
        pid = hosted;
    }
    return ProcessIdentity(pid);
}

std::optional<Identity> ProcessIdentity(const std::uint32_t pid) noexcept {
    if (pid == 0) return std::nullopt;
    const HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,
                                       FALSE, pid);
    if (process == nullptr) return std::nullopt;
    BOOL is_wow64 = FALSE;
    if (!IsWow64Process(process, &is_wow64) || is_wow64) {
        CloseHandle(process);
        return std::nullopt;
    }
    FILETIME created{}, exited{}, kernel{}, user{};
    const bool valid = GetProcessTimes(process, &created, &exited,
                                       &kernel, &user) != FALSE;
    CloseHandle(process);
    if (!valid) return std::nullopt;
    const std::uint64_t creation_time =
        (static_cast<std::uint64_t>(created.dwHighDateTime) << 32) |
        created.dwLowDateTime;
    if (creation_time == 0) return std::nullopt;
    return Identity{pid, creation_time};
}

}  // namespace gtg::fps
