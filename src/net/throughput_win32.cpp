// winsock2 ahead of windows.h, or windows.h brings in winsock 1.1 and the two
// definitions of the socket types collide. This is the reason the declaration
// in throughput_win32.hpp names no Win32 type: the order is contained here
// instead of being imposed on every caller.
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

#include "net/throughput_win32.hpp"

namespace gtg::net {
namespace {

using GetIfEntry2Fn = decltype(&::GetIfEntry2);
using GetBestInterfaceExFn = decltype(&::GetBestInterfaceEx);

// A routing table lookup key, not a host.
//
// GetBestInterfaceEx asks the routing table which adapter *would* carry a
// packet to this address. **Nothing is sent, no name is resolved and no
// connection is opened.** The address is a well-known globally routable one
// chosen only because it is guaranteed not to match a local subnet; any such
// address would do.
//
// It is spelled out because a thermal tool whose source suddenly contains a
// public IP address invites exactly the wrong conclusion.
// The four octets are written individually rather than through htonl, which
// would pull in ws2_32.lib -- a link dependency this feature promised not to
// add. Writing them in order is also endian-proof by construction.
constexpr unsigned char kRoutableProbeOctets[4]{8, 8, 8, 8};

}  // namespace

Sampler::~Sampler() { Close(); }

void Sampler::Close() noexcept {
    if (module_ != nullptr) {
        FreeLibrary(static_cast<HMODULE>(module_));
        module_ = nullptr;
    }
    get_entry_ = nullptr;
    best_interface_ = nullptr;
    interface_index_ = 0;
    route_checked_ms_ = 0;
    availability_ = Availability::NoLibrary;
    adapter_.clear();
}

bool Sampler::Open() noexcept {
    Close();
    // Dynamically, exactly as nvml.dll is loaded: nothing is added to the
    // link line, so the single /MT executable keeps its only dependencies.
    const HMODULE module = LoadLibraryW(L"iphlpapi.dll");
    if (module == nullptr) return false;
    module_ = module;
    get_entry_ = reinterpret_cast<void*>(GetProcAddress(module, "GetIfEntry2"));
    best_interface_ =
        reinterpret_cast<void*>(GetProcAddress(module, "GetBestInterfaceEx"));
    if (get_entry_ == nullptr || best_interface_ == nullptr) {
        Close();
        return false;
    }
    availability_ = Availability::NoRoute;
    return ResolveInterface();
}

bool Sampler::ResolveInterface() noexcept {
    if (best_interface_ == nullptr) return false;
    sockaddr_in destination{};
    destination.sin_family = AF_INET;
    destination.sin_addr.S_un.S_un_b.s_b1 = kRoutableProbeOctets[0];
    destination.sin_addr.S_un.S_un_b.s_b2 = kRoutableProbeOctets[1];
    destination.sin_addr.S_un.S_un_b.s_b3 = kRoutableProbeOctets[2];
    destination.sin_addr.S_un.S_un_b.s_b4 = kRoutableProbeOctets[3];
    DWORD index = 0;
    const DWORD status =
        reinterpret_cast<GetBestInterfaceExFn>(best_interface_)(
            reinterpret_cast<sockaddr*>(&destination), &index);
    if (status != NO_ERROR || index == 0) {
        // A machine with no route out is a normal state, not a failure: an
        // unplugged cable, an aeroplane, a VPN mid-reconnect.
        availability_ = Availability::NoRoute;
        return false;
    }
    interface_index_ = static_cast<std::uint32_t>(index);
    availability_ = Availability::Ready;
    return true;
}

std::optional<Counters> Sampler::Read(const std::uint64_t monotonic_ms) noexcept {
    if (get_entry_ == nullptr) {
        availability_ = Availability::NoLibrary;
        return std::nullopt;
    }
    // Re-check the route on a slow cadence. Roaming leaves the old adapter
    // present and perfectly readable, just idle, so waiting for a failure
    // would report an unchanging zero for as long as the reader kept looking.
    if (interface_index_ == 0 ||
        monotonic_ms - route_checked_ms_ >= kRouteRecheckMs) {
        route_checked_ms_ = monotonic_ms;
        if (!ResolveInterface() && interface_index_ == 0) return std::nullopt;
    }

    MIB_IF_ROW2 row{};
    row.InterfaceIndex = interface_index_;
    if (reinterpret_cast<GetIfEntry2Fn>(get_entry_)(&row) != NO_ERROR) {
        // The adapter was removed between readings. Drop it and let the next
        // tick pick whatever now carries the route.
        interface_index_ = 0;
        availability_ = Availability::NoInterface;
        return std::nullopt;
    }
    availability_ = Availability::Ready;
    try {
        adapter_.assign(row.Alias);
    } catch (...) {
        adapter_.clear();
    }
    return Counters{row.InOctets,
                    row.OutOctets,
                    monotonic_ms,
                    interface_index_,
                    row.ReceiveLinkSpeed,
                    row.TransmitLinkSpeed};
}

}  // namespace gtg::net
