#include "topapp/sampler.hpp"

#include <windows.h>
#include <psapi.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <winsvc.h>
#include <winver.h>
#include <dxgi.h>
#include <tlhelp32.h>

#include <algorithm>
#include <array>
#include <format>
#include <iterator>
#include <map>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "logging/logger.hpp"
#include "topapp/app_name.hpp"
#include "topapp/cpu_ledger.hpp"
#include "topapp/leader.hpp"
#include "topapp/pdh_instance.hpp"
#include "topapp/sweep_budget.hpp"
#include "topapp/system_process.hpp"

namespace gtg::topapp {
namespace {

// Room for eight times the 1008 processes measured.
constexpr std::size_t kMaxProcesses = 8'192;
constexpr std::size_t kMaxCachedNames = 256;
// A rate counter averages over the time since its previous collection. After
// a silence longer than this, that average describes the past, not now.
constexpr std::uint64_t kStaleRateMs = 10'000;
// Paths are kept for processes seen recently, so a process that comes and
// goes between sweeps is not asked again every time.
constexpr std::uint64_t kPathRetentionSweeps = 30;

// PROCESS_MEMORY_COUNTERS_EX2 is declared only for NTDDI_WIN10_CU and later;
// this build targets plain Windows 10. Same layout, documented; a Windows
// that does not know it rejects the size and the RAM half falls back.
struct MemoryCountersEx2 {
    PROCESS_MEMORY_COUNTERS_EX base;
    SIZE_T private_working_set;
    ULONG64 shared_commit;
};

template <typename T>
T Resolve(const HMODULE module, const char* name) noexcept {
    return module == nullptr ? nullptr : reinterpret_cast<T>(GetProcAddress(module, name));
}

HMODULE LoadSystemLibrary(const wchar_t* name) noexcept {
    return LoadLibraryExW(name, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
}

double QpcMs() noexcept {
    static const double frequency = [] {
        LARGE_INTEGER f{};
        QueryPerformanceFrequency(&f);
        return static_cast<double>(f.QuadPart);
    }();
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    return static_cast<double>(now.QuadPart) * 1000.0 / frequency;
}

std::uint64_t Join(const FILETIME& time) noexcept {
    return (static_cast<std::uint64_t>(time.dwHighDateTime) << 32) | time.dwLowDateTime;
}

// The calling thread's own CPU time, user and kernel, in milliseconds. It
// advances in clock ticks (about 15.6 ms), resolution enough against a 50 ms
// budget over five sweeps.
double ThreadCpuMs() noexcept {
    FILETIME creation{}, exit{}, kernel{}, user{};
    if (!GetThreadTimes(GetCurrentThread(), &creation, &exit, &kernel, &user)) return 0.0;
    return static_cast<double>(Join(kernel) + Join(user)) / 10'000.0;
}

// The NVIDIA adapter's LUID, as PDH spells it in instance names, if there is
// exactly one. Two are the multi-GPU case the plan defers: guessing which one
// GTG is watching would put another GPU's apps on the record, so the answer is
// none and the GPU and VRAM records stay silent.
std::optional<std::wstring> SingleNvidiaLuid() noexcept {
    try {
        const HMODULE dxgi = LoadSystemLibrary(L"dxgi.dll");
        using Create = HRESULT(WINAPI*)(REFIID, void**);
        const auto create = Resolve<Create>(dxgi, "CreateDXGIFactory1");
        std::optional<std::wstring> found;
        int count = 0;
        IDXGIFactory1* factory = nullptr;
        if (create != nullptr &&
            SUCCEEDED(create(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory)))) {
            for (UINT i = 0;; ++i) {
                IDXGIAdapter1* adapter = nullptr;
                if (factory->EnumAdapters1(i, &adapter) != S_OK || adapter == nullptr) break;
                DXGI_ADAPTER_DESC1 desc{};
                if (SUCCEEDED(adapter->GetDesc1(&desc)) && desc.VendorId == 0x10DE) {
                    ++count;
                    found = std::format(L"0x{:08x}_0x{:08x}",
                                        static_cast<unsigned long>(desc.AdapterLuid.HighPart),
                                        desc.AdapterLuid.LowPart);
                }
                adapter->Release();
            }
            factory->Release();
        }
        if (dxgi != nullptr) FreeLibrary(dxgi);
        return count == 1 ? found : std::nullopt;
    } catch (...) {
        return std::nullopt;
    }
}

struct PdhApi {
    HMODULE module{};
    decltype(&PdhOpenQueryW) open{};
    decltype(&PdhAddEnglishCounterW) add{};
    decltype(&PdhCollectQueryData) collect{};
    decltype(&PdhGetFormattedCounterArrayW) array{};
    decltype(&PdhCloseQuery) close{};
};

struct VersionApi {
    HMODULE module{};
    decltype(&GetFileVersionInfoSizeExW) size{};
    decltype(&GetFileVersionInfoExW) info{};
    decltype(&VerQueryValueW) query{};
};

// The app's own description, in `language` if it has one, then US English,
// then whatever it has. Empty when it has none.
std::wstring Description(const VersionApi& api, const std::wstring& path, const LANGID language) {
    if (api.size == nullptr || api.info == nullptr || api.query == nullptr) return {};
    DWORD ignored = 0;
    const DWORD bytes = api.size(FILE_VER_GET_LOCALISED, path.c_str(), &ignored);
    if (bytes == 0) return {};
    std::vector<std::uint8_t> block(bytes);
    if (!api.info(FILE_VER_GET_LOCALISED, path.c_str(), 0, bytes, block.data())) return {};

    struct Translation {
        WORD language;
        WORD codepage;
    };

    Translation* translations = nullptr;
    UINT length = 0;
    if (!api.query(block.data(), L"\\VarFileInfo\\Translation",
                   reinterpret_cast<void**>(&translations), &length) ||
        translations == nullptr) {
        return {};
    }
    const std::size_t count = length / sizeof(Translation);
    std::vector<Translation> order;
    for (const WORD wanted : {static_cast<WORD>(language), static_cast<WORD>(0x0409)}) {
        for (std::size_t i = 0; i < count; ++i) {
            if (translations[i].language == wanted) order.push_back(translations[i]);
        }
    }
    for (std::size_t i = 0; i < count; ++i)
        order.push_back(translations[i]);
    for (const Translation& t : order) {
        const std::wstring key =
            std::format(L"\\StringFileInfo\\{:04x}{:04x}\\FileDescription", t.language, t.codepage);
        wchar_t* text = nullptr;
        UINT chars = 0;
        if (api.query(block.data(), key.c_str(), reinterpret_cast<void**>(&text), &chars) &&
            text != nullptr && chars > 1) {
            return std::wstring(text, wcsnlen(text, chars));
        }
    }
    return {};
}

struct ProcessFacts {
    Identity identity;
    std::wstring path;
    std::uint64_t busy_100ns{};
    std::optional<std::uint64_t> private_bytes;
};

// Which services each service host is running, from the service control
// manager. A service host's description says only that it hosts services;
// the services are what a reader can place.
class ServiceMap {
public:
    struct Service {
        std::wstring name;      // the key, stable across languages
        std::wstring display;   // what a reader sees
    };

    // The services in `pid`, or nothing. A process the map does not know
    // refreshes it, at most once every kRefreshSweeps: service hosts are
    // long-lived, and one that hosts no running service must not send every
    // sweep back to the service manager.
    const std::vector<Service>* Find(const DWORD pid, const std::uint64_t generation) {
        auto found = by_pid_.find(pid);
        if (found == by_pid_.end() &&
            (refreshed_ == 0 || generation >= refreshed_ + kRefreshSweeps)) {
            refreshed_ = generation;
            Refresh();
            found = by_pid_.find(pid);
        }
        return found == by_pid_.end() ? nullptr : &found->second;
    }

private:
    static constexpr std::uint64_t kRefreshSweeps = 10;

    void Refresh() {
        by_pid_.clear();
        const SC_HANDLE manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_ENUMERATE_SERVICE);
        if (manager == nullptr) return;
        std::vector<std::uint8_t> buffer(64 * 1024);
        DWORD resume = 0;
        while (true) {
            DWORD needed = 0;
            DWORD count = 0;
            const BOOL ok = EnumServicesStatusExW(
                manager, SC_ENUM_PROCESS_INFO, SERVICE_WIN32, SERVICE_ACTIVE, buffer.data(),
                static_cast<DWORD>(buffer.size()), &needed, &count, &resume, nullptr);
            const DWORD error = ok ? ERROR_SUCCESS : GetLastError();
            if (!ok && error != ERROR_MORE_DATA) break;
            const auto* services =
                reinterpret_cast<const ENUM_SERVICE_STATUS_PROCESSW*>(buffer.data());
            for (DWORD i = 0; i < count; ++i) {
                const DWORD pid = services[i].ServiceStatusProcess.dwProcessId;
                if (pid == 0 || services[i].lpServiceName == nullptr) continue;
                by_pid_[pid].push_back(
                    {services[i].lpServiceName,
                     services[i].lpDisplayName != nullptr ? services[i].lpDisplayName : L""});
            }
            if (ok) break;
            // More to come: grow if the next entry did not fit at all.
            if (count == 0) {
                if (needed <= buffer.size() || needed > 4 * 1024 * 1024) break;
                buffer.resize(needed);
            }
        }
        CloseServiceHandle(manager);
    }

    std::unordered_map<DWORD, std::vector<Service>> by_pid_;
    std::uint64_t refreshed_{};
};

// The system process list: name and parent of every process. Returns the
// system time read before the list, so any process created earlier is in it.
std::optional<std::uint64_t> ReadProcessList(std::vector<ProcessEntry>& entries) {
    FILETIME now{};
    GetSystemTimeAsFileTime(&now);
    const double started = QpcMs();
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return std::nullopt;
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    for (BOOL ok = Process32FirstW(snapshot, &entry); ok; ok = Process32NextW(snapshot, &entry)) {
        entries.push_back({entry.th32ProcessID, entry.th32ParentProcessID, entry.szExeFile});
    }
    CloseHandle(snapshot);
    (void)logging::TryInfo(std::format(L"top app process list read; processes={} ms={:.1f}",
                                       entries.size(), QpcMs() - started));
    return Join(now);
}

// What a reader sees for a process named by what it is.
std::wstring SystemName(const SystemProcess& process, const LANGID language) {
    switch (process.kind) {
        case SystemKind::Wsl:
            return L"WSL";
        case SystemKind::VirtualMachine:
            return language == MAKELANGID(LANG_CHINESE, SUBLANG_CHINESE_TRADITIONAL)
                       ? L"虛擬機器"
                       : L"Virtual machine";
        case SystemKind::Kernel:
            break;
    }
    return process.name;
}

// Everything one sweep keeps between sweeps. Lives on the sampler thread only.
class Engine {
public:
    Engine() {
        logical_ = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
        pids_.resize(kMaxProcesses);
    }

    ~Engine() {
        if (query_ != nullptr && pdh_.close != nullptr) pdh_.close(query_);
        if (pdh_.module != nullptr) FreeLibrary(pdh_.module);
        if (version_.module != nullptr) FreeLibrary(version_.module);
    }

    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    // Everything measured goes: the next alert starts from nothing.
    void Forget() {
        for (Leader& leader : leaders_)
            leader.Reset();
        ledger_.Reset();
    }

    // Records that have left the alert lose their leader; CPU loses its
    // ledger, since its deltas would otherwise span the gap.
    void ForgetRecords(const std::uint32_t bits) {
        for (std::size_t r = 0; r < kRecordCount; ++r) {
            if ((bits & (1U << r)) != 0) leaders_[r].Reset();
        }
        if ((bits & Bit(Record::Cpu)) != 0) ledger_.Reset();
    }

    Result Sweep(const std::uint32_t mask, const LANGID language) {
        Load();
        ++generation_;
        const std::uint64_t now_ms = GetTickCount64();
        const bool want_gpu = (mask & Bit(Record::Gpu)) != 0;
        const bool want_vram = (mask & Bit(Record::Vram)) != 0;
        const bool want_cpu = (mask & Bit(Record::Cpu)) != 0;
        const bool want_ram = (mask & Bit(Record::Ram)) != 0;

        std::array<std::map<std::wstring, double>, kRecordCount> groups;
        std::unordered_map<DWORD, std::wstring> key_by_pid;
        std::map<std::wstring, std::wstring> path_by_key;
        // Names decided here rather than read from an executable's resources.
        std::map<std::wstring, std::wstring> name_by_key;
        // The process's group key, or nothing when it cannot be named: it is
        // then not ranked at all.
        const auto remember = [&](const DWORD pid,
                                  const ProcessFacts& facts) -> const std::wstring* {
            if (facts.path.empty() || IsVmWorker(facts.path)) {
                // No executable (a VM's memory process, Memory Compression) or
                // a VM's worker: the process list says what it is.
                const SystemProcess* system =
                    system_.Find(pid, facts.identity.created, generation_, ReadProcessList);
                if (system == nullptr) return nullptr;
                name_by_key.emplace(system->key, SystemName(*system, language));
                return &key_by_pid.emplace(pid, system->key).first->second;
            }
            std::wstring key = GroupKey(facts.path);
            if (IsServiceHost(facts.path)) {
                // Grouped by what each one hosts, not by its image: every
                // service host shares one, and summing them ranks an app that
                // does not exist.
                const auto* services = services_.Find(pid, generation_);
                if (services != nullptr && !services->empty()) {
                    std::vector<std::wstring> names;
                    std::vector<std::wstring> displays;
                    for (const ServiceMap::Service& service : *services) {
                        names.push_back(GroupKey(service.name));
                        displays.push_back(service.display.empty() ? service.name
                                                                   : service.display);
                    }
                    std::sort(names.begin(), names.end());
                    key += L'|';
                    for (const std::wstring& name : names)
                        key += name + L',';
                    name_by_key.emplace(key, ServiceHostName(displays));
                } else {
                    key += std::format(L"|pid:{}", pid);
                }
            }
            path_by_key.emplace(key, facts.path);
            return &key_by_pid.emplace(pid, std::move(key)).first->second;
        };

        if (want_cpu || want_ram) {
            DWORD used = 0;
            if (!K32EnumProcesses(pids_.data(), static_cast<DWORD>(pids_.size() * sizeof(DWORD)),
                                  &used)) {
                used = 0;
            }
            ledger_.BeginSweep(now_ms);
            for (std::size_t i = 0; i < used / sizeof(DWORD); ++i) {
                const DWORD pid = pids_[i];
                if (pid == 0) continue;
                const auto facts = Query(pid, want_ram);
                // No identity, or no name: the process is not ranked at all.
                // Grouping every such process under one key once invented an
                // app holding 14.7 GB of video memory that did not exist.
                if (!facts) continue;
                const std::wstring* found = remember(pid, *facts);
                if (found == nullptr) continue;
                const std::wstring& key = *found;
                if (want_cpu) {
                    if (const auto percent =
                            ledger_.Update(facts->identity, facts->busy_100ns, logical_)) {
                        groups[static_cast<std::size_t>(Record::Cpu)][key] += *percent;
                    }
                }
                if (want_ram && facts->private_bytes) {
                    groups[static_cast<std::size_t>(Record::Ram)][key] +=
                        static_cast<double>(*facts->private_bytes);
                }
            }
            ledger_.EndSweep();
        }

        if ((want_gpu || want_vram) && luid_ && query_ != nullptr) {
            std::unordered_map<DWORD, double> busiest;
            std::unordered_map<DWORD, double> dedicated;
            CollectGpu(now_ms, busiest, dedicated);
            const auto add = [&](const std::unordered_map<DWORD, double>& by_pid,
                                 const Record record) {
                for (const auto& [pid, value] : by_pid) {
                    if (value <= 0.0) continue;
                    const std::wstring* key = nullptr;
                    if (const auto found = key_by_pid.find(pid); found != key_by_pid.end()) {
                        key = &found->second;
                    } else if (const auto facts = Query(pid, false)) {
                        key = remember(pid, *facts);
                    }
                    if (key == nullptr) continue;
                    groups[static_cast<std::size_t>(record)][*key] += value;
                }
            };
            if (want_gpu) add(busiest, Record::Gpu);
            if (want_vram) add(dedicated, Record::Vram);
        }

        Result result;
        for (std::size_t r = 0; r < kRecordCount; ++r) {
            if ((mask & (1U << r)) == 0) continue;
            std::vector<Candidate> candidates;
            candidates.reserve(groups[r].size());
            for (const auto& [key, value] : groups[r])
                candidates.push_back({key, value});
            const auto& shown = leaders_[r].Update(now_ms, candidates);
            if (shown) {
                const auto named = name_by_key.find(*shown);
                const auto path = path_by_key.find(*shown);
                if (named != name_by_key.end() && !named->second.empty()) {
                    result.names[r] = named->second;
                } else if (path != path_by_key.end()) {
                    result.names[r] = NameFor(*shown, path->second, language);
                }
            }
        }

        for (auto it = paths_.begin(); it != paths_.end();) {
            it = generation_ - it->second.generation > kPathRetentionSweeps ? paths_.erase(it)
                                                                            : std::next(it);
        }
        return result;
    }

private:
    // Loaded on the first sweep, not at thread start: a reader who never sees
    // an alert never loads them. A missing library or export disables its half
    // and says so once.
    void Load() {
        if (loaded_) return;
        loaded_ = true;
        pdh_.module = LoadSystemLibrary(L"pdh.dll");
        pdh_.open = Resolve<decltype(pdh_.open)>(pdh_.module, "PdhOpenQueryW");
        pdh_.add = Resolve<decltype(pdh_.add)>(pdh_.module, "PdhAddEnglishCounterW");
        pdh_.collect = Resolve<decltype(pdh_.collect)>(pdh_.module, "PdhCollectQueryData");
        pdh_.array = Resolve<decltype(pdh_.array)>(pdh_.module, "PdhGetFormattedCounterArrayW");
        pdh_.close = Resolve<decltype(pdh_.close)>(pdh_.module, "PdhCloseQuery");
        if (pdh_.open && pdh_.add && pdh_.collect && pdh_.array && pdh_.close &&
            pdh_.open(nullptr, 0, &query_) == ERROR_SUCCESS) {
            if (pdh_.add(query_, L"\\GPU Engine(*)\\Utilization Percentage", 0, &engine_) !=
                    ERROR_SUCCESS ||
                pdh_.add(query_, L"\\GPU Process Memory(*)\\Dedicated Usage", 0, &memory_) !=
                    ERROR_SUCCESS) {
                pdh_.close(query_);
                query_ = nullptr;
            }
        }
        version_.module = LoadSystemLibrary(L"version.dll");
        version_.size =
            Resolve<decltype(version_.size)>(version_.module, "GetFileVersionInfoSizeExW");
        version_.info = Resolve<decltype(version_.info)>(version_.module, "GetFileVersionInfoExW");
        version_.query = Resolve<decltype(version_.query)>(version_.module, "VerQueryValueW");
        luid_ = SingleNvidiaLuid();
        (void)logging::TryInfo(
            std::format(L"top app sampling ready; gpu_counters={} nvidia_adapter={} app_names={} "
                        L"logical_processors={}",
                        query_ != nullptr ? L"yes" : L"no", luid_ ? L"one" : L"none-or-several",
                        version_.query != nullptr ? L"yes" : L"no", logical_));
    }

    std::optional<ProcessFacts> Query(const DWORD pid, const bool want_memory) {
        const HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (process == nullptr) return std::nullopt;
        ProcessFacts facts;
        FILETIME creation{};
        FILETIME exit{};
        FILETIME kernel{};
        FILETIME user{};
        // An exited process that something still holds open has no load and
        // no executable left to read; left in, it would ask for the process
        // list every time one appears.
        if (!GetProcessTimes(process, &creation, &exit, &kernel, &user) || Join(exit) != 0) {
            CloseHandle(process);
            return std::nullopt;
        }
        facts.identity = {pid, Join(creation)};
        facts.busy_100ns = Join(kernel) + Join(user);
        auto cached = paths_.find(facts.identity);
        if (cached == paths_.end()) {
            wchar_t path[MAX_PATH * 2];
            DWORD length = static_cast<DWORD>(std::size(path));
            std::wstring text;
            if (QueryFullProcessImageNameW(process, 0, path, &length)) text.assign(path, length);
            cached = paths_.emplace(facts.identity, PathEntry{std::move(text), 0}).first;
        }
        cached->second.generation = generation_;
        facts.path = cached->second.path;
        if (want_memory) {
            MemoryCountersEx2 ex2{};
            ex2.base.cb = sizeof(ex2);
            if (K32GetProcessMemoryInfo(process, reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&ex2),
                                        sizeof(ex2))) {
                facts.private_bytes = ex2.private_working_set;
            } else {
                PROCESS_MEMORY_COUNTERS_EX ex{};
                ex.cb = sizeof(ex);
                if (K32GetProcessMemoryInfo(
                        process, reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&ex), sizeof(ex))) {
                    facts.private_bytes = ex.PrivateUsage;
                }
            }
        }
        CloseHandle(process);
        return facts;
    }

    // Per process on the NVIDIA adapter: the busiest engine's utilisation
    // (Task Manager's per-process GPU figure) and dedicated memory in bytes.
    void CollectGpu(const std::uint64_t now_ms, std::unordered_map<DWORD, double>& busiest,
                    std::unordered_map<DWORD, double>& dedicated) {
        if (pdh_.collect(query_) != ERROR_SUCCESS) return;
        const bool rates_fresh = last_collect_ms_ != 0 && now_ms - last_collect_ms_ <= kStaleRateMs;
        last_collect_ms_ = now_ms;
        const std::wstring& luid = *luid_;
        const auto expand = [&](const PDH_HCOUNTER counter, const DWORD format,
                                const auto& consume) {
            DWORD bytes = 0;
            DWORD count = 0;
            if (pdh_.array(counter, format, &bytes, &count, nullptr) !=
                static_cast<PDH_STATUS>(PDH_MORE_DATA)) {
                return;
            }
            buffer_.resize(bytes);
            auto* items = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(buffer_.data());
            if (pdh_.array(counter, format, &bytes, &count, items) != ERROR_SUCCESS) return;
            for (DWORD i = 0; i < count; ++i) {
                const auto status = items[i].FmtValue.CStatus;
                if (status != PDH_CSTATUS_VALID_DATA && status != PDH_CSTATUS_NEW_DATA) continue;
                InstanceId id;
                if (items[i].szName == nullptr || !ParseInstance(items[i].szName, id) ||
                    !EqualsFolded(id.luid, luid)) {
                    continue;
                }
                consume(static_cast<DWORD>(id.pid), items[i].FmtValue);
            }
        };
        // The first collection after a silence only primes the rate; its
        // average would describe the silence.
        if (rates_fresh) {
            expand(engine_, PDH_FMT_DOUBLE | PDH_FMT_NOCAP100,
                   [&](const DWORD pid, const PDH_FMT_COUNTERVALUE& value) {
                       double& slot = busiest[pid];
                       slot = std::max(slot, value.doubleValue);
                   });
        }
        expand(memory_, PDH_FMT_LARGE, [&](const DWORD pid, const PDH_FMT_COUNTERVALUE& value) {
            dedicated[pid] += static_cast<double>(value.largeValue);
        });
    }

    std::wstring NameFor(const std::wstring& key, const std::wstring& path, const LANGID language) {
        if (language != names_language_) {
            names_.clear();
            names_language_ = language;
        }
        const auto found = names_.find(key);
        if (found != names_.end()) return found->second;
        if (names_.size() >= kMaxCachedNames) names_.clear();
        std::wstring name = DisplayName(Description(version_, path, language), path);
        return names_.emplace(key, std::move(name)).first->second;
    }

    struct PathEntry {
        std::wstring path;
        std::uint64_t generation{};
    };

    bool loaded_{};
    PdhApi pdh_;
    VersionApi version_;
    PDH_HQUERY query_{};
    PDH_HCOUNTER engine_{};
    PDH_HCOUNTER memory_{};
    std::optional<std::wstring> luid_;
    std::uint64_t last_collect_ms_{};
    std::vector<std::uint8_t> buffer_;
    std::vector<DWORD> pids_;
    unsigned logical_{};
    std::uint64_t generation_{};
    std::map<Identity, PathEntry> paths_;
    std::unordered_map<std::wstring, std::wstring> names_;
    LANGID names_language_{};
    CpuLedger ledger_;
    ServiceMap services_;
    SystemProcessMap system_;
    std::array<Leader, kRecordCount> leaders_{
        Leader(Dominance::Absolute), Leader(Dominance::ShareOfTotal),
        Leader(Dominance::ShareOfTotal), Leader(Dominance::ShareOfTotal)};
};

}  // namespace

bool Sampler::Start() noexcept {
    if (thread_.joinable()) return true;
    wake_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (wake_ == nullptr) return false;
    stop_.store(false, std::memory_order_release);
    mask_.store(0, std::memory_order_release);
    try {
        thread_ = std::thread(&Sampler::Run, this);
    } catch (...) {
        CloseHandle(wake_);
        wake_ = nullptr;
        return false;
    }
    return true;
}

void Sampler::Stop() noexcept {
    if (thread_.joinable()) {
        stop_.store(true, std::memory_order_release);
        SetEvent(wake_);
        thread_.join();
    }
    if (wake_ != nullptr) {
        CloseHandle(wake_);
        wake_ = nullptr;
    }
    mask_.store(0, std::memory_order_release);
    try {
        Publish({});
    } catch (...) {
    }
}

void Sampler::SetMask(const std::uint32_t mask) noexcept {
    const std::uint32_t previous = mask_.exchange(mask, std::memory_order_acq_rel);
    if (previous != mask && wake_ != nullptr) SetEvent(wake_);
}

std::optional<Result> Sampler::TryRead() const {
    std::unique_lock lock(published_mutex_, std::try_to_lock);
    if (!lock.owns_lock()) return std::nullopt;
    return published_;
}

void Sampler::Publish(Result result) {
    // Built outside, swapped inside: the lock is held for a move.
    std::lock_guard lock(published_mutex_);
    published_ = std::move(result);
}

namespace {

constexpr std::array<const wchar_t*, kRecordCount> kRecordNames{L"gpu", L"vram", L"cpu", L"ram"};

// One line when the records in alert change, so a reader's report of a
// missing name can be placed: the alert never started, or nothing was named.
void LogMask(const std::uint32_t mask) noexcept {
    try {
        std::wstring records;
        for (std::size_t r = 0; r < kRecordCount; ++r) {
            if ((mask & (1U << r)) == 0) continue;
            if (!records.empty()) records += L',';
            records += kRecordNames[r];
        }
        (void)logging::TryInfo(
            std::format(L"top app alert records={}", records.empty() ? L"none" : records));
    } catch (...) {
    }
}

// One line when the published names change. Rare by construction: a name
// needs two agreeing sweeps and is held for five seconds.
void LogNames(const Result& result) noexcept {
    try {
        std::wstring line = L"top app names";
        for (std::size_t r = 0; r < kRecordCount; ++r)
            line += std::format(L" {}={}", kRecordNames[r],
                                result.names[r] ? *result.names[r] : std::wstring(L"-"));
        (void)logging::TryInfo(line);
    } catch (...) {
    }
}

// A virtual machine's worker and memory processes run under per-machine
// accounts whose security does not let Administrators query them, so without
// the debug privilege the sweep cannot open them and a VM's load goes
// unnamed. GTG holds the privilege but has it disabled. It is enabled on a
// copy of the process token that only this thread uses: every other thread,
// and every process GTG starts, keeps the process token unchanged. What the
// sweep asks for stays limited query. On failure the thread runs on the
// process token and VMs are left unnamed, as before. Returns whether the
// thread is impersonating and must revert.
bool EnableDebugPrivilegeOnThisThread() noexcept {
    if (!ImpersonateSelf(SecurityImpersonation)) {
        const DWORD error = GetLastError();
        try {
            (void)logging::TryWarning(std::format(
                L"top app sampler runs without the debug privilege: no thread token, error {}; "
                L"virtual machines are not named",
                error));
        } catch (...) {
        }
        return false;
    }
    DWORD error = ERROR_SUCCESS;
    HANDLE token = nullptr;
    if (OpenThreadToken(GetCurrentThread(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, TRUE, &token)) {
        TOKEN_PRIVILEGES privileges{};
        privileges.PrivilegeCount = 1;
        privileges.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
        // A failed call leaves its own error. AdjustTokenPrivileges reports
        // success even when it granted nothing, and then leaves
        // ERROR_NOT_ALL_ASSIGNED, so the error code is the only way to know.
        (void)(LookupPrivilegeValueW(nullptr, SE_DEBUG_NAME, &privileges.Privileges[0].Luid) &&
               AdjustTokenPrivileges(token, FALSE, &privileges, 0, nullptr, nullptr));
        error = GetLastError();
        CloseHandle(token);
    } else {
        error = GetLastError();
    }
    if (error == ERROR_SUCCESS) {
        (void)logging::TryInfo(L"top app sampler thread has the debug privilege");
        return true;
    }
    (void)RevertToSelf();
    try {
        (void)logging::TryWarning(std::format(
            L"top app sampler runs without the debug privilege: error {}; virtual machines are "
            L"not named",
            error));
    } catch (...) {
    }
    return false;
}

}  // namespace

void Sampler::Run() noexcept {
    // Lowest CPU, I/O and memory priority for everything this thread does.
    (void)SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN);
    const bool impersonating = EnableDebugPrivilegeOnThisThread();
    try {
        Engine engine;
        DWORD wait = INFINITE;
        std::uint32_t previous = 0;
        std::uint32_t logged_mask = 0;
        Result logged;
        SweepBudget budget;
        // While paused, a change of mask still wakes the thread; the pause
        // holds until this time.
        std::uint64_t paused_until = 0;
        while (true) {
            (void)WaitForSingleObject(wake_, wait);
            if (stop_.load(std::memory_order_acquire)) break;
            const std::uint32_t mask = mask_.load(std::memory_order_acquire);
            if (mask != logged_mask) {
                LogMask(mask);
                logged_mask = mask;
            }
            if (mask == 0) {
                if (previous != 0) {
                    engine.Forget();
                    Publish({});
                    logged = {};
                }
                previous = 0;
                budget.Reset();
                paused_until = 0;
                wait = INFINITE;
                continue;
            }
            engine.ForgetRecords(previous & ~mask);
            previous = mask;
            if (const std::uint64_t now = GetTickCount64(); now < paused_until) {
                wait = static_cast<DWORD>(paused_until - now);
                continue;
            }
            const double started = QpcMs();
            const double started_cpu = ThreadCpuMs();
            Result result =
                engine.Sweep(mask, static_cast<LANGID>(language_.load(std::memory_order_relaxed)));
            const double cost = ThreadCpuMs() - started_cpu;
            const double elapsed = QpcMs() - started;
            const SweepBudget::Next next = budget.Charge(cost);
            if (next.paused) {
                // Stale names are worse than none: the records go quiet for
                // the pause.
                paused_until = GetTickCount64() + next.wait_ms;
                Publish({});
                logged = {};
                if (next.first_pause) {
                    (void)logging::TryWarning(std::format(
                        L"top app sampling paused for {} s: {} sweeps in a row over {} ms of its "
                        L"own CPU time, the last {:.1f} ms ({:.1f} ms on the clock)",
                        next.wait_ms / 1'000, SweepBudget::kOverLimit, SweepBudget::kBudgetMs, cost,
                        elapsed));
                }
                wait = next.wait_ms;
                continue;
            }
            if (next.resumed) {
                (void)logging::TryInfo(std::format(
                    L"top app sampling resumed: a sweep took {:.1f} ms of CPU time ({:.1f} ms on "
                    L"the clock)",
                    cost, elapsed));
            }
            if (result.names != logged.names) {
                LogNames(result);
                logged = result;
            }
            Publish(std::move(result));
            wait = next.wait_ms;
        }
    } catch (...) {
        // Fails closed: nothing is named until the reader turns the setting
        // off and on again.
        try {
            Publish({});
        } catch (...) {
        }
        (void)logging::TryError(L"top app sampling ended after an unexpected failure");
    }
    if (impersonating) (void)RevertToSelf();
    (void)SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_END);
}

}  // namespace gtg::topapp
