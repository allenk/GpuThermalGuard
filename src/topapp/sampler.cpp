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
#include "topapp/network_meter.hpp"
#include "topapp/pdh_instance.hpp"
#include "topapp/sweep_budget.hpp"
#include "topapp/system_process.hpp"
#include "topapp/working_directory.hpp"

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

// All four records, as Bit() flags.
constexpr std::uint32_t kAllRecords = (1U << kRecordCount) - 1;

// The network trace delivers its first events about two seconds after it
// starts (measured: its buffers flush once a second). A rate read before then
// is an absence of data, not an idle network.
constexpr std::uint64_t kNetworkWarmupMs = 2'000;
// A network trace that ended under the sampler is started again after this
// long, so one that keeps ending costs a start every ten seconds and no more.
constexpr std::uint64_t kNetworkRetryMs = 10'000;
// A process can change its working directory, so one read is trusted this
// long; the representatives of a steady table cost a read each per interval.
constexpr std::uint64_t kDirectoryRefreshMs = 5'000;

// The records a sweep measures: those in alert, and all of them for Details.
constexpr std::uint32_t Wanted(const std::uint32_t mask, const bool details) noexcept {
    return details ? kAllRecords : mask;
}

struct Adapter {
    // As PDH spells it in instance names.
    std::wstring luid;
    // Its dedicated video memory: what Details measures VRAM against.
    std::uint64_t dedicated_bytes{};
};

// The NVIDIA adapter, if there is exactly one. Two are the multi-GPU case the
// plan defers: guessing which one GTG is watching would put another GPU's apps
// on the record, so the answer is none and the GPU and VRAM records stay
// silent.
std::optional<Adapter> SingleNvidiaAdapter() noexcept {
    try {
        const HMODULE dxgi = LoadSystemLibrary(L"dxgi.dll");
        using Create = HRESULT(WINAPI*)(REFIID, void**);
        const auto create = Resolve<Create>(dxgi, "CreateDXGIFactory1");
        std::optional<Adapter> found;
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
                    found =
                        Adapter{std::format(L"0x{:08x}_0x{:08x}",
                                            static_cast<unsigned long>(desc.AdapterLuid.HighPart),
                                            desc.AdapterLuid.LowPart),
                                static_cast<std::uint64_t>(desc.DedicatedVideoMemory)};
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

// The machine's physical memory in bytes, or zero: what Details measures RAM
// against.
double PhysicalBytes() noexcept {
    MEMORYSTATUSEX status{};
    status.dwLength = sizeof(status);
    return GlobalMemoryStatusEx(&status) ? static_cast<double>(status.ullTotalPhys) : 0.0;
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

    // Everything measured goes: the next alert starts from nothing. The
    // network trace too, which only an open Details keeps running.
    void Forget() {
        for (Leader& leader : leaders_)
            leader.Reset();
        ledger_.Reset();
        CloseNetwork();
        directories_.clear();
    }

    // Records that have left the alert lose their leader. Records no longer
    // measured at all -- in alert or for Details -- lose their rates: CPU its
    // ledger, since its deltas would otherwise span the gap.
    void ForgetRecords(const std::uint32_t unnamed, const std::uint32_t unmeasured) {
        for (std::size_t r = 0; r < kRecordCount; ++r) {
            if ((unnamed & (1U << r)) != 0) leaders_[r].Reset();
        }
        if ((unmeasured & Bit(Record::Cpu)) != 0) ledger_.Reset();
    }

    Result Sweep(const std::uint32_t mask, const bool details, const LANGID language) {
        Load();
        ++generation_;
        const std::uint64_t now_ms = GetTickCount64();
        const std::uint32_t wanted = Wanted(mask, details);
        const bool want_gpu = (wanted & Bit(Record::Gpu)) != 0;
        const bool want_vram = (wanted & Bit(Record::Vram)) != 0;
        const bool want_cpu = (wanted & Bit(Record::Cpu)) != 0;
        const bool want_ram = (wanted & Bit(Record::Ram)) != 0;
        // What this sweep measured, for Details: a rate on its first sweep, or
        // a half that could not run, is not a zero.
        std::uint32_t measured = 0;
        std::uint32_t processes = 0;

        // One group per Details column; the records are the first four.
        std::array<std::map<std::wstring, double>, kColumnCount> groups;
        std::unordered_map<DWORD, std::wstring> key_by_pid;
        std::map<std::wstring, std::wstring> path_by_key;
        // Names decided here rather than read from an executable's resources.
        std::map<std::wstring, std::wstring> name_by_key;
        // For Details: each column's heaviest process of each app, and who
        // each process is, so its directory is read from the right one.
        std::array<std::unordered_map<std::wstring, Heaviest>, kColumnCount> heaviest;
        std::unordered_map<DWORD, Identity> identity_by_pid;
        const auto offer = [&](const Column column, const std::wstring& key, const DWORD pid,
                               const double value) {
            if (details) heaviest[static_cast<std::size_t>(column)][key].Offer(pid, value);
        };
        // The process's group key, or nothing when it cannot be named: it is
        // then not ranked at all.
        const auto remember = [&](const DWORD pid,
                                  const ProcessFacts& facts) -> const std::wstring* {
            if (details) identity_by_pid.emplace(pid, facts.identity);
            if (IsWslRelay(facts.path)) {
                // WSL's relay: grouped with the VM, so a transfer forwarded
                // through localhost is WSL's on one end and the client's on
                // the other.
                const std::wstring key(kWslKey);
                name_by_key.emplace(key, SystemName({SystemKind::Wsl, {}, {}}, language));
                return &key_by_pid.emplace(pid, key).first->second;
            }
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
                ++processes;
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
                        offer(Column::Cpu, key, pid, *percent);
                        measured |= Bit(Record::Cpu);
                    }
                }
                if (want_ram && facts->private_bytes) {
                    const auto bytes = static_cast<double>(*facts->private_bytes);
                    groups[static_cast<std::size_t>(Record::Ram)][key] += bytes;
                    offer(Column::Ram, key, pid, bytes);
                    measured |= Bit(Record::Ram);
                }
            }
            ledger_.EndSweep();
        }

        if ((want_gpu || want_vram) && adapter_ && query_ != nullptr) {
            std::unordered_map<DWORD, double> busiest;
            std::unordered_map<DWORD, double> dedicated;
            const Collected collected = CollectGpu(now_ms, busiest, dedicated);
            if (collected.rates) measured |= Bit(Record::Gpu);
            if (collected.memory) measured |= Bit(Record::Vram);
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
                    offer(ColumnOf(record), *key, pid, value);
                }
            };
            if (want_gpu) add(busiest, Record::Gpu);
            if (want_vram) add(dedicated, Record::Vram);
        }

        // The network column, while Details is open: what the trace counted
        // since the last sweep, by app. Its first two seconds are not a
        // measurement -- the trace delivers its first events about two
        // seconds after it starts -- so they are counted but not shown.
        std::uint32_t network_lost = 0;
        if (details) {
            if (const auto taken = TakeNetwork(now_ms)) {
                std::unordered_map<DWORD, std::wstring> network_keys;
                for (const auto& [pid, bytes] : taken->bytes_by_pid) {
                    if (const auto found = key_by_pid.find(pid); found != key_by_pid.end()) {
                        network_keys.emplace(pid, found->second);
                    } else if (const auto facts = Query(pid, false)) {
                        if (const std::wstring* key = remember(pid, *facts))
                            network_keys.emplace(pid, *key);
                    }
                }
                groups[static_cast<std::size_t>(Column::Network)] =
                    NetworkByApp(taken->bytes_by_pid, network_keys, taken->seconds);
                for (const auto& [pid, bytes] : taken->bytes_by_pid) {
                    if (const auto key = network_keys.find(pid); key != network_keys.end())
                        offer(Column::Network, key->second, pid, static_cast<double>(bytes));
                }
                network_lost = taken->lost;
                if (now_ms - network_started_ms_ >= kNetworkWarmupMs)
                    measured |= Bit(Column::Network);
            }
        } else {
            CloseNetwork();
            // Directories are read only for an open Details.
            directories_.clear();
        }

        // A group's name: decided here, or read from its executable.
        const auto name_of = [&](const std::wstring& key) -> std::optional<std::wstring> {
            const auto named = name_by_key.find(key);
            if (named != name_by_key.end() && !named->second.empty()) return named->second;
            const auto path = path_by_key.find(key);
            if (path != path_by_key.end()) return NameFor(key, path->second, language);
            return std::nullopt;
        };

        Result result;
        for (std::size_t r = 0; r < kRecordCount; ++r) {
            if ((mask & (1U << r)) == 0) continue;
            std::vector<Candidate> candidates;
            candidates.reserve(groups[r].size());
            for (const auto& [key, value] : groups[r])
                candidates.push_back({key, value});
            const auto& shown = leaders_[r].Update(now_ms, candidates);
            if (shown) result.names[r] = name_of(*shown);
        }

        if (details) {
            Details table;
            table.processes = processes;
            table.available = Bit(Record::Cpu) | Bit(Record::Ram);
            if (adapter_ && query_ != nullptr)
                table.available |= Bit(Record::Gpu) | Bit(Record::Vram);
            // Available unless Windows refused the trace: one waiting to start
            // again is measuring, not missing.
            if (!network_refused_) table.available |= Bit(Column::Network);
            table.measured = measured;
            double network_total = 0.0;
            for (const auto& [key, rate] : groups[static_cast<std::size_t>(Column::Network)])
                network_total += rate;
            table.totals = {100.0, adapter_ ? static_cast<double>(adapter_->dedicated_bytes) : 0.0,
                            100.0, PhysicalBytes(), network_total};
            table.network_lost = network_lost;
            std::unordered_map<std::wstring, std::uint32_t> members;
            for (const auto& [pid, key] : key_by_pid)
                ++members[key];
            // Stepped into once, at the first directory not in the cache.
            std::optional<ProcessTokenScope> own_token;
            for (const std::wstring& key : MergeTopKeys(groups, kDetailsRows)) {
                auto name = name_of(key);
                if (!name) continue;
                DetailsRow row;
                row.name = std::move(*name);
                const auto count = members.find(key);
                row.processes = count == members.end() ? 1 : count->second;
                const auto path = path_by_key.find(key);
                for (std::size_t c = 0; c < kColumnCount; ++c) {
                    if ((measured & (1U << c)) == 0) continue;
                    const auto value = groups[c].find(key);
                    row.values[c] = value == groups[c].end() ? 0.0 : value->second;
                    const auto held = heaviest[c].find(key);
                    if (held == heaviest[c].end() || held->second.pid == 0) continue;
                    Representative& who = row.representatives[c];
                    who.pid = held->second.pid;
                    // A process named by what it is -- a VM, the kernel's --
                    // has no executable and no directory of its own.
                    const auto identity = identity_by_pid.find(who.pid);
                    if (path == path_by_key.end() || identity == identity_by_pid.end()) continue;
                    who.directory = DirectoryOf(identity->second, now_ms, own_token);
                    who.folder = FolderOf(who.directory, path->second, windows_directory_);
                }
                table.rows.push_back(std::move(row));
            }
            result.details = std::move(table);
        }

        for (auto it = paths_.begin(); it != paths_.end();) {
            it = generation_ - it->second.generation > kPathRetentionSweeps ? paths_.erase(it)
                                                                            : std::next(it);
        }
        for (auto it = directories_.begin(); it != directories_.end();) {
            it = generation_ - it->second.generation > kPathRetentionSweeps ? directories_.erase(it)
                                                                            : std::next(it);
        }
        return result;
    }

private:
    struct DirectoryEntry {
        std::uint64_t created{};
        std::wstring directory;
        std::uint64_t read_ms{};
        std::uint64_t generation{};
    };

    // A process's working directory, from the cache while it is younger than
    // kDirectoryRefreshMs, else read again on the process token. Empty when it
    // cannot be read.
    std::wstring DirectoryOf(const Identity& identity, const std::uint64_t now_ms,
                             std::optional<ProcessTokenScope>& own_token) {
        const auto found = directories_.find(identity.pid);
        if (found != directories_.end() && found->second.created == identity.created &&
            now_ms - found->second.read_ms < kDirectoryRefreshMs) {
            found->second.generation = generation_;
            return found->second.directory;
        }
        if (!own_token) own_token.emplace();
        std::wstring directory;
        if (own_token->Ready()) directory = ReadWorkingDirectory(identity).value_or(L"");
        directories_[identity.pid] = {identity.created, directory, now_ms, generation_};
        return directory;
    }

    struct NetworkTake {
        std::vector<std::pair<std::uint32_t, std::uint64_t>> bytes_by_pid;
        std::uint32_t lost{};
        double seconds{};
    };

    // The trace, started on the first sweep with Details open; after that,
    // what it counted since the previous sweep. Nothing on the sweep that
    // starts it, and nothing when Windows refuses it -- tried once each time
    // Details opens. A trace that ends under it -- its session stopped from
    // outside, or its delivery ended -- counts nothing from then on, so it is
    // stopped, said so, and started again kNetworkRetryMs later.
    std::optional<NetworkTake> TakeNetwork(const std::uint64_t now_ms) {
        if (network_.Running()) {
            if (const auto end = network_.Ended()) {
                (void)logging::TryWarning(std::format(
                    L"top app network trace ended by itself after {} s: status={} session={}; "
                    L"starting it again in {} s",
                    (now_ms - network_started_ms_) / 1'000, end->status,
                    end->session_exists ? L"running" : L"gone", kNetworkRetryMs / 1'000));
                network_.Stop();
                network_retry_ms_ = now_ms + kNetworkRetryMs;
                return std::nullopt;
            }
        }
        if (!network_.Running()) {
            if (network_refused_ || now_ms < network_retry_ms_) return std::nullopt;
            if (!network_.Start()) {
                network_refused_ = true;
                (void)logging::TryWarning(
                    L"top app network trace refused; Details shows no network column");
                return std::nullopt;
            }
            network_started_ms_ = now_ms;
            network_taken_ms_ = now_ms;
            (void)logging::TryInfo(L"top app network trace started");
            return std::nullopt;
        }
        NetworkMeter::Taken taken = network_.Take();
        NetworkTake out;
        out.bytes_by_pid = std::move(taken.bytes_by_pid);
        out.lost = taken.lost;
        out.seconds = static_cast<double>(now_ms - network_taken_ms_) / 1'000.0;
        network_taken_ms_ = now_ms;
        return out;
    }

    void CloseNetwork() noexcept {
        if (network_.Running()) {
            network_.Stop();
            (void)logging::TryInfo(L"top app network trace stopped");
        }
        network_refused_ = false;
        network_retry_ms_ = 0;
    }

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
        wchar_t windows[MAX_PATH]{};
        const UINT length = GetWindowsDirectoryW(windows, MAX_PATH);
        if (length > 0 && length < MAX_PATH) windows_directory_.assign(windows, length);
        adapter_ = SingleNvidiaAdapter();
        (void)logging::TryInfo(
            std::format(L"top app sampling ready; gpu_counters={} nvidia_adapter={} app_names={} "
                        L"logical_processors={}",
                        query_ != nullptr ? L"yes" : L"no", adapter_ ? L"one" : L"none-or-several",
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

    // What a collection produced: utilisation needs a previous collection,
    // memory does not.
    struct Collected {
        bool rates{};
        bool memory{};
    };

    // Per process on the NVIDIA adapter: the busiest engine's utilisation
    // (Task Manager's per-process GPU figure) and dedicated memory in bytes.
    Collected CollectGpu(const std::uint64_t now_ms, std::unordered_map<DWORD, double>& busiest,
                         std::unordered_map<DWORD, double>& dedicated) {
        if (pdh_.collect(query_) != ERROR_SUCCESS) return {};
        const bool rates_fresh = last_collect_ms_ != 0 && now_ms - last_collect_ms_ <= kStaleRateMs;
        last_collect_ms_ = now_ms;
        const std::wstring& luid = adapter_->luid;
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
        return {rates_fresh, true};
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
    std::optional<Adapter> adapter_;
    std::uint64_t last_collect_ms_{};
    std::vector<std::uint8_t> buffer_;
    std::vector<DWORD> pids_;
    unsigned logical_{};
    std::uint64_t generation_{};
    std::map<Identity, PathEntry> paths_;
    // Details' working directories by process, and Windows' own folder, where
    // a directory says nothing about the app.
    std::unordered_map<DWORD, DirectoryEntry> directories_;
    std::wstring windows_directory_;
    std::unordered_map<std::wstring, std::wstring> names_;
    LANGID names_language_{};
    CpuLedger ledger_;
    ServiceMap services_;
    SystemProcessMap system_;
    // Details' network column: the trace exists only while Details is open.
    NetworkMeter network_;
    std::uint64_t network_started_ms_{};
    std::uint64_t network_taken_ms_{};
    bool network_refused_{};
    // When a trace that ended under the sampler may start again.
    std::uint64_t network_retry_ms_{};
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
    details_.store(false, std::memory_order_release);
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
    details_.store(false, std::memory_order_release);
    try {
        Publish({});
    } catch (...) {
    }
}

void Sampler::SetMask(const std::uint32_t mask) noexcept {
    const std::uint32_t previous = mask_.exchange(mask, std::memory_order_acq_rel);
    if (previous != mask && wake_ != nullptr) SetEvent(wake_);
}

void Sampler::SetDetails(const bool open) noexcept {
    const bool previous = details_.exchange(open, std::memory_order_acq_rel);
    if (previous != open && wake_ != nullptr) SetEvent(wake_);
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

// One line when Details opens or closes. Its rows are never logged: no
// process name goes into the journal.
void LogDetails(const bool open) noexcept {
    (void)logging::TryInfo(open ? L"top app details open" : L"top app details closed");
}

// What is published while sampling is paused: no names, and for an open
// Details a table that says why it is empty.
Result Quiet(const bool details) {
    Result result;
    if (details) {
        result.details = Details{};
        result.details->paused = true;
    }
    return result;
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
        bool previous_details = false;
        std::uint32_t logged_mask = 0;
        bool logged_details = false;
        Result logged;
        SweepBudget budget;
        // While paused, a change of mask still wakes the thread; the pause
        // holds until this time.
        std::uint64_t paused_until = 0;
        while (true) {
            (void)WaitForSingleObject(wake_, wait);
            if (stop_.load(std::memory_order_acquire)) break;
            const std::uint32_t mask = mask_.load(std::memory_order_acquire);
            const bool details = details_.load(std::memory_order_acquire);
            if (mask != logged_mask) {
                LogMask(mask);
                logged_mask = mask;
            }
            if (details != logged_details) {
                LogDetails(details);
                logged_details = details;
            }
            if (mask == 0 && !details) {
                if (previous != 0 || previous_details) {
                    engine.Forget();
                    Publish({});
                    logged = {};
                }
                previous = 0;
                previous_details = false;
                budget.Reset();
                paused_until = 0;
                wait = INFINITE;
                continue;
            }
            engine.ForgetRecords(previous & ~mask,
                                 Wanted(previous, previous_details) & ~Wanted(mask, details));
            previous = mask;
            previous_details = details;
            if (const std::uint64_t now = GetTickCount64(); now < paused_until) {
                // Details opened during a pause says so, rather than staying
                // blank for the rest of it.
                Publish(Quiet(details));
                wait = static_cast<DWORD>(paused_until - now);
                continue;
            }
            const double started = QpcMs();
            const double started_cpu = ThreadCpuMs();
            Result result = engine.Sweep(
                mask, details, static_cast<LANGID>(language_.load(std::memory_order_relaxed)));
            const double cost = ThreadCpuMs() - started_cpu;
            const double elapsed = QpcMs() - started;
            const SweepBudget::Next next = budget.Charge(cost);
            if (next.paused) {
                // Stale names are worse than none: the records go quiet for
                // the pause.
                paused_until = GetTickCount64() + next.wait_ms;
                Publish(Quiet(details));
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
