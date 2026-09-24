// winsock2 ahead of windows.h; see throughput_win32.cpp for why this order is
// contained here rather than imposed on the callers.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2ipdef.h>
#include <windows.h>
#include <iphlpapi.h>
#include <tcpestats.h>

#include <map>
#include <utility>
#include <vector>

#include "fps/dxgi_observer.hpp"
#include "net/estats_win32.hpp"

namespace gtg::net {
namespace {

using GetExtendedTcpTableFn = decltype(&::GetExtendedTcpTable);
using GetPerTcpConnectionEStatsFn = decltype(&::GetPerTcpConnectionEStats);
using SetPerTcpConnectionEStatsFn = decltype(&::SetPerTcpConnectionEStats);
using FlushIpPathTableFn = decltype(&::FlushIpPathTable);
using ResolveIpNetEntry2Fn = decltype(&::ResolveIpNetEntry2);
using GetBestRoute2Fn = decltype(&::GetBestRoute2);

// The four counters that change; the rest of TCP_ESTATS_PATH_ROD_v0 is not
// needed and is deliberately not carried around.
struct Baseline {
    ULONG count_rtt{};
    ULONG retransmitted{};
    ULONG duplicate_acks{};
    ULONG timeouts{};
};

// A connection is identified by both ends, because a port is reused.
using Key = std::pair<std::uint64_t, std::uint64_t>;

constexpr Key KeyOf(const MIB_TCPROW& row) noexcept {
    return {(static_cast<std::uint64_t>(row.dwLocalAddr) << 32) | row.dwLocalPort,
            (static_cast<std::uint64_t>(row.dwRemoteAddr) << 32) | row.dwRemotePort};
}

}  // namespace

struct Probe::State {
    HMODULE module{nullptr};
    GetExtendedTcpTableFn get_table{nullptr};
    GetPerTcpConnectionEStatsFn get_estats{nullptr};
    SetPerTcpConnectionEStatsFn set_estats{nullptr};
    FlushIpPathTableFn flush_path{nullptr};
    ResolveIpNetEntry2Fn resolve_neighbour{nullptr};
    GetBestRoute2Fn best_route{nullptr};
    std::vector<MIB_TCPROW> rows;
    std::map<Key, Baseline> baseline;
};

Probe::~Probe() {
    if (state_ != nullptr) {
        // The estats flags are left exactly as they are. Disabling them is the
        // one thing the documentation asks callers not to do.
        if (state_->module != nullptr) FreeLibrary(state_->module);
        delete state_;
        state_ = nullptr;
    }
}

bool Probe::Begin() noexcept {
    const auto foreground = fps::ForegroundCandidate();
    if (!foreground) {
        status_ = ProbeStatus::NoForeground;
        return false;
    }
    return BeginFor(foreground->pid);
}

bool Probe::BeginFor(const std::uint32_t process_id) noexcept {
    try {
        delete state_;
        state_ = new State();
    } catch (...) {
        state_ = nullptr;
        status_ = ProbeStatus::NoLibrary;
        return false;
    }
    watched_ = 0;

    state_->module = LoadLibraryW(L"iphlpapi.dll");
    if (state_->module == nullptr) {
        status_ = ProbeStatus::NoLibrary;
        return false;
    }
    const auto bind = [&](const char* name) {
        return reinterpret_cast<void*>(GetProcAddress(state_->module, name));
    };
    state_->get_table = reinterpret_cast<GetExtendedTcpTableFn>(bind("GetExtendedTcpTable"));
    state_->get_estats =
        reinterpret_cast<GetPerTcpConnectionEStatsFn>(bind("GetPerTcpConnectionEStats"));
    state_->set_estats =
        reinterpret_cast<SetPerTcpConnectionEStatsFn>(bind("SetPerTcpConnectionEStats"));
    state_->flush_path = reinterpret_cast<FlushIpPathTableFn>(bind("FlushIpPathTable"));
    state_->resolve_neighbour =
        reinterpret_cast<ResolveIpNetEntry2Fn>(bind("ResolveIpNetEntry2"));
    state_->best_route = reinterpret_cast<GetBestRoute2Fn>(bind("GetBestRoute2"));
    if (state_->get_table == nullptr || state_->get_estats == nullptr ||
        state_->set_estats == nullptr) {
        status_ = ProbeStatus::NoLibrary;
        return false;
    }

    // Whose connections. Asking about every connection on the machine would be
    // both useless -- 337 of them, measured -- and the heavy-handedness the
    // documentation warns against.
    DWORD size = 0;
    state_->get_table(nullptr, &size, FALSE, AF_INET, TCP_TABLE_OWNER_PID_ALL, 0);
    if (size == 0) {
        status_ = ProbeStatus::NoConnections;
        return false;
    }
    std::vector<char> buffer;
    try {
        buffer.resize(size);
    } catch (...) {
        status_ = ProbeStatus::NoLibrary;
        return false;
    }
    auto* table = reinterpret_cast<PMIB_TCPTABLE_OWNER_PID>(buffer.data());
    if (state_->get_table(table, &size, FALSE, AF_INET, TCP_TABLE_OWNER_PID_ALL, 0) !=
        NO_ERROR) {
        status_ = ProbeStatus::NoConnections;
        return false;
    }

    try {
        for (DWORD i = 0; i < table->dwNumEntries; ++i) {
            const auto& entry = table->table[i];
            if (entry.dwState != MIB_TCP_STATE_ESTAB) continue;
            if (entry.dwOwningPid != process_id) continue;
            MIB_TCPROW row{};
            row.dwState = entry.dwState;
            row.dwLocalAddr = entry.dwLocalAddr;
            row.dwLocalPort = entry.dwLocalPort;
            row.dwRemoteAddr = entry.dwRemoteAddr;
            row.dwRemotePort = entry.dwRemotePort;
            state_->rows.push_back(row);
        }
    } catch (...) {
        status_ = ProbeStatus::NoLibrary;
        return false;
    }
    if (state_->rows.empty()) {
        // A UDP-only game, or nothing in the foreground that talks. Saying so
        // is the honest answer; inventing a verdict from the machine's other
        // traffic would not be about the program the reader is looking at.
        status_ = ProbeStatus::NoConnections;
        return false;
    }

    int permitted = 0;
    for (auto& row : state_->rows) {
        TCP_ESTATS_PATH_RW_v0 rw{};
        rw.EnableCollection = TRUE;
        if (state_->set_estats(&row, TcpConnectionEstatsPath,
                               reinterpret_cast<PUCHAR>(&rw), 0, sizeof(rw),
                               0) == NO_ERROR)
            ++permitted;
    }
    if (permitted == 0) {
        // Measured: ERROR_ACCESS_DENIED without elevation. This program's
        // manifest requires administrator, so reaching here means something
        // else refused.
        status_ = ProbeStatus::NotPermitted;
        return false;
    }
    watched_ = permitted;

    // The baseline. Counters are cumulative, so only the differences over the
    // window say anything about now.
    try {
        for (auto& row : state_->rows) {
            TCP_ESTATS_PATH_RW_v0 rw{};
            TCP_ESTATS_PATH_ROD_v0 rod{};
            if (state_->get_estats(&row, TcpConnectionEstatsPath,
                                   reinterpret_cast<PUCHAR>(&rw), 0, sizeof(rw),
                                   nullptr, 0, 0, reinterpret_cast<PUCHAR>(&rod),
                                   0, sizeof(rod)) != NO_ERROR)
                continue;
            if (!rw.EnableCollection) continue;
            state_->baseline[KeyOf(row)] =
                Baseline{rod.CountRtt, rod.PktsRetrans, rod.DupAcksIn, rod.Timeouts};
        }
    } catch (...) {
        status_ = ProbeStatus::NoLibrary;
        return false;
    }

    status_ = ProbeStatus::Ready;
    return true;
}

void Probe::Attempt() noexcept {
    if (state_ == nullptr || status_ != ProbeStatus::Ready) return;
    if (state_->flush_path != nullptr) (void)state_->flush_path(AF_UNSPEC);
    if (state_->resolve_neighbour == nullptr || state_->best_route == nullptr) return;

    // Re-resolve the next hop. A neighbour entry that has gone stale costs the
    // next packet an address resolution, which is the only mechanism among
    // these with a plausible effect -- and even that was not detectable above
    // the noise when it was measured.
    SOCKADDR_INET destination{};
    destination.si_family = AF_INET;
    destination.Ipv4.sin_family = AF_INET;
    // A routing table lookup key, never contacted; see throughput_win32.cpp.
    destination.Ipv4.sin_addr.S_un.S_un_b.s_b1 = 8;
    destination.Ipv4.sin_addr.S_un.S_un_b.s_b2 = 8;
    destination.Ipv4.sin_addr.S_un.S_un_b.s_b3 = 8;
    destination.Ipv4.sin_addr.S_un.S_un_b.s_b4 = 8;
    MIB_IPFORWARD_ROW2 route{};
    SOCKADDR_INET source{};
    NET_LUID luid{};
    if (state_->best_route(&luid, 0, nullptr, &destination, 0, &route, &source) !=
        NO_ERROR)
        return;
    MIB_IPNET_ROW2 neighbour{};
    neighbour.Address = route.NextHop;
    neighbour.InterfaceLuid = route.InterfaceLuid;
    (void)state_->resolve_neighbour(&neighbour, nullptr);
}

Verdict Probe::Finish() noexcept {
    if (state_ == nullptr || status_ != ProbeStatus::Ready) return {};
    VerdictBuilder builder;
    for (auto& row : state_->rows) {
        TCP_ESTATS_PATH_RW_v0 rw{};
        TCP_ESTATS_PATH_ROD_v0 rod{};
        if (state_->get_estats(&row, TcpConnectionEstatsPath,
                               reinterpret_cast<PUCHAR>(&rw), 0, sizeof(rw),
                               nullptr, 0, 0, reinterpret_cast<PUCHAR>(&rod), 0,
                               sizeof(rod)) != NO_ERROR)
            continue;
        if (!rw.EnableCollection) continue;
        const auto found = state_->baseline.find(KeyOf(row));
        const Baseline before = found == state_->baseline.end() ? Baseline{}
                                                                : found->second;
        // A counter that went backwards means the connection was reset or the
        // statistics were re-enabled by someone else. Treated as no samples
        // rather than as a negative delta.
        const auto delta = [](const ULONG now, const ULONG then) -> std::uint32_t {
            return now >= then ? now - then : 0;
        };
        builder.Add(Readings{rod.SmoothedRtt, rod.RttVar,
                             delta(rod.PktsRetrans, before.retransmitted),
                             delta(rod.DupAcksIn, before.duplicate_acks),
                             delta(rod.Timeouts, before.timeouts),
                             delta(rod.CountRtt, before.count_rtt)});
    }
    return builder.Result();
}

}  // namespace gtg::net
