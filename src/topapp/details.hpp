#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "topapp/board.hpp"

namespace gtg::topapp {

// How many apps Details lists for a column, and so how many the sampler keeps
// for each.
inline constexpr std::size_t kDetailsRows = 10;

// Details' columns: the four top-app records, in their order and with their
// values, then the network. Network is a column and not a record: nothing
// alerts on it and no name floats over its lane, so the alert machinery keeps
// its four.
enum class Column : std::uint8_t { Gpu = 0, Vram = 1, Cpu = 2, Ram = 3, Network = 4 };
inline constexpr std::size_t kColumnCount = 5;

[[nodiscard]] constexpr Column ColumnOf(const Record record) noexcept {
    return static_cast<Column>(record);
}

[[nodiscard]] constexpr std::uint32_t Bit(const Column column) noexcept {
    return 1U << static_cast<unsigned>(column);
}

// The process of an app that holds the most of one column: the one a reader
// asking "who" means. Several processes of one image can be different
// projects -- two Pythons of one install -- and the executable cannot tell
// them apart; the directory each works in can.
struct Representative {
    // Zero when the app holds none of the column.
    std::uint32_t pid{};
    // Its working directory, whole, as NormalDirectory makes it; empty when it
    // could not be read (another account's or a 32-bit process, a VM's).
    std::wstring directory;
    // The directory's last folder, or empty where the directory says nothing
    // about the app (FolderOf).
    std::wstring folder;

    [[nodiscard]] bool operator==(const Representative&) const = default;
};

// One app in Details: its name, how many processes it is made of, and what it
// holds of each column -- GPU and CPU in percent, VRAM and RAM in bytes, the
// network in bytes per second sent and received together. A value that could
// not be measured is empty, not zero. For each column, the process holding
// the most of it.
struct DetailsRow {
    std::wstring name;
    std::uint32_t processes{};
    std::array<std::optional<double>, kColumnCount> values;
    std::array<Representative, kColumnCount> representatives;

    [[nodiscard]] bool operator==(const DetailsRow&) const = default;
};

// The process holding the most of something, offered one process at a time:
// the larger value wins, and of two equal ones the lower id, so a sweep's
// order never decides. Nothing at or below zero is held.
struct Heaviest {
    std::uint32_t pid{};
    double value{};

    void Offer(const std::uint32_t candidate, const double amount) noexcept {
        if (!(amount > 0.0)) return;
        if (pid == 0 || amount > value || (amount == value && candidate < pid)) {
            pid = candidate;
            value = amount;
        }
    }
};

// What the sampler publishes while Details is open.
struct Details {
    // Every app among the first kDetailsRows of any column, once each, so the
    // reader can sort by any column without waiting for a sweep.
    std::vector<DetailsRow> rows;
    // Processes the sweep read.
    std::uint32_t processes{};
    // Columns this machine can measure at all, as Bit() flags: GPU and VRAM
    // need the counters and exactly one NVIDIA adapter, as the alert names do;
    // the network needs its trace, which needs Administrator.
    std::uint32_t available{};
    // Columns this sweep measured. A rate needs two sweeps, so the first one
    // after opening has CPU, GPU and the network available but not measured.
    std::uint32_t measured{};
    // What 100 % of each column is: 100 for the percentages, the adapter's
    // dedicated memory and the machine's physical memory in bytes, and for the
    // network every app's traffic added up. Zero when unknown.
    std::array<double, kColumnCount> totals{};
    // Network events the trace dropped since the last sweep: the column
    // undercounts by that much.
    std::uint32_t network_lost{};
    // The sweep budget paused sampling; there are no rows.
    bool paused{};

    [[nodiscard]] bool operator==(const Details&) const = default;
};

// The keys of the first `count` groups by value, largest first, ties by key.
// Only values above zero are ranked: an idle app is not "in the top ten".
[[nodiscard]] inline std::vector<std::wstring> TopKeys(const std::map<std::wstring, double>& groups,
                                                       const std::size_t count) {
    std::vector<std::pair<std::wstring, double>> ranked;
    ranked.reserve(groups.size());
    for (const auto& [key, value] : groups) {
        if (value > 0.0) ranked.emplace_back(key, value);
    }
    std::sort(ranked.begin(), ranked.end(), [](const auto& a, const auto& b) {
        return a.second != b.second ? a.second > b.second : a.first < b.first;
    });
    if (ranked.size() > count) ranked.resize(count);
    std::vector<std::wstring> keys;
    keys.reserve(ranked.size());
    for (auto& [key, value] : ranked)
        keys.push_back(std::move(key));
    return keys;
}

// Every key in the first `count` of any column, each once, in key order.
template <std::size_t N>
[[nodiscard]] std::vector<std::wstring> MergeTopKeys(
    const std::array<std::map<std::wstring, double>, N>& groups, const std::size_t count) {
    std::vector<std::wstring> merged;
    for (const auto& group : groups) {
        for (std::wstring& key : TopKeys(group, count))
            merged.push_back(std::move(key));
    }
    std::sort(merged.begin(), merged.end());
    merged.erase(std::unique(merged.begin(), merged.end()), merged.end());
    return merged;
}

// The rows to list for `column`: indices into `details.rows`, largest first,
// ties by name, at most `count`, and only apps holding more than nothing.
[[nodiscard]] inline std::vector<std::size_t> Ranked(const Details& details, const Column column,
                                                     const std::size_t count) {
    const auto index = static_cast<std::size_t>(column);
    std::vector<std::size_t> order;
    for (std::size_t i = 0; i < details.rows.size(); ++i) {
        const auto& value = details.rows[i].values[index];
        if (value && *value > 0.0) order.push_back(i);
    }
    std::sort(order.begin(), order.end(), [&](const std::size_t a, const std::size_t b) {
        const double left = *details.rows[a].values[index];
        const double right = *details.rows[b].values[index];
        return left != right ? left > right : details.rows[a].name < details.rows[b].name;
    });
    if (order.size() > count) order.resize(count);
    return order;
}

// --- The network column's events ---------------------------------------------
//
// Microsoft-Windows-Kernel-Network reports each send and receive of TCP and
// UDP, over IPv4 and IPv6, with the owning process and the byte count as the
// payload's first two 32-bit fields (the provider's manifest: `PID`, `size`).
// The event header's process id is NOT that process: on this machine it
// differed in 40 % of events, since a receive is often reported in another
// process's context.

// TCP sent/received over IPv4 and IPv6, then UDP the same.
inline constexpr std::array<std::uint16_t, 8> kNetworkDataEvents{10, 11, 26, 27, 42, 43, 58, 59};

[[nodiscard]] constexpr bool IsNetworkDataEvent(const std::uint16_t id) noexcept {
    for (const std::uint16_t data : kNetworkDataEvents) {
        if (data == id) return true;
    }
    return false;
}

struct NetworkEvent {
    std::uint32_t pid{};
    std::uint32_t bytes{};
};

// The process and byte count of one data event, or nothing for another event
// or a payload too short to hold them.
[[nodiscard]] inline std::optional<NetworkEvent> DecodeNetworkEvent(
    const std::uint16_t id, const void* payload, const std::size_t length) noexcept {
    if (!IsNetworkDataEvent(id) || payload == nullptr || length < 8) return std::nullopt;
    NetworkEvent event;
    std::memcpy(&event.pid, payload, 4);
    std::memcpy(&event.bytes, static_cast<const std::uint8_t*>(payload) + 4, 4);
    return event;
}

// Bytes per second per app, from the bytes each process moved over `seconds`
// and the app each process belongs to. A process the sweep did not see -- it
// exited between the two, or it is the kernel's own (PID 0) -- is not
// listed: there is no app to name.
template <class KeyByPid>
[[nodiscard]] std::map<std::wstring, double> NetworkByApp(
    const std::vector<std::pair<std::uint32_t, std::uint64_t>>& bytes_by_pid,
    const KeyByPid& key_by_pid, const double seconds) {
    std::map<std::wstring, double> rates;
    if (!(seconds > 0.0)) return rates;
    for (const auto& [pid, bytes] : bytes_by_pid) {
        const auto key = key_by_pid.find(pid);
        if (key == key_by_pid.end() || bytes == 0) continue;
        rates[key->second] += static_cast<double>(bytes) / seconds;
    }
    return rates;
}

}  // namespace gtg::topapp
