#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include "net/throughput.hpp"

// Reading the adapter, kept behind a declaration that names no Win32 type.
//
// `iphlpapi.h` requires `winsock2.h` ahead of `windows.h`, and every caller
// here has already included `windows.h` long before. Rather than impose an
// include order on the whole tray, the implementation keeps its own in a .cpp
// and this header stays plain C++.
namespace gtg::net {

// Why a reading is unavailable. The cell shows the same dash either way, but
// the journal should be able to say which, and "this build cannot do it at
// all" is worth distinguishing from "nothing is routed right now".
enum class Availability {
    Ready,
    NoLibrary,      // iphlpapi.dll absent, or an export missing
    NoRoute,        // nothing to the outside world
    NoInterface,    // the adapter went away between two readings
};

// Owns the dynamically loaded module and the interface it is watching.
//
// Not copyable: it holds a module handle. One instance lives on the tray
// dialog and is read from the tray's own refresh timer, never from the
// protection worker.
class Sampler final {
public:
    Sampler() = default;
    ~Sampler();

    Sampler(const Sampler&) = delete;
    Sampler& operator=(const Sampler&) = delete;

    // Resolves iphlpapi and picks the interface the default route uses.
    // Returns false when the feature simply cannot run here, which is a
    // reportable state rather than an error.
    [[nodiscard]] bool Open() noexcept;
    void Close() noexcept;

    // The cumulative counters for the watched interface, or nothing.
    [[nodiscard]] std::optional<Counters> Read(std::uint64_t monotonic_ms) noexcept;

    [[nodiscard]] Availability availability() const noexcept { return availability_; }
    // The adapter's own name, for the main window. Empty until the first read.
    [[nodiscard]] const std::wstring& adapter() const noexcept { return adapter_; }

private:
    // The route is re-checked on a slow cadence rather than every reading.
    // Roaming between Wi-Fi and Ethernet leaves the old adapter present and
    // readable -- it simply stops carrying traffic -- so a failure alone would
    // never notice. Measured at 191 us, so twice a second would be wasteful
    // and once every few seconds costs nothing.
    static constexpr std::uint64_t kRouteRecheckMs = 3'000;

    [[nodiscard]] bool ResolveInterface() noexcept;

    void* module_{nullptr};
    void* get_entry_{nullptr};
    void* best_interface_{nullptr};
    std::uint32_t interface_index_{0};
    std::uint64_t route_checked_ms_{0};
    Availability availability_{Availability::NoLibrary};
    std::wstring adapter_;
};

}  // namespace gtg::net
